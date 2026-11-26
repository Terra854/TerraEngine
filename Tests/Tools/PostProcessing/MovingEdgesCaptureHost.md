# Native moving-edge capture host

`PostProcessMovingEdgesCapture` collects actual Vulkan images from a normally
imported and cooked generic fixture scene. It uses `Engine::Update`, imported
animation, protected renderer publications, the ordinary native draw path, and
explicit Raw/Processed backend readbacks. It never renders UI quads as AA input,
substitutes a software rasterizer, or renders additional simulation frames while
waiting for readback.

The default `--route exploratory` produces **exploratory direct-mesh evidence**.
The frozen `moving-edges-v1` controls require indexed raster. Exploratory
`collection.json` therefore has
`evidenceKind: NativeExploratoryDirectMeshReadback`, `controlsVerified: false`,
`frozenIndexedRouteSatisfied: false`, and `qualificationComplete: false`.
It cannot be assembled into a manifest accepted by the scorer. An actual
exploratory run has completed all 240 genuine Raw/Processed images, establishing
collection progress without establishing the indexed-route or image-quality gate.
Use `--route indexed` only with an installed engine that advertises and prepares
setting ID91 (`geometry.rasterMode=Indexed`). Every accepted phase must expose
that effective value and exactly four actual recorded native indexed draws at
the requested Scene extent. Unsupported or rejected indexed requests fail;
they never fall back to exploratory collection.
The separate fixed-phase seeks also do not certify temporal previous-state
continuity. Neither limitation can be removed by relabeling the output JSON.
Even successful indexed collection requires separate image scoring and integrated
acceptance; `qualificationComplete` remains false.

## Build dependencies

The parent post-processing tools CMake file includes `MovingEdgesCapture.cmake`
after integrating the native observer. The target depends on the existing
`TerraEngine-Engine`, bundled capture decoder, bundled GLFW, and Windows SDK
`bcrypt` for SHA-256. No package manager or downloaded dependency is used.
Engine shaders stay in the installed engine shader tree.

The native renderer must expose the read-only exact-frame observer
`VulkanContext::ReadAcceptedRenderFramePublication(expectedFrameId)`. A binary
built before that observer fails explicitly; it cannot infer accepted camera or
draw state from statistics alone.

The orchestrator's configure/build/install sequence is:

```powershell
cmake --preset Testing -DBUILD_TESTING=ON
cmake --build --preset Testing --target install --parallel 2
```

## Required cooked project

Use a fresh ordinary editor project containing the materialized frozen
`MovingEdgesContinuous.gltf`. Import/cook it through the installed editor's normal
asset import, instantiate its imported prefab once, and save the scene. The
project's `Assets/**/*.import.json` records must identify their actual payloads
under `Build/Assets/`; the native host registers those existing UUID/path pairs
from the nested `animationManifest.subAssets` publication (or a standalone root
`subAssets` publication) because it does not start the editor AssetDatabase.
Generated mesh/material summaries are checked against that authoritative table.

The scene must have exactly these four mesh objects with their original names:

- `slanted-panel`
- `thin-rigid-foreground`
- `deforming-ribbon`
- `frozen-background`

The first three use the frozen geometry from
`Tests/Fixtures/PostProcessing/MovingEdgesPoses.json`. The background is an
ordinary opaque double-sided emissive quad covering `[-1024,1024]` in X/Y at
Z=-8 with linear radiance `(0.04,0.04,0.04)`. All materials use zero RGB reflected
base color, opaque alpha, no textures, and the exact frozen emissive factors.
Each selected full-detail meshlet must retain four vertices and two triangles.
Additional cooked LODs are allowed but never selected: the common mesh LOD scale
is 64, and each actual accepted draw must prove selected LOD0 with one full-detail
meshlet. This common source profile is pinned across all reference/candidate runs.
The ribbon must
retain its actual imported two-joint skin and 184-key animation; a static quad
with changing vertex uploads is rejected.

The host selects the normal CPU-authoritative animation policy to inspect the
real prepared palette. It holds automatic playback, queues a typed native Seek
for each exact phase, and executes one fixed `1/60` Engine update. This preserves
actual current geometry but is deliberately not temporal-motion qualification.
It verifies the resulting deformed positions against the frozen positions within
the production cooked UNORM16 step, recording actual float32 vertices, palette,
packed-buffer hashes, deviation, and quantization step. It never overwrites those
native values with the frozen oracle.

