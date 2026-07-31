# TerraEngine Virtual Geometry

## Feature 045 production workflow

Clean-checkout validation always configures, builds, tests, and installs:

```powershell
git submodule update --init --recursive
cmake --fresh --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine TerraEngine-Editor TerraEngine-EngineTests TerraEngine-EditorTests install
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R "Engine.VirtualGeometry|Editor.VirtualGeometry" --output-on-failure
```

Use installed editor automation (`asset.virtualGeometry.inspect`,
`asset.virtualGeometry.settings.get`, `renderer.virtualGeometry.stats`) for
read-only codec/profile/hash/cache/page/error presentation. Rebuild, cancel,
migration, and targeted repair must preserve last-known-good output; canceled or
failed work is never a successful repair. Missing identities remain
`unavailable`, not estimated.

Tooling is under `Tools/VirtualGeometry/`: run `RunCodecExperiment.ps1` for the
32/64/128 KiB matrix, capture with `CaptureVirtualGeometryPerformance.ps1`,
compare at least five comparable measured runs with
`CompareVirtualGeometryPerformance.ps1`, and use
`CompareVirtualGeometryImages.ps1` for conservative image/hash checks. Missing
tools, images, hashes, or raw samples fail closed.

`RunBackendParityMatrix.ps1`, `RunAnimatedVirtualGeometryBenchmark.ps1`,
`RunVirtualGeometryStreamingSoak.ps1`, and
`RunVirtualGeometryProductionSoak.ps1` cover backend, animation, residency, and
bounded-growth validation. Unattached AMD/NVIDIA/Intel Vulkan lanes are
explicitly unavailable and excluded from pass counts; D3D12/DXIL lanes are
deferred beyond feature 045. The installed phase gate
(`RunVirtualGeometryPhaseGate.ps1`) consumes `PhaseScenarios/*.json`; `not-run`
is incomplete, never passing.

This guide tracks the staged implementation of TerraEngine virtual geometry.
The feature targets static opaque meshes on the mesh shader path and deliberately
keeps the active TEMF version at `1` while allowing the payload contract to
break during engine development.

## Implementation Stages

1. Cook virtual mesh assets with adaptive detail chains, spatial clusters,
   meshlets, material section metadata, conservative bounds, and cook diagnostics.
2. Validate LOD correctness so generated detail levels preserve material
   identity, conservative bounds, and acceptable geometric error.
3. Render selected sparse cluster ranges through the existing task/mesh shader
   path with fail-open behavior for missing metadata.
4. Repair Hi-Z usage by keeping depth pyramid generation warm while occlusion
   remains gated by view-history validation and zero-false-occlusion tests.
5. Expose editor inspection and a Virtual Geometry Visualizer with LOD,
   meshlet/triangle identity, bounds, culled, Hi-Z, residency, and error modes.
6. Extend model package import, page residency, automation, export validation,
   and stress tests after the cooked asset and render contracts are stable.

## Adaptive Virtual Geometry Roadmap

The next virtual geometry milestone turns the first whole-object LOD path into
an adaptive, page-streamed system. The order is intentionally conservative:

1. Replace fixed cook ratios with an adaptive profile that requests smoother
   half-octave-style targets, then skips tiny, duplicate-looking, unsafe, or
   non-beneficial levels.
2. Let developers select an imported or package-generated mesh asset and tune
   sparse per-mesh LOD overrides in the Inspector. These controls must not be
   shown for non-mesh asset selections.
3. Explain every requested target as generated, skipped, merged, or rejected so
   artists can trust the cooker without reading raw TEMF payloads.
4. Stream real high-detail geometry pages under budgets while keeping fallback
   pages pinned so missing detail never becomes missing geometry.
5. Strengthen Hi-Z validation until first-frame, camera-cut, resize,
   projection-change, multi-view, shadow, thin-geometry, and self-occlusion
   cases all fail open with explicit reasons.
6. Select a non-overlapping cluster hierarchy cut per view so one large object
   can use high detail nearby and coarser resident fallback detail farther away.

Validation for this milestone is phased the same way: each setup, foundation,
and user-story slice must pass its focused tests before the next slice begins.
Final acceptance still requires the full compile, focused CTest, regression,
and install flow so the installed editor/runtime tree contains matching docs,
shaders, binaries, and generated metadata.

## Feature 045 measurement semantics

Feature 045 benchmark captures are versioned evidence, not estimates. A
capability may be planned or supported without executing; only a completed
live command with generation-safe readback can be marked executed, and only a
capture with validation artifacts can be marked validated. Timing states are
`pending`, `unavailable`, `invalid`, or `measured`; pending and unavailable
values must remain non-numeric rather than becoming zero.

The processor-expanded route is captured first as the immutable correctness and
performance oracle. Use the manifests in `Tools/VirtualGeometry/` and the
capture/comparison scripts to preserve workload, revision, adapter, driver,
route, quality, raw samples, image artifacts, and hashes. Competitive claims
require at least five comparable measured runs in randomized paired order.
Until a runtime adapter and authoritative readback are supplied, the benchmark
writer emits an explicit unavailable skeleton and makes no GPU performance
claim.

## Virtual Geometry Hardening Roadmap

Feature `specs/029-virtual-geometry-hardening` turns the current adaptive
baseline into a validation-ready system. The hardening order is deliberately
correctness-first:

1. Make Hi-Z occlusion conservative enough that visible meshlets fail open
   instead of disappearing, while fully hidden meshlets still cull after the
   depth pyramid is warm.
2. Publish per-view Hi-Z explanations so developers can tell whether a meshlet
   was visible, occluded, unsupported, or kept by a fail-open rule.
3. Stream real high-detail geometry pages through the resource residency path,
   with fallback pages pinned so missing detail never becomes missing geometry.
4. Label runtime counter sources so prepared, delayed GPU-readback, and
   unavailable data are not confused with authoritative zero values.
5. Tune cluster and LOD quality across tiny, dense, thin, curved, hard-surface,
   and multi-material fixtures.
6. Stabilize detail transitions with view-local state so stationary cameras do
   not flicker near thresholds.
7. Keep Scene View and Game View visualizer, Hi-Z, counter, residency, and LOD
   state independent.
8. Keep the user-facing virtual geometry visualizer inventory compact and
   documented.
9. Validate all of the above with deterministic fixture coverage and optional
   large-asset manual stress passes.

Each hardening story must be independently testable before the next story
starts. The default focused pass is `Engine.VirtualGeometryHardening` and
`Editor.VirtualGeometryHardening`, followed by the broader virtual geometry
regression tests and the install target.

## GPU-Driven Batching And Streaming Scaling Roadmap

Feature `specs/030-vg-gpu-driven-batching` moves the hardening baseline toward
a scalable virtual geometry renderer. The goal is not to change the working
Hi-Z behavior; it is to reduce command pressure, make residency more explicit,
and add gates that keep the renderer from regressing while future GPU-driven
work lands.

The scaling order is:

1. Convert the selected sparse cluster cut into cluster packets, then group
   compatible packets into bounded batches. The current one-cluster-draw path
   remains a correctness fallback while the batched path is validated.
2. Compact candidate packets into view-visible work before submission. Frustum,
   Hi-Z, residency, future cone safety, and fail-open decisions must publish
   source-labeled counters instead of silently dropping work.
