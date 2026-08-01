#!/usr/bin/env python3
"""Measure gateway ingest throughput and write a traceable results file.

Reads engine_orders_in from the shared stats region rather than trusting the
client's send rate: a client's send() returns once the data is buffered, so its
reported "throughput" is offered load, not what the engine consumed.

The counter choice is load-bearing. orders_in is incremented by the OrderManager,
three hops downstream of the engine, and undercounts whenever the drop-copy queue
overflows — reading it measures the audit logger. engine_orders_in is incremented
by the matching engine itself, per shard, on its own cache line.

SO_REUSEPORT distributes accepted CONNECTIONS across gateway workers, so N
workers only do N workers' worth of work if at least N clients are connected.
The sweep therefore varies clients as well as workers.

The environment block is not decoration. Without SCHED_FIFO (ulimit -r), with a
scaling governor, or on a loaded box, the absolute ceiling and especially the
latency tails are not meaningful -- the numbers describe this machine, not the
design. Re-run on an isolated box to populate the real figures.

Usage:  python3 scripts/measure_throughput.py [output_file]
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from monitoring.config import Config                       # noqa: E402
from monitoring.feeds.stats_reader import StatsReader      # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Root-level cmake drops binaries in build/bin; a standalone `cmake -S hft_engine`
# drops them in build/. Match run_sharding.sh and accept either.
BIN = next((d for d in (f"{ROOT}/build/bin", f"{ROOT}/build")
            if os.path.exists(f"{d}/exchange")), f"{ROOT}/build/bin")
SWEEP = ((4, 1), (4, 2), (4, 4))
RUNS = 9
WINDOW_S = 1.5
RUN_TIMEOUT_S = 240       # watchdog per run; generous vs the 180s drain wait below
# Which CPUs the exchange is pinned to. "0-7" is the four P-cores *including*
# their SMT siblings; "0,2,4,6" is one thread per physical P-core. The default
# extends to 10 because AUX_CORES puts the three cold threads on E-cores 8-10 --
# taskset's mask is a hard ceiling, so a pin outside it just fails.
CPUSET = os.environ.get("EXCHANGE_CPUSET", "0-10")
# Where the load generators run. Unpinned testers float across all 16 CPUs and
# compete with the exchange for the very P-cores being measured.
TESTER_CPUSET = os.environ.get("TESTER_CPUSET", "11-15")
RT_STATUS = []            # one "RT_SCHED: granted=N/M priority=P" per run

# The core map has to be pushed into the child's environment explicitly. The C++
# side reads GATEWAY_CORES/ENGINE_CORES/AUX_CORES with getenv(), and only the
# bash entry points (`run_sharding.sh`, `multi_firm_run.sh`) `source config.env`.
# Python inherits os.environ, which does NOT contain them -- so before this,
# every sweep silently ran on the hardcoded fallbacks in exchange.cpp/tcp_server.h,
# where an unset GATEWAY_CORES means the gateway workers are not pinned at all.
CFG = Config()
CORE_ENV = {k: CFG.get(k) for k in ("GATEWAY_CORES", "ENGINE_CORES", "AUX_CORES")}


def _read(path, default="n/a"):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def environment():
    isol = [w for w in _read("/proc/cmdline", "").split() if w.startswith("isolcpus=")]
    try:
        import resource
        rt = resource.getrlimit(resource.RLIMIT_RTPRIO)[0]
    except Exception:                                       # noqa: BLE001
        rt = "n/a"
    return {
        "cores": os.cpu_count(),
        "load_1min": os.getloadavg()[0],
        "governor": _read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"),
        "rtprio_limit": rt,                                 # 0 => no SCHED_FIFO
        "isolcpus": isol[0].split("=", 1)[1] if isol else "none",
        "cpuset": CPUSET,
        "tester_cpuset": TESTER_CPUSET,
        "cores_env": " ".join(f"{k}={v}" for k, v in CORE_ENV.items()),
    }


def one_run(workers, clients):
    env = dict(os.environ, **CORE_ENV, GATEWAY_THREADS=str(workers),
               AUDIT_LOG_PATH="/dev/shm/measure_audit.log")
    # `timeout` is a watchdog, not a schedule. A run that wedges -- which is a real
    # possibility with RT_PRIORITY>0, where SCHED_FIFO shards busy-spin and cannot
    # be preempted by the harness trying to stop them -- dies on its own instead of
    # taking the machine with it. A healthy run is killed by SIGINT long before this.
    eng = subprocess.Popen(["timeout", "-s", "INT", "-k", "10", str(RUN_TIMEOUT_S),
                            "taskset", "-c", CPUSET, f"{BIN}/exchange"],
                           cwd=ROOT, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    rt_line = "RT_SCHED: not reported"
    for line in eng.stdout:                                 # the I8 READY barrier
        if line.startswith("RT_SCHED:"):
            # What the exchange actually got, not what `ulimit -r` promised. The
            # exchange's stderr goes to /dev/null, so before this existed a run
            # whose threads all failed to reach SCHED_FIFO looked identical to
            # one where they succeeded.
            rt_line = line.strip()
        if line.strip() == "READY":
            break
    RT_STATUS.append(rt_line)

    reader = StatsReader(Config().get_path("STATS_SHM_PATH"))
    procs = [subprocess.Popen(["taskset", "-c", TESTER_CPUSET, f"{BIN}/tester"], cwd=ROOT,
                              env=dict(env, TARGET_RATE="0", WORKLOAD_TYPE="2"),
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
             for _ in range(clients)]
    try:
        time.sleep(0.3)                                     # let the pipes fill
        s0, t0 = reader.read(), time.monotonic()
        time.sleep(WINDOW_S)
        s1, t1 = reader.read(), time.monotonic()
        orders = (sum(sh.engine_orders_in for sh in s1.shards)
                  - sum(sh.engine_orders_in for sh in s0.shards))
        return orders / (t1 - t0)
    finally:
        for p in procs:
            p.terminate()
        for p in procs:
            p.wait(timeout=60)
        reader.close()
        eng.send_signal(2)                                  # SIGINT
        eng.wait(timeout=180)                               # draining a backlog is slow
        print(f"\n--- Engine stdout for {workers} workers, {clients} clients ---")
        print(eng.stdout.read())
        print("------------------------------------------------------\n")
        time.sleep(1)


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "benchmark_results.txt")
    if not os.path.exists(f"{BIN}/exchange"):
        raise SystemExit(f"{BIN}/exchange not found -- build first")

    env = environment()
    lines = [f"# Gateway ingest sweep  {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}",
             f"# cores={env['cores']} load1={env['load_1min']:.2f} "
             f"governor={env['governor']} rtprio_limit={env['rtprio_limit']} "
             f"isolcpus={env['isolcpus']} exchange_cpuset={env['cpuset']} "
             f"tester_cpuset={env['tester_cpuset']}",
             f"# core map: {env['cores_env']}",
             "# realtime: PENDING",      # filled in after the sweep, from the runs
             "# orders/s measured from stats-region engine_orders_in (incremented by "
             "the matching engine itself, not the downstream OrderManager) over a "
             f"{WINDOW_S}s window, {RUNS} runs per point (median reported)",
             "#",
             "# workers  clients  median_orders_per_s  spread_pct"]

    print("\n".join(lines))
    for workers, clients in SWEEP:
        rates = sorted(one_run(workers, clients) for _ in range(RUNS))
        median = rates[len(rates) // 2]
        spread = (rates[-1] - rates[0]) / (sum(rates) / len(rates)) * 100
        row = f"{workers:9d}  {clients:7d}  {median:19.0f}  {spread:9.1f}"
        print(row)
        lines.append(row)

    # What the exchange actually achieved, not what ulimit promised. If any run
    # differed, say so rather than reporting one of them.
    seen = sorted(set(RT_STATUS))
    lines[lines.index("# realtime: PENDING")] = (
        "# realtime (achieved by the exchange, not promised by ulimit): "
        + (" | ".join(seen) if seen else "n/a"))

    if env["rtprio_limit"] == 0 or env["governor"] not in ("performance",):
        lines += ["#",
                  "# WARNING: this run had no SCHED_FIFO privilege and/or a scaling",
                  "# governor. Treat the shape (does it scale?) as meaningful and the",
                  "# absolute ceiling as a lower bound for this box only."]

    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"\nwritten to {out_path}")


if __name__ == "__main__":
    main()
