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

# Realtime is opt-in here. Left at its default (80), the engine's four matching
# shards busy-spin at SCHED_FIFO and are never preempted by anything lower --
# including this script's own `kill -INT`, and including the IRQ threads for the
# machine's network card. On a box where `ulimit -r` is non-zero that wedges the
# machine hard enough to need a power cycle. RT_PRIORITY=0 is also what the
# published benchmarks use (docs/scheduling.md), so it is the honest default for
# the documented entry point. Export RT_PRIORITY explicitly to override.
: "${RT_PRIORITY:=0}"
export RT_PRIORITY

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
# LATENCY_PROFILE=1 turns on the gateway's per-order e2e/TCP latency reservoir so
# the shutdown report includes P50/P99/P99.9, not just averages. Kept off for the
# throughput sweep (measure_throughput.py) to avoid charging it for the reservoir.
GATEWAY_THREADS="$GATEWAY_THREADS" LATENCY_PROFILE=1 "$BIN/exchange" > "$ENGINE_LOG" 2>&1 &
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
    echo ""
    echo "# ---- Cycle attribution + latency percentiles (this run) ----"
    # Capture from the first real-time [Metrics] window to end of log. That window
    # carries the engine's matching-latency percentiles (OrderManager prints then
    # clear()s each 1s window, so they never survive to the shutdown block), and
    # the tail carries the gateway cycle attribution, EXPERIMENT-4 stage averages,
    # calibrated TSC, and the e2e/TCP P50/P99/P99.9 (LATENCY_PROFILE=1). Harvested
    # verbatim so results.txt is self-contained. (This is the short deterministic
    # liquidity run, so it's a window or two — not pages of metrics.)
    awk '/\[Metrics\]|Shutdown signal received/{f=1} f' "$ENGINE_LOG"
    echo ""
    echo "# ---- How to read the numbers above ----"
    echo "#"
    echo "# This is a FUNCTIONAL run: a deterministic crossing driver (tools/liquidity)"
    echo "# proving the pipeline matches orders end to end. The counts (NEW, matches,"
    echo "# FILLED, REJECTED) are the point. The timings are not a latency benchmark,"
    echo "# for three specific reasons:"
    echo "#"
    echo "# 1. UDP Path / Trading Engine / SPSC Queue read 0 BY CONSTRUCTION."
    echo "#    liquidity.cpp stamps t1=t2=t3=t4 with a single send-time TSC because it"
    echo "#    has no firm-side pipeline to measure. Only t5-t4 (TCP path) is real here."
    echo "#    For a populated five-point decomposition, drive the exchange with the"
    echo "#    trading firm (LocalExchangeConnector), which stamps each stage separately."
    echo "#"
    echo "# 2. TCP path and end-to-end are BURST QUEUEING DELAY, not per-order latency."
    echo "#    liquidity stamps every order at send time and blasts 20k of them"
    echo "#    unthrottled, so the gateway drains a backlog it never had a chance to"
    echo "#    keep up with. t5-t4 therefore grows through the burst and its average is"
    echo "#    roughly half the total drain. It measures the queue, which is real, but"
    echo "#    it is not the number to quote for network latency."
    echo "#"
    echo "# 3. The gateway cycle attribution here is NOT comparable to"
    echo "#    cycle_attribution.txt. epoll_wait in particular is inflated: this run is"
    echo "#    ~95% idle and amortises blocking time over only 20k orders, where the"
    echo "#    sustained sweep amortises it over millions. Use cycle_attribution.txt"
    echo "#    for per-stage cost; use this file for correctness."
    echo "#"
    echo "# Gateway ingest throughput sweep (workers x clients): see"
    echo "#   benchmark_results.txt (regenerate with scripts/measure_throughput.py)."
    echo "# For the latency matrix across shard counts, re-run this script with"
    echo "#   GATEWAY_THREADS=1 / 2 / 4 / 8."
    echo "#"
    echo "# NOTE: absolute latency/throughput here are a LOWER BOUND unless the box"
    echo "#   grants SCHED_FIFO (ulimit -r) and uses the 'performance' governor with"
    echo "#   isolated cores. The measurement code is complete; the numbers become"
    echo "#   publishable only on such hardware (docs/benchmarks.md, Phase 3.5)."
    echo "#"
    echo "# RT_SCHED threads=N above is the readiness barrier's own denominator. If it"
    echo "#   is short of $(( GATEWAY_THREADS + 4 + 3 )) for this run, READY fired early and the numbers"
    echo "#   below it describe a partially-started engine."
} > "$RESULTS"

echo "Benchmark complete. Results written to results.txt:"
cat "$RESULTS"