3. Treat high-detail virtual geometry pages as real residency records with
   requested, pending IO, pending upload, resident, failed, evicting, evicted,
   and pinned fallback states. Missing detail must substitute resident fallback
   coverage instead of creating holes.
4. Preserve the current working Hi-Z baseline. This milestone adds regression
   checks and source-labeled diagnostics, not new Hi-Z quality tiers or a new
   rejection policy.
5. Add virtual geometry performance gates for submitted batch count, CPU render
   preparation cost, selected/compacted/dispatched meshlet ratios, page pressure,
   false occlusion, shadow work reduction, and multi-view isolation.
6. Give shadow views their own virtual geometry selection and culling state.
   Shadow visibility must be evaluated from the light view, not borrowed from
   the camera view or camera Hi-Z pyramid.
7. Enable cone culling only for clusters whose imported and cooked metadata
   proves one-sided opaque safety, consistent winding, low thin-shell risk, and
   valid cone data. Unknown or risky clusters stay conservative.

The Hi-Z preservation story is deliberately narrow. The renderer publishes a
per-view `HiZRegressionSnapshot` copied from the current validation state and
meshlet counters so Game View, Scene View, automation, and tests can prove that
batching, compaction, and page residency did not change the accepted behavior.
If a current pyramid is unavailable the path fails open; if a current pyramid
exists, camera motion, target ownership, quality metadata, and guard metadata
remain diagnostic-only. There are no user-facing or shader-facing Hi-Z quality
tier controls in this milestone.

## Visible Command Reuse Optimization

Feature `specs/032-visible-command-reuse` keeps the existing virtual geometry
selection and Hi-Z behavior unchanged while reducing static-frame reuse cost.
When a visible-command cache record is rebuilt, the renderer now derives a
deduplicated touch set for visible meshes, materials, and textures. Cache-hit
frames publish that compact set through ResourceManager instead of walking every
cached draw command and material binding again.

The optimization is intentionally fail-open:

1. If the visible-command signature changes, the renderer rebuilds the command
   list and virtual geometry payload.
2. If a cached streaming touch set is missing or untrusted, the renderer falls
   back to the older per-command refresh path and reports a conservative-refresh
   warning.
3. Compact virtual geometry counters stay available every frame, while full
   asset/runtime diagnostics remain behind explicit diagnostics demand.
4. Scene View, Game View, runtime, offscreen, and shadow ownership stay separated
   by the existing cache signature and output id.

Profiler and automation surfaces expose cache hit state, copied command count,
unique visible touches, duplicate touches eliminated, conservative-refresh
reason, and compact-versus-detailed diagnostic demand. This makes the reuse path
measurable without re-enabling noisy always-on overlays.

GPU packet buffers, packet compaction, and generated submission records carry
the current view's Hi-Z state as ownership and diagnostics metadata only. They
do not select a lower, medium, high, or debug Hi-Z path, and they do not change
the task shader's current-pyramid rejection test. If generated packet work is
stale, unsupported, or mismatched, TerraEngine keeps the accepted fail-open
coverage path and reports the fallback reason instead of publishing a
new occlusion policy.

```powershell
ctest --test-dir out/build/Debug -R "Engine.VirtualGeometryBatching|Engine.VirtualGeometryCompaction|Engine.VirtualGeometryPerformanceGate|Engine.VirtualGeometryShadow|Engine.VirtualGeometryCulling|Engine.VirtualGeometryRenderer|Engine.VirtualGeometryResource|Editor.VirtualGeometryAssetPipeline|Editor.VirtualGeometryAutomation|Editor.VirtualGeometryExport|Editor.VirtualGeometryDiagnostics" --output-on-failure -C Debug
```

Implementation must complete and test one phase or user story before the next
one starts. Final acceptance still requires the broader virtual geometry
regression pass, affected target build, and install target so the installed
editor/runtime tree contains matching shaders, binaries, docs, and metadata.

## GPU Packet Compaction Roadmap

Feature `specs/031-vg-gpu-compaction` takes the packet and batching foundation
from feature `030` and moves the hot submission path toward GPU-consumed packet
buffers and GPU-generated compacted submission records. The current
CPU-selected cluster cut remains the correctness oracle while this migration is
validated. This keeps coverage, LOD selection, material ownership, residency
fallbacks, and debug visualizer identity diagnosable while reducing CPU command
pressure.

The implementation order is:

1. Register explicit scheduler resources for per-view GPU packet frame sets,
   compaction dispatches, generated submission records, and counter readbacks.
2. Convert existing selected cluster packets and compatible cluster batches into
   GPU-visible packet and batch buffers owned by one view and one frame.
3. Validate GPU packet buffers against the CPU-selected cluster cut before they
   can become the shader-visible submission source.
4. Compact candidate packets on the GPU into visible, rejected, fallback, and
   fail-open work while preserving page-need and fallback coverage metadata.
5. Generate submission records from compacted packets and use them only when
   frame ownership, view ownership, layout, capacity, and counter checks pass.
6. Preserve the current task-shader Hi-Z behavior exactly. This milestone does
   not add quality tiers, new user controls, or a retuned rejection policy.
7. Publish per-view diagnostics, automation, MCP, overlay, and profiler
   snapshots with `Prepared`, `GPU Readback`, `Delayed GPU Readback`,
   `Unsupported`, or `Unavailable` source labels instead of placeholder zeroes.
8. Gate dense batch ratio, CPU preparation cost, compaction truth, generated
   submission validity, multi-view isolation, and Hi-Z baseline preservation.

Uncertain generated work fails open to the current correct path. Fallback is
not silent: diagnostics must name unsupported backends, invalid layouts, stale
views, counter mismatches, compaction overflow, readback unavailability, or
validation fallback. Scene View, Game View, runtime views, and future offscreen
views own separate packet buffers and counter records so camera, target size,
debug mode, and Hi-Z state cannot leak across views.

The generated path publishes three diagnostic layers:

- GPU packet records and batch descriptors describe the selected cluster cut in
  a shader-readable table, including view ownership and material compatibility.
- Compaction dispatch records describe candidate, compacted, rejected,
  fail-open, and fallback packet work before final submission.
- Generated submission records describe the packet ranges that may drive
  mesh-shader submission after frame id, view id, packet capacity, batch
  compatibility, and counter sanity all validate.

If any layer is unsupported, stale, delayed, or mismatched, TerraEngine keeps
the conservative visible fallback path and records the reason. The renderer also
publishes compact stats for performance gates: total gates, failed gates,
unsupported gates, and the first failed gate label. Detailed diagnostics and
automation responses include each gate's fixture id, view type, view id,
metric, threshold, actual value, source, status, and diagnostic text.

```powershell
ctest --test-dir out/build/Debug -R "Engine.VirtualGeometryBatching|Engine.VirtualGeometryCompaction|Engine.VirtualGeometryPerformanceGate|Engine.VirtualGeometryHiZ|Engine.VirtualGeometryMultiView|Engine.VirtualGeometryRenderer|Engine.Renderer|Engine.SystemScheduler" --output-on-failure -C Debug
```

Implementation must complete and test one phase or user story before the next
one starts. Final acceptance also requires the broader virtual geometry
regression pass, affected target build, and install target so the installed
editor/runtime tree contains matching shader assets, binaries, docs, and
diagnostics.

## Page-Sourced Geometry Streaming Roadmap

