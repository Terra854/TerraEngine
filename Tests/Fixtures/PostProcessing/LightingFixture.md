# Screen-space lighting fixture

This generic fixture separates headless control/graph checks from actual native
image and GPU timing checks. It contains no game assets or external content.
See [the fixture README](README.md) and [the testing workflow](../../../Docs/Testing.md)
for the installed build prerequisites.

## Inputs and ownership

`Tests/Tools/PostProcessing/PostProcessScreenLightingGeometry.cpp` constructs real
ordinary packed meshlets through the canonical vertex codec, validates them with
`TryBuildIndexedCornerGeometry`, and registers ordinary Default PBR materials and
meshes through `ResourceManager`. The native foundation host seals immutable
render publications and uses the regular indexed MRT path. No scene storage is
mutated and no generated image, draw receipt, or completion substitutes for GPU
execution. CPU materialization is bounded; asynchronous native topology preparation
must settle before a recorded draw is accepted.

The 320 × 180, single-sample view uses a fixed orthographic projection, fixed
exposure at zero EV, no HUD and no AA. Its contact room has a sloped reflective
floor, a colored wall and a five-face contact block. Ordinary material occlusion
remains enabled. A separate isolated front-facing wall has valid depth and normals
but no other surface along outgoing reflection/indirect rays.

The MRT producer supplies current scene-linear HDR, standard-Z depth, signed
view-space normals/perceptual roughness and separate incident environment/diffuse
terms with their receiving material weights. Auxiliary attachments are demanded
per consumer. The frame owner retains images, descriptors and immutable resources
through actual GPU completion. Enhanced AO selects the producer variant that
omits only legacy **screen** AO; authored material occlusion remains in the weights.
The composite replaces indirect contributions once and retains direct/emissive
lighting. SSR/SSGI misses preserve their original environment terms.

## Running

From an x64 Visual Studio developer shell with the bundled dependencies:

```powershell
cmake --preset Testing -DBUILD_TESTING=ON
cmake --build --preset Testing --target install --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Build/install failed.' }
$installRoot = (Resolve-Path 'out/build/Testing/Build').Path
$env:TERRAENGINE_TEST_FILTER = 'Renderer.PostProcess.Lighting.'
& (Join-Path $installRoot 'Tests/Engine/TerraEngine-EngineTests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Lighting contract tests failed.' }
Remove-Item Env:TERRAENGINE_TEST_FILTER
$runId = [guid]::NewGuid().ToString('N')
$output = Join-Path ((Resolve-Path 'out').Path) "lighting-$runId.json"
& (Join-Path $installRoot 'Tests/Tools/PostProcessing/PostProcessFoundationFixture.exe') `
	--install-root $installRoot --output $output --run-nonce $runId
if ($LASTEXITCODE -ne 0) { throw "Native fixture failed: $output" }
```

The native host requires an available Windows Vulkan device, its validation layer
and a real window/presentation path. Use fresh evidence output and retain installed
artifact hashes and source revision with each run. The host also runs its other
foundation cases; lighting rows are `screenLightingGeometryCases` and the seven
sky-only combinations are `screenLightingCases`.

## Assertions and evidence scope

The headless `PostProcessLightingTests.cpp` exercises the actual typed resolver,
capability admission and graph helpers: all eight toggle combinations, zero
intensity without work, invalid mixed-tuple atomicity, stale resource-certificate
bypass and depth/color-before-overlay ordering. A supplementary source contract
checks the native legacy-AO omission guard and composite replacement/miss policy.
These tests do not execute shaders or establish per-view image parity.

Each native contact-room case enables one effect independently and requires three
actual indexed draws. Each isolated-wall miss requires one actual indexed draw.
Raw and processed PNG readbacks must match the exact frame, settings revision,
resource lifetime and extent, with no backend fallback. Contact cases require a
nonzero pixel response; isolated SSR/SSGI misses require bit-exact equality. All
cases preserve alpha and the known background strip. Named effect and composite
GPU costs require completed native timestamp receipts for the same view, frame,
settings lifecycle and target generation. Pending preparation is bounded and is
never reported as a successful draw.

One installed run on NVIDIA GeForce RTX 4080 Laptop GPU passed all five cases:
`out/validation/post-processing/screen-space/geometry-20261123-074449/foundation.json`.
That ignored local evidence records 8,311 GTAO, 10,460 SSR and 27,586 SSGI changed
pixels, two exact isolated misses, unchanged alpha/background and separately
completed GPU costs. These counts describe that run, not portable golden values
or a performance budget.

A second run enabled depth-driven hierarchical traversal using the exact current
view's standard-Z MAX pyramid. The 640 × 360 input produced nine mip dispatches.
The four measured pyramid builds took 0.026–0.028 ms each on an RTX 4080 Laptop
GPU. SSR changed 10,460 pixels, SSGI changed 27,589, and both isolated misses
remained byte-identical. The MAX value is the farthest bound used to establish
when a ray is behind an entire tile. Direct current-depth tracing is available
when the pyramid is unavailable. Perspective, multiview, motion, MSAA/VRS, other
formats, large scenes and temporal stability require separate qualification.
