# Post-Processing Validation Fixtures

This directory defines source inputs for post-processing and graphics-settings validation.
Setup defines the inputs. Later tasks add the managed fixture project, authored
scenes, shaders, and validation drivers. A recipe entry becomes executable when
its inputs and installed runtime operations are available.

## Source and work directories

Keep authored fixture input in this directory. Generate import results, managed
references, `Build/`, `Library/`, `Temp/`, `obj/`, desktop exports, and evidence in
a disposable work project under `out/validation/post-processing/`. Give each run
its own directory and record the source revision and installed artifact hashes.

The small [export baseline source](../ExportRuntimeBaseline/README.md) supplies
the existing project/export conventions. Its managed project stages installed
`TerraEngine.CSharp.dll` and the export analyzer into the disposable project's
`Build/Assembly-CSharp/References/`. The post-processing managed fixture
follows that convention. The runner owns its disposable project and output
directory. Leave the authored source in place.

## Legacy project input

Use an authored project with explicit existing graphics values and no new
post-processing fields. Stage a disposable copy of the existing export baseline
source, retaining its startup scene, model source, build profile, and render
settings. Record the input bytes and the disabled-output camera pose before
running the candidate build. The managed calls and installed
driver verify old and expanded callers against this input.

The legacy checks load the project, read its existing values, and compare the
new build's disabled output with the captured legacy output at the same camera,
resolution, lighting, and geometry settings. Missing fields resolve through
legacy defaults. Play Mode exit and rejected requests preserve the authored
settings. Save player choices in the game's separate persistent-data namespace.

## Two-camera input

The two-camera fixture supplies the managed project and script in
`Assembly-CSharp.csproj` and `Scripts/GraphicsSettingsFixture.cs`. Its disposable
runtime scene contains these named objects and publishes both camera views.
Retain their runtime IDs and generation information in the test evidence.

| Input | Required fixture state |
| --- | --- |
| `CameraA` and `CameraB` | Two enabled cameras at distinct poses viewing the same scene, each with a 640 × 360 output. Keep projection, clipping planes, lighting, and starting settings recorded and stable for paired captures. |
| Visible geometry | One stationary opaque reference object and a second object that can move through the view. Preserve ordinary indexed geometry for the required coverage/temporal fixture. |
| Lighting and UI | Fixed daylight lighting and a static, legible HUD marker. Camera effects operate before the default HUD composition. |
| Script controller | Resolve each managed `Camera` into the initial graphics API's typed target, read the published snapshots, and retain request IDs and results. |

The graphics-settings fixture queries both cameras, checks distinct view identity, retires one camera, and
verifies stale-target rejection. Native contract tests exercise scoped patch and
reset behavior using a fixture setting. The color fixture adds a fresh two-camera scene check
that changes fixed exposure on one camera and verifies the other camera's
effective value and image. The camera-effects fixture adds the equivalent focus/lens isolation check.
Use a newly created camera for each destruction test so later checks have two
live targets.

## Native Vulkan foundation

Configure with `cmake --preset RelWithDebInfo -DBUILD_TESTING=ON` and install with
`cmake --build --preset RelWithDebInfo --target install --parallel 2` from a
Visual Studio developer shell. The fixture uses an owned Windows GLFW window and
requires a working Vulkan device and validation layer. It reads installed shaders
and writes its evidence to a fresh file.

```powershell
$buildRoot = (Resolve-Path 'out/build/RelWithDebInfo/Build').Path
$runId = [guid]::NewGuid().ToString('N')
$evidence = Join-Path ((Resolve-Path 'out').Path) "post-process-foundation-$runId.json"
& (Join-Path $buildRoot 'Tests/Tools/PostProcessing/PostProcessFoundationFixture.exe') `
	--install-root $buildRoot --output $evidence --run-nonce $runId
if ($LASTEXITCODE -ne 0) { throw "Native post-processing fixture failed. See $evidence." }
```

The report includes GPU submission and completion receipts, timing samples, and
decoded assertions for the raw, processed, and composited camera stages. The
runtime image check reads its dedicated output and checks main runtime execution
through the separately attributed GPU timestamps.

## Motion and temporal inputs

The moving-edge fixture supplies deterministic moving-edge and oversampled-reference inputs,
with moving rigid and skinned geometry for temporal checks. The required positive
matrix uses 4× MSAA at native extent, TAA at native extent, and temporal
reconstruction from 320 × 180 to 640 × 360 on the reference Windows Vulkan
device. Record actual active modes, sample counts, and extents in Play Mode and
the desktop export. The moving-edge fixture defines the fixed masks and
numerical comparisons.

## Later installed checks

`Tests/Tools/PostProcessing/TestInstalledGraphicsSettings.ps1` drives
the managed fixture in Play Mode and an export, including explicit next-launch
staging and original-request receipt lookup. The authoring fixture adds the isolated exported
custom-effect checks. See [Testing](../../../Docs/Testing.md) for the installed
test workflow.

After the applicable inputs and tests are implemented, run the installed build's
registered CTest coverage from the repository root.

```powershell
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'CTest failed.' }
```

The parent implementation session owns integrated compilation, installation,
full-suite execution, live editor/export validation, and final task completion.
