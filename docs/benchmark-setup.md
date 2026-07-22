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
# 1. Real-time scheduling privilege (this shell + children)
ulimit -r unlimited

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
`SCHED_FIFO` priority 99. A normal user has an `RLIMIT_RTPRIO` (`ulimit -r`) of `0`, so
the request fails and the code falls back to normal scheduling (it warns to stderr).

**Grant it** — pick one:

```bash
# Per shell (simplest; must launch the exchange from this same shell):
ulimit -r unlimited

# Or persist for your user (survives reboot):
echo "$USER  -  rtprio  unlimited" | sudo tee -a /etc/security/limits.conf
#   log out and back in

# Or run the exchange as root / with CAP_SYS_NICE.
```

**Verify:**
```bash
ulimit -r          # want: unlimited  (or >= 99)
```
After a run, check the results header: `rtprio_limit=0` means it did **not** take.

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
ENGINE_CORES=2,4,6,8      # engine shards
AUX_CORES=0,9             # publisher, order_manager
```

So isolate **cores 1–8** (leave 0 and the rest for the OS + load generator). Edit the
kernel command line in `/etc/default/grub`:

```
GRUB_CMDLINE_LINUX_DEFAULT="quiet splash isolcpus=1-8 nohz_full=1-8 rcu_nocbs=1-8"
```

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