Use the same cooked project and scene for every reference/candidate process.
Do not reimport, move objects, change materials, or resize the logical projection
between modes. Reference4x changes only sampling extent to 2560Ã—1440; all other
runs use 640Ã—360. Projection always spans 640Ã—360 world units. Source file hashes,
cooked payload hashes, effective configuration, actual view lifetime, accepted
draw matrices, and original native frame IDs remain in each collection.

## Commands

Validate frozen inputs without initializing a window or GPU:

```powershell
& ./Build/Tests/Tools/PostProcessing/PostProcessMovingEdgesCapture.exe `
  --validate-inputs ./Tests/Fixtures/PostProcessing/MovingEdgesPoses.json

# Registers and hashes real cooked import payloads without opening a window/GPU.
& ./Build/Tests/Tools/PostProcessing/PostProcessMovingEdgesCapture.exe `
  --validate-cooked ./out/validation/moving-edge-project
```

After the normal import/cook/instantiate/save prerequisite, run each mode in a
fresh process and a fresh output directory. Adjust the installed Build and
disposable project locations to the actual local install:

```powershell
$install = (Resolve-Path ./out/build/Testing/Build).Path
$project = (Resolve-Path ./out/validation/moving-edge-project).Path
$fixture = (Resolve-Path ./Tests/Fixtures/PostProcessing/MovingEdgesPoses.json).Path
$hostExe = Join-Path $install Tests/Tools/PostProcessing/PostProcessMovingEdgesCapture.exe
foreach ($mode in @('Reference4x','OffNative','FXAA','SMAA','CMAA2')) {
  & $hostExe --install-root $install --project-root $project `
    --scene Assets/Scenes/MovingEdges.tescene --fixture $fixture `
    --route indexed --mode $mode --output-root (Join-Path ./out/validation/moving-edge-images $mode)
  if ($LASTEXITCODE -ne 0) { throw "Native capture failed for $mode" }
}

# Assembly preserves original native IDs and validates source profile, poses,
# completed indexed-draw proofs, exact effective extents, and image byte hashes.
& $hostExe --assemble-spatial ./out/validation/moving-edge-images
if ($LASTEXITCODE -ne 0) { throw 'Indexed capture assembly failed' }
pwsh -NoProfile -Command '& ./Tests/Tools/PostProcessing/TestPostProcessImageQuality.ps1 -Manifest ./out/validation/moving-edge-images/manifest.json -Output ./out/validation/moving-edge-score.json -Modes @("FXAA","SMAA","CMAA2")'
```

For diagnostics on an engine without indexed support, use `--route exploratory`
and a different fresh output directory. The assembler deliberately rejects those
collections. No native AA image-quality pass is implied by completed PNG files.

Each completed collection contains 64 ordered warmup receipts and 120 measured
Raw/Processed PNG pairs for phases 64â€“183. Setup frames used for real binding and
settings preparation are recorded separately and use a bounded wall deadline.
Scene extent/grid/camera demand is reissued every frame because the normal
renderer clears viewport ingress after completion. The camera view matrix is
reference-local; frozen canonical camera displacement is supplied separately.
The stopped animator remains at sample index zero during setup. Each readback requires the exact
accepted frame, source allocation/session, stage, extent, opaque alpha, and one
linear-to-sRGB conversion. Native lifetime IDs remain separate from logical
fixture generation and physical image-allocation generation.

Failure writes partial `collection.json` when possible and returns a nonzero
process exit code. Existing output directories and files are rejected. A zero
exit code means the requested collection or assembly completed; it does not mean
the AA quality threshold passed. The orchestrator owns actual
GPU execution, image scoring after the route prerequisite, editor/export parity,
and integrated acceptance.

## Independent TAA cut and resize reference lanes

These options are separate from the spatial assembler and the default 184-frame
continuous run. Use an engine containing command-side `temporalJitterPhase`
observations and accepted cut detection. Missing TAA/indexed capability, rejected
settings, wrong frozen geometry, or missing reset evidence fails the collection.

