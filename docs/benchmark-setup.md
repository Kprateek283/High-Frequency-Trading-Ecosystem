# Benchmark Setup — Reproducing the Measurements

The measurement harness is complete and runs on any Linux box. But the **absolute**
latency and throughput numbers are only trustworthy on hardware configured for
deterministic execution. On a default laptop (power-saving governor, no real-time
priority, shared cores) the same code produces a *lower bound* — the tail describes
the Linux scheduler, not the engine.

This guide covers the three OS-level prerequisites, how to verify each, and the two
commands that produce the full capacity matrix. **No code changes are required** — this
is environment setup only.

---

## TL;DR

```bash
# 1. Real-time scheduling privilege (needs root; `ulimit -r` cannot self-raise)
sudo cp scripts/99-hft-realtime.conf /etc/security/limits.d/   # then re-login

# 2. Performance CPU governor (all cores)
sudo cpupower frequency-set -g performance

# 3. Isolated cores  -> set once in GRUB + reboot (see §3). Verify:
cat /proc/cmdline    # expect: ... isolcpus=1-8 nohz_full=1-8 rcu_nocbs=1-8

# Then run the matrix:
for gt in 1 2 4 8; do GATEWAY_THREADS=$gt ./scripts/run_sharding.sh; done   # -> results.txt
python3 scripts/measure_throughput.py                                       # -> benchmark_results.txt
```

Both output files record the environment in their header, so you can always tell
whether a run was done under the right conditions or is a lower bound.

---

## Why these three

The engine pins its threads with `pthread_setaffinity_np` + `SCHED_FIFO` (Phase 3.2)
and calibrates the TSC at startup. That machinery only pays off when the OS actually
grants what it asks for:

| Prerequisite | Without it | Effect on the numbers |
|---|---|---|
| `SCHED_FIFO` priority | `pthread_setschedparam` fails silently; threads run `SCHED_OTHER` | Arbitrary preemption inflates p99/p99.9 — measures the scheduler |
| `performance` governor | Cores idle down to power-saving clocks between bursts | Frequency scaling adds variance; first-order latency wrong |
| `isolcpus` | The load generator and OS share the engine's cores | Oversubscription and migration; throughput plateaus early |

---

## 1. `SCHED_FIFO` — real-time scheduling privilege

The gateway workers, engine shards, publisher, and OrderManager request
`SCHED_FIFO` at `RT_PRIORITY` (default **80**, see `core/realtime.h`). A normal user has
an `RLIMIT_RTPRIO` (`ulimit -r`) of `0`, so the request fails with `EPERM` and every
thread runs `SCHED_OTHER`.

> **`ulimit -r unlimited` does not work**, and an earlier version of this page wrongly
> recommended it. The *hard* limit is also 0 on a default Fedora/Ubuntu install, and an
> unprivileged process cannot raise its own hard limit:
> `bash: ulimit: real-time priority: cannot modify limit: Operation not permitted`.
> The limit has to be granted by root, via PAM, before login.

**Grant it:**

```bash
sudo cp scripts/99-hft-realtime.conf /etc/security/limits.d/
# log out and back in -- PAM applies limits at login, not to running shells
ulimit -r          # want: 80
```

To try it without logging out, run the exchange under a systemd scope that carries
the limit:

```bash
sudo systemd-run --uid=$(id -u) --pty -p LimitRTPRIO=80 ./build/bin/exchange
```

**Verify what actually happened, not what was permitted.** The exchange prints a line
on stdout at the READY barrier and again at shutdown:

```
RT_SCHED: granted=11/11 priority=80     # every thread got it
RT_SCHED: granted=0/11 priority=80      # none did -- the run is SCHED_OTHER
```

`benchmark_results.txt` records this per sweep. It exists because `ulimit -r` in the
*harness* process only describes what was permitted; the exchange's own stderr warnings
were being sent to `/dev/null` by `measure_throughput.py`, so a run whose threads all
failed to reach `SCHED_FIFO` was indistinguishable from one where they succeeded.

**Priority is 80, not 99.** 99 shares a band with kernel migration and watchdog threads.
Override with `RT_PRIORITY=<0-99>`; `RT_PRIORITY=0` disables the request entirely.

### The trap: RT throttling turns a busy-poll loop into a 50 ms stall

This is the part that will quietly ruin a latency measurement.

The engine never blocks. `engine.cpp` polls its queues and, when empty, executes
`__builtin_ia32_pause()` and loops — so an engine shard consumes 100% of its core
whether it is processing 2M orders/sec or none. That is deliberate for latency, and it
collides with the kernel's realtime throttle:

```bash
cat /proc/sys/kernel/sched_rt_runtime_us   # 950000
cat /proc/sys/kernel/sched_rt_period_us    # 1000000
```

RT tasks may run at most 950 ms of every 1000 ms period. A thread that spins 100% of the
time therefore gets **forcibly descheduled for ~50 ms every second** — a 50,000 µs
stall that would dominate any p99.9 figure and be trivially mistaken for a queueing
pathology in the engine.