Feature `specs/033-vg-page-streaming` changes the virtual geometry payload
contract from selected meshlet ranges inside the fully resident owning mesh to
selected clusters resolving to resident streamed page payloads. The current
cluster selection, GPU packet batching, generated submission validation, and
Hi-Z policy stay as the correctness baseline; this milestone changes geometry
source and residency ownership.

The implementation order is:

1. Define typed page payload records, resident page sets, page requests,
   retirement records, and scheduler resources before renderer behavior
   changes.
2. Render fallback-only and mixed-resident assets from resident page payloads,
   with the old embedded owning-mesh path reported only as a conservative
   fallback source.
3. Keep fallback pages pinned through pending, failed, and evicted high-detail
   page transitions so missing detail never becomes missing geometry.
4. Request, upload, evict, and re-request high-detail pages under a configured
   virtual geometry page budget while keeping fallback coverage visible.
5. Preserve per-cluster mixed detail by resolving each selected cluster to a
   compatible resident page payload instead of falling back to whole-object LOD.
6. Protect the accepted Hi-Z behavior exactly. Page payload bounds and source
   signatures may be tracked, but no Hi-Z quality tiers or retuned rejection
   policy are introduced.
7. Publish per-view diagnostics for resident bytes, requested pages, emitted
   source, fallback substitution, embedded fallback use, missing coverage, and
   source-labeled counters.
8. Validate cooked assets before rendering by checking page payload bytes,
   fallback links, cluster links, material ranges, bounds, byte costs, and
   small-mesh skipped-streaming reasons.

Use the page-streaming fixture slice while implementing this feature:

```powershell
ctest --test-dir out/build/Debug -R "VirtualGeometryPageStreaming|VirtualGeometryPageResidency|Engine.VirtualGeometryResource|Engine.VirtualGeometryHiZ|Engine.VirtualGeometryMultiView|Engine.VirtualGeometryPerformanceGate" --output-on-failure -C Debug
```

US4 page-local mixed-detail validation uses the registered
`Engine.VirtualGeometryRenderer` CTest target. The literal
`VirtualGeometry.Render` regex currently matches no top-level CTest entry, so
the renderer target is the canonical checkpoint for sparse cluster cuts,
material/page boundary diagnostics, page-payload cache invalidation, and shader
packet source identity.

Each phase or user story must run its focused CTest slice before the next one
starts. Final acceptance still requires the broader virtual geometry and
resource labels plus the install target so the installed editor/runtime tree
contains matching shaders, diagnostics, docs, and page payload metadata.

## Page-Sourced Render Flow

At runtime, virtual geometry follows one view-local immutable path:

1. Render preparation selects the sparse cluster cut for each object.
2. Each selected cluster resolves to a resident high-detail page payload, a
   resident fallback page payload, or a labeled conservative embedded fallback.
3. The renderer records page needs for missing high-detail pages before
   fallback substitution, so streaming can request detail without creating
   visible holes.
4. Vulkan upload planning publishes page-local payload metadata, packet buffers,
   batch descriptors, generated submissions, and counter readbacks only after
   protected resource publication has completed.
5. The task/mesh shader path consumes the selected page-local meshlet ranges and
   labels the emitted source. Normal frames should report resident page payloads
   or resident fallback payloads; embedded owning-mesh fallback is a diagnostic
   fail-open path, not the scalable path.
6. Editor overlays, automation, the profiler, and performance gates read the
   immutable per-view page-source diagnostics instead of inferring residency
   from draw counts.

## True Page Residency And Exact Memory Accounting

Feature `specs/034-vg-true-page-residency` tightens the page-streaming contract
from "page-aware diagnostics" to "page-owned rendering and exact residency."
`Stream CPU Bytes` and `Render GPU Bytes` must describe current live ownership
for the specific asset row only. They must not include estimates, desired load
sizes, pending uploads, package-parent bytes, diagnostic-only metadata, or
bytes owned by another generated child asset.

When exact ownership cannot be proven, developer-facing surfaces must show a
non-numeric unavailable state and a diagnostic reason. Showing `0` is valid
only when the current load or allocation state proves that no bytes are resident
in that domain.

The renderer should bind resident high-detail page payloads or protected
fallback page payloads for selected virtual geometry packets whenever valid
streamable page data exists. The full owning mesh payload remains a legacy or
diagnosed fail-open route, but normal page-streamed frames should not require
all high-detail mesh bytes to be resident.

Current implementation status: exact CPU/GPU byte ownership, per-view page
source diagnostics, protected fallback accounting, owning-mesh fallback
reasons, and page-local shader decoding are implemented. Resident high-detail
and fallback packets publish standalone vertex, primitive, remap, meshlet,
bounds, decode-info, and material streams. The task and mesh shaders select
those streams before reading decode records or bounds, so the normal
page-streamed path does not require full owner geometry buffers. The owner path
remains a diagnosed fail-open route for real page-policy failures and must
report exact owner bytes and a reason instead of being counted as page-owned
rendering.

This milestone preserves the accepted Hi-Z behavior. Page residency may change
payload ownership, descriptor selection, and byte accounting, but it must not
introduce Hi-Z quality tiers, soft disable paths, or shared mutable state
between Scene View and Game View.

## Asset Rebuild Expectations

The current plan breaks older compiled mesh payloads in place. Users must
rebuild affected static mesh and model package outputs from source after the
virtual geometry payload lands. No legacy compatibility loader is planned while
the engine format is still unstable.

### Rebuild Workflow

1. Rebuild each source mesh or model package through the editor asset import
   path or `TerraEngineMeshCompiler`.
2. Keep meshlet bounds enabled for virtual geometry cooks. The renderer uses
   cullable-group and cluster bounds for useful selection and diagnostics.
3. Treat `virtualGeometryStatus=Cooked` as a generated LOD chain and
   `virtualGeometryStatus=Skipped` as a valid one-detail fallback for small
   meshes that do not benefit from simplification.
4. Rebuild immediately when the runtime logs stale or incomplete virtual
   geometry header metadata. TEMF remains version `1`, but the in-place payload
   generation is intentionally allowed to change during development.

Expected cook diagnostics:

- `virtualGeometryDetailLevels`: number of selectable adaptive detail levels.
- `virtualGeometryMeshlets`: resident meshlet/cullable-group count across all
  emitted LODs.
- `virtualGeometrySourceTriangles`: original source triangle count before
  simplification.
- `virtualGeometryFallbackAvailable`: whether a renderable fallback detail is
  present.
- `virtualGeometryRequestedTargets`: number of adaptive targets the cook tried
  to satisfy.
- `virtualGeometryGeneratedLevels`: number of actual detail levels emitted into
  the TEMF payload.
- `virtualGeometrySkippedLevels`: number of requested targets that were skipped,
  merged into a nearby generated level, or rejected for safety.
- `virtualGeometryWarnings`: count of warnings also stored in the JSON report
  and TEMF diagnostic table.

### Adaptive Default Ratios And Skips

The default adaptive profile requests eight half-octave-style triangle ratios:
`1.0`, `0.70710677`, `0.5`, `0.35355338`, `0.25`, `0.17677669`,
`0.125`, and `0.08838835`. This gives smoother transitions than the earlier
hardcoded coarse chain while keeping the default cook small enough for fast
iteration.

