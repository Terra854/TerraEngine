# Terrain Authoring

This document is the user and validation entry point for the massive terrain
authoring pipeline. Terrain v1 supports translation-only instances, exact
signed world coordinates, streamed authored areas, detached previews, typed
atomic edits, surface layers, holes, derived rendering, and heightfield
collision.

## Build and install

From a clean checkout:

```powershell
cmake --fresh --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine TerraEngine-Editor TerraEngineTerrainCompiler TerraEngineTerrainBenchmark TerraEngine-EngineTests TerraEngine-EditorTests
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R "^(Engine|Editor)\\.Terrain|TransactionalMutation|SystemScheduler|VirtualGeometry|Physics" --output-on-failure
cmake --build --preset RelWithDebInfo --target install
```

Validate the installed tree under `out/build/RelWithDebInfo/Build`. Runtime
terrain content must resolve through loose or TEPAK `ContentMount` paths; do
not validate against arbitrary source-tree copies.

## Validation tools

Installed terrain tools live in `Tests/Tools/Terrain`. Use the workload records
in `Tests/Tools/Terrain/workloads.json` and the fixture identities in
`Tests/Fixtures/Terrain/manifest.json` for reproducible seam, collision,
automation-parity, large-world, and soak runs.

Generate the sparse large-world fixture and deterministic route evidence from
the installed tools:

```powershell
$installRoot = (Resolve-Path 'out/build/RelWithDebInfo/Build').Path
$terrainTools = Join-Path $installRoot 'Tests/Tools/Terrain'
& (Join-Path $terrainTools 'TerraEngineTerrainFixtureGenerator.exe') `
    --output (Join-Path $terrainTools 'terrain-256km.json')
& (Join-Path $terrainTools 'TerraEngineTerrainBenchmark.exe') `
    --fixture (Join-Path $terrainTools 'terrain-256km.json') `
    --route terrain-1000km-edit-route `
    --output (Join-Path $terrainTools 'terrain-benchmark-result.json')
```

The fixture represents the full 256 x 256 km extent with sparse materialized
areas and exact signed lattice coordinates; it must not be interpreted as a
fully resident sample dump.

For surface validation, run the `terrain-surface-layers` workload after creating
16 stable named layers. Runtime terrain graphics ship as a task/mesh/fragment
shader set; the installed audit rejects retired vertex-stage terrain artifacts.
Verify four-contributor normalization, fifth-layer
preview/displacement, missing-material fallback, control-page residency and
generation coupling, boundary seams, and identical canonical root/area hashes
after save/reload. The installed runtime and editor terrain shader bundles must
be present before this evidence is considered available.

Collision validation uses the `terrain-jolt-heightfield` workload and the
`Engine.Terrain.Collision` / `Editor.Terrain.CollisionAlignment` filters. It
must record Jolt heightfield build and replacement latency, allocation bytes,
sample counts 257/513/1025, hole/material behavior, and query alignment. Keep
the provider-neutral collision recipe and `TerrainCollisionSystem` headers in
the installed `include/Physics/Terrain` tree; live Jolt types remain private.

Procedural validation uses `Engine.Terrain.Procedural` and
`Editor.Terrain.Procedural`. It covers world/height/slope/mask/layer selections,
strict bounds, deterministic remap/terrace/noise/stamp and erosion operations,
cooperative cancellation, stale job rejection, and bounded workload estimates.
Spline grading additionally verifies road/river width, shoulder, bank, depth,
falloff, cross-area continuity, deterministic output, and one grouped history
entry. Every operation must publish through the same typed transaction path and
must leave authored state unchanged when cancelled.

## Workflow coverage

The acceptance workflow covers flat/PNG16/RAW16 creation, sculpt previews and
commits, four-area seams, 16-layer surface painting, reversible holes,
heightfield collision replacement, procedural jobs, save/reopen recovery, and
visible-tool/automation parity.

## Out of scope for v1

Voxels, caves and overhangs, runtime deformation, collaborative editing,
vegetation/biomes, navigation generation, water simulation, and a general
material graph are intentionally out of scope.

## Evidence requirements

Record command lines, backend/capability identity, artifact hashes, raw timing
counters, residency high-water marks, and explicit unavailable states. A
missing executable, shader, fixture, capture, or supported-hardware result is
not a passing result.

## History and save/recovery workflow

Run the installed transaction stress executable before collecting interactive
evidence. It prepares candidates concurrently, injects deterministic failures,
and verifies that every attempt is accounted for as one complete commit or one
rejection:

```powershell
$installRoot = (Resolve-Path 'out/build/RelWithDebInfo/Build').Path
$stress = Join-Path $installRoot 'Tests/Tools/Terrain/TerraEngineTerrainTransactionStress.exe'
& $stress --workers 8 --iterations 25000 --seed 0x7e11a9
```