So `SCHED_FIFO` alone does not make the tail publishable. Either:

- **Keep the throttle** (safe default). Median and p99 improve; p99.9 will show the
  throttle stalls. Say so when publishing, or
- **Disable it** — `sudo sysctl -w kernel.sched_rt_runtime_us=-1` — which is only
  defensible together with §3 `isolcpus`, so the spinning RT threads sit on cores
  nothing else needs. Without isolation a spinning RT thread starves `ksoftirqd` on its
  core, and since this workload's ingress *is* network softirq processing, that breaks
  the very path being measured. Have a way back in (SSH from another box, or SysRq)
  before trying it on a laptop.

---

## 2. `performance` CPU governor

```bash
sudo cpupower frequency-set -g performance
# no cpupower? write it directly:
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

**Verify:**
```bash
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor    # want: performance
```
Optional but recommended: disable turbo so the clock is fixed (the calibrated TSC
frequency is stable regardless, but a fixed clock removes per-run variance):
```bash
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo
```

---

## 3. `isolcpus` — dedicate cores to the engine

Isolate the cores the engine pins to, so the Linux scheduler places nothing else
there and the load generator runs elsewhere. The core map lives in `config.env`:

```
GATEWAY_CORES=1,3,5,7     # gateway workers
ENGINE_CORES=0,2,4,6      # engine shards
AUX_CORES=8,9,10          # publisher, order_manager, dispatcher
```

So isolate **CPUs 0–7** — the gateway + engine hot path, which on this box is four
physical P-cores and their SMT siblings. CPUs **8–10** host the cold aux threads and stay
non-isolated alongside the OS; **11–15** are where the load generators run
(`TESTER_CPUSET`). Edit the kernel command line in `/etc/default/grub`:

```
GRUB_CMDLINE_LINUX_DEFAULT="quiet splash isolcpus=0-7 nohz_full=0-7 rcu_nocbs=0-7"
```

> **`scripts/setup_isolcpus.sh` is stale** — it still writes `isolcpus=1-8`, which
> matches neither this map nor the P/E boundary at CPU 7 (CPU 8 is an efficiency core).
> Do not run it without editing it first.
>
> **Isolating alone will not give you the hot path.** `iwlwifi` IRQs are currently
> serviced on CPUs 3, 5, 6 and 7 — inside the set above — so IRQ affinity has to move
> first. See [`machine-profile.md`](./machine-profile.md).
>
> And note what isolation cannot fix here: 11 threads request realtime and there are only
> 4 physical P-cores, which is why `SCHED_FIFO` currently measures *slower*
> ([`scheduling.md`](./scheduling.md)).

Then:
```bash
sudo update-grub && sudo reboot
```

**Verify:**
```bash
cat /proc/cmdline                                   # shows isolcpus=1-8 ...
```
`measure_throughput.py` reads and records `isolcpus` in its header, so a run will show
`isolcpus=none` if it wasn't set.

> Keep `isolcpus` in sync with `config.env`. If you change `GATEWAY_CORES` /
> `ENGINE_CORES`, isolate the same set. Isolating fewer cores than the engine pins to
> re-introduces oversubscription on the un-isolated ones.

---

## Running the matrix

```bash
# Latency percentiles + gateway cycle attribution + accepted/rejected split,
# one results.txt per gateway thread count (the matrix rows):
for gt in 1 2 4 8; do GATEWAY_THREADS=$gt ./scripts/run_sharding.sh; done

# Ingest throughput sweep (gateway workers x concurrent clients):
python3 scripts/measure_throughput.py
```

`run_sharding.sh` sets `LATENCY_PROFILE=1`, so `results.txt` includes:
- accepted / matched / rejected split (from the audit log),
- engine matching-latency percentiles (P50/P99/P99.9, ingress → match),
- gateway end-to-end (`t1→t5`) and TCP-path (`t4→t5`) latency percentiles,
- per-stage gateway cycle attribution (`epoll` / `read` / decode / validate / enqueue),
- the calibrated TSC frequency used for every cycles → ns conversion.

`benchmark_results.txt` holds the throughput sweep with its own environment header.

---

## How to know your run is publishable, not a lower bound

Check the header lines the tools write. A **good** run looks like:

```
# cores=… load1=0.xx governor=performance rtprio_limit=unlimited isolcpus=1-8
```

A **lower-bound** run (like the one committed in this repo, taken on a laptop) looks
like:

```
# cores=16 load1=1.90 governor=powersave rtprio_limit=0 isolcpus=…
# WARNING: this run had no SCHED_FIFO privilege and/or a scaling governor …
```

If you see `governor=powersave`, `rtprio_limit=0`, or `isolcpus=none`, fix the
corresponding step above and re-run — the code is already correct; only the
environment needs to change.

See [`benchmarks.md`](./benchmarks.md) for what each number means and the current
(lower-bound) values measured on the development laptop.
