# Virtual Geometry Fixtures

This directory documents fixtures used by the TerraEngine virtual geometry test
surface. Keep committed fixtures small enough for normal checkout and build
flows. Larger stress assets should be generated from scripts or described with
clear source attribution instead of committed as heavy binary files.

## Fixture Categories

- High-detail static mesh: at least one source mesh large enough to generate
  multiple useful LOD levels.
- Small static mesh: a mesh where simplification should be skipped cleanly.
- Adaptive medium mesh: a mesh that produces only part of the default requested
  half-octave target chain so tests can prove ratios are advisory.
- Per-mesh override mesh: a generated package mesh with stable identity for
  Inspector override, rebuild, reset, and reimport tests.
- Multi-material mesh: separate material sections that must remain stable across
  generated detail levels.
- Border continuity mesh: adjacent regions that reveal cracks or holes if LOD
  generation breaks shared edges.
- Hi-Z validation scene: thin geometry, moving occluders, camera cuts, target
  resizes, and multi-view mismatch cases.
- Streaming pressure scene: enough virtual geometry pages to exceed a small test
  budget while preserving visible fallback detail.
- Page-sourced resident scene: selected clusters resolve to resident
  page-local payload ranges so tests can prove rendering no longer requires the
  fully resident owning meshlet stream.
- Page fallback-only scene: high-detail pages are pending, failed, or evicted
  while pinned fallback pages stay renderable without holes.
- Page budget churn scene: camera motion requests, uploads, evicts, and
  re-requests detail pages under a deliberately small geometry budget.
- Page integrity scene: malformed page ranges, fallback links, material spans,
  bounds, and byte costs are rejected before they become live render sources.
- Mixed-distance cluster scene: one large object spanning near and far view
  regions so hierarchical cluster selection can refine only nearby regions.
- Cluster batching scene: dense selected clusters that must collapse into
  bounded virtual geometry batch work instead of one draw per selected cluster.
- GPU packet buffer scene: dense selected clusters represented as per-view
  GPU-visible packet and batch buffers while preserving the CPU-selected
  cluster cut.
- Runtime compaction scene: visible, frustum-rejected, Hi-Z-rejected,
  residency-substituted, and fail-open cluster packets in one deterministic
  camera path.
- Generated submission scene: compacted packet output that drives submitted
  mesh-shader work only after frame, view, layout, capacity, and counter
  validation succeeds.
- Shadow-view scene: one camera-hidden caster that is visible to a light plus
  one light-frustum-rejected caster, so shadow selection proves it does not
  reuse camera visibility.
- Cone-safety scene: one-sided opaque, double-sided, mixed-winding, thin-shell,
  and alpha-risk clusters used to validate conservative cone eligibility.
- Hardening manifest: `hardening-fixtures.json` lists the clean-checkout
  categories required by feature `029`, expected diagnostics for each category,
  importer/export coverage tags, package formats, and which larger assets are
  optional manual stress coverage.

## Coverage Tags

Generated package meshes can publish `virtualGeometry.coverageTags` in their
`.import.json` sidecar. These tags are diagnostics for tests and tools; they do
not enable rendering behavior. Current tags include:

- `package-gltf`, `package-glb`, and `package-fbx` for package-format coverage.
- `package-static-geometry` and `package-imported` for generated static meshes.
- `adaptive-targets`, `cluster-hierarchy`, `fallback-coverage`, and
  `page-links` for required virtual geometry metadata.
- `cone-safety-metadata`, `cone-culling-eligible`, and
  `cone-culling-disabled` for the required cone-safety table and its current
  conservative eligibility state.
- `tiny-mesh-skip`, `dense-cluster-selection`, `multi-material-package`, and
  `material-coverage-review` for broad asset categories.
- `streaming-pressure`, `pinned-fallback`, and `budget-pressure` for residency
  and eviction stress coverage.