For editor history, apply a mixed sequence of sculpt and surface operations,
undo it to the initial canonical root/area hashes, then redo it to the final
hashes. Preset create/duplicate/update/delete operations must not add terrain
history entries or dirty the terrain asset.

For save recovery, use `Tests/Fixtures/Terrain/SaveRecovery/manifest.json` and inject a
failure at each listed stage: blob write, directory write, root-manifest write,
`CURRENT` replacement, and flush. After reopening, verify that the predecessor
root remains current, no partial blob is referenced, and the diagnostic names
the failed stage and affected location. A successful save replaces `CURRENT`
only after all immutable blobs, directories, and the root manifest are durable.

## Artist workflow and ownership

Create terrain from a flat area or a lossless PNG16/RAW16 source, choose the
asset/instance ownership explicitly, then work through detached previews and
bounded commits. Sculpt, layer, hole, and procedural edits share one typed
transaction/history path. Shared terrain assets require an explicit choice of
shared edit or instance-local ownership; automation cannot silently clone or
mutate a shared root.

Layers are named and generation-tracked, with four local contributors per
control page. Holes use the canonical cell mask shared by rendering and
heightfield collision. Heightfield collision supports complete replacement and
last-known-good fallback. Creation, scene loading, Play Mode world creation,
committed height/hole edits, and packaged runtime startup publish collision
automatically; authors do not add a separate collider component. Voxels, caves,
overhangs, runtime deformation, vegetation, navigation, and water are outside
terrain v1.

To texture and paint a terrain in the editor:

1. Import the source textures and create a Material asset for each desired
	surface, such as grass, rock, or soil.
2. Select the terrain object, open **Terrain Tools**, choose **Layers**, and add
	or select a layer. Drag the compiled Material asset onto **Drop Material Here**.
3. Choose **Paint**, select the stable layer, then set opacity, brush size, and
	spacing. Drag directly over the terrain in Scene View; releasing the pointer
	commits the complete stroke as one undoable transaction.
4. Use Fill, Replace, or Normalize for whole-area changes, then save the terrain
	asset through the existing authoring workflow.

The live editor proxy blends up to four stable contributors at each control
sample and resolves those contributors against up to 16 enabled layer materials.
The weight and stable-ID control textures publish together at a protected
boundary. A missing authored material is intentionally rendered magenta so the
broken layer is visible instead of silently changing the terrain's appearance.

## Clean-checkout installed contract

Configure, build, install, and audit only the installed tree:

```powershell
cmake --fresh --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine TerraEngine-Editor TerraEngineTerrainCompiler TerraEngineTerrainFixtureGenerator TerraEngineTerrainBenchmark TerraEngine-EngineTests TerraEngine-EditorTests
cmake --build --preset RelWithDebInfo --target install
$install = (Resolve-Path 'out/build/RelWithDebInfo/Build').Path
& (Join-Path $install 'Tests/Tools/Terrain/VerifyInstalledTerrainContract.ps1') -InstallRoot $install -DryRun
```

The audit checks executable/schema/fixture/workload/help/shader presence,
records SHA-256 hashes, and reports ContentMount/TEPAK discovery separately.
Missing artifacts or unavailable backend capabilities are not passing evidence;
raw counters and a fixed workstation identity are required for performance
claims. Terrain content must resolve through the installed loose or TEPAK
`ContentMount`, never through source or build-tree paths.

## Validation status

Terrain remains `ValidatedCandidate`. Automated correctness, transaction,
recovery, large-coordinate, and installed-editor invariants are exercised.
Five comparable installed benchmark captures record preview, cook, upload,
publication, and collision P50/P95 values, and the installed 256-km/1,000-km
route completed 139,689 iterations over 1,800.009 seconds against a hashed
97-entry TEPAK. The production designation is intentionally withheld because
streaming/decode/IO, save/recovery, and residency high-water counters remain
unavailable. User-study criteria, live render seam captures, live collision
latency, full visible/automation workflow parity, and launched TEPAK runtime
evidence are also unavailable and therefore non-passing. The current
machine-readable comparison is written to
`out/evidence/terrain/acceptance-summary.json`; unavailable entries must never
be converted into synthetic zeroes or passing results.

## Budgets, diagnostics, and recovery

Interactive work declares maximum areas, samples, prepared bytes, and workload
class. Large imports, erosion, spline operations, rebuilds, saves, and
validation may use durable cancellable jobs. A stale generation, conflict,
budget violation, missing area, or failed derived build rejects atomically and
retains the last-known-good published state. Diagnostics identify location,
severity, impact, action, source, generation, and stage percentiles.
