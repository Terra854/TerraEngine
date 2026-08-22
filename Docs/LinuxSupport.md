# TerraEngine Linux Support

TerraEngine supports native Linux development through the same CMake lifecycle
used by the rest of the project. The first supported target is Linux x64 on a
mainstream desktop or CI distribution with CMake, a C++20 compiler,
Vulkan-capable runtime libraries, and the usual X11
desktop development packages required by GLFW.

## Prerequisites

- Git with submodule support.
- CMake 3.22.1 or newer.
- Ninja for the checked-in `Debug` preset. Unix Makefiles are acceptable for
  local validation when Ninja is unavailable.
- A C++20 compiler such as GCC or Clang.
- Network access for the first configure, unless the hash-pinned SDK and NuGet
  caches were populated earlier. No machine-wide .NET SDK is required.
- Vulkan loader/runtime support for systems that launch the editor or renderer.
- X11 development libraries for GLFW's default Linux backend.

## Dependency Setup

```bash
git submodule update --init --recursive
```

All third-party project dependencies must come from `External/`. CMake
bootstraps the exact `global.json` SDK into
`External/dotnet/sdk/linux-x64/<version>` and verifies the archive against
`cmake/TerraEngineDotNetSdk.lock.json`. Do not replace vendored dependencies
with global package-manager copies.

## Configure

```bash
cmake --preset Debug
```

If Ninja is not installed, use an equivalent Makefiles tree:

```bash
cmake -S . -B out/build/LinuxDebugMake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=out/install/LinuxDebugMake
```

## Build Unit Tests

```bash
cmake --build out/build/Debug --target TerraEngine-EngineTests
cmake --build out/build/Debug --target TerraEngine-EditorTests
```

For the Makefiles fallback, replace `out/build/Debug` with
`out/build/LinuxDebugMake`.

## Run CTest

```bash
ctest --test-dir out/build/Debug --output-on-failure
```

## Install

```bash
cmake --build out/build/Debug --target install
```

The install target builds native tools, managed assemblies, shaders, and the
runtime layout. It copies the pinned SDK to `Build/Editor/DotNet`; installed
script compilation and NativeAOT export invoke that executable directly.

## Startup Smoke Validation

After install, run the documented launcher or export-bootstrap validation
command from the installed tree. On a headless machine or a machine without the
required GPU/display capabilities, the command must fail quickly with an
actionable message instead of hanging or crashing.

## Script Compilation Smoke Validation

After the installed Linux editor starts, open or create a project, create or
edit a C# script, and trigger the script refresh/build action. This validates
the editor path that generates the Assembly-CSharp workspace, starts the script
build, captures compiler output, and reloads the resulting script assembly.

Historical failure fixed by the Linux scripting implementation:

```text
Assembly-CSharp build failed: The current scripting build runner is implemented for Windows only.
```

Final expected behavior: the editor starts its bundled `DotNet/dotnet build`
for the generated
Assembly-CSharp project from the project workspace, captures stdout/stderr and
the process exit code, then either reloads the newly built script assembly or
reports normal compiler/process diagnostics. Live editor scripting uses the
managed Assembly-CSharp output from `dotnet build`; on Linux that output may
still be `Assembly-CSharp.dll`, which is separate from exported NativeAOT
payloads such as `Assembly-CSharp.so`.

## C# Script Editing

Visual Studio automation is Windows-specific and is not expected on Linux.
Linux script editing should use a configured editor command or the desktop's
default file opener.

Suggested editor command targets:

- Visual Studio Code: `code {solution}` for the generated workspace or
  `code {script}` for a single C# file.
- JetBrains Rider: `rider {solution}` for the generated workspace.
- Default text editor: use the platform default opener when no dedicated IDE is
  configured.

The placeholders are expected to resolve to the generated solution, project, or
script path once the Linux editor-launch implementation is complete. If no
editor can be launched, TerraEngine should report how to configure one rather
than requiring Visual Studio.

