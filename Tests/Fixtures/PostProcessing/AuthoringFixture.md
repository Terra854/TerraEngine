# Installed post-process authoring fixture

`TestInstalledPostProcessAuthoring.ps1` collects the custom-effect subset of
installed authoring checks using the installed editor, ordinary importers, the managed authoring
facade, and an isolated exported game. It never counts a settings-only report as
image evidence. Its successful subset status is `passed-custom-authoring`, with
`qualificationComplete: false`.

The separate shared-profile/two-scene traversal, overlapping-volume blend/order,
player-disable, missing *packaged* payload, and full post-process authoring acceptance gates remain
required. This collector does not claim those tests through its custom-effect
captures or through an export rejected for a missing dependency.

## Prerequisites and commands

Use Windows, PowerShell 7, an unlocked Vulkan desktop, and a clean installed
`BUILD_TESTING` tree. All engine/compiler/SDK dependencies come from that install.
The image checks use the same PowerShell .NET `System.Drawing.Common` decoder as
the installed color collector; no Python or package installation is performed.

```powershell
cmake --preset RelWithDebInfo -DBUILD_TESTING=ON
cmake --build --preset RelWithDebInfo --target install --parallel 2

pwsh -NoProfile -File <install>/Tests/Tools/PostProcessing/TestInstalledPostProcessAuthoring.ps1 `
  -InstallRoot <install> -OutputDirectory <fresh-evidence-directory>
```

Only the orchestrator runs the integrated build/install and native acceptance.
A source-only check is explicitly separate and launches no editor/compiler/game:

```powershell
pwsh -NoProfile -File Tests/Tools/PostProcessing/TestInstalledPostProcessAuthoring.ps1 `
  -InstallRoot <intended-install> -OutputDirectory <fresh-dry-run-directory> -DryRun
```

Dry run writes `authoring-acceptance.json` with `status: "dry-run"`, a prerequisite
inventory, the planned codec envelope, and `integration-contract.json`. Missing
installed prerequisites are inventoried in this mode. An ordinary run rejects
missing prerequisites and never silently converts itself to dry run.

The installed tree must contain:

- `Editor/TerraEngine-Editor.exe`, `Editor/TerraEngine-Export.exe`, and their
  normal bundled dependencies and managed SDK/compiler.
- The maintained `TestInstalledPostProcessColor.ps1` materializer and its
  canonical `ManagedExportProject`, color fixture, and reference assemblies.
- `Tests/Fixtures/PostProcessing/Scripts/CustomPostProcessFixture.cs` and
  `Tests/Fixtures/PostProcessing/Assembly-CSharp.csproj`.
- The **source** `Editor/Shaders/PostProcess/Validation/PostProcessFixtureTint.frag.hlsl`,
  copied from the dedicated engine shader source for ordinary project import.
- Native exported-game support for `--validate-postprocess-authoring <root>` as
  described below. Unavailable native support is an integration failure.

## Authored inputs and actual import

The runner asks the installed color collector to `-MaterializeOnly`, obtaining
its ordinary emissive glTF chart and maintained scene schema without running the
color test. It changes only that fresh project's controller and identity, then
imports and instantiates two chart prefabs through the editor tools.

| Identity | Exact UUID |
| --- | --- |
| CameraA | `75000000000000000000000000000001` |
| CameraB | `75000000000000000000000000000002` |
| Controller | `75000000000000000000000000000003` |
| Fragment shader | `75000000000000000000000000000010` |
| Effect | `75000000000000000000000000000011` |
| Shared profile | `75000000000000000000000000000012` |
| Deliberately absent shader | `750000000000000000000000000000ff` |

The materialized shader is `Assets/PostProcessing/Tint.frag.hlsl`, using a real
`ShaderImporter` metadata identity, stage `ps`, entry `PSMain`, target
`ps_6_0`, and `settings.shaderUse: "post-process"`. The importer passes the
dedicated `--shader-use post-process` mode to the bundled compiler; omitting it
keeps the ordinary authored-surface push-constant restriction. This mode does
not bypass compiled fragment interface or effect parameter-layout checks.
Its source-color combined sampler is set0/binding0. Its independent
push parameters are Vector4 `tint` at byte0 and Scalar `gain` at byte16, with a
20-byte used parameter extent verified from the actual compiled shader.

