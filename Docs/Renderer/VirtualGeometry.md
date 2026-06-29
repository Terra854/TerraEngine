# TerraEngine Virtual Geometry

This guide tracks the staged implementation of TerraEngine virtual geometry.
The feature targets static opaque meshes on the mesh shader path and deliberately
keeps the active TEMF version at `1` while allowing the payload contract to
break during engine development.

## Implementation Stages

1. Cook virtual mesh assets with automatic whole-mesh LOD chains, meshlets,
   material section metadata, conservative bounds, and cook diagnostics.
2. Validate LOD correctness so generated detail levels preserve material
   identity, conservative bounds, and acceptable geometric error.
3. Render selected LOD ranges through the existing task/mesh shader path with
   fail-open behavior for missing metadata.
4. Repair Hi-Z usage by keeping depth pyramid generation warm while occlusion
   remains gated by view-history validation and zero-false-occlusion tests.
5. Expose editor inspection and a Virtual Geometry Visualizer with LOD,
   meshlet/triangle identity, bounds, culled, Hi-Z, residency, and error modes.
6. Extend model package import, page residency, automation, export validation,
   and stress tests after the cooked asset and render contracts are stable.

## Asset Rebuild Expectations

The current plan breaks older compiled mesh payloads in place. Users must
rebuild affected static mesh and model package outputs from source after the
virtual geometry payload lands. No legacy compatibility loader is planned while
the engine format is still unstable.

### MVP Rebuild Workflow

1. Rebuild each source mesh or model package through the editor asset import
   path or `TerraEngineMeshCompiler`.
2. Keep meshlet bounds enabled for virtual geometry cooks. The MVP maps each
   cullable group to a meshlet bounds record, so bounds are required for useful
   renderer diagnostics.
3. Treat `virtualGeometryStatus=Cooked` as a generated LOD chain and
   `virtualGeometryStatus=Skipped` as a valid one-detail fallback for small
   meshes that do not benefit from simplification.
4. Rebuild immediately when the runtime logs stale or incomplete virtual
   geometry header metadata. TEMF remains version `1`, but the in-place payload
   generation is intentionally allowed to change during development.

Expected cook diagnostics:

- `virtualGeometryDetailLevels`: number of selectable whole-mesh LODs.
- `virtualGeometryMeshlets`: resident meshlet/cullable-group count across all
  emitted LODs.
- `virtualGeometrySourceTriangles`: original source triangle count before
  simplification.
- `virtualGeometryFallbackAvailable`: whether a renderable fallback detail is
  present.
- `virtualGeometryWarnings`: count of warnings also stored in the JSON report
  and TEMF diagnostic table.

## Visual Correctness Rules

Generated LODs must stay conservative until a later story adds richer
section-aware import metadata:

- Each detail level owns at least one non-empty material section range, and the
  ranges must not overlap resident meshlets.
- The current MVP compiler emits one default material section per LOD; package
  importers can expand this once source primitive/material slots are threaded
  through the raw mesh handoff.
- Detail bounds must remain conservative. The MVP stores source-position bounds
  on every detail so renderer-side LOD choice cannot shrink object culling
  below the authored mesh extent.
- Geometric error must be finite, monotonic, and no greater than `0.05` for an
  accepted simplification candidate. Candidates outside that tolerance are
  skipped and the previous safe detail remains the fallback.
- Runtime diagnostics should flag aggressive `maxLodDrop` and scaled error
  combinations because those settings can visibly pop during fast camera motion.

## Mesh Shader Rendering Controls

The first renderer path is intentionally mesh-shader-only. A cooked virtual mesh
is still submitted through the existing task and mesh shaders; render
preparation selects one whole-mesh LOD range per visible object and passes the
selected meshlet offset/count through push constants.

- `virtualGeometry.enabled` must be true before render preparation selects lower
  detail levels as active virtual geometry. Disabled virtual geometry still
  draws through a fail-open path and chooses the best resident fallback detail
  instead of trusting a non-resident high-detail page.
