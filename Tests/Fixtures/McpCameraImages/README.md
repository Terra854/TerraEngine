# MCP Camera Image Fixtures

These notes describe the authored camera states used by MCP camera image tests.
The current automated coverage builds the objects directly in editor automation
tests so the suite stays headless and deterministic.

- Active Game View camera: an enabled object with an active `CameraComponent`
  represents the runtime camera source for `game_view.capture`.
- Enabled non-game camera: an authored object with `Transform` and
  `CameraComponent` can be addressed by UUID, entity id, or display name through
  `camera.capture_object`.
- Disabled camera: an inactive object or inactive `CameraComponent` remains
  readable when `includeDisabled` is true. Capture must not enable, activate,
  select, reparent, or otherwise mutate the object.
- Custom camera: an ad-hoc pose and projection supplied in JSON produces an
  immutable descriptor. Validation rejects zero-sized targets, invalid clipping
  planes, and zero-length direction vectors before any renderer boundary work.

Headless tests decode the actual image bytes returned by the deterministic
fallback and MCP image envelope. Successful synthetic output is black RGBA at
the exact requested extent, with alpha 255 when opaque or 0 when transparent.
Its metadata must report `backendReadback=false`,
`deterministicFallback=true`, and `overlaysIncluded=false`. Invalid extents,
prohibited fallback, encoding/allocation failure, cancellation and deadlines
must return one explicit failure and allow later requests to progress.

[invalid-fallback.png](invalid-fallback.png) deliberately preserves the original
68-byte malformed fixed fallback. It is a negative checksum control, not an image
to repair or regenerate. The [encoding regressions](../../Engine/Renderer/RenderCaptureEncodingTests.cpp)
must reject it. The shared [independent decoder](../../Support/DecodedCaptureAssertions.cpp)
uses bundled `pv_png` with its CRC/Adler checks enabled and separately verifies
every PNG chunk CRC, including IEND. Real images are checked from their actual
Base64/PNG content; matching metadata or a signature alone is insufficient.

From the repository root in PowerShell 7 inheriting an x64 Visual Studio developer
environment with Windows SDK, CMake and Ninja:

```powershell
git submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { throw 'Dependency checkout failed.' }
cmake --preset RelWithDebInfo -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw 'Configure failed.' }
cmake --build --preset RelWithDebInfo --target install --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Compile/install failed.' }
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
	-R '^(Engine\.(RenderCaptureContracts|ReadbackLifetime|VulkanImageReadback)|Editor\.RenderCaptureContracts|Runtime\.ReadbackLifetime\.ProtocolContracts)$' `
	--no-tests=error --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Capture/readback regression failed.' }
```

Installed GPU/editor acceptance requires a usable Windows desktop/Vulkan adapter,
enabled timeline-semaphore support and the host diagnostic
`VK_LAYER_KHRONOS_validation` layer. That layer is an explicit diagnostic
prerequisite; setting an SDK path or requesting validation does not prove it was
enabled. Missing capabilities leave the affected acceptance incomplete.

Invoke the installed runner, choosing `Native`, `Editor` or `All`, with a fresh
work directory outside the installation:

```powershell
$cameraInstall = (Resolve-Path 'out/build/RelWithDebInfo/Build').Path
$cameraWork = Join-Path $PWD ('out/validation/camera-readback-' + [guid]::NewGuid().ToString('N'))
& (Join-Path $cameraInstall 'Tests/Tools/ReadbackLifetime/RunInstalledReadbackLifetime.ps1') `
	-InstallRoot $cameraInstall -WorkRoot $cameraWork -Mode All -Iterations 100
if ($LASTEXITCODE -eq 2) { throw 'Camera readback prerequisites are incomplete.' }
if ($LASTEXITCODE -ne 0) { throw 'Installed camera readback acceptance failed.' }
```

`Native` runs 100 timeout, 100 resize and 100 orderly-shutdown operations, plus one
separate C++ allocation-fault qualification. The real submission waits for value 1
at TRANSFER on a timeline semaphore initially at 0. After actual `VK_NOT_READY`
and a terminal timeout with native owners retained, an actual host signal to 1
permits completion and exactly-once retirement. Source checks, deterministic
controller adapters and the isolated premature-release mutant establish their
own narrower contracts; they cannot pass this GPU lane.

`Editor` performs one warmup and 100 read-only captures with 100 observed native
target resizes, then closes its single owned session normally. Both
`game_view.capture` with `freshness=nextFrame` and `camera.capture_custom` must
return `backendReadback=true` and `deterministicFallback=false`. Actual
`backendTarget` generation/source extent must advance and match each requested
and independently decoded PNG extent. Scene bytes, authored objects and dirty
state remain unchanged. These observations do not establish editor GPU timeout
retention or 100 separate shutdowns.

The runner pins the installed binary/support closure and retains raw native
observations, RPCs, PNG/decoder results, watchdogs and complete validation logs.
Exit codes are 0 for verified requested lanes, 1 for failure and 2 for incomplete
prerequisites. Validation errors and forced termination cannot count as success.
Results qualify their recorded installed hashes, not later builds or overall
feature completion. See [the complete testing workflow](../../../Docs/Testing.md#mcp-capture-bytes-and-readback-ownership)
and [installed-tool instructions](../../Tools/ReadbackLifetime/README.md).
