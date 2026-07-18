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
    for name in MODULES:
        mod = importlib.import_module(name)
        try:
            mod._selftest()
        except Exception as e:                       # noqa: BLE001 - report and continue
            failed.append((name, e))
            print(f"{name}: FAIL {e!r}")
    if failed:
        raise SystemExit(f"\n{len(failed)} module(s) failed: {[f[0] for f in failed]}")
    print(f"\nall {len(MODULES)} module self-tests passed")


if __name__ == "__main__":
    main()
