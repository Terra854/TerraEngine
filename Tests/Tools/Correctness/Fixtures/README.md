# Installed correctness callback fixture

`ManagedExportProject/` contains authored inputs for one small disposable Windows
project. The scene has a camera, a healthy script and a controlled throwing
sibling. It needs no imported mesh, audio device, generated payload or custom
shader. Its project and profile follow the maintained export baseline pattern.

Copy this entire project directory to a **fresh runner-owned project directory**.
Do not run the editor against the checked-in fixture. Do not copy any historical
`Build/`, `Builds/`, `Library/`, `Temp/`, `obj/`, DLL or evidence directory.

The parent install rule should copy this `Fixtures/` directory under
`Tests/Tools/Correctness/Fixtures/` in the install tree. The installed runner must
resolve this copy, reject missing files and record their hashes with its evidence.
There are no build-tree paths in the project.

## Installed inputs and setup

The disposable project's `Assembly-CSharp.csproj` targets the repository's
`net10.0` managed contract. Before opening it, stage these installed inputs:

- `Editor/Managed/TerraEngine.CSharp.dll` to
  `Build/Assembly-CSharp/References/TerraEngine.CSharp.dll`.
- `Editor/Managed/Analyzers/TerraEngine.Export.Generator.dll` to
  `Build/Assembly-CSharp/References/TerraEngine.Export.Generator.dll`.

The installed editor still needs its managed runtimeconfig/deps files, native
runtime closure, bundled dotnet/offline NuGet/NativeAOT toolchain and installed
shaders/export tools. The production editor/compiler creates all generated
workspace files and managed output. The runner must fail if these inputs are
missing; a source-tree or build-tree fallback would invalidate installed evidence.

Seed `Library/LastScene.txt` in the disposable project with
`Scenes/Startup.tescene`, and use isolated editor preferences. Create a fresh
observation directory **outside the project's Assets directory** and pass its
absolute path in the editor child's environment variable
`TERRAENGINE_CORRECTNESS_OBSERVATION_ROOT`. Keep the environment override local
to that process; do not change the parent environment. The two scripts do not
create or clear that directory.

## Observable behavior

`CorrectnessHealthyScript.OnUpdate()` atomically replaces `healthy.json` with
`{"instanceId":"<32 lowercase hex digits>","updates":<unsigned count>}`.
The runner must first prove callbacks execute, then compare receipts before,
during and after an export. The same `instanceId` and increasing `updates` prove
that the same gameplay instance remains usable during isolated export. Compare
public `script.status.loadedOutput` fingerprints separately; instance identity
is not an assembly generation number. File reads may briefly contend with atomic
replacement on Windows, so retry them within the runner's existing deadline.

`CorrectnessControlledFaultScript` remains harmless until the runner creates
`fault.release` in that observation directory. On its next update it writes
`fault-observed.txt` containing `Correctness.ControlledManagedFault`, then throws
`InvalidOperationException` with the same message. Each instance throws once.
The receipt alone proves reaching the gate, not successful exception containment.
Require the paused fault frame's public `script.status.frame.failedScripts` to be
positive, retain the engine diagnostic, and prove a later healthy count increases
while normal MCP reads remain usable. This is a contained managed callback fault;
it does not inject a catastrophic native required-frame failure.

For precise fault-frame observation, enter Play, pause, record a healthy receipt,
create the gate and request one `play.step` with `expectedState: paused`. Query
`script.status` before requesting another step; a later frame can overwrite the
latest failed-script count. Remove the gate before recompiling or starting a new
Play session, because a replacement instance deliberately re-arms the fault.

The profile ID for `export.start` is `correctness`; published output goes to
`Builds/Correctness`. Both scripts are partial subclasses so the installed export
analyzer can generate NativeAOT registration. Without the observation environment
variable the exported game still runs the callbacks, and the fault remains gated.

For deterministic process cancellation/owner-close checks, the runner also sets
`TERRAENGINE_CORRECTNESS_HOLD_EXPORT=1` and
`TERRAENGINE_CORRECTNESS_PROCESS_FIXTURE` to the installed child fixture's absolute
path. The authored `Directory.Build.targets` runs its gate only with
`PublishAot=true` and that opt-in environment flag. MSBuild imports this separate
file even after the editor regenerates `Assembly-CSharp.csproj` from its installed
template during project load or compilation. It starts the fixture with two descendants, writes
`export-pid.0` through `export-pid.2`, and waits for `export.release` in the
observation directory. The normal live managed Build does not reach this gate.
Cancellation must kill/reap these actual export-owned descendants. For the retry,
create `export.release`; clear that disposable gate and PID records only after
settlement before the separate close-during-export leg. The gate's five-minute
deadline is a cleanup bound, never evidence of a successful cancellation.

## Source-only validation

JSON and project XML parsing can be checked without compiling. This fixture does
not claim managed compilation, callback dispatch or successful export until the
parent's installed acceptance run exercises the real compiler and editor.

## Texture and translated-control observation project

