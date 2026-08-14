# TerraEngine Tools

This directory contains production-facing engine and editor utilities:

- `HeadlessExport` implements the non-interactive export executable.
- `VirtualGeometryAsset` provides guarded stage-cache inspection and repair.
- `Dependencies` refreshes TerraEngine's pinned, self-contained dependencies.

Benchmarks, fixtures, stress and soak runners, evidence collectors, and release
validation harnesses belong under `Tests/Tools/`. Keeping that boundary explicit
prevents test infrastructure from being mistaken for a runtime dependency.