- `page-sourced-rendering`, `resident-page-payload`,
  `no-owning-mesh-normal-path`, `fallback-page-pinned`,
  `missing-detail-substitution`, `page-budget`, `page-rerequest`, and
  `page-integrity` for feature `033` page-streamed geometry coverage.
- `cluster-packet-batching`, `runtime-compaction`, `bounded-batches`, and
  `performance-gate` for feature `030` scaling coverage.
- `gpu-packet-buffer`, `gpu-compaction`, `generated-submission`,
  `counter-readback`, and `gpu-fail-open` for feature `031` GPU packet
  compaction coverage.
- `shadow-view-selection`, `light-frustum-culling`, `cone-safety`, and
  `hiz-baseline-preservation` for feature `030` culling coverage.

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
- Adaptive LOD fixtures should include a tiny mesh, a medium mesh, and a large
  mesh so tests can validate skipped targets, partial chains, and full chains.
- Streaming fixtures should keep page `0` or an equivalent fallback page small
  and resident while high-detail pages exceed the configured test budget.
- Mixed-detail fixtures should expose parent/child cluster coverage mistakes by
  making overlapping or missing regions obvious in false-color debug modes.

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

## Feature 029 Hardening Quickstart

Use `specs/029-virtual-geometry-hardening/quickstart.md` for the staged
hardening workflow. The clean-checkout pass should use the manifest in this
directory and the focused CTest slice:

```powershell
ctest --test-dir out/build/Debug -R "Engine.VirtualGeometryHardening|Editor.VirtualGeometryHardening" --output-on-failure -C Debug
```

Manual stress assets may be downloaded or generated locally, but they are not
required for the default fixture pass. Keep those optional assets outside the
critical clean-checkout path unless a later feature explicitly approves them.

## Feature 030 Scaling Quickstart

Use `specs/030-vg-gpu-driven-batching/quickstart.md` for the staged scaling
workflow. The clean-checkout pass should use this manifest and the focused CTest
slice:

```powershell
ctest --test-dir out/build/Debug -R "Engine.VirtualGeometryBatching|Engine.VirtualGeometryCompaction|Engine.VirtualGeometryPerformanceGate|Engine.VirtualGeometryShadow|Engine.VirtualGeometryCulling|Engine.VirtualGeometryRenderer|Engine.VirtualGeometryResource|Editor.VirtualGeometryAssetPipeline|Editor.VirtualGeometryAutomation|Editor.VirtualGeometryExport|Editor.VirtualGeometryDiagnostics" --output-on-failure -C Debug
```

The dense batching fixture must select at least 500 clusters while submitted
virtual geometry batches remain no more than 10 percent of selected clusters.
The compaction fixture must expose candidate, compacted, rejected, fail-open,
and submitted packet counts. The Hi-Z fixture protects the current working
behavior; it must not introduce or require new quality tiers.

## Feature 031 GPU Packet Compaction Quickstart

Use `specs/031-vg-gpu-compaction/quickstart.md` for the staged GPU packet
workflow. The clean-checkout pass should use this manifest and the focused CTest
slice:

```powershell
ctest --test-dir out/build/Debug -R "Engine.VirtualGeometryBatching|Engine.VirtualGeometryCompaction|Engine.VirtualGeometryPerformanceGate|Engine.VirtualGeometryHiZ|Engine.VirtualGeometryMultiView|Engine.VirtualGeometryRenderer|Engine.Renderer|Engine.SystemScheduler" --output-on-failure -C Debug
```

The dense GPU packet fixture must select at least 500 virtual geometry packets
while submitted batches remain no more than 10 percent of selected packets. The
GPU compaction fixture must expose candidate, compacted, rejected, fail-open,
generated submission, and fallback counters. Performance gates must report the
fixture id, view id, threshold, actual value, source, status, and diagnostic
text. Counter rows must label `Prepared`, `GPU Readback`,
`Delayed GPU Readback`, `Unsupported`, or `Unavailable`; placeholder zeroes are
not valid readback data. The Hi-Z fixture remains preservation-only and must not
add or require quality tiers.

