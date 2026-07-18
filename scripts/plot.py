#!/usr/bin/env python3
"""Plot the Phase-3.5 run results — reproducible from data, not hardcoded arrays.

Reads results.txt (the file run_sharding.sh writes) and plots the order-outcome
split it records. The old version embedded per-thread latency/cycle arrays that
no run produced (review B/C: "one more copy of unverifiable data"); those numbers
are TODO(measure) on this box (plan 3.5), so this script plots only what the
results file actually contains and says so about the rest.

load_results() is pure stdlib and self-tested; matplotlib is imported lazily,
only when actually rendering, so the parse is verifiable without the plot dep.
"""
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(REPO_ROOT, "results.txt")

# The outcome keys run_sharding.sh emits (decode_audit.py counts).
OUTCOME_KEYS = ["NEW", "FILLED", "PARTIAL_FILL", "CANCELED", "REJECTED"]


def load_results(path=RESULTS):
    """Parse `KEY : value` lines into {key: int|str}. Ignores comments/blanks."""
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or ":" not in line:
                continue
            key, val = line.split(":", 1)
            val = val.strip()
            out[key.strip()] = int(val) if val.isdigit() else val
    return out


def plot_outcomes(results, out_dir):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    labels = [k for k in OUTCOME_KEYS if k in results]
    values = [results[k] for k in labels]
    colors = {"NEW": "#3498db", "FILLED": "#2ecc71", "PARTIAL_FILL": "#f1c40f",
              "CANCELED": "#95a5a6", "REJECTED": "#e74c3c"}

    fig, ax = plt.subplots(figsize=(8, 6))
    bars = ax.bar(labels, values, color=[colors[l] for l in labels])
    threads = results.get("Gateway Threads", "?")
    ax.set_title(f"Order outcomes — {threads}-thread gateway run", fontsize=14, pad=20)
    ax.set_ylabel("Count", fontsize=12)
    for b in bars:
        ax.text(b.get_x() + b.get_width() / 2, b.get_height(),
                f"{int(b.get_height()):,}", ha="center", va="bottom", fontsize=10, fontweight="bold")
    ax.grid(axis="y", linestyle="--", alpha=0.7)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    fig.tight_layout()

    os.makedirs(out_dir, exist_ok=True)
    dest = os.path.join(out_dir, "order_outcomes.png")
    fig.savefig(dest, dpi=300, bbox_inches="tight")
    print(f"Generated {dest}")


def _selftest():
    import tempfile
    sample = ("# run\nGateway Threads : 4\nentries : 30000\nNEW : 10000\n"
              "matches : 20000\nFILLED : 20000\nPARTIAL_FILL : 0\n"
              "# TODO(measure): matrix\n")
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write(sample)
        name = f.name
    r = load_results(name)
    assert r["Gateway Threads"] == 4 and r["NEW"] == 10000 and r["FILLED"] == 20000
    assert "TODO" not in r and r["matches"] == 20000
    os.unlink(name)
    print("plot.load_results: OK")


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        _selftest()
        sys.exit(0)
    if not os.path.exists(RESULTS):
        sys.exit(f"no results file at {RESULTS} — run scripts/run_sharding.sh first")
    results = load_results()
    plot_outcomes(results, os.path.join(REPO_ROOT, "docs", "images"))
    # The throughput/latency matrix is not in results.txt (plan 3.5 TODO(measure)
    # on this box); nothing to plot for it, and we won't fabricate it.
    print("Note: latency/throughput matrix is TODO(measure); no chart generated for it.")
