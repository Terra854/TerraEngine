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
