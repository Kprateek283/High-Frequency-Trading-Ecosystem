#!/bin/bash
# End-to-end multi-firm run: N GENUINELY different firms competing on one
# exchange, with the private-ack loop driving each firm's own position/PnL, and
# the per-firm /dev/shm/firm_stats_<ID> regions a Python monitor can watch.
#
#   ./scripts/multi_firm_run.sh                         # 2 firms (A maker, B taker)
#   NUM_FIRMS=4 ./scripts/multi_firm_run.sh             # A,B,C,D (maker/taker alternating)
#   RUN_SECONDS=120 NUM_FIRMS=6 ./scripts/multi_firm_run.sh   # keep alive for the TUI
#   BIN=<dir with exchange/liquidity/market_maker> FIRM_BIN=<dir with trading_firm> ./scripts/multi_firm_run.sh
#
# Firms (distinct identities + non-overlapping token slices, so the exchange's
# SessionManager never sees them collide): firm k (k=0..NUM_FIRMS-1) gets
#   FIRM_ID = A,B,C,...   TOKEN_BASE = k*TOKEN_SLICE   STRATEGY = maker/taker (alternating)
# up to MAX_FIRMS=16 (TOKEN_SLICE = MAX_CLIENT_ORDERS(50M)/MAX_FIRMS(16)).
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

: "${NUM_FIRMS:=2}"             # how many firms to launch (1..16)
: "${RUN_SECONDS:=6}"           # how long to drive liquidity bursts
: "${TAKER_THRESHOLD:=0.1}"     # low enough that the taker actually fires
: "${TAKER_SIZE:=20}"
: "${TOKEN_SLICE:=3125000}"     # MAX_CLIENT_ORDERS(50M) / MAX_FIRMS(16)
MAX_FIRMS=16

if [ "$NUM_FIRMS" -lt 1 ] || [ "$NUM_FIRMS" -gt "$MAX_FIRMS" ]; then
    echo "NUM_FIRMS must be 1..$MAX_FIRMS (got $NUM_FIRMS)"; exit 1
fi

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
echo "Engine bin: $BIN   Firm bin: $FIRM_BIN   Firms: $NUM_FIRMS"

# Firm k -> id letter A,B,C,... (k=0..15). Single letters, 16 max.
firm_id() { printf "\\$(printf '%03o' $((65 + $1)))"; }

WORK="$(mktemp -d)"
AUDIT="$REPO_ROOT/order_audit.log"     # default path so the monitor/decode find it
ELOG="$WORK/engine.log"

FIRM_PIDS=()
FIRM_IDS=()
cleanup() {
    [ "${#FIRM_PIDS[@]}" -gt 0 ] && kill "${FIRM_PIDS[@]}" 2>/dev/null
    kill "${MM:-}" 2>/dev/null
    kill -INT "${EX:-}" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT INT TERM

# Start each firm's region clean.
for k in $(seq 0 $((NUM_FIRMS - 1))); do rm -f "/dev/shm/firm_stats_$(firm_id "$k")"; done

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

# Launch firm k: even k = maker, odd k = taker (so >=2 firms always includes a
# maker+taker pair that can cross). TOKEN_BASE = k*TOKEN_SLICE keeps slices disjoint.
for k in $(seq 0 $((NUM_FIRMS - 1))); do
    id="$(firm_id "$k")"
    base=$((k * TOKEN_SLICE))
    if [ $((k % 2)) -eq 0 ]; then strat=maker; else strat=taker; fi
    FIRM_IDS+=("$id")
    echo "Firm $id: TOKEN_BASE=$base STRATEGY=$strat"
    FIRM_ID="$id" TOKEN_BASE="$base" STRATEGY="$strat" \
        TAKER_THRESHOLD="$TAKER_THRESHOLD" TAKER_SIZE="$TAKER_SIZE" \
        "$FIRM_BIN/trading_firm" local > "$WORK/firm_$id.log" 2>&1 &
    FIRM_PIDS+=($!)
done
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
FIRM_IDS="$(IFS=,; echo "${FIRM_IDS[*]}")" AUDIT="$AUDIT" TOKEN_SLICE="$TOKEN_SLICE" \
    python3 "$REPO_ROOT/scripts/multi_firm_evidence.py"

echo
echo "engine log: $ELOG   firm logs: $WORK/firm_*.log"
printf 'regions:   '
for id in "${FIRM_IDS[@]}"; do printf ' /dev/shm/firm_stats_%s' "$id"; done
echo
echo "(watch them live: python3 -m monitoring.tui.app)"
# cleanup() runs on EXIT