- Task shader culling must be available and enabled for the object. If task
  culling is disabled, unsupported, or missing required meshlet bounds, virtual
  geometry fails open to LOD0.
- `lodErrorScale` tightens or relaxes geometric-error acceptance after cooking.
  A selected LOD whose scaled error is above the current tolerance backs down to
  a safer detail level.
- `maxLodDrop` limits how many whole-mesh LODs a single selection step may skip.
  Use small values while tuning camera-motion popping.
- Render-preparation cache signatures include the selected LOD index and
  meshlet range, so camera-distance changes cannot reuse stale immutable draw
  packets.
- Fail-open diagnostics record the mesh identifier and the reason visibility was
  preserved through fallback rendering. Current reasons include disabled quality
  settings, disabled task culling, invalid bounds, and missing detail metadata.

## Quality Controls And Failure Modes

Virtual geometry quality lives in the project/runtime `RenderSettings.json`
under `settings.virtualGeometry`. The current fields are:

- `enabled`: toggles active virtual geometry selection. Off means visible,
  fail-open resident fallback rendering.
- `lodErrorScale`: clamps to `[0.05, 64.0]` and scales cooked geometric error
  before a lower detail level is accepted.
- `maxLodDrop`: limits how far selection may move away from the highest detail.
- `hiZOcclusionEnabled`: allows meshlet Hi-Z culling only after the view-owned
  history gates pass.
- `hiZSelfOcclusionGuardEnabled`: requires the per-draw guard bit before Hi-Z
  can reject meshlets.
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

Hi-Z mip generation stays active after the scene depth pass so the renderer can
warm the pyramid without trusting it for visibility. Meshlet occlusion only
activates when every gate below passes:

- The previous pyramid belongs to the same view/output, target size, and
  view-projection signature as the current frame.
- Virtual geometry and `hiZOcclusionEnabled` are both enabled in the runtime
  quality profile.
- The per-draw self-occlusion guard bit is present. Missing guard data fails
  open so thin geometry and self-overlapping meshlets remain visible.
- Editor/offscreen and first-frame paths are treated as unsupported or
  first-frame history until they publish their own matching history.

Invalid reasons are tracked explicitly: first frame, missing pyramid, resize,
projection change, view mismatch, quality disabled, unsupported view, and
self-occlusion risk. Task shaders only sample Hi-Z when the frame culling
payload says the history is validated and the draw packet says the guard
passed. Otherwise they keep the meshlet visible and record no Hi-Z cull.

## Scheduler And Mutation Boundaries

Virtual geometry rendering must use immutable render snapshots and prepared
packets. Worker-safe preparation may read asset data, view data, residency
snapshots, and quality settings, but it must not mutate live scene, object,
hierarchy, or component storage. Asset registration, residency publication, GPU
submission, and export packaging remain protected boundaries with explicit
diagnostics.

## Debug Visualization Inventory

The editor visualizer should expose these modes as the implementation matures:

- `None`: normal material rendering.
- `LOD`: false-color selected detail level.
- `MeshletId`: stable false-color meshlet, cullable group, or triangle identity.
- `Bounds`: cullable-unit or group bounds.
- `Culled`: rejected and fail-open units.
- `HiZ`: Hi-Z accepted, rejected, disabled, and invalid-history states.
- `Residency`: resident, requested, missing, failed, and fallback pages.
- `Error`: geometric error and quality pressure.

## Editor Inspection And Visualizer Workflow

Mesh assets expose a read-only Virtual Geometry inspector section when their
compiled TEMF can be read. The section reports cook status, payload generation,
detail count, source triangle count, resident meshlets, and rebuild guidance for
missing or stale payload metadata.

Game View owns the first visualizer control. The `VG` mode selector writes the
runtime virtual geometry debug mode into `RenderSystem` quality settings, and
the default fragment shader produces false-color output for LOD, meshlet
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

