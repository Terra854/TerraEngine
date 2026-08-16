# Effectively Unbounded Scenes

TerraEngine scenes use an exact cell plus a bounded local offset as their only absolute-position authority. A `WorldPosition` contains three signed 64-bit cell coordinates and three double-precision metre offsets normalized to `[-32768, 32768)`. This represents practical world extents far beyond `±10^15 m` while preserving millimetre-scale detail near each active reference frame.

Rendering, physics, audio, queries, streaming, and editor tools never turn that address into one global floating-point vector. Each view, physics world, listener, and region operation selects an exact canonical origin and derives bounded local floats for its own workload. Async results include origin, spatial, residency, and provider generations so stale work fails closed.

Scene writes are typed transactions. `SetWorldTransform`, `TranslateWorld`, reparenting, residency publication, and physics-frame migration prepare from immutable data and publish only at declared protected scheduler boundaries. Rejected groups restore all prior state.

This feature is intentionally breaking. There is no `Vector3 Position`, global float transform, legacy scene reader, converter, dual writer, or compatibility alias. Existing projects must be recreated in the new formats.

For build, focused test, installed validation, performance, and six-hour soak commands, see [Testing](../Testing.md) and `specs/052-unbounded-scenes/quickstart.md`.
