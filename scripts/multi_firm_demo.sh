#!/bin/bash
# Multi-firm demo: run the exchange with several competing order sources and
# watch the matching engine handle NEW / FILL / PARTIAL / REJECT. Same
# READY-wait + audit-decode flow as run_sharding.sh, just with more participants.
#
#   ./scripts/multi_firm_demo.sh
#   BIN=/path/to/binaries ./scripts/multi_firm_demo.sh   # if not in build/bin
#   LIQUIDITY_ORDERS=5000 WITH_TESTER=0 ./scripts/multi_firm_demo.sh
#
# Participants:
#   liquidity : two crossing clients on STK00000            -> FILL / PARTIAL
#   tester    : 1M-order multi-symbol flood (WORKLOAD_TYPE) -> REJECT + some NEW
#   (market_maker is multicast-fed; add it yourself once multicast works locally)
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
[ -f config.env ] && set -a && . ./config.env && set +a
: "${GATEWAY_THREADS:=4}"
: "${LIQUIDITY_ORDERS:=1000}"   # crossing pairs on STK00000 -> guaranteed fills
: "${WITH_TESTER:=1}"           # 1 = also run the reject-heavy flood

# Find the binaries: honour $BIN, else try the usual build locations (top-level
# build/bin, or a standalone `cmake -S hft_engine -B build` that drops them in build/).
if [ -z "${BIN:-}" ]; then
    for d in "$REPO_ROOT/build/bin" "$REPO_ROOT/build" "$REPO_ROOT/hft_engine/build"; do
        [ -x "$d/exchange" ] && BIN="$d" && break
    done
fi
if [ -z "${BIN:-}" ] || [ ! -x "$BIN/exchange" ] || [ ! -x "$BIN/liquidity" ]; then
    echo "Could not find exchange/liquidity binaries. Build hft_engine (no OpenSSL needed):"
    echo "  cmake -S hft_engine -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j\$(nproc)"
    echo "then re-run, or pass BIN=<dir with the binaries> ./scripts/multi_firm_demo.sh"
    exit 1
fi
echo "Using binaries in: $BIN"

WORK="$(mktemp -d)"
AUDIT="$WORK/order_audit.log"
ELOG="$WORK/engine.log"
echo "Work dir: $WORK"

echo "Starting exchange (${GATEWAY_THREADS} gateway workers, SO_REUSEPORT)..."
AUDIT_LOG_PATH="$AUDIT" STATS_SHM_PATH="$WORK/hft_stats" PID_FILE="$WORK/exchange.pid" \
    GATEWAY_THREADS="$GATEWAY_THREADS" "$BIN/exchange" > "$ELOG" 2>&1 &
EX=$!

printf 'Waiting for engine READY'
for _ in $(seq 1 600); do
    grep -q '^READY$' "$ELOG" 2>/dev/null && { printf ' ok\n'; break; }
    kill -0 "$EX" 2>/dev/null || { printf '\nengine died:\n'; cat "$ELOG"; exit 1; }
    printf '.'; sleep 0.1
done

echo "Firm A+B: liquidity  (two crossing clients on STK00000, ${LIQUIDITY_ORDERS} pairs)"
LIQUIDITY_ORDERS="$LIQUIDITY_ORDERS" "$BIN/liquidity" >/dev/null 2>&1 &
PIDS=$!
if [ "$WITH_TESTER" = "1" ] && [ -x "$BIN/tester" ]; then
    echo "Firm C:   tester     (multi-symbol flood -> rejects + some accepts)"
    "$BIN/tester" >/dev/null 2>&1 &
    PIDS="$PIDS $!"
fi

wait $PIDS
sleep 1
echo "Stopping exchange..."
kill -INT "$EX" 2>/dev/null
wait "$EX" 2>/dev/null

echo
echo "======== engine [Metrics] (live 1s windows: Orders/Trades/Reject%) ========"
grep -A4 '\[Metrics\]' "$ELOG" | tail -25
echo
echo "======== audit decode: per-state totals (NEW/FILL/PARTIAL/REJECT) ========"
python3 "$REPO_ROOT/scripts/decode_audit.py" "$AUDIT"
echo
echo "engine log: $ELOG"
echo "audit log:  $AUDIT   (re-decode: python3 scripts/decode_audit.py '$AUDIT')"
