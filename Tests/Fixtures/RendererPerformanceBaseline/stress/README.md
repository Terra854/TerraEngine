# Renderer Stress Fixture

Stress validation intentionally randomizes renderer job completion order,
injects delayed resource readiness, forces fallback paths, and verifies that
cancelled or stale work cannot publish visible frame output.

The engine tests use synthetic snapshots for this fixture so CI can exercise the
threading contracts without needing a GPU-backed editor session.