Those ratios are targets, not guarantees. The cooker records every requested
target, then decides whether it became:

- `Generated`: a real emitted detail level with triangle count, meshlet count,
  actual ratio, and geometric error.
- `Merged`: close enough to a previous generated level that a separate LOD
  would add switching cost without useful visual or meshlet savings.
- `Skipped`: intentionally omitted because the requested output would be too
  small to be useful.
- `Rejected`: simplification produced invalid output or exceeded safety limits.

Small meshes stay renderable by emitting at least the highest-detail fallback.
The default count gates are `1` source triangle, `1` generated triangle, and
`1` meshlet, while both useful-reduction percentage gates default to `0%`.
Therefore, import keeps every smaller valid simplification that stays within the
default `0.1` geometric-error limit. Invalid or duplicate simplifier output is
still rejected safely. Artists can set nonzero triangle and meshlet reduction
percentages per mesh; skipped and merged diagnostics record the actual candidate
counts and the selected threshold.

### Inspector Per-Mesh Overrides

Per-mesh adaptive settings are authored from the Inspector, not from project
settings JSON by hand. Select a loose imported mesh asset to edit a `.meta`
override, or select a generated package mesh child to edit that child's package
sidecar override. The source glTF/GLB/FBX package row itself, materials,
textures, scripts, scenes, and other non-mesh assets do not show these controls.

The Inspector override panel stages the resolved project defaults first. Enable
`Per-Mesh Override`, tune the target count, ratios, count thresholds, minimum
triangle/meshlet reduction percentages, and maximum geometric error, then choose `Save Settings` or
`Rebuild`. Rebuild requests go through the normal AssetDatabase import boundary,
so compiled mesh publication remains atomic and failed rebuilds leave the last
usable compiled mesh intact. `Reset` removes the override and returns the mesh
to project defaults on the next rebuild.

Package-generated mesh overrides are preserved by stable generated mesh GUIDs
in the import sidecar. The current package compiler still emits one package
TEMF for the package path, so the first preserved generated-mesh override is
used as the package cook profile until later work splits package meshes into
independent virtual-geometry cook units.

### Cook Diagnostic Interpretation

Every requested adaptive target is exposed through compiler JSON, package
import sidecars, the mesh Inspector, and `asset.virtualGeometry.inspect`.
Loose mesh Inspector rows are read from TEMF requested/skipped/detail tables;
package generated mesh rows are read from the package sidecar.

- `Generated`: the request produced a renderable detail level. The row reports
  actual ratio, triangle count, meshlet count, and geometric error where
  available.
- `Merged`: the candidate was too similar to the previous accepted level, so
  the previous generated level remains the nearest fallback.
- `Skipped`: the request was intentionally omitted, usually because it was too
  small to meet triangle or meshlet thresholds.
- `Rejected`: the candidate was unsafe, invalid, or exceeded the geometric
  error limit and is never registered as renderable data.

For automation, read `settings` to see the resolved project/default or
per-mesh profile, and read `cookDiagnostics.requestedTargets` to explain each
target without opening the cooked file.

## Visual Correctness Rules

Generated LODs must stay conservative until a later story adds richer
section-aware import metadata:

- Each detail level owns at least one non-empty material section range, and the
  ranges must not overlap resident meshlets.
- The current compiler emits one default material section per LOD; package
  importers can expand this once source primitive/material slots are threaded
  through the raw mesh handoff.
- Package-generated mesh sidecars compare authored material assignments with
  cooked virtual-geometry material sections. A multi-material package whose
  cooked TEMF still exposes only the default section is marked `Needs Review`
  in the Inspector instead of silently claiming section-safe cluster coverage.
- Detail bounds must remain conservative. The cooker stores source-position bounds
  on every detail so renderer-side LOD choice cannot shrink object culling
  below the authored mesh extent.
- Geometric error must be finite, monotonic, and no greater than `0.1` for an
  accepted simplification candidate. Candidates outside that tolerance are
  skipped and the previous safe detail remains the fallback.
- Runtime diagnostics should flag aggressive `maxLodDrop` and scaled error
  combinations because those settings can visibly pop during fast camera motion.

## Mesh Shader Rendering Controls

The first renderer path is intentionally mesh-shader-only. A cooked virtual mesh
is still submitted through the existing task and mesh shaders; render
preparation selects a non-overlapping sparse cluster cut per visible object and
publishes that cut as cluster packets. Valid GPU packet and generated
submission records can drive the submitted mesh-shader work; invalid or
unsupported generated records fail open to the older visible draw-range path.

- All imported mesh assets are treated as virtual geometry candidates. Render
  preparation still fails open to the best resident fallback detail when a mesh
  lacks valid cooked metadata or a requested high-detail page is unavailable.
- Task shader culling must be available and enabled for the object. If task
  culling is disabled, unsupported, or missing required meshlet bounds, virtual
  geometry fails open to LOD0.
- `lodErrorScale` tightens or relaxes geometric-error acceptance after cooking.
  A selected LOD whose scaled error is above the current tolerance backs down to
  a safer detail level.
- `maxLodDrop` limits how many adaptive detail levels a single selection step may skip.
  Use small values while tuning camera-motion popping.
- `lodTransitionHysteresisPixels` defaults to `0.35` pixels. When a cluster
  split/merge candidate is within this projected-error band, the renderer keeps
  the last stable cut instead of flipping every frame.
- `lodTransitionStableFrames` defaults to `3` frames. A near-threshold candidate
  must persist for this many rendered frames before it replaces the current
  stable cut; decisive movement outside the hysteresis band can switch
  immediately.
- Render-preparation cache signatures include the selected detail index,
  cluster packet, meshlet range, and view-local transition state, so
  camera-distance or mode changes cannot reuse stale immutable draw packets.
- Fail-open diagnostics record the mesh identifier and the reason visibility was
  preserved through fallback rendering. Current reasons include disabled quality
  settings, disabled task culling, invalid bounds, and missing detail metadata.

### Hierarchical Cluster Selection

Mixed-detail rendering is expressed as a non-overlapping cluster hierarchy cut,
not as independent per-meshlet picks from unrelated whole-mesh LODs. The cooker
emits stable cluster ids, parent/child links, material section ownership, page
references, conservative bounds, geometric error, and a fallback cluster path.
The loader rejects cycles, missing fallback coverage, invalid material sections,
and cluster-page links that do not fit the declared cluster/page group ranges.

Current cooks sort source triangles into Morton-ordered fine regions, then build
every emitted cluster from a contiguous range of those source regions. Dense
source meshes can produce up to sixty-four fine regions by default, while tiny
meshes stay whole so the cooker does not create pointless dispatch work. Coarser
LOD clusters cover unions of fine regions and link only to adjacent-detail child
clusters whose source-region ranges exactly tile the parent. The cooker sets
`ClusterCoverageValidated` only after that tiling proof passes; otherwise render
preparation fails open to the complete highest-detail range instead of
dispatching a sparse mixed-detail cut that could create holes.

At render preparation time, the selector starts from each root cluster. If the
projected screen error is small, the parent cluster is selected. If the screen
error is large enough, the selector refines into child clusters and publishes
those children instead. Cluster refinement requires a slightly stronger signal
than one pixel of error so flat, tiny, or hard-surface regions do not split on
barely-visible numerical noise. Screen error is evaluated from each cluster's
own conservative bounds when camera data is available, so a nearby region can
refine while a far region of the same object stays coarse. A selected set may
contain siblings, but it must not contain a parent beside any child that covers
the same surface.

