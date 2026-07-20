#!/usr/bin/env python3
"""Run every module self-test (plan gate: green across 5.0-5.3, plus tui/plot).

Plain asserts, no framework — mirrors the C++ side's plain-assert CTest harness.
The orchestrator test launches the real engine if build/bin/exchange exists,
else self-skips. Run: python3 -m monitoring.run_tests
"""
import importlib

MODULES = [
    "monitoring.wire", "monitoring.config", "monitoring.models", "monitoring.clock",
    "monitoring.feeds.stats_reader", "monitoring.feeds.audit_reader",
    "monitoring.feeds.multicast",
    "monitoring.core.orderbook", "monitoring.core.metrics", "monitoring.health",
    "monitoring.orchestrator", "monitoring.tui.app",
]


def main():
    failed = []
    skipped = []
    for name in MODULES:
        # Import inside the guard: tui needs `rich` (the one non-stdlib
        # dependency, see requirements.txt) and an absent optional dependency
        # must not take the other eleven suites down with it.
        try:
            mod = importlib.import_module(name)
        except ImportError as e:
            skipped.append((name, e))
            print(f"{name}: SKIP (missing dependency: {e.name})")
            continue
        try:
            mod._selftest()
        except Exception as e:                       # noqa: BLE001 - report and continue
            failed.append((name, e))
            print(f"{name}: FAIL {e!r}")
    if failed:
        raise SystemExit(f"\n{len(failed)} module(s) failed: {[f[0] for f in failed]}")
    ran = len(MODULES) - len(skipped)
    tail = f" ({len(skipped)} skipped: {[s[0] for s in skipped]})" if skipped else ""
    print(f"\nall {ran} module self-tests passed{tail}")


if __name__ == "__main__":
    main()