`TextureUiProject/` is a separate disposable rendering/input fixture. Copy its
installed authored files into a fresh runner-owned project; never open the
checked-in fixture directly. Use the installed `TerraEngineTextureUiFixtureGenerator`
with `--output-root <fresh-project>/Assets/Textures` to create the agreed texture
inputs. The camera and UI document require no imported mesh or custom shader.
The runner must copy both installed default `EditorLayout.ini` and
`EditorLayout.terra` files into the fresh project's `UserSettings`, isolate its
editor preferences, and retain normal managed-build readiness. The project
contains no historical build outputs, local SDK paths or editor preferences.

`Assets/UI/TranslatedControls.tuiml` uses existing UI markup/styles. Its images
reference generator filenames beneath `Textures/`; the first row presents the
linear red/blue witness, four-color quadrants, and sRGB midtone. The second row
presents partial edges, alpha, and a compressed BC7 control. These displayed
surfaces supplement the independently captured texture pixel checks; scaling,
filtering, display color management and screenshots alone do not prove exact
mip bytes or color-space correctness. See the texture generator README for
independent source pixels and dimensions.

Enter Play and wait for the visible `Ready; hover translated:` label. The normal
managed `TextureUiObserver` waits for the production UI root and hides the
negative ancestor before readiness. Its counters read `UI.WasElementClicked`;
hover indicators read `UI.IsElementHovered`. It neither injects input nor
manufactures successful counts. `Back` and `hidden` counts must remain zero.

For bounded machine-readable observation, the runner may set
`TERRAENGINE_TEXTURE_UI_OBSERVATION_ROOT` to a precreated disposable evidence
directory before editor launch. The observer atomically replaces
`texture-ui.json` through `texture-ui.pending`, with `schemaVersion: 1`,
`ready`, `updates`, `translatedClicks`, `frontClicks`, `backClicks`,
`hiddenClicks`, `translatedHovered`, `frontHovered`, `backHovered`, and
`hiddenHovered`. It writes on observation changes and every 60 managed updates.
These receipts supplement the visible labels and record real polling results;
they do not establish that input was physical or that pixels were presented.
The optional file receipt is absent from the exported script path.

All coordinates below are logical Game View coordinates, not desktop pixels.
The top-left of the displayed game image is the origin. For a displayed image
rectangle `(left, top, width, height)` and logical batch `(W,H)`, target logical
`(x,y)` at screen `(left + x*width/W, top + y*height/H)`. Use the actual image
rectangle, including its letterboxing; the editor's existing Game View mapping
applies that conversion. Do not substitute the surrounding dock/panel bounds.

| Control | Displayed logical rectangle | Observation point | Expected result |
|---|---|---|---|
| Red translated button | `(80,0)` size `(40,40)`; parent `(0,0)` size `(40,40)` | `(90,10)` | Translated hover true; one count per accepted press |
| Green `Front`, over blue `Back` | `(80,130)` size `(40,40)` | `(90,140)` | Front hover/click only; back remains zero |
| Hidden subtree | Its excluded button would occupy `(80,170)` size `(40,40)` | `(90,180)` | No magenta button, no hidden hover/click |
| Outside Game View image | Outside the actual active image rectangle | A point in the letterbox/panel beyond image bounds | All button hover false; counts unchanged |

Move the physical pointer to the red button and capture its visible hover label.
Press, hold for several frames, release, then press and release again. Capture
`Translated clicks: 2; front: 0; back: 0; hidden: 0`. Click the green overlap once
and capture `front: 1`, leaving the other counts unchanged. Click the hidden
region and outside the image; capture unchanged counts and no unintended hover.
Record both screen/logical coordinates and screenshots with the actual control
and observation labels in view. Finally close the owned editor normally and
retain process/log evidence. A locked desktop, unavailable physical input or
missing render capture leaves that observation pending; native synthetic
pointer tests and label inspection cannot substitute for physical acceptance.

The same UI document and managed observer also use normal exported-runtime
window input. If exercised there, window-local pixels replace the editor image
coordinate mapping. The native regression preserves existing document input
precedence separately; this fixture does not introduce a cross-document stack.

Authored JSON/XML and managed compilation are local supporting checks only.
Installed GPU rendering, physical hover/click behavior and normal close remain
parent acceptance work; this fixture's existence does not claim they passed.

## Displayed UI capture prerequisite

For the texture/UI fixture, activate the Game View tab and keep its image area
large enough to show the full swatch/mip row. Use `game_view.capture` with
`freshness: latestCompleted` after observing that live view. Its `nextFrame`
route and `camera.capture_custom` render a separate camera with an empty UI
batch, so those images establish camera/readback availability only. Inspect
raw backend PNG pixels, account for any reported resizing, and retain a native
target-size capture for tiny mip controls. Resize the Game View and verify that
its presented controls still match physical hover/click coordinates.
After `pointer-ready.json` appears, activate and observe Game View before writing
`game-view-ready.json` (`observed: true`, matching owned `editorPid`). The runner
then captures the displayed target and waits for the physical/pixel review.
