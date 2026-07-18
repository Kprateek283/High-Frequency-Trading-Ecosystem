#!/bin/bash
# Documented benchmark entry point. Resolves everything from the repo root so it
# runs the same regardless of the directory you invoke it from, sources the one
# config file, and writes a results file the README actually points at.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# One config source; defaults below fill anything it does not set.
[ -f config.env ] && set -a && . ./config.env && set +a
: "${GATEWAY_THREADS:=4}"

BIN="$REPO_ROOT/build/bin"
RESULTS="$REPO_ROOT/results.txt"
AUDIT="$REPO_ROOT/order_audit.log"

if [ ! -x "$BIN/exchange" ] || [ ! -x "$BIN/liquidity" ]; then
    echo "Build first:  mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j\$(nproc)"
    exit 1
fi

echo "Starting the Exchange Gateway with ${GATEWAY_THREADS}-thread SO_REUSEPORT sharding..."
GATEWAY_THREADS="$GATEWAY_THREADS" "$BIN/exchange" &
EXCHANGE_PID=$!
sleep 2

echo "Driving crossing liquidity (two clients) to generate matches..."
"$BIN/liquidity"

sleep 1
echo "Stopping exchange..."
kill -INT "$EXCHANGE_PID" 2>/dev/null
wait "$EXCHANGE_PID" 2>/dev/null

# The audit log is the cumulative source of truth for what the engine did.
{
    echo "# HFT ecosystem run  $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "Gateway Threads : ${GATEWAY_THREADS}"
    python3 "$REPO_ROOT/scripts/decode_audit.py" "$AUDIT"
    echo "# TODO(measure): throughput/latency matrix not re-measured on this box (plan 3.5)."
} > "$RESULTS"

echo "Benchmark complete. Results written to results.txt:"
cat "$RESULTS"