The `.teposteffect` uses schema1 `TerraEngine.PostProcessEffect`, insertion
`display-image`, domain `display-linear`, `distinct-color-write`, and one
`source-color`/`sampled-read` combined-sampler input. Tint defaults to `[1,1,1,1]`
with bounds0..2 per channel; gain defaults1 with bounds0..2. The `.tepostprofile`
uses schema1 `TerraEngine.PostProcessProfile`, empty setting values/mask, and an
explicit custom-effect list containing this effect. The runner obtains cooked
paths from actual importer metadata and hashes the real compiled outputs.

Both cameras retain a real Game-scope manual exposure mode at EV0, legacy tone,
and bloom disabled, established by the managed fixture's native request and
effective publication. This keeps Raw/Processed/Composited available even when
the custom effect is disabled. CameraA binds the profile and receives transient
effect changes; CameraB remains a separate unchanged camera owner.

The saved scene initially contains empty profile UUIDs for both cameras. The
fixture's SetProfile request binds CameraA during each actual Play/export session;
its protected scene-visible frame and subsequent native effective frame are
checked separately. This is not an expectation that a Play-only edit had already
been saved into the exported scene. The runtime adapter compares the live camera
component, accepted native authoring snapshot, and observed profile as UUID
values. Managed empty asset text `""` and native canonical 32 zeroes both mean
`EMPTY_UUID`; malformed nonempty text is rejected. Independent native profile
checks, including camera name and all three identities, remain in the runtime
report before any mismatch fails the run.

## Managed observation and acknowledgement

The runner passes these environment values to both lanes:

```text
TERRAENGINE_CUSTOM_POST_PROCESS_FIXTURE_ROOT=<fresh lane directory>
TERRAENGINE_CUSTOM_POST_PROCESS_FIXTURE_RUN_ID=<ASCII run ID>
TERRAENGINE_CUSTOM_POST_PROCESS_FIXTURE_SHADER_ID=<shader UUID>
TERRAENGINE_CUSTOM_POST_PROCESS_FIXTURE_EFFECT_ID=<effect UUID>
TERRAENGINE_CUSTOM_POST_PROCESS_FIXTURE_PROFILE_ID=<profile UUID>
TERRAENGINE_GRAPHICS_FIXTURE_DATA_ROOT=<fresh lane save directory>
```

`CustomPostProcessFixture` publishes `custom-post-process-observation.json`
atomically. Schema1 includes `runId`, fresh `bootId`, `status`
(`pending`, `passed`, `failed`), `phase`, `captureTag`, decimal-string `requestId`,
`failure`, `checks[]`, `receipts[]`, `cameraA`, `cameraB`, and
`captureAcknowledgements[]`. The snapshots retain native graphics revision and
frame separately from authoring revision, scene-visible frame, and effective
frame. Each retains the full scene/object/component/binding/view and
renderer/backend lifetimes. `cameraA.customEffect` carries the effective asset
and shader identities, Boolean enabled state, Scalar gain and four tint values.

Each receipt retains its original ID, actual state/reason, effective revision,
applied frame, scene-visible frame, and effective frame. A scene-visible authored
profile write alone does not prove native execution. Capture waits for actual
effective state and its positive native frame/revision.

| Ordered tag | CameraA effective state | Required image witness |
| --- | --- | --- |
| `baseline` | Disabled | Nonblack imported chart |
| `enabled` | Enabled, tint `[.5,1,.75,1]`, gain1 | Processed image changes |
| `parameter-changed` | Same tint, gain.5 | Changes from both baseline and enabled |
| `disabled` | Disabled | Returns to baseline |

For **each** tag and camera the collector obtains actual 1280×720 Raw, Processed,
and Composited native backend PNGs. It rejects deterministic fallback, wrong
stage/camera, older frames, mismatched settings revision, missing target/session,
wrong extents, and missing PNGs. It records each real native generation; captures
on different completed frames are not relabeled as simultaneous.

Only after retaining all six images does the collector write the unique file
`custom-post-process-capture-ack-<tag>.json`:

