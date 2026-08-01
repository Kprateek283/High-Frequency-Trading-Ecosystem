#!/usr/bin/env bash
# Show where each exchange thread actually lands, so config.env can be checked
# instead of trusted. Prints thread -> cpu -> physical core -> P/E.
#
#   ./scripts/verify_core_map.sh
#
# RT_PRIORITY=0 is not optional here. With SCHED_FIFO granted (ulimit -r > 0),
# the four matching-engine shards busy-spin at realtime priority and are never
# preempted by anything lower -- including the shell trying to kill them, and
# including the IRQ threads for wifi, which land on the same P-cores. That wedges
# the machine. `timeout -s INT` is the second guard: even if this script's own
# cleanup never runs, the exchange dies on its own.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

SECS="${SECS:-6}"
BIN=$([ -x build/bin/exchange ] && echo build/bin || echo build)
[ -x "$BIN/exchange" ] || { echo "$BIN/exchange not found -- build first"; exit 1; }

set -a; . ./config.env; set +a
CPUSET="${EXCHANGE_CPUSET:-0-10}"

echo "config.env: GATEWAY_CORES=$GATEWAY_CORES ENGINE_CORES=$ENGINE_CORES AUX_CORES=$AUX_CORES"
echo "taskset:    $CPUSET   RT_PRIORITY=0 (forced)   window=${SECS}s"
echo

# Map cpu -> "core N (P|E)" from the real topology; MAXMHZ is the only reliable
# P/E discriminator, since a scaling governor makes current clocks meaningless.
declare -A WHERE
PMAX=$(lscpu -e=MAXMHZ | tail -n +2 | sort -rn | head -1)
while read -r cpu core mhz; do
    [ "$(printf '%.0f' "$mhz")" -ge "$(printf '%.0f' "$PMAX")" ] && t=P || t=E
    WHERE[$cpu]="core $core ($t $(printf '%.0f' "$mhz") MHz)"
done < <(lscpu -e=CPU,CORE,MAXMHZ | tail -n +2)

RT_PRIORITY=0 setsid timeout -s INT -k 5 "$SECS" \
    taskset -c "$CPUSET" "$BIN/exchange" >/tmp/verify_core_map.log 2>&1 &
trap 'pkill -INT -x exchange 2>/dev/null' EXIT

for _ in $(seq 50); do pid=$(pgrep -x exchange | head -1); [ -n "${pid:-}" ] && break; sleep 0.1; done
[ -n "${pid:-}" ] || { echo "exchange never started; see /tmp/verify_core_map.log"; exit 1; }
sleep 3   # let every thread reach its pin

printf '%-18s %-6s %-8s %s\n' THREAD TID CPUS PLACEMENT
for t in /proc/"$pid"/task/*; do
    tid=$(basename "$t")
    cpus=$(taskset -pc "$tid" 2>/dev/null | sed 's/.*list: //')
    printf '%-18s %-6s %-8s %s\n' "$(cat "$t/comm" 2>/dev/null)" "$tid" "$cpus" "${WHERE[$cpus]:-}"
done | sort

echo
grep -E "^RT_SCHED:" /tmp/verify_core_map.log | head -1
wait 2>/dev/null