## Troubleshooting

- Missing X11 development packages usually appear during CMake configure while
  GLFW is being configured.
- Missing Ninja appears before CMake configures the project when using
  `cmake --preset Debug`; install Ninja or use the Makefiles fallback above.
- A missing or damaged repository SDK fails during configure with its expected
  path. Reconfigure with `-DTERRAENGINE_DOTNET_SDK_MODE=AUTO` to repair it, or
  use `OFF` to keep a disconnected build fail-closed.
- Missing Vulkan runtime support usually appears when launching renderer-owned
  entry points.
- Missing managed runtime payloads usually mention `Managed/Assembly-CSharp.so`
  for Linux exports or `Managed/Assembly-CSharp.dll` for Windows exports.
- A missing installed `Editor/DotNet/dotnet` prevents Assembly-CSharp builds;
  rerun the CMake install target instead of installing a system SDK.
- A failed editor script build should report the `dotnet build` command,
  working directory, process exit code, and captured compiler output. Compiler
  errors are normal build failures; missing command/output details are scripting
  runner bugs.
- If the editor reports that the scripting build runner is implemented for
  Windows only, the editor is running an older build; rebuild and reinstall the
  Linux editor before treating the scripting workflow as supported.
- If a successful editor script build cannot find its output, verify the
  generated project workspace contains `Build/Assembly-CSharp/Assembly-CSharp.dll`.
  This managed DLL is expected for live script reload on Linux and should not be
  confused with exported NativeAOT `Assembly-CSharp.so` packages.
- If shader installation fails, confirm `External/dxc/linux/bin/dxc` and its
  `libdxcompiler.so` and `libdxil.so` libraries are present.
- If opening a C# script reports a Windows-only Visual Studio or editor-launch
  message, complete the Linux script editor launch tasks in the active Spec Kit
  plan and configure a Linux editor command such as `code {script}` or
  `rider {solution}`.
# Audio validation

Linux audio uses miniaudio ALSA/PulseAudio backends selected by the runtime
environment. Headless CI should use the deterministic stress runner, which does
not open a device. For audible validation, confirm the active server with
`pactl info` (PulseAudio/PipeWire) and select an available ALSA device before
launching the installed editor. Device names and latency are machine-specific;
record them as manual evidence rather than treating a missing device as a
preparation failure.

The following checks are reproducible from an installed Linux tree (replace
`<install>` with the actual prefix):

```bash
pwsh -File <install>/Tests/Tools/Audio/RunAudioStress.ps1 -RequestedSources 128 -RepeatCount 100 \
  -Output <install>/Tests/Tools/Audio/Evidence/audio-stress.json
pwsh -File <install>/Tests/Tools/Audio/CompareAudioEvidence.ps1 \
  -Evidence <install>/Tests/Tools/Audio/Evidence/audio-stress.json
```

PowerShell (`pwsh`) is required for these wrappers. The 30-minute soak is an
explicit release-gate workload, not a quick smoke test. If Linux or `pwsh` is
not available in the validation environment, report the Linux lane as
*not run*; Windows/device-independent results do not establish Linux support.
Audible scenarios remain manual and must include the `pactl info` output and
selected device in their evidence.

The audio runtime foundation was validated on Ubuntu 24.04 under WSL with GCC 13.3. Strict
offline restore used the repository-locked `linux-x64` runtime and NativeAOT
packs; the EngineTests and EditorTests binaries built, all 12 audio CTest groups
passed, and the complete CMake install target succeeded. The native
`TerraEngineAudioStressRunner` and its production miniaudio/ALSA/PulseAudio
translation unit also compiled successfully; 128 sources across 100 replays
produced the same hash as Windows (`10560182529143497585`), honored the
64-voice limit, retained no resources, and measured 0.005482 ms median /
0.006189 ms p95. No Linux playback device was exposed by WSL, so this validates
the Linux build, installation, and device-independent behavior, not audible
Linux output.
