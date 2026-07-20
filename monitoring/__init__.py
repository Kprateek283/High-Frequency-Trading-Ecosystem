"""Python monitoring for the HFT engine (Phase 5).

Layered exactly like the C++ side: schema (wire) → readers (feeds) → derived
state (core) → orchestration/UI. A module only imports from strictly lower
tiers; runtime data feeds may cross sideways. See docs/dependency.md §3.
"""