When a desired child cluster page is missing, failed, evicting, or evicted, the
selector records a page need and substitutes the first resident fallback cluster
in the cluster fallback chain. If that fallback is an ancestor of the desired
child, the ancestor suppresses sibling child selections for that subtree so the
frame keeps coverage without cracks or double-drawn parent/child regions.

The current hardening renderer can expand the selected cluster cut into one
draw command per selected cluster. Feature `030` replaces that scaling shape
with cluster packets grouped into bounded batches while preserving the old path
as a correctness fallback. Each packet still describes the selected cluster's
meshlet offset/count and selected-cluster metadata, so the task and mesh shaders
dispatch the actual sparse cluster set instead of a whole-object min/max
envelope. Arbitrary per-meshlet LOD mixing is still avoided because independent
meshlets from unrelated LODs do not prove shared coverage, material ownership,
or fallback ancestry, and can therefore create holes, duplicate surfaces, or
unstable debug output.

`ClusterHierarchyValidated` means ids, ranges, parent links, fallback links, and
page links are structurally valid. `ClusterCoverageValidated` is the stronger
bit required for mixed-detail rendering; it means every LOD cluster covers a
disjoint source-region range, every detail level tiles the full source region
set, and every parent-child relationship preserves that coverage exactly.
Missing coverage validation is visible as fail-open diagnostics and conservative
LOD0 dispatch.

## Quality Controls And Failure Modes

Virtual geometry quality lives in the project/runtime `RenderSettings.json`
under `settings.virtualGeometry`. The current fields are:

- `lodErrorScale`: clamps to `[0.05, 64.0]` and scales cooked geometric error
  before a lower detail level is accepted.
- `maxLodDrop`: limits how far selection may move away from the highest detail.
- `lodTransitionHysteresisPixels`: clamps to `[0.0, 8.0]` and defines the
  projected-error band used to hold the last stable cluster cut.
- `lodTransitionStableFrames`: clamps to `[1, 16]` and defines how many frames a
  near-threshold candidate must remain stable before it is accepted.
- `hiZOcclusionEnabled`: retained for settings compatibility, but the runtime
  Hi-Z path is always active once the pyramid resource exists.
- `hiZSelfOcclusionGuardEnabled`, `hiZNearPlaneGuardEnabled`, and
  `hiZThinGeometryGuardEnabled`: retained for settings compatibility and
  diagnostics, but no longer gate task-shader Hi-Z culling.
- `debugVisualizationMode`: selects the editor/game-view false-color debug
  output.
- `geometryBudgetBytes`: feeds residency diagnostics and budget pressure
  reporting.
- `preloadBias`: biases streaming requests ahead of the exact selected detail.

Unsupported hardware is reported as `Missing Mesh Shader Route`. Vulkan exposes
this separately from task shader support because TerraEngine has a mesh-only
shader collection that can still render visible geometry when task-stage
culling is absent. If the backend does not expose the mesh shader route, active
virtual geometry is treated as unsupported rather than silently producing
partial output.

Invalid virtual geometry metadata is rejected before it becomes live resource
state. A failed mesh reload keeps the previous usable mesh cached, and editor
mesh rebuilds publish through a temporary compiled file before replacing the
existing `.temf`. Unless a destructive delete/import cleanup path is explicitly
requested, failed rebuilds should leave the last good compiled asset on disk.

Incompatible compiled artifacts show rebuild guidance in the mesh Inspector.
Because TEMF remains version `1` during development, stale virtual geometry
payload generations are expected to require a source rebuild instead of a
compatibility shim.

## Hi-Z Validation Gates

Hi-Z mip generation stays active after the scene depth pass. New pyramids are
cleared to standard-Z far depth (`1.0`) before first use, so task shaders can
sample them immediately without false occlusion. Meshlet occlusion only requires
that the GPU pyramid resource exists; all history, quality, guard, ownership,
camera, and depth-scene metadata is diagnostic-only.

View ownership, target ownership, projection, and depth-scene signatures remain
visible in diagnostics, but they do not hard-disable Hi-Z during normal
camera/object/editor-view motion. The task shader still requires sampled depth
to conservatively prove that a meshlet is hidden; if the previous pyramid cannot
prove occlusion for the projected bounds, that meshlet stays visible.

The only inactive reason that should affect runtime Hi-Z is a missing pyramid
resource. First frame, resize, view/target ownership, projection changes,
camera-cut metadata, depth-scene changes, quality toggles, and guard metadata
remain diagnostic fields without turning Hi-Z off. Task shaders sample Hi-Z
whenever the frame culling payload says the pyramid exists. If sampled depth is
invalid or cannot conservatively prove occlusion for a meshlet's projected
bounds, that meshlet stays visible and records no Hi-Z cull.

US1 hardening adds explicit per-view Hi-Z validation state to immutable
visibility snapshots. Each view reports whether a current pyramid is ready, the
sampled pyramid size, render size, camera/depth-scene signatures, fail-open
reason, and warm-up frame count. A moving opaque blocker must cull only meshlets
that are fully hidden, reveal them again within two rendered frames after the
blocker moves away, and resume culling when the blocker returns. Meshlets whose
projected sphere crosses the near plane or screen edge fail open instead of
sampling a clipped Hi-Z rectangle.

US2 adds bounded `OcclusionDebugSample` records to the same per-view snapshots.
Each sample identifies a representative object, mesh, cluster, meshlet, cull
outcome, sampled mip category, projected bounds placeholder, and explanation
string. The current renderer synthesizes these records from selected draw
ranges and fail-open paths; future GPU readback can fill the same structure
without changing editor consumers. Diagnostics keep at most 64 samples per view
so the visualizer and automation remain predictable.

## Scheduler And Mutation Boundaries

Virtual geometry rendering must use immutable render snapshots and prepared
packets. Worker-safe preparation may read asset data, view data, residency
snapshots, and quality settings, but it must not mutate live scene, object,
hierarchy, or component storage. Asset registration, residency publication, GPU
submission, and export packaging remain protected boundaries with explicit
diagnostics.

## Debug Visualization Inventory

The Scene View and Game View `VG` selector expose the same compact mode
inventory. Both views read `GetVirtualGeometryDebugVisualizationModes()`, so
the editor cannot drift into different menus per viewport. Hover the `VG`
control or any menu row to show the same color legend that is documented below.
Obsolete or internal-only numeric mode values are normalized to `Off` instead
of appearing as user-facing menu entries.

| Mode | Color meaning |
| --- | --- |
| `Off` | Normal material rendering. |
| `LOD` | Stable false color per selected detail level. Different colors should appear as the adaptive selector chooses coarser or finer detail. |
| `Cluster` | Stable false color from the selected cluster packet. This helps spot when adaptive cluster identity changes. |
| `Meshlet Id` | Stable false color per rendered meshlet id. This is the easiest way to inspect selected meshlet density. |
| `Bounds` | Blue to yellow maps small to large bounds. Magenta means bounds or culling metadata is unavailable. |
| `Culling` | Green means drawn, red means frustum, cyan means drawn while a Hi-Z pyramid is available, and magenta means culling data is missing. Cone culling contributes only when every selected cluster has proven one-sided opaque safety metadata. |
| `Residency` | Green means resident/active virtual geometry, orange means requested or missing page pressure, and brown means inactive fallback. |
| `Hi-Z` | Cyan means the current view has a sampled Hi-Z pyramid, red means fail-open or uncertain projected bounds, and gray means the pyramid resource is missing. |
| `Error` | Cyan means no error pressure, orange means missing-page pressure, and red means fail-open. |