```powershell
foreach ($sequence in @('cut','resize')) {
  foreach ($role in @('reset','fresh')) {
    & $hostExe --install-root $install --project-root $project `
      --scene Assets/Scenes/MovingEdges.tescene --fixture $fixture `
      --route indexed --mode TAA --temporal-sequence $sequence `
      --temporal-role $role --output-root (Join-Path ./out/validation/taa-reset "$sequence-$role")
    if ($LASTEXITCODE -ne 0) { throw "TAA $sequence/$role collection failed" }
  }
}
```

Each command starts a separate process and must use a fresh output directory.
`reset` retains active indexed TAA through 64 frozen continuous warmup frames,
then records the four exact saved poses in `separateSequences.cut` or `.resize`.
The cut jumps to the frozen canonical camera position; resize changes the real
Scene target from 640x360 to 800x450. It never substitutes a requested snapshot
for native recording. Resize admission skips are retained in
`temporalResetSequence.skippedBoundaryFrames`; the first exact accepted target
must itself satisfy the reset checks.

`fresh` creates an independent renderer/view/target in its own process at the
same first saved pose and exact target extent. Its first captured frame is the
actual settings activation publication, not a subsequent redraw. If activation
already consumed temporal history, the command fails rather than relabeling the
next frame as fresh. Sample index zero in both roles must have native predecessor0 and
jitter sample index zero. Sample indices oneâ€“3 must name the immediately preceding accepted sequence
frame and corresponding jitter phase. Every phase joins four actual indexed
and motion draws plus native temporal recording identity to completed Raw and
Processed readbacks with matching session, source allocation and settings.

Each collection writes four actual PNG pairs and preserves the frozen pose
hashes, crop, logical reference generation and native identities separately.
For image acceptance compare the same phase in reset/fresh collections using
the frozen crop (`cut`:48,32,592,328; `resize`:60,40,740,410), matching source
profile and exact extent. No cross-process native lifetime values are expected
to equal. Collection success establishes command/readback joins and reset
receipts; it does not claim pixel equality, disocclusion correctness, a temporal
quality score or an integrated acceptance pass. The parent owns native launches
and image comparison.

## Native depth-of-field lane

Use the same cooked frozen geometry with an installed engine that advertises
and actually prepares depth-of-field controls. This optional lane changes no
recipe, animation, spatial-AA run, or temporal sequence:

```powershell
& $hostExe --install-root $install --project-root $project `
  --scene Assets/Scenes/MovingEdges.tescene --fixture $fixture `
  --route indexed --mode OffNative --depth-of-field-native `
  --output-root ./out/validation/native-depth-of-field
if ($LASTEXITCODE -ne 0) { throw 'Native depth-of-field collection failed' }
```

The bare switch requires the indexed route and OffNative AA at native 640x360,
sample1. It cannot be combined with the separate temporal sequence options.
The host seeks the identical frozen sample index zero between every Engine update. Normal
Game-scoped typed transactions request baseline-Off, focus1m, focus8m,
enabled-with-zero-strength, then disabled. Focus range is0.25m; active radius
is12px. A case proceeds only after its actual terminal Applied receipt and an
exact accepted Scene settings/geometry publication agree. Unsupported controls
and failed preparations fail explicitly.

All five cases save completed native Raw/Processed PNG pairs. Raw decoded pixels
must remain identical across cases; Off and zero-strength processed pixels must
match their raw input. Enabled near/far images must differ from raw and each
other, with a nonzero response in the fixed ribbon region[105,155,515,190]. The
frozen depth bands are ribbon1m, rigid line2m, panel3m, and flat background8m.
The8m background contains no authored edge, so this fixture does not claim a
far-background edge-sharpness assertion. The region response is evidence for
focus inspection, not a complete blur-quality metric.

Active cases require a measured, fence-complete `depth-of-field` GPU timestamp
for the exact captured frame/settings/session/view lifetime. Post-processing
allocation generation is retained separately from the raw source allocation
generation. Off and zero-strength cases must have no active measured DOF work;
an unavailable numeric duration is never substituted with zero. The collection
contains receipts, actual native draws, source hashes, image hashes and timing
owners. Partial progress is saved after each case.

This Scene lane contains no HUD and performs no picking readback. Its explicit
`hudUnaffectedVerified` and `pickingUnaffectedVerified` remain false; the native
HUD/identity lane must establish those properties separately. Collection success
also leaves quality/integrated acceptance false. The orchestrator owns actual
GPU launches, image inspection and final acceptance.

### Strict reset/fresh identity and normalized-state evidence

The fresh role first renders and retires three genuine non-TAA Scene setup
extents (638x358,642x362,636x356), with accepted frame/native view lifetime and
command-side no-temporal-recording receipts archived in `retiredSetupViews`.
It then restores the exact frozen final extent/pose and enables TAA. This makes
its actual native lifetime different from reset's original or first-resize
lifetime even when independent renderer sessions restart their local counters.
No session, view or lifetime number is rewritten for the scorer.