`asset.virtualGeometry.rebuild` accepts `target`, `force`,
`includePackageOutputs`, and `diagnosticsLevel`. `target` may be an asset path,
asset UUID, or `all`. The response reports `completed`, `partial`, or `failed`
status plus `rebuiltAssets`, `skippedAssets`, `failedAssets`, `warnings`, and
`durationMs`.

`asset.virtualGeometry.inspect` accepts `asset`, `includeRuntimeState`, and an
optional `viewType`. The response includes cook metadata, meshlet counts,
runtime virtual geometry view snapshots when available, and residency totals.

Successful automation rebuild/import results become visible only after the
AssetDatabase `FinishImport` registration boundary. Failed requests do not mark
generated mesh outputs usable; failed package imports, failed virtual geometry
metadata, and `NeedsRebuild` generated mesh records are skipped during generated
resource registration.

## Geometry Page Residency

US7 introduces MVP page metadata and deterministic residency planning while
keeping LOD0 visible as the fallback detail. The cooker emits one page record
per detail level for now. Each record names the covered LOD range, cullable
group range, byte estimate, priority, and fallback page. LOD0 is its own
fallback page and is treated as resident when a mesh is first mirrored into
runtime virtual geometry metadata.

The Resource layer owns page state transitions:

- `NotResident` -> `Requested` when a visible view needs a page.
- `Requested` -> `Loading` while IO or upload work is pending.
- `Loading` -> `Resident` only at the protected publication boundary.
- `Requested` or `Loading` -> `Failed` when a page cannot be loaded.
- `Resident` -> `Evicting` -> `Evicted` for non-fallback pages under budget
  pressure.

Fallback pages are preserved by the page eviction planner even when the geometry
budget is exceeded. Higher-detail pages are ordered for eviction by lower
priority, older use, and larger reclaimable byte size. Streaming need planning
is worker-safe: it turns visible virtual geometry intent into ordinary
`StreamingNeed` records for desired and fallback pages, but ResourceManager cache
mutation, GPU upload scheduling, and residency publication remain protected
boundaries.

Render preparation selects the desired whole-mesh LOD first, then checks page
residency. If the desired page is missing, failed, or evicted, the draw fails
open to the resident fallback page and records a fallback reason. Diagnostics
count resident, requested/loading, and missing/failed pages and include page byte
estimates in virtual geometry memory totals.

## Runtime Counter Domains

Virtual geometry diagnostics are published per view so Scene View, Game View,
and later shadow/export views can be compared without sharing mutable state.
Current CPU-side counters mean:

- `sourceMeshlets`: cullable groups available in the asset metadata.
- `selectedMeshlets`: meshlets selected by the current whole-mesh LOD choice.
- `dispatchedMeshlets`: selected meshlets submitted before GPU cull readback is
  available.
- `culledByFrustum`, `culledByCone`, `culledByHiZ`: separated cull domains.
- `residentPages`: pages available for rendering, including the fallback page.
- `requestedPages`: desired pages in requested or loading state.
- `missingPages`: not-resident, failed, evicting, or evicted pages that require
  fallback rendering.
- `failedPages` and `evictedPages`: subsets of `missingPages` with stronger
  failure/budget meaning.
- `estimatedGeometryBytes`: metadata plus page byte estimates for the view.
- `budgetPressureBytes`: estimated bytes above the configured geometry budget.

Inactive states use explicit reasons instead of zero-only counters. Common
examples are `Disabled By Settings`, `Missing Mesh Shader Route`, `Invalid Asset
Metadata`, `Missing Culling Metadata`, `Missing Residency Data`, and
`Unsupported View`. Active diagnostics can still report warnings for missing
pages, budget pressure, risky LOD transitions, and Hi-Z fail-open history.

