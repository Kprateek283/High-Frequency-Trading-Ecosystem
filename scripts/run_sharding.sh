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

ENGINE_LOG="$REPO_ROOT/engine_run.log"

echo "Starting the Exchange Gateway with ${GATEWAY_THREADS}-thread SO_REUSEPORT sharding..."
: > "$ENGINE_LOG"
GATEWAY_THREADS="$GATEWAY_THREADS" "$BIN/exchange" > "$ENGINE_LOG" 2>&1 &
EXCHANGE_PID=$!

# Wait for the engine's own READY line rather than sleeping a fixed 2s. That
# sleep was a race and it lost on any first run: the engine preallocates a
# 1.28GB mmap'd audit log before it binds the port, which takes ~1.7s cold, so
# liquidity connected to a closed port and the benchmark reported zero matches.
# The engine prints READY once sockets are bound and workers are pinned; this is
# the same barrier monitoring/orchestrator.py uses.
printf 'Waiting for engine READY'
READY=0
for _ in $(seq 1 600); do                       # 60s ceiling
    if grep -q '^READY$' "$ENGINE_LOG" 2>/dev/null; then READY=1; break; fi
    if ! kill -0 "$EXCHANGE_PID" 2>/dev/null; then break; fi   # died before READY
    printf '.'
    sleep 0.1
done
printf '\n'

if [ "$READY" -ne 1 ]; then
    echo "Engine never reported READY. Its output:"
    cat "$ENGINE_LOG"
    kill -INT "$EXCHANGE_PID" 2>/dev/null
    exit 1
fi

echo "Driving crossing liquidity (two clients) to generate matches..."
"$BIN/liquidity"

sleep 1
echo "Stopping exchange..."
kill -INT "$EXCHANGE_PID" 2>/dev/null
wait "$EXCHANGE_PID" 2>/dev/null

# The engine's stdout went to a file so we could watch for READY; surface it now
# (shutdown stats, drop counters, gateway attribution) exactly as before.
cat "$ENGINE_LOG"

# The audit log is the cumulative source of truth for what the engine did.
{
    echo "# HFT ecosystem run  $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "Gateway Threads : ${GATEWAY_THREADS}"
    python3 "$REPO_ROOT/scripts/decode_audit.py" "$AUDIT"
    echo "# TODO(measure): throughput/latency matrix not re-measured on this box (plan 3.5)."
} > "$RESULTS"

echo "Benchmark complete. Results written to results.txt:"
cat "$RESULTS"