Mode changes are part of the virtual geometry quality-profile signature used by
the visible-command cache. Scene View and Game View therefore refresh on the
next rendered frame even when the camera is stationary. The current mesh shader
path colors the selected sparse cluster draw range and passes selected cluster
count, page-need count, and fallback pressure through a compact payload word.
Future page-backed GPU buffers can replace that packed word with richer
per-cluster records without changing authored materials.

### Multi-View State Isolation

Scene View and Game View store their active visualizer mode as separate
view-local runtime preferences. The project quality profile keeps only the
default mode; changing one toolbar must not rewrite project render settings or
force the other view into the same diagnostic mode.

Every rendered view owns its virtual geometry state independently:

- Scene View and Game View keep separate visualizer modes, target sizes, camera
  signatures, transition histories, selected cluster cuts, page requests,
  counter sources, and Hi-Z validation records.
- A hidden or zero-sized view publishes an explicit missing-pyramid/fail-open
  state instead of borrowing the other view's Hi-Z pyramid or counters.
- Visible-command cache signatures include the view id, target size,
  visualizer mode, and LOD transition state so a cached packet from one view
  cannot be replayed into another view.
- Shadow views remain conservative and do not expose a user-facing visualizer
  mode; their state is reported separately as `Shadow`.

## Editor Inspection And Visualizer Workflow

Mesh assets expose a read-only Virtual Geometry inspector section when their
compiled TEMF can be read. The section reports cook status, payload generation,
detail count, source triangle count, resident meshlets, cone-safety record
counts, eligible cone-culling clusters, disabled cone-culling reasons, and
rebuild guidance for missing or stale payload metadata.

Game View owns the first visualizer control. The `VG` mode selector writes a
view-local virtual geometry debug mode into `RenderSystem`, and the default
fragment shader produces false-color output for LOD, meshlet
identity approximation, bounds, culled/fail-open state, Hi-Z state, residency,
and error modes. A compact overlay reads immutable virtual geometry diagnostics
snapshots so the selected mode is paired with current meshlet and Hi-Z state.

Generated package sub-assets are shown as read-only in Project Browser. Their
lifecycle is owned by the source package and reimport path, so editor UI should
surface metadata and allow drag/drop assignment without offering destructive
standalone edits.

## Model Package Import And Reimport

FBX and glTF package imports compile supported static mesh output through the
same TEMF virtual geometry cook used by loose mesh assets. Each generated mesh
record in the package `.import.json` sidecar carries:

- Stable generated mesh UUID.
- Compiled TEMF path.
- `virtualGeometry.requested`, `cooked`, and `status`.
- Detail level, meshlet, and source triangle counts.
- `virtualGeometry.coverageTags` such as `package-gltf`, `package-glb`,
  `package-fbx`, `adaptive-targets`, `fallback-coverage`, `page-links`, and
  `multi-material-package`.
- Non-fatal cook warnings.

Supported status values are `Cooked`, `Skipped`, `Failed`, and `NeedsRebuild`.
Importers only report `Cooked` after the compiled TEMF header and virtual
geometry root metadata validate. Failed or invalid generated mesh outputs remain
visible in diagnostics but are skipped by package resource registration so they
are not treated as assignable runtime meshes.

Generated mesh, material, and texture identifiers are deterministic children of
the package GUID, child role, and source index. Reimporting an unchanged source
hierarchy therefore preserves generated sub-asset references and material-slot
links. The package source file remains the owner; generated children are
read-only browser entries and should be changed by editing or reimporting the
package.

Unsupported animated, skinned, camera, light, or constraint content is reported
as package diagnostics when static geometry can still be extracted. These
warnings downgrade the import to `SucceededWithWarnings` without blocking usable
static virtual geometry output.

## Automation Requests

Editor automation and MCP-facing tools expose virtual geometry through the
asset command surface. These commands mutate only asset/build outputs and
ResourceManager registration; they must not mutate live scene objects.

`asset.import` returns virtual geometry fields for model package generated
meshes:

```json
{
  "path": "Models/asset.gltf",
  "force": true,
  "status": "Succeeded",
  "generatedMeshes": [
    {
      "guid": "...",
      "sourcePath": "Models/Generated/mesh.temesh",
      "compiledPath": "Models/Generated/mesh.temf",
      "usable": true,
      "virtualGeometryStatus": "cooked",
      "detailLevelCount": 3,
      "sourceTriangleCount": 2048,
      "selectedFallbackAvailable": true,
      "warnings": [],
      "errors": []
    }
  ]
}
```

`asset.virtualGeometry.settings.get` reads the project defaults when no asset is
provided. When `asset` names an imported mesh or a package-generated mesh, the
response also includes `hasMeshOverride`, the sparse `meshOverride`, the
resolved inherited settings, a deterministic `settingsSignature`, and validation
messages. Non-mesh assets fail validation instead of showing virtual geometry
override controls.

`asset.virtualGeometry.settings.set` saves a sparse per-mesh override for an
imported mesh asset. The command accepts `validateOnly` for dry-run editor or
MCP workflows and `rebuildAfterSave` when the caller wants the compiled TEMF to
be refreshed immediately. Invalid overrides are rejected before metadata is
written, so the previously usable settings and cooked mesh remain intact.

`asset.virtualGeometry.settings.reset` clears the per-mesh override and resolves
the selected mesh back to project defaults. `rebuildAfterReset` follows the same
asset registration boundary as a normal rebuild.

`asset.virtualGeometry.rebuild` accepts `target`, `force`,
`includePackageOutputs`, `settingsMode`, and `diagnosticsLevel`. `target` may be
an asset path, asset UUID, package source, or `all`. The response reports
`completed`, `partial`, or `failed` status plus `rebuiltAssets`,
`skippedAssets`, `failedAssets`, `warnings`, and `durationMs`. Rebuilt records
include the settings source, requested/generated/skipped detail counts, cluster
count, page count, fallback coverage state, resolved settings, cluster summary,
and page residency summary.

`asset.virtualGeometry.inspect` accepts `asset`, `includeSettings`,
`includeCookDiagnostics`, `includeClusterHierarchy`, `includeRuntimeState`, and
an optional `viewType`. The response includes settings, cook diagnostics,
cluster summary, page residency summary, runtime selection summary,
Hi-Z validation state, stale reasons, meshlet counts, and the latest immutable
runtime virtual geometry view snapshots when available.

Successful automation rebuild/import results become visible only after the
AssetDatabase `FinishImport` registration boundary. Failed requests do not mark
generated mesh outputs usable; failed package imports, failed virtual geometry
metadata, invalid override requests, failed rebuilds, and `NeedsRebuild`
generated mesh records are skipped during generated resource registration.

## Geometry Page Residency

The cooker emits streamable page metadata for every adaptive detail level. Each
page record names the covered LOD range, cullable group range, byte estimate,
priority, fallback page, and cluster-page coverage links. LOD0 is its own
fallback page and the loader rejects current virtual geometry payloads that do
not carry fallback coverage validation flags, page byte costs, and cluster-page
links. TEMF remains v1 while this format is still allowed to break.