The Game View visualizer overlay shows selected/dispatched/source meshlets,
page residency, cull reason totals, and a compact LOD histogram. The Resource
Residency window shows the latest per-view page counters and byte totals beside
the existing RAM/VRAM streaming tables. Budget contributor data is sorted by
reclaimable high-detail bytes so developers can identify which virtual geometry
assets create the most pressure.

## Multi-View, Shadow, And Export Correctness

US9 stores virtual geometry selection state per view. Scene View, Game View,
shadow passes, and exported runtime views each receive their own copied meshlet
range records in the render snapshot. A view can select a different LOD for the
same object without mutating another view's decision, and diagnostics are built
from those immutable per-view records.

Shadow passes use conservative visibility. They select the highest resident
fallback detail, disable camera-owned Hi-Z occlusion, clear the Hi-Z
self-occlusion guard bit before Vulkan records shadow draws, and keep task
culling disabled for the shadow depth pass. This preserves silhouettes until a
light-space virtual geometry culling path exists.

Export manifests include a `virtualGeometry` block with the export profile
switches, required mesh shader resources, diagnostics toggle, and optional page
budget. Mesh entries that reference `.temf` payloads can mark
`virtualGeometry.required = true`; package validation then checks that the
compiled mesh payload carries the current in-place TEMF virtual geometry header
and generation before the build is accepted.

## Verification Checkpoints

Each phase or user story should finish with focused tests before moving to the
next phase. Any milestone that changes C++ or shaders must also compile the
affected targets and run the install target so the installed editor/runtime tree
contains the matching binaries and shader assets.

## Clean-Checkout Validation

Configure from a clean checkout with the checked-in Debug preset and bundled
dependencies:

```powershell
cmake --preset Debug
```

Build the affected runtime/editor targets from a Visual Studio developer
environment:

```powershell
cmd /c "C:\PROGRA~1\MIB055~1\18\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake --build --preset Debug --target TerraEngine TerraEngine-Editor TerraEngine-EngineTests TerraEngine-EditorTests"
```

Run the focused virtual geometry CTest entries:

```powershell
ctest --test-dir out/build/Debug -R "Engine.VirtualGeometry(Renderer|Resource|Foundation)|Editor.VirtualGeometry(AssetPipeline|Automation|Export|Foundation)" --output-on-failure -C Debug
```

The named CTest entries cover:

- `Engine.VirtualGeometryRenderer`: LOD selection, debug counters, Hi-Z gates,
  shader layout, quality settings, worker-preparation declarations, and dense
  scene stress.
- `Engine.VirtualGeometryResource`: TEMF loading, page residency, streaming
  needs, eviction ordering, and budget-pressure churn.
- `Editor.VirtualGeometryAssetPipeline`: clean-checkout synthetic cooking,
  inspector rebuild guidance, package import metadata, and rejected sidecar
  reads.
- `Editor.VirtualGeometryAutomation`: import/rebuild/inspect automation command
  contracts and invalid-request atomicity.
- `Editor.VirtualGeometryExport`: export validation, required mesh shader
  resources, and visualizer mode inventory.

Finish with install verification:

```powershell
cmd /c "C:\PROGRA~1\MIB055~1\18\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake --build --preset Debug --target install"
```

Expected installed files include `Editor/Shaders/Default.task.tspf`,
`Editor/Shaders/Default.mesh.tspf`,
`Editor/Shaders/PostProcess/HiZGenerate.comp.spv`, `Docs/VirtualGeometry.md`,
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
- Camera-owned Hi-Z occlusion remains fail-open unless the history belongs to
  the same view, target size, and projection signature and the self-occlusion
  guard is present.

Validation scenes should include a deterministic grid/high-curvature static
mesh, a small simplification-skip mesh, a multi-material or material-band mesh,
thin/self-occluding Hi-Z cases, a dense camera-motion stress scene, and a
streaming-pressure scene whose high-detail pages exceed a small geometry
budget while page `0` fallback remains resident.