Each reset row exposes `simulationStateSha256` and `effectiveSettingsSha256`.
Their complete normalized input documents are retained in `preparedState` for
inspection. Simulation uses actual canonical camera/model matrices, projection,
resolved animation tick, local joint transforms, real deformation palette and
native world vertices. Controls use actual typed effective values (including
variant kinds), native raster samples, internal/output extents and copied native
render controls. Objects are keyed by stable authored name. Frame IDs, queue
sequences, publication/resource generations and temporal history counters are
excluded from these comparison documents; they remain in separate provenance.
Only reference-origin bookkeeping is removed from matrices. Numeric native
poses, palettes, vertices and controls are never replaced by the frozen oracle
or quantized to force a matching hash.

## Half-resolution baseline and temporal reconstruction

These are independent indexed lanes; `--assemble-spatial` continues to accept
only its frozen spatial mode set. Collect the candidate, comparators and
oversampled reference through Game View using the same imported scene, cooked
payloads and 184 frozen poses in separate fresh directories:

```powershell
foreach ($mode in @('OffHalf','OffNative','Reference4x','TemporalUpscale')) {
  & $hostExe --install-root $install --project-root $project `
    --scene Assets/Scenes/MovingEdges.tescene --fixture $fixture `
    --route indexed --view game --mode $mode `
    --output-root (Join-Path ./out/validation/temporal-upscale-images $mode)
  if ($LASTEXITCODE -ne 0) { throw "Native capture failed for $mode" }
}

# CPU-only request/extent inspection; does not initialize the engine or GPU.
& $hostExe --describe-game-capture-mode TemporalUpscale
& $hostExe --describe-capture-mode OffHalf
```

OffHalf uses standalone AA Off and a real 320×180 Game target with no scaling
option. OffNative and Reference4x use the same Game output at 640×360 and
2560×1440. The native Game TemporalUpscale lane atomically
requests Game-scoped ID64=Off, ID91=Indexed, ID89=Temporal and ID66=0.5 through
`QueueGraphicsSettingsRequest`, with a 640×360 Game output target. It requires
actual `temporalUpscalingSupported` capability. All retain the unchanged
640×360 world projection, material witnesses and pose protocol.

Collection checks the effective graph, exact native four-draw indexed counters,
actual internal/output command extents, sample count and reconstruction history.
TemporalUpscale Raw must originate at 320×180 and Processed at 640×360. Native
backend source dimensions and decoded PNG dimensions must both match; resizing
a readback to disguise another source extent fails. Fixed interior witnesses
are evaluated at each stage's actual extent. No quality threshold changes.

TAA's separate `cut`/`resize` reset/fresh protocol also accepts
`--view game --mode TemporalUpscale`. Output resize 800×450 requires actual 400×225 internal
attachments, and the first accepted resized view must independently prove
predecessor zero and jitter zero. Existing pose, simulation and effective-state
hashes remain strict. Image scoring and final GPU qualification belong to the
orchestrator; successful collection is not a quality or reset-pixel pass.

The production backend currently qualifies temporal reconstruction at reduced
resolution for native Game outputs. A Scene TemporalUpscale request still fails
its production guard; no collector override removes that restriction. The
explicit `--view game` lane uses `SetGameViewTargetSize` and `SetGameViewCamera`,
not a relabeled Scene capture. All existing Scene collectors remain unchanged.

Game override cameras do not expose a native pose sequence or orthographic flag.
The collector retains actual `poseSequence=0`, records the local fixture ingress
sequence separately, and proves each pose through its exact accepted native
Game frame, frozen projection matrix, reference-local view matrix and canonical
origin. Every actual selected geometry matrix, transformed frozen vertex,
imported animation seek receipt and current palette remains checked. The Game
reference-frame ID is only a lookup key: missing exact accepted publication,
wrong output kind, stale matrices or missing indexed/motion counters fails.
It never invents a Game preview or a Scene-style native pose stamp.

This is native Game fixture evidence. It does not certify managed Play Mode,
exported runtime behavior or C# acceptance. `qualificationComplete` stays false;
the orchestrator owns actual GPU execution and strict scoring.

The native Game lane adds one ordinary runtime-only `CameraComponent` object
with its normal `Transform` after discovering the four imported mesh objects.
Between joined Engine frames it updates that camera's canonical pose and passes
its actual UUID plus current scene/object-lifetime/component/binding generations
to the ordinary `SetGameViewCamera` overload. This provides genuine registered
camera/view authority; an anonymous override cannot establish the view lifetime
required by fresh/reset collection. The object has no mesh, profile override or
animation, and no scene-save or asset-import operation occurs. Saved scene and
cooked payload hashes remain checked at completion.

`gameCameraAuthority` records the runtime-only authority. For a failed fresh
setup, at most eight `rejectedSetupViewWitnesses` retain actual output kind,
extent, pose sequence and missing camera/view predicates. A successful matrix
join does not replace that native camera/view lifetime proof.