```json
{"schemaVersion":1,"runId":"<run>","bootId":"<boot>","tag":"enabled"}
```

The managed fixture must consume the exact four ACKs before reporting passed.
The collector requires independent native Applied receipts for the checkpoints,
and these managed checks: `custom-toggle-on`, `custom-parameter-change`,
`custom-toggle-off`, `custom-camera-isolation`, and `authored-profile-binding`.
The fixture also exercises wrong parameter kind and out-of-range rejection;
their native rollback observations remain in its receipt/check transcript.

## Negative import and dependency cases

The runner removes only the final closing-brace byte from the **copied** HLSL
file and invokes ordinary `asset.import`. That tool's successful envelope means
the request was queued; it is not falsely treated as compilation success or
failure. The collector waits for actual importer settlement, requires unchanged
preceding compiled bytes, obtains a real processed health capture, and retains
the actual compiler/import diagnostic from the owned process logs. It restores
the original bundled source and imports it normally before export.

After the positive isolated runtime lane, a separate disposable project changes
the effect's shader dependency to the absent UUID above. Both source and cooked
effect envelopes are changed so stale cached bytes cannot hide the deliberate
negative input. A real installed export must reject with a useful missing shader
dependency diagnostic. An unrelated crash/timeout or forced termination fails.
This is a missing-dependency **export** test, not proof of graceful handling of a
physically removed payload in an already valid archive.

## Native isolated-export integration contract

The installed export packages the real compiled fragment and profile/effect
dependencies. The runner copies the package into a separate directory and moves
the disposable authored project away from its original absolute path before
launching the game with that package as its working directory. Normal exported
runtime package mounting must supply the assets; source HLSL is not a fallback.

The test-build native runtime argument `--validate-postprocess-authoring <root>`
must drive normal fixed-step engine updates and the same managed observer. At a
Capture checkpoint it queues actual authored-camera backend stage readbacks,
polls their completion without manufacturing frames, writes native PNGs and
metadata, and writes the exact ACK only after every required image completes.
Shutdown must settle queued/submitted resources normally.

It writes `runtime-captures.json` with schema1, matching run ID, a fresh boot ID,
`mode: "export"`, `status: "passed"`, `orderlyShutdown: true`,
`settingsObservation` (the terminal managed report), four `checkpoints`, and
24 `captures`. Each capture row carries:

```text
tag, camera, cameraUUID, stage, png (relative inside lane), sha256,
width, height, frameId, settingsRevision, expectedSettingsRevision,
sourceEncoding, deliveredEncoding, backendReadback=true,
deterministicFallback=false, backendTarget, snapshot
```

`backendTarget` retains actual target generation, renderer session, source extent,
and dedicated capture ownership. `snapshot` retains the exact native camera
lifetime and settings consumed. Missing fields or unavailable integration fail;
the collector never manufactures a runtime report from editor imagery or C#
settings observations.

Pixel comparisons use a fixed every-fourth-pixel raster and exact inverse sRGB
transfer before RGB errors. CameraB, Raw images, and disabled identity must stay
within .004 mean absolute linear error; enabled and changed CameraA effects must
change at least .005. Both baseline charts must have mean linear energy at least
.01. Matched editor/export stages must stay within .012. These static chart
witnesses establish effect execution/isolation and transfer consistency, not
temporal quality or performance.

Evidence is retained under the fresh output tree: input hashes, RPC metadata,
native PNGs/sidecars, checkpoint/ACK transcripts, owned process logs/lifecycles,
separate export diagnostics, and `authoring-acceptance.json`. A positive native
run has not been claimed merely because this runner parses or its dry run passes.

The importer metadata's `compiledPath` is relative to the editor asset database's
canonical `<project>/Build/Assets` root. The runner validates containment and
uses that exact metadata reference, including for the negative export's cooked
effect. It does not probe alternate directories for a plausible stale output.
If the real compiled output is absent, `*-unavailable.json` retains the queued
admission, actual metadata, expected path, and available compiler diagnostics.
After orderly process/log draining, `authoring-acceptance.json` also reports the
actual compiler/import rejection. A queued RPC envelope is never relabeled as a
terminal import result merely because a file is present or absent.
