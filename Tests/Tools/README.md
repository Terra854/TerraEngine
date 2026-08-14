# TerraEngine Test Tools

This directory owns executable and scripted test infrastructure that exercises
the production engine and editor without becoming part of either runtime.

It includes fixture generators, stress and soak runners, performance benchmarks,
evidence schemas, contract probes, release gates, and installed-tree validation
harnesses. CMake exposes compiled utilities only when `BUILD_TESTING` is enabled.

Genuine engine/editor utilities belong under `Tools/` instead.
