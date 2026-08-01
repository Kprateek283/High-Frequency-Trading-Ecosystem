# Machine Profile — the box every number in this repo was measured on

Every benchmark figure in this repository comes from one laptop. This page records
exactly what that machine is, so a reader can tell which results are properties of the
design and which are properties of the hardware.

Captured 2026-08-01. Regenerate with `scripts/machine_profile.sh`.

---

## CPU — Intel Core i5-1240P (Alder Lake-P, hybrid)

| | |
|---|---|
| Model | 12th Gen Intel Core i5-1240P |
| Physical cores | **12** — 4 performance (P) + 8 efficiency (E) |
| Logical CPUs | **16** — P-cores are SMT (2 threads each), E-cores are not |
| Max clock | **4400 MHz** on P-cores, **3300 MHz** on E-cores |
| Min clock | 400 MHz |
| L1d / L1i | 448 KiB / 640 KiB (12 instances) |
| L2 | 9 MiB (6 instances) |
| L3 | 12 MiB, shared |
| NUMA nodes | 1 |
| TSC | `constant_tsc`, `nonstop_tsc`; clocksource `tsc` |

### Topology — which logical CPU is what

This mapping is load-bearing for every pinning decision in the project:

| Logical CPU | Physical core | Type | Max MHz |
|---|---|---|---|
| 0, 1 | 0 | P (SMT pair) | 4400 |
| 2, 3 | 1 | P (SMT pair) | 4400 |
| 4, 5 | 2 | P (SMT pair) | 4400 |
| 6, 7 | 3 | P (SMT pair) | 4400 |
| 8 | 4 | E | 3300 |
| 9 | 5 | E | 3300 |
| 10 | 6 | E | 3300 |
| 11 | 7 | E | 3300 |
| 12 | 8 | E | 3300 |
| 13 | 9 | E | 3300 |
| 14 | 10 | E | 3300 |
| 15 | 11 | E | 3300 |

Two traps follow from this and have both already produced wrong conclusions in this
repo:

1. **"CPUs 0–7" is four physical cores, not eight.** Pinning eight threads to `0-7`
   guarantees SMT sharing. This is the source of the bimodal cycles/order in
   [`bottlenecks.md`](./bottlenecks.md) §10.
2. **CPU 8 is not a P-core.** Anything counting "the first eight CPUs" as the fast ones
   is wrong past CPU 7.

A third trap is reading `lscpu`'s *current* MHz as the core's capability: an idle P-core
reports ~400–1700 MHz under the `powersave` governor. Only `MAXMHZ` distinguishes P
from E.

---

## Memory

| | |
|---|---|
| Total | 15 GiB |
| Huge pages | **0 configured** (`vm.nr_hugepages = 0`) |

`MemoryPool` requests huge pages and falls back, which is why every run prints
`[MemoryPool] Warning: Huge Pages failed, falling back to 4KB pages.` That warning is
expected on this box, not a fault.

---

## OS and toolchain

| | |
|---|---|
| Distribution | Fedora Linux 44 (Workstation Edition) |
| Kernel | 6.19.10-300.fc44.x86_64 |
| Kernel cmdline | `ro rhgb quiet mem_sleep_default=deep` — **no `isolcpus`, no `nohz_full`, no `rcu_nocbs`** |
| GCC | 16.1.1 |
| CMake | 4.3.0 |
| Python | 3.14.3 |
| SELinux | **Enforcing** |

SELinux enforcing matters for one practical reason: a binary in `$HOME` is labelled
`user_home_t` and **cannot be executed by a systemd system service** (`203/EXEC`,
Permission denied). `sudo systemd-run ... ./build/bin/exchange` fails for this reason,
not for a permissions or path reason.

---

## Power and frequency — the numbers move on their own here

| | |
|---|---|
| Scaling driver | `intel_pstate` |
| Governor | `powersave` |
| Turbo | enabled (`no_turbo = 0`) |
| tuned profile | **`balanced-battery`** (tuned is active) |
| On AC | **no — running on battery** |

**`tuned` will silently undo a manual governor change.** Setting `performance` with
`cpupower` works, and then `tuned` reverts it when the power source or profile dictates.
This happened mid-session: a sweep intended to run under `performance` recorded
`governor=powersave` in its own header. The results header is the only reliable record
of what a run actually used — do not trust a governor you set earlier in the session.

To hold `performance`, either put the machine on AC and set a matching tuned profile
(`sudo tuned-adm profile throughput-performance`) or stop tuned for the duration.

---

## Realtime scheduling

| | |
|---|---|
| `RLIMIT_RTPRIO` | **80** (via `/etc/security/limits.d/99-hft-realtime.conf`) |
| `RLIMIT_MEMLOCK` | unlimited |
| `kernel.sched_rt_runtime_us` | 950000 |
| `kernel.sched_rt_period_us` | 1000000 |