The Resource layer owns page state transitions:

- `NotResident` -> `Requested` when a visible view needs a page.
- `Requested` -> `PendingIO` when asynchronous file work starts.
- `PendingIO` -> `PendingUpload` after page data validates and awaits the
  protected GPU publication boundary.
- `PendingUpload` -> `Resident`, or `Pinned` for visible fallback pages, after
  protected backend upload publication.
- `Requested`, `PendingIO`, or `PendingUpload` -> `Failed` when load,
  validation, upload, or stale-version checks fail.
- `Resident` -> `Evicting` -> `Evicted` for non-fallback pages after deferred
  GPU retirement is safe.

Fallback pages are pinned while an asset is renderable. They are excluded from
eviction candidates even when the geometry budget is exceeded, so missing
high-detail data reduces quality instead of creating holes. Higher-detail pages
are ordered for eviction by lower priority, older use, and larger reclaimable
byte size. Streaming need planning is worker-safe: immutable render selection
snapshots are converted into page need inputs and async request packets for
desired and fallback pages, while ResourceManager cache mutation, GPU upload
scheduling, and residency publication remain protected boundaries.

The worker-safe page request plan now publishes both request packets and failure
records. If a desired high-detail page has failed, the planner records the
failed page, fallback availability, fallback residency, and reason, then still
requests the fallback page when it is not resident. This keeps load failures
visible to tooling without letting a failed detail page remove renderable
fallback coverage.

Render preparation selects the desired sparse cluster cut first, then checks
page residency for every desired cluster. If a desired page is missing, failed,
evicting, or evicted, the selector records a page need and substitutes the best
resident fallback cluster or ancestor for that region. Current Vulkan upload
publishes page-local payload records and page-source labels for the backend.
The owning mesh buffer is retained only as a conservative embedded fallback and
must be visible in diagnostics when used, so performance gates can catch any
regression back to full high-detail owning-mesh residency.

## Runtime Counter Domains

Virtual geometry diagnostics are published per view so Scene View, Game View,
and later shadow/export views can be compared without sharing mutable state.
Current CPU-side counters mean:

- `sourceMeshlets`: cullable groups available in the asset metadata.
- `selectedMeshlets`: meshlets selected by the current sparse cluster draw
  ranges.
- `dispatchedMeshlets`: selected cluster meshlets submitted before GPU cull
  readback is available.
- `culledByFrustum`, `culledByCone`, `culledByHiZ`: separated cull domains.
- `residentPages`: pages available for rendering, including the fallback page.
- `requestedPages`: desired pages in requested, loading, pending IO, or pending
  upload state.
- `pendingIoPages` / `pendingUploadPages`: page work waiting for disk or
  protected GPU publication.
- `missingPages`: not-resident, failed, evicting, or evicted pages that require
  fallback rendering.
- `failedPages`, `evictingPages`, and `evictedPages`: subsets of
  `missingPages` with stronger failure, upload-retirement, or budget meaning.
- `pinnedFallbackPages`: resident fallback pages protected from eviction.
- `requestedBytes`, `pendingIoBytes`, `pendingUploadBytes`, `failedBytes`,
  `evictingBytes`, `evictedBytes`, and `pinnedFallbackBytes`: byte-level page
  pressure buckets used by resource tooling.
- `estimatedGeometryBytes`: metadata plus page byte estimates for the view.
- `budgetPressureBytes`: estimated bytes above the configured geometry budget.
- `gpuPacketRecordCount` and `gpuBatchDescriptorCount`: shader-readable packet
  and compatible-batch records produced for the view.
- `gpuCompactionDispatchCount`: planned compaction dispatch records for packet
  frame sets.
- `generatedSubmissionCount`, `validGeneratedSubmissionCount`, and
  `invalidGeneratedSubmissionCount`: generated submission records and their
  validation status.
- `gpuFallbackRecordCount` and `gpuFallbackReason`: fail-open records explaining
  why generated work was not trusted.
- `virtualGeometryPerformanceGates`: pass/fail/unsupported gate records for
  dense batching, CPU prep cost, compaction truth, generated submission
  validity, Hi-Z baseline preservation, and source availability.

Every counter snapshot carries a source label:

- `Prepared`: CPU-side render preparation estimate for the current frame.
- `GPU Readback`: completed GPU meshlet counter readback for the frame.
- `Delayed GPU Readback`: GPU counters are one frame behind, so the overlay is
  showing prepared estimates with the delay reason.
- `Unsupported` or `Unavailable`: no trustworthy counter value exists for the
  current view; UI rows must show unavailable text instead of authoritative
  zeroes.

Deterministic validation accepts a counter when it is within one meshlet or
within 1 percent of the expected fixture value, whichever is larger. The
tolerance is stored with the counter snapshot so editor overlays, automation,
and tests all report the same rule.

Inactive states use explicit reasons instead of zero-only counters. Common
examples are `Disabled By Settings`, `Missing Mesh Shader Route`, `Invalid Asset
Metadata`, `Missing Culling Metadata`, `Missing Residency Data`, and
`Unsupported View`. Active diagnostics can still report warnings for missing
pages, budget pressure, risky LOD transitions, and Hi-Z fail-open history.

The Game View visualizer overlay shows selected/dispatched/source meshlets,
page residency, cull reason totals, and a compact LOD histogram. The Resource
Residency window shows the latest per-view resident/requested/pending/missing
page counters, pinned fallback pages, pinned fallback bytes, budget pressure,
byte totals, and top pressure assets beside the existing RAM/VRAM streaming
tables. Budget
contributor data is sorted by reclaimable high-detail bytes so developers can
identify which virtual geometry assets create the most pressure.

## Multi-View, Shadow, And Export Correctness

US9 stores virtual geometry selection state per view. Scene View, Game View,
shadow passes, and exported runtime views each receive their own copied meshlet
range records in the render snapshot. A view can select a different LOD for the
same object without mutating another view's decision, and diagnostics are built
from those immutable per-view records.

Shadow passes own separate virtual geometry selection. They build shadow packet
ranges from the light frustum, cull light-frustum-rejected clusters, and publish
shadow-selected, shadow-dispatched, shadow-rejected, and fallback counters
without reusing camera visibility or the camera Hi-Z pyramid. Shadow culling
remains conservative for occlusion: there is no camera-Hi-Z shortcut in the
shadow path.

Export manifests include a `virtualGeometry` block with the export profile
switches, required mesh shader resources, diagnostics toggle, and optional page
budget. The block also serializes the adaptive quality controls used by export:
`geometryBudgetBytes`, `lodErrorScale`, `maxLodDrop`, `preloadBias`, and the required metadata list
`adaptiveTargets`, `fallbackCoverage`, `clusterHierarchy`, `pageLinks`, and
`coneSafety`.
Feature `029` export validation also requires a `coverage` object that names
`Tests/Fixtures/VirtualGeometry/hardening-fixtures.json` and includes the
`export` category in `requiredCategories`. That coverage block is diagnostic
metadata for QA; TEMF headers and validation flags remain the runtime source of
truth.
Mesh entries that reference `.temf` payloads can mark
`virtualGeometry.required = true`; package validation then checks that the
compiled mesh payload carries the current in-place TEMF virtual geometry header,
requested-target metadata, cluster hierarchy, page table, cluster-page links,
fallback coverage flag, and cone-safety metadata before the build is accepted.