Feature `031` gate thresholds are encoded in `hardening-fixtures.json` and
`Tests/Fixtures/RendererPerformanceBaseline/workloads.json`. Dense packet
fixtures use `maxGpuBatchDescriptorRatio <= 0.1`; generated submission fixtures
use `maxInvalidGeneratedSubmissions == 0` and
`maxCompactionOverflowPackets == 0`. Unsupported or unavailable counter sources
must publish unsupported gates instead of passing as zero-valued metrics.

## Feature 034 True Page Residency Quickstart

Use `specs/034-vg-true-page-residency/quickstart.md` for exact-memory and
true-page-payload validation. The clean-checkout pass should include package
children, unavailable accounting, resident page payloads, protected fallback
payloads, and explicit owning-mesh fallback diagnostics.

```powershell
ctest --test-dir out/build/Debug -C Debug -R "ResourceResidency|ResourceStreaming|ResourceManagerMesh|ResourceResidencyWindow" --output-on-failure
ctest --test-dir out/build/Debug -C Debug -R "VirtualGeometry\\.(Render|PageStreaming|ShaderLayout|Batching|HiZ|Hardening|MultiView|Diagnostics|PageResidency)" --output-on-failure
```

Manual validation should open Resource Residency while a dense page-budget
scene is visible. `Stream CPU Bytes` and `Render GPU Bytes` must be exact live
bytes or `unavailable`; package parent rows and pending page requests must not
contribute numeric current bytes unless they own live allocations.

For manual large-asset passes, import one dense scan-style mesh, one
hard-surface multi-material mesh, one one-sided opaque cone-safe mesh, and one
thin/foliage-like or double-sided occlusion mesh into a local scratch project.
Rebuild virtual geometry, enable the Game View stats overlay, then check LOD,
Cluster, Residency, Hi-Z, and Error modes while moving the camera from close-up
to far distance. Record meshlet selected/dispatched counts, pinned fallback page
bytes, requested/missing page counts, cone eligible/disabled counts, disabled
reasons, and whether the object stays visible when high-detail pages are
missing.

## Feature 033 Page Streaming Quickstart

Use `specs/033-vg-page-streaming/quickstart.md` for the staged page-sourced
geometry workflow. The clean-checkout pass should use this manifest and the
focused CTest slices:

```powershell
ctest --test-dir out/build/Debug -R "VirtualGeometryPageStreaming|VirtualGeometryPageResidency|Engine.VirtualGeometryResource|Engine.VirtualGeometryHiZ|Engine.VirtualGeometryMultiView|Engine.VirtualGeometryPerformanceGate" --output-on-failure -C Debug
```

The page-sourced resident fixture must emit resident page payloads as the normal
source and report zero embedded full-mesh fallback uses. The fallback-only
fixture must keep visible coverage when high-detail pages are missing, pending,
failed, or evicted. The page budget fixture must keep resident page bytes within
the configured budget while exposing requested, resident, evicted, and
re-requested page counters. The malformed page fixture must fail integrity
validation before corrupt page payloads become renderable.

For feature `034`, exact residency and fallback diagnostics are active before
the shader path has a standalone page-local decode layout. If a fixture renders
through the owning mesh vertex/remap/primitive streams, diagnostics must label
that as owning-mesh fallback and the no-full-owner residency gate must fail.
Do not mark such a run as true resident-page rendering until the shader consumes
page-local geometry buffers for actual vertex and primitive decode.

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
- Feature `030` dense batching tests must keep submitted virtual geometry
  batches at or below `10%` of selected clusters for the dense clean-checkout
  fixture.
- Feature `030` shadow-view tests must reduce dispatched shadow meshlets by at
  least `40%` against the conservative all-selected baseline while preserving
  the expected silhouette.
- Feature `030` cone-safety tests must reject missing, double-sided,
  mixed-winding, alpha-risk, and high thin-shell-risk metadata by failing open.
  Only clusters with valid cone data and proven one-sided opaque low-risk
  metadata may set the shader cone-culling flag.