RT tasks are capped at 950 ms per 1000 ms period. Since the engine busy-spins, this
costs a ~50 ms deschedule every second. See [`scheduling.md`](./scheduling.md).

PAM applies limits **at session creation**. A logout is not enough if the systemd user
manager and the tmux server survive it — both keep the old limits, and everything
launched under them inherits `rtprio 0`. Reboot, or `loginctl terminate-user`.

---

## Network — the benchmark never touches a NIC

| Interface | State | Note |
|---|---|---|
| `lo` | UP, MTU 65536 | **all benchmark traffic** |
| `enp43s0` | DOWN | wired, unused |
| `wlp0s20f3` | UP | wifi, the machine's actual connectivity |
| `br-*` | UP | docker bridge |

Client and exchange are both local, so ingest goes over loopback with a 65536-byte MTU —
no driver, no PHY, no real NIC interrupt path. Loopback is *faster* and lower-jitter than
a physical 10G path, so absolute latency here is optimistic relative to a real venue; it
also means `SO_BUSY_POLL` and any kernel-bypass argument cannot be evaluated on this box.

**Wifi interrupts land on the hot-path CPUs.** `iwlwifi` IRQs are serviced on CPUs 3, 5,
6 and 7 — the exact P-core siblings the gateway workers pin to. IRQ affinity has to move
before `isolcpus` will actually isolate anything.

---

## Repository core map (`config.env`) — currently misaligned

```
GATEWAY_THREADS=4
GATEWAY_CORES=1,3,5,7
ENGINE_CORES=2,4,6,8
AUX_CORES=0,9
```

Resolved against the topology above:

| Physical core | Type | Occupants |
|---|---|---|
| 0 | P | aux0 (publisher) @cpu0 + gateway worker @cpu1 — **collision** |
| 1 | P | engine shard 0 @cpu2 + gateway worker @cpu3 — **collision** |
| 2 | P | engine shard 1 @cpu4 + gateway worker @cpu5 — **collision** |
| 3 | P | engine shard 2 @cpu6 + gateway worker @cpu7 — **collision** |
| 4 | **E** | engine shard 3 @cpu8 — **on an efficiency core** |
| 5 | **E** | aux1 (order_manager) @cpu9 |

Every engine shard shares a physical core with a gateway worker, and the fourth shard
runs on a 3300 MHz E-core while its peers run at 4400 MHz. The map reads as "even CPUs
for engines, odd for gateway", which would be correct on a non-SMT, non-hybrid machine.
It is wrong on this one, and it is a direct cause of the per-order cost variance recorded
in [`bottlenecks.md`](./bottlenecks.md) §10.

Fixing this is a prerequisite for the `isolcpus` work, not a separate cleanup.

---

## What this machine cannot measure

- **Any effect smaller than roughly 30%.** This is the binding limitation, and it is
  easy to miss because it is not about the ceiling — it is about resolution. Measured
  run-to-run spread across four 9-run arms was 13–101%, most points landing at 35–75%:

  | Arm | 4×1 | 4×2 | 4×4 |
  | --- | ---: | ---: | ---: |
  | baseline | 13% | 7% | 53% |
  | corrected map + pinned generators | 38% | 72% | 19% |
  | corrected map, generators free | 35% | **101%** | 77% |
  | gateway unpinned | 66% | 66% | 64% |

  Concretely: the corrected core map measured −6%, which is indistinguishable from zero
  here, so it was published as "no measured effect" rather than as a number. Pinning the
  load generators measured +31% and *did* clear the floor. Most worthwhile optimisations
  to this engine land in the 5–20% band — below this box's resolution entirely.
- **Latency percentiles.** No `isolcpus`, a scaling governor that reverts itself, and RT
  throttling against a busy-poll engine. p99/p99.9 here describe the Linux scheduler.
- **True ingest ceiling.** The load generators share the box with the exchange; at the
  published operating point the gateway is ~48% idle and the engine shards ~89% idle.
- **Anything about real NICs.** Loopback only, 65536 MTU, no driver and no interrupts —
  so the `t4→t5` "TCP path" is a memcpy through the kernel, and the kernel-bypass case
  (DPDK / `ef_vi`) is not merely unmeasured here but unevaluable.
- **Whether `SCHED_FIFO` helps the *design*.** [`scheduling.md`](./scheduling.md) records
  that it hurts *this box* by 59% at one client. That is a statement about 4 physical
  cores hosting 11 realtime threads, not about realtime scheduling. On a machine with
  more isolated cores than realtime threads the conclusion may well invert.
- **Sustained all-core clocks.** Battery power plus a laptop thermal envelope.

What it *can* measure reliably: per-order cycle counts (TSC is invariant, so cycle deltas
are real work rather than frequency scaling), **large** relative differences between
configurations run back-to-back under identical settings, correctness under ASan/TSan,
and the shape of scaling curves.

The practical rule this yields: use this box to answer *"did that make things better or
worse"* when the change is big, and do not use it to answer *"by how much"* — or to
answer anything at all when the change is small.