Validation blocks packaging when required adaptive cook data, fallback coverage,
or page metadata is missing. Missing optional high-detail data can only become a
warning after the fallback path remains complete and renderable.

## Verification Checkpoints

Each phase or user story should finish with focused tests before moving to the
next phase. Any milestone that changes C++ or shaders must also compile the
affected targets and run the install target so the installed editor/runtime tree
contains the matching binaries and shader assets.

## Runtime Diagnostic Ownership

Game View `Stats` is intentionally compact. It should only report immediate
frame-health rows such as FPS, frame time, draw calls, triangles, and whether
detailed renderer diagnostics are compact or expanded elsewhere. It must not
grow back into a long virtual geometry packet/page/cache/Hi-Z list because that
list covers renderer internals rather than the rendered scene being inspected.

Detailed virtual geometry diagnostics belong in Resource Residency under the
Virtual Geometry section. That section owns meshlet selected/dispatched/culled
counts, Hi-Z culled counts, cluster packet counts, generated submission state,
GPU packet records, page source buckets, page payload bytes, request/retire
counts, and visible command cache counters. Keeping them grouped there makes it
clear that the numbers describe residency and renderer diagnostic state instead
of gameplay HUD data.

The Profiler owns timing interpretation. CPU and editor wall timing are valid
profiler streams when captured. Game View and Scene View GPU timing streams must
carry an explicit status; until the Vulkan backend records timestamp-query
samples for those views, they are reported as unavailable rather than as valid
zero-millisecond timings.

## Clean-Checkout Validation

Configure from a clean checkout with the checked-in Debug preset and bundled
dependencies:

```powershell
cmake --preset Debug
```

Build the affected runtime/editor targets from a Visual Studio developer
environment:

```powershell
cmd.exe /s /c '"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && cmake --build --preset Debug --target TerraEngine TerraEngine-Editor TerraEngineMeshCompiler TerraEngineModelPackageCompiler TerraEngineShaderCompiler TerraEngine-EngineTests TerraEngine-EditorTests'
```

Run the focused virtual geometry CTest entries for the GPU packet and
compaction path:

```powershell
ctest --test-dir out/build/Debug -R "Engine.VirtualGeometryBatching|Engine.VirtualGeometryCompaction|Engine.VirtualGeometryPerformanceGate|Engine.VirtualGeometryHiZ|Engine.VirtualGeometryMultiView|Engine.VirtualGeometryRenderer|Engine.Renderer|Engine.SystemScheduler" --output-on-failure -C Debug
ctest --test-dir out/build/Debug -R "Editor.VirtualGeometryDiagnostics|Editor.VirtualGeometryAutomation|Editor.Automation|Editor.Renderer" --output-on-failure -C Debug
```

Then run the broader renderer and virtual geometry regression pass:

```powershell
ctest --test-dir out/build/Debug -R "Engine.VirtualGeometry|Editor.VirtualGeometry|Engine.Renderer|Engine.Resource|Editor.AssetPipeline.VirtualGeometry|Editor.Automation|Editor.Export" --output-on-failure -C Debug
```

The named CTest entries cover:

- `Engine.VirtualGeometryRenderer`: LOD selection, debug counters, Hi-Z gates,
  shader layout, quality settings, worker-preparation declarations, and dense
  scene stress.
- `Engine.VirtualGeometryResource`: TEMF loading, page residency, streaming
  needs, eviction ordering, and budget-pressure churn.
- `Engine.VirtualGeometryBatching`, `Engine.VirtualGeometryCompaction`,
  `Engine.VirtualGeometryPerformanceGate`, `Engine.VirtualGeometryShadow`, and
  `Engine.VirtualGeometryCulling`: bounded batch submission, compacted packet
  work, performance gate records, light-view shadow selection, and safe cone
  eligibility.
- `Editor.VirtualGeometryAssetPipeline`: clean-checkout synthetic cooking,
  inspector rebuild guidance, package import metadata, and rejected sidecar
  reads.
- `Editor.VirtualGeometryAutomation`: import/rebuild/inspect automation command
  contracts and invalid-request atomicity.
- `Editor.VirtualGeometryExport`: export validation, required mesh shader
  resources, and visualizer mode inventory.

Finish with install verification:

```powershell
cmd.exe /s /c '"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && cmake --build --preset Debug --target install'
```

Expected installed files include `Editor/Shaders/Default.task.tspf`,
`Editor/Shaders/Default.mesh.tspf`,
`Editor/Shaders/PostProcess/HiZGenerate.comp.spv`, source
`Docs/Renderer/VirtualGeometry.md` installed as `Docs/VirtualGeometry.md`,
`Docs/TerraEngineMeshFile.md`, and
`Tests/Fixtures/VirtualGeometry/README.md`.

## Unsupported Content And Validation Scenes

Current virtual geometry content support is intentionally narrow:

- Static opaque mesh geometry is supported. Animated, skinned, camera, light,
  and constraint data is reported as package diagnostics and ignored for static
  output.
- Small meshes may report `Skipped`; this is a valid one-detail fallback, not a
  failed cook.
- Meshes without meshlet bounds, stale virtual geometry headers, unreadable
  roots, or mismatched payload generation report `NeedsRebuild` and stay on the
  conservative fallback path.
- Mesh-shader support is required for active virtual geometry. Missing backend
  support reports `Missing Mesh Shader Route` and keeps visible fallback
  rendering.
- Camera-owned Hi-Z occlusion is resource-gated only. It samples as soon as a
  current-view pyramid exists; self-occlusion, near-plane, thin-geometry, view,
  target, projection, resize, camera-cut, and depth-scene metadata remain
  diagnostics only during interactive editor rendering.
- The Hi-Z history also stores a depth-scene signature built from the visible
  draw list, transforms, materials, and selected virtual-geometry cluster
  ranges. Moving or editing an occluder reports the diagnostic change while
  keeping Hi-Z active; individual meshlets still fail open when the sampled
  pyramid cannot prove occlusion.
- The moving-wall Hi-Z fixture must cover fully blocked, partially visible,
  unblocked, blocker-restored, resized-view, camera-cut, and near-plane cases.
  Fully blocked meshlets may cull; partially visible or uncertain meshlets must
  remain visible.

Validation scenes should include a deterministic grid/high-curvature static
mesh, a small simplification-skip mesh, a multi-material or material-band mesh,
thin/self-occluding Hi-Z cases, a dense camera-motion stress scene, and a
streaming-pressure scene whose high-detail pages exceed a small geometry
budget while page `0` fallback remains resident.

Feature `029` expands that fixture set with the manifest in
`Tests/Fixtures/VirtualGeometry/hardening-fixtures.json`. The committed manifest
describes required deterministic categories, importer/export coverage tags,
package formats, expected diagnostics, and whether a fixture is clean-checkout
required or optional manual stress coverage. The clean-checkout categories are
tiny, dense, thin, curved, hard-surface, multi-material package,
streaming-pressure, multi-view, and export-runtime metadata. Optional manual
stress should use local dense scan, hard-surface multi-material, and thin
occlusion-heavy assets while keeping those large files out of the repository.
