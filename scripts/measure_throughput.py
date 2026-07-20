#!/usr/bin/env python3
"""Measure gateway ingest throughput and write a traceable results file.

Reads orders_in from the shared stats region rather than trusting the client's
send rate: a client's send() returns once the data is buffered, so its reported
"throughput" is offered load, not what the engine consumed.

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
SWEEP = ((1, 1), (4, 1), (4, 2), (4, 4), (4, 8))           # (workers, clients)
RUNS = 3
WINDOW_S = 1.5


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
    }


def one_run(workers, clients):
    env = dict(os.environ, GATEWAY_THREADS=str(workers),
               AUDIT_LOG_PATH="/tmp/measure_audit.log")
    eng = subprocess.Popen([f"{ROOT}/build/bin/exchange"], cwd=ROOT, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    for line in eng.stdout:                                 # the I8 READY barrier
        if line.strip() == "READY":
            break

    reader = StatsReader(Config().get_path("STATS_SHM_PATH"))
    procs = [subprocess.Popen([f"{ROOT}/build/bin/tester"], cwd=ROOT,
                              env=dict(env, TARGET_RATE="0", WORKLOAD_TYPE="2"),
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
             for _ in range(clients)]
    try:
        time.sleep(0.3)                                     # let the pipes fill
        s0, t0 = reader.read(), time.monotonic()
        time.sleep(WINDOW_S)
        s1, t1 = reader.read(), time.monotonic()
        orders = (sum(sh.orders_in for sh in s1.shards)
                  - sum(sh.orders_in for sh in s0.shards))
        return orders / (t1 - t0)
    finally:
        for p in procs:
            p.terminate()
        for p in procs:
            p.wait(timeout=60)
        reader.close()
        eng.send_signal(2)                                  # SIGINT
        eng.wait(timeout=180)                               # draining a backlog is slow
        time.sleep(1)


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "benchmark_results.txt")
    if not os.path.exists(f"{ROOT}/build/bin/exchange"):
        raise SystemExit("build/bin/exchange not found -- build first")

    env = environment()
    lines = [f"# Gateway ingest sweep  {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}",
             f"# cores={env['cores']} load1={env['load_1min']:.2f} "
             f"governor={env['governor']} rtprio_limit={env['rtprio_limit']} "
             f"isolcpus={env['isolcpus']}",
             "# orders/s measured from stats-region orders_in over a "
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
