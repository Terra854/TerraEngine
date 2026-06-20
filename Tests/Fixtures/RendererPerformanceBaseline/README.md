# Renderer Performance Baseline

This fixture describes the renderer workloads used to compare threaded renderer
changes against repeatable local baselines. The files are intentionally small:
tests read the manifest shape and synthetic workload names, while manual editor
profiling can use the same names for larger project scenes.

The baseline covers small, medium, large, streaming, UI-heavy, shadow-heavy, and
randomized stress workloads so regressions cannot hide in a single scene shape.
