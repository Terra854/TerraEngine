# Virtual Geometry Fixtures

This directory documents fixtures used by the TerraEngine virtual geometry test
surface. Keep committed fixtures small enough for normal checkout and build
flows. Larger stress assets should be generated from scripts or described with
clear source attribution instead of committed as heavy binary files.

## Fixture Categories

- High-detail static mesh: at least one source mesh large enough to generate
  multiple useful LOD levels.
- Small static mesh: a mesh where simplification should be skipped cleanly.
- Multi-material mesh: separate material sections that must remain stable across
  generated detail levels.
- Border continuity mesh: adjacent regions that reveal cracks or holes if LOD
  generation breaks shared edges.
- Hi-Z validation scene: thin geometry, moving occluders, camera cuts, target
  resizes, and multi-view mismatch cases.
- Streaming pressure scene: enough virtual geometry pages to exceed a small test
  budget while preserving visible fallback detail.

## Visual Correctness Stress Notes

- Border fixtures should include open edges and thin silhouettes so future LOD
  tests can prove simplification does not introduce visible holes.
- Material-slot fixtures should include adjacent primitives with different
  slots; current MVP tests use one default section, but future package import
  work should verify every slot keeps a continuous per-LOD section range.
- Silhouette fixtures should include high-curvature profiles where geometric
  error and `maxLodDrop` settings can produce obvious popping.
- Hi-Z fixtures should include thin occluders and self-occluding meshes once
  occlusion is enabled for virtual geometry.

## Attribution Rules

Every committed fixture must include source, license, generator command, and any
post-processing steps. Synthetic fixtures should describe the deterministic
generation recipe so contributors can rebuild them from a clean checkout.

## Size Rules

Prefer procedural or compact fixtures for CTest. Manual profiling scenes may be
larger, but they should live outside the critical test path unless explicitly
approved for the repository.

## Synthetic Large-Mesh Recipe

The focused CTest suite uses generated fixtures instead of committed heavy
meshes. Recreate the compiler stress shape with this deterministic recipe:

- Build a regular grid with `quadsPerSide=36`; emit `(quadsPerSide + 1)^2`
  vertices and `quadsPerSide^2 * 2` triangles.
- Position `x` and `y` in `[-1, 1]`, set `z = 0.05 * sin(x * 12) * cos(y * 10)`,
  and use normal `(0, 0, 1)`.
- For multi-material-style stress without requiring material imports, divide
  vertices into four deterministic bands, add `band * 0.0025` to `z`, and nudge
  `uv.x` by the same offset. This keeps the fixture procedural while still
  exposing section/bounds drift if the compiler changes.
- Compile with virtual geometry enabled and meshlet bounds enabled. Expected
  invariants: status `Cooked`, at least three detail levels, one MVP page per
  detail level, non-zero cullable groups, and a resident fallback detail.

## Stress Counter Thresholds

The headless dense-scene stress test creates 96 synthetic runtime assets with
three whole-mesh LODs each and 48 scripted camera-motion frames. It is not a
GPU benchmark; it verifies deterministic counter relationships:

- Fully resident pages must produce zero fail-open selections during the stress
  camera path.
- Selected meshlet work must remain at or below `0.55 + 10%` of LOD0 meshlet
  work.
- Every asset must expose two LOD transition candidates.
- A deliberately tiny `geometryBudgetBytes` value must produce positive budget
  pressure and sorted budget contributors.
- Residency pressure tests may evict high-detail pages, but page `0` fallback
  records must never appear in eviction candidates.
