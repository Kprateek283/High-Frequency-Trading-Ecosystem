#!/bin/bash
# End-to-end multi-firm run: two GENUINELY different firms competing on one
# exchange, with the private-ack loop driving each firm's own position/PnL, and
# the per-firm /dev/shm/firm_stats_<ID> regions a Python monitor can watch.
#
#   ./scripts/multi_firm_run.sh
#   RUN_SECONDS=8 TAKER_THRESHOLD=0.1 ./scripts/multi_firm_run.sh
#   BIN=<dir with exchange/liquidity/market_maker> FIRM_BIN=<dir with trading_firm> ./scripts/multi_firm_run.sh
#
# Participants (distinct identities + non-overlapping token slices, so the
# exchange's SessionManager never sees them collide):
#   Firm A : FIRM_ID=A  TOKEN_BASE=0        STRATEGY=maker  -> tokens [0, 3.125M)
#   Firm B : FIRM_ID=B  TOKEN_BASE=3125000  STRATEGY=taker  -> tokens [3.125M, 6.25M)
#
# Seeding (multi-firm-plan "seeding gotcha"): a firm maker needs a STANDING
# two-sided book to form a valid signal, and the taker needs quotes to lift. The
# `liquidity` tool only crosses at one price (leaves an empty book), so we anchor
# the book with the `market_maker` engine tool (posts BUY 50000 / SELL 50020 and
# maintains it) and periodically inject `liquidity` bursts for trade volume.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
[ -f config.env ] && set -a && . ./config.env && set +a

: "${RUN_SECONDS:=6}"            # how long to drive liquidity bursts
: "${TAKER_THRESHOLD:=0.1}"      # low enough that the taker actually fires
: "${TAKER_SIZE:=20}"
: "${TOKEN_SLICE:=3125000}"      # MAX_CLIENT_ORDERS(50M) / MAX_FIRMS(16)

# Engine binaries (exchange + injectors).
if [ -z "${BIN:-}" ]; then
    for d in "$REPO_ROOT/build-eng" "$REPO_ROOT/build/bin" "$REPO_ROOT/build" "$REPO_ROOT/hft_engine/build"; do
        [ -x "$d/exchange" ] && BIN="$d" && break
    done
fi
# Firm binary (built separately, WITH_CRYPTO=OFF).
if [ -z "${FIRM_BIN:-}" ]; then
    for d in "$REPO_ROOT/build-firm" "$REPO_ROOT/hft-trading-firm/build"; do
        [ -x "$d/trading_firm" ] && FIRM_BIN="$d" && break
    done
fi
if [ -z "${BIN:-}" ] || [ ! -x "$BIN/exchange" ] || [ ! -x "$BIN/market_maker" ] || [ ! -x "$BIN/liquidity" ]; then
    echo "Missing engine binaries. Build them:"
    echo "  cmake -S hft_engine -B build-eng -DCMAKE_BUILD_TYPE=Release && cmake --build build-eng -j\$(nproc)"
    exit 1
fi
if [ -z "${FIRM_BIN:-}" ] || [ ! -x "$FIRM_BIN/trading_firm" ]; then
    echo "Missing firm binary. Build it:"
    echo "  cmake -S hft-trading-firm -B build-firm -DCMAKE_BUILD_TYPE=Release && cmake --build build-firm --target trading_firm -j\$(nproc)"
    exit 1
fi
echo "Engine bin: $BIN   Firm bin: $FIRM_BIN"

WORK="$(mktemp -d)"
AUDIT="$REPO_ROOT/order_audit.log"     # default path so the monitor/decode find it
ELOG="$WORK/engine.log"
rm -f /dev/shm/firm_stats_A /dev/shm/firm_stats_B    # start each firm's region clean

cleanup() {
    kill "${FA:-}" "${FB:-}" "${MM:-}" 2>/dev/null
    kill -INT "${EX:-}" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT INT TERM

echo "Starting exchange (default STATS_SHM_PATH=/dev/shm/hft_stats)..."
AUDIT_LOG_PATH="$AUDIT" PID_FILE="$WORK/exchange.pid" "$BIN/exchange" > "$ELOG" 2>&1 &
EX=$!
printf 'Waiting for engine READY'
for _ in $(seq 1 600); do
    grep -q '^READY$' "$ELOG" 2>/dev/null && { printf ' ok\n'; break; }
    kill -0 "$EX" 2>/dev/null || { printf '\nengine died:\n'; cat "$ELOG"; exit 1; }
    printf '.'; sleep 0.1
done

echo "Seeding a standing two-sided book (market_maker tool: BUY 50000 / SELL 50020)..."
"$BIN/market_maker" > "$WORK/mm.log" 2>&1 &
MM=$!
sleep 0.5

echo "Firm A: FIRM_ID=A TOKEN_BASE=0 STRATEGY=maker"
FIRM_ID=A TOKEN_BASE=0 STRATEGY=maker \
    "$FIRM_BIN/trading_firm" local > "$WORK/firmA.log" 2>&1 &
FA=$!
echo "Firm B: FIRM_ID=B TOKEN_BASE=$TOKEN_SLICE STRATEGY=taker (thr=$TAKER_THRESHOLD size=$TAKER_SIZE)"
FIRM_ID=B TOKEN_BASE="$TOKEN_SLICE" STRATEGY=taker \
    TAKER_THRESHOLD="$TAKER_THRESHOLD" TAKER_SIZE="$TAKER_SIZE" \
    "$FIRM_BIN/trading_firm" local > "$WORK/firmB.log" 2>&1 &
FB=$!
sleep 1

echo "Driving flow for ${RUN_SECONDS}s (periodic liquidity bursts)..."
end=$(( $(date +%s) + RUN_SECONDS ))
while [ "$(date +%s)" -lt "$end" ]; do
    "$BIN/liquidity" > /dev/null 2>&1
    sleep 0.3
done
sleep 1   # let the last acks land + a final ~100ms stats publish

echo
echo "================ EVIDENCE ================"
FIRM_ID_A=A FIRM_ID_B=B AUDIT="$AUDIT" TOKEN_SLICE="$TOKEN_SLICE" \
    python3 "$REPO_ROOT/scripts/multi_firm_evidence.py"

echo
echo "engine log: $ELOG   firm logs: $WORK/firmA.log $WORK/firmB.log"
echo "regions:    /dev/shm/firm_stats_A  /dev/shm/firm_stats_B  (persist for the TUI)"
# cleanup() runs on EXIT
