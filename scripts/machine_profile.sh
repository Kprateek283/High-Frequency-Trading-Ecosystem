#!/usr/bin/env bash
# Dump the machine facts that docs/machine-profile.md records, so the doc can be
# checked against reality instead of trusted. Read-only; no sudo.
#
#   ./scripts/machine_profile.sh
set -uo pipefail

hdr() { printf '\n=== %s ===\n' "$1"; }

hdr "CPU"
lscpu | grep -E "^Model name|^Architecture|^CPU\(s\)|^Thread\(s\) per core|^Core\(s\) per socket|^Socket|^NUMA node\(s\)|^CPU max|^CPU min|^L1d|^L1i|^L2|^L3"

hdr "Topology (P-cores have the higher MAXMHZ; SMT siblings share a CORE)"
lscpu -e=CPU,CORE,SOCKET,MAXMHZ

hdr "Memory"
free -h | head -2
echo "hugepages: $(cat /proc/sys/vm/nr_hugepages)"

hdr "OS / toolchain"
. /etc/os-release && echo "$PRETTY_NAME"
uname -r
echo "cmdline: $(cat /proc/cmdline)"
gcc --version | head -1
cmake --version | head -1
python3 --version
echo "selinux: $(getenforce 2>/dev/null || echo n/a)"

hdr "Power / frequency"
echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
echo "driver:   $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver)"
echo "no_turbo: $(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo n/a)"
echo "tuned:    $(tuned-adm active 2>/dev/null | tail -1 || echo 'not installed')"
echo "on_AC:    $(cat /sys/class/power_supply/AC*/online 2>/dev/null | head -1 || echo n/a)"

hdr "Realtime"
echo "ulimit -r (RLIMIT_RTPRIO): $(ulimit -r)"
echo "ulimit -l (RLIMIT_MEMLOCK): $(ulimit -l)"
for s in kernel.sched_rt_runtime_us kernel.sched_rt_period_us; do
    echo "$s = $(sysctl -n $s 2>/dev/null)"
done

hdr "Network (the benchmark runs over loopback)"
ip -br link 2>/dev/null | head -6
echo "IRQs on hot-path CPUs:"
grep -E "iwlwifi|eth|enp|wlp" /proc/interrupts 2>/dev/null | head -4

hdr "TSC"
echo "clocksource: $(cat /sys/devices/system/clocksource/clocksource0/current_clocksource)"
grep -o "constant_tsc\|nonstop_tsc" /proc/cpuinfo | sort -u | paste -sd' '

hdr "config.env core map, resolved against the real topology"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python3 - "$ROOT" <<'PY'
import subprocess, sys, os, re
root = sys.argv[1]
env = {}
for line in open(os.path.join(root, "config.env")):
    m = re.match(r"\s*(\w+)\s*=\s*([^\s#]+)", line)
    if m: env[m.group(1)] = m.group(2)
out = subprocess.run(["lscpu", "-e=CPU,CORE,MAXMHZ"], capture_output=True, text=True).stdout.splitlines()[1:]
core = {int(c): (int(k), float(m)) for c, k, m in (l.split() for l in out)}

def cpus(key):
    return [int(x) for x in env.get(key, "").split(",") if x.strip().isdigit()]

occ = {}
for i, c in enumerate(cpus("GATEWAY_CORES")): occ.setdefault(core[c][0], []).append(f"gateway{i}@cpu{c}")
for i, c in enumerate(cpus("ENGINE_CORES")):  occ.setdefault(core[c][0], []).append(f"engine{i}@cpu{c}")
for i, c in enumerate(cpus("AUX_CORES")):     occ.setdefault(core[c][0], []).append(f"aux{i}@cpu{c}")

pmax = max(m for _, m in core.values())
print(f"{'core':<6}{'MHz':>7} type  occupants")
for k in sorted(occ):
    mhz = next(m for kk, m in core.values() if kk == k)
    kind = "P" if mhz >= pmax else "E"
    note = "  <-- SMT COLLISION" if len(occ[k]) > 1 else ("  <-- on an E-core" if kind == "E" else "")
    print(f"  {k:<4}{mhz:7.0f} {kind}    {', '.join(occ[k])}{note}")
PY
