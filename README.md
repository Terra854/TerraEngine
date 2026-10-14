# TerraEngine

**A C++20 game engine and editor with Vulkan rendering, C# scripting, and a task-based runtime.**

TerraEngine brings scene authoring, asset compilation, rendering, physics, animation, terrain, navigation, and audio into one native engine and editor. Its architecture centers on parallel preparation and transactional publication: systems prepare work from snapshots, then make changes visible at explicit frame boundaries.

The project is under active development. Runtime APIs, authored formats, and editor workflows can change, including changes that require recreating or recooking older project data. Start here to build the editor, create a project, and explore the implementation.

> **Platform status:** Windows x64 is the primary development platform and the recommended starting point. Linux support is experimental and is currently likely to fail to compile or run.

[Getting started](#getting-started) · [Features](#features) · [Documentation](#documentation) · [Contributing](#contributing)

## Features

| Area | What is in the repository |
| --- | --- |
| **Rendering** | Vulkan renderer with mesh shaders, physically based materials and lighting, shadows, sky rendering, Hi-Z occlusion infrastructure, and virtual geometry cooking, streaming, and diagnostics. |
| **Editor** | Native Dear ImGui interface with a project hub, scene and game views, hierarchy, component inspectors, transform gizmos, asset browser, prefab authoring, undo/redo, and profiling tools. |
| **Scenes and runtime** | Component-based objects, deterministic hierarchy handling, task scheduling, typed scene transactions, and large-world positions represented as exact cells plus bounded local offsets. |
| **Scripting and export** | C# gameplay APIs, editor script compilation and reload, NativeAOT export tooling, cooked asset formats, and TEPAK content archives. |
| **Physics and movement** | Jolt-backed rigid bodies, colliders, constraints, character controllers, and bounded character locomotion with optional navigation and animation integration. |
| **Animation** | Animated model import, skeletal playback, controllers and blending, root motion, retargeting, procedural animation, motion matching, ragdolls, and dedicated authoring and diagnostic tools. |
| **Terrain and navigation** | Streamed terrain areas, surface layers, holes, heightfield collision, and Recast/Detour-backed ground navigation with direct route queries and optional navigation agents. |
| **Audio and UI** | WAV/Ogg import, miniaudio playback and spatialization, mixer controls, audio diagnostics, and runtime UI assets and rendering. |
| **Editor automation** | An opt-in local Model Context Protocol (MCP) bridge for inspection and editor operations, with read-only mode, transactional writes, approval controls, and activity history. |

Feature availability depends on the host, graphics capabilities, and project data. The linked guides describe each subsystem's supported behavior and limits; some also contain future design work.

## What a public checkout contains

The public repository contains engine and editor source, dependency references, build configuration, documentation, and checked-in test fixtures. A development workspace can contain additional files that are **not part of the public checkout**:

- Local game projects, imported assets, and demo content.
- Compiled binaries, cooked assets, captures, logs, and validation reports.
- Generated .NET SDK and NuGet caches.
- Local planning documents, assistant configuration, and generated knowledge graphs.

The setup below uses the published source, recursive submodules, and CMake's pinned dependency bootstrap. It does not require a private demo project. Tests that need separately acquired media, external projects, or live hardware have additional prerequisites documented alongside their fixtures and tools.

## Getting started

### Requirements

| Requirement | Details |
| --- | --- |
| **Host** | Windows x64 is recommended. Linux x64 is experimental; successful compilation and execution are not currently expected. |
| **Native toolchain** | A C++20 compiler. On Windows, use Visual Studio's MSVC C++ tools and Windows SDK from an x64 developer shell. Experimental Linux builds use GCC or Clang. |
| **Build tools** | Git, CMake **3.22.1 or newer**, and Ninja for the checked-in presets. |
| **Graphics** | A Vulkan **1.3** runtime and GPU/driver exposing `VK_EXT_mesh_shader` with `meshShader` support. Task-shader support enables the task/mesh paths that use it. A usable display is needed for the interactive editor. |
| **Experimental Linux prerequisites** | X11 development libraries for the default GLFW backend. Optional Wayland configuration and intended setup are described in [the Linux notes](Docs/LinuxSupport.md); these are not confirmation of a working Linux build. |
| **Initial network access** | Required to fetch submodules and populate the hash-pinned .NET SDK and NuGet caches. |

Third-party engine libraries are sourced from `External/`, using pinned submodules or bundled files. CMake obtains the exact .NET SDK selected by [global.json](global.json) and the audited packages in [the NuGet lock](cmake/TerraEngineNuGet.lock.json). A machine-wide .NET SDK or Vulkan development SDK is not required; the OS toolchain, graphics driver, and Linux platform libraries remain host prerequisites.

### 1. Clone with dependencies

```sh
git clone --recurse-submodules https://github.com/Terra854/TerraEngine.git
cd TerraEngine
```

For an existing clone:

```sh
git submodule update --init --recursive
```

Use a Git checkout rather than GitHub's source ZIP so dependency revisions and submodules are available. After pulling changes that update dependency references, run the submodule command again.

### 2. Configure, build, and install

On **Windows**, run these commands from an **x64 Visual Studio developer shell** at the repository root:

```powershell
cmake --preset RelWithDebInfo -DBUILD_TESTING=ON
cmake --build --preset RelWithDebInfo --target install --parallel 2
```

For contributors investigating **experimental Linux support**, the intended commands are below. They may fail to configure, compile, or install; this workflow is unverified:

```bash
cmake --preset RelWithDebInfo -DBUILD_TESTING=ON
cmake --build --preset RelWithDebInfo --target install --parallel 2
```

Check that each command succeeds before continuing. The first configure populates the pinned managed dependency caches; the first build compiles the native and managed targets. Two parallel build jobs are a conservative starting point for the large translation units; adjust to fit your machine.

**Complete the install target before launching.** It stages matching native libraries, managed assemblies, the bundled SDK, project templates, tools, and compiled shaders. Building an executable alone does not produce the complete runtime layout.

The maintained runnable tree is:

```text
out/build/RelWithDebInfo/Build/
├── Editor/       Editor, asset/export tools, templates, Managed/, DotNet/, shaders
├── Engine/       Engine runtime payload
├── Launcher/     Export launcher and its native dependencies
├── Tests/        Installed validation tools and fixtures when testing is enabled
└── Docs/         Selected installed guides
```

These paths are controlled by `ENGINE_INSTALL_DESTINATION` in `CMakeLists.txt`. Use this `Build/` tree even though the presets also define an `out/install/` prefix.

The other available presets are `Debug`, `Release`, and `MinSizeRel`. Replace `RelWithDebInfo` consistently in the commands and output paths when using another preset. For experimental Linux setup notes and an intended Makefiles alternative, see [the Linux notes](Docs/LinuxSupport.md).

### 3. Launch the editor

Windows:

```powershell
& ./out/build/RelWithDebInfo/Build/Editor/TerraEngine-Editor.exe
```

Experimental Linux, only after a successful build and install (startup and runtime behavior remain unverified):

```bash
./out/build/RelWithDebInfo/Build/Editor/TerraEngine-Editor
```

Use **New Project** in the project hub to create a project in a writable folder, or **Open Project** to select an existing TerraEngine project. Import your own assets into that project's `Assets` directory, author a scene, and use the editor's Play controls to exercise it. The installed editor includes the templates needed for project creation.

For C# gameplay, start with the [runtime API guide](Docs/CSharpRuntimeAPI.md). For packaged builds, the [export runtime baseline](Docs/ExportRuntimeBaseline.md) provides a reproducible workflow using a checked-in fixture.

## Testing

After the build and install succeed, run CTest from the repository root:

```sh
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo --output-on-failure
```

To run the aggregate native engine and editor unit entries:

```sh
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R "^(Engine\.SpatialUnitTests|Editor\.ExportUnitTests)$" --no-tests=error --output-on-failure
```

[Docs/Testing.md](Docs/Testing.md) is the authority for the current CTest workflow, focused regressions, stress/soak runs, and installed runtime acceptance. Full validation can include long-running tests, Windows-specific automation, separately acquired fixtures, and live renderer checks. Read the relevant prerequisites before running those gates. A native unit-test pass alone does not establish live GPU or installed-editor acceptance.

## Documentation

| Topic | Guides |
| --- | --- |
| Build and validation | [Testing](Docs/Testing.md), [experimental Linux notes](Docs/LinuxSupport.md), [dependency inventory](External/README.md) |
| Scenes and world coordinates | [Scene graph and prefabs](Docs/SceneGraphRuntime.md), [large-world scenes](Docs/Scene/UnboundedScenes.md), [scene format](Docs/TerraEngineSceneFile.md) |
| Rendering | [Physical lighting](Docs/Renderer/PhysicalLighting.md), [sky](Docs/Renderer/SeamlessSky.md), [virtual geometry](Docs/Renderer/VirtualGeometry.md) |
| Physics and movement | [Physics](Docs/Physics.md), [collider assets](Docs/PhysicsColliderAssets.md), [constraints and characters](Docs/PhysicsConstraintsAndCharacters.md), [locomotion](Docs/Locomotion.md) |
| Animation | [Playback](Docs/Animation/Playback.md), [controllers](Docs/Animation/Controllers.md), [importing animated models](Docs/Animation/ImportingAnimatedModels.md), [editor tools](Docs/Animation/Editor.md), [validation overview](Docs/Animation/README.md) |
| Terrain and navigation | [Terrain authoring](Docs/Terrain.md), [terrain automation](Docs/TerrainAutomation.md), [navigation](Docs/Navigation.md) |
| Audio and scripting | [Audio](Docs/Audio.md), [C# runtime API](Docs/CSharpRuntimeAPI.md), [C# animation API](Docs/Animation/CSharpAnimationAPI.md) |
| Automation and export | [Editor MCP bridge](Docs/EditorMcpBridge.md), [export runtime baseline](Docs/ExportRuntimeBaseline.md) |
| Asset formats | [Meshes](Docs/TerraEngineMeshFile.md), [materials](Docs/TerraEngineMaterialFile.md), [textures](Docs/TerraEngineTextureFile.md), [shaders](Docs/TerraEngineShaderFile.md), [content archives](Docs/TerraEnginePackFile.md) |

## Architecture and source layout

Frame systems declare their scheduler phase and read/write resources. Worker-safe work prepares snapshots or typed packets; live scene and component changes pass through transactional or deferred mutation APIs. Protected flush boundaries determine when later systems can observe those changes, and rejected batches must leave no partial state behind.

Components follow `Component<Derived>` and expose a stable `kComponentName`; engine systems inherit from `SystemsBase`. These contracts keep gameplay, native subsystems, managed callbacks, and editor automation on the same lifecycle and mutation model.

```text
Engine/           Native runtime, managed gameplay API, and engine shaders
Editor/           Native editor, asset compilers, export tooling, and editor shaders
Launcher/         Exported application entry point
External/         Vendored libraries, submodules, and bundled dependency tools
cmake/            Build modules and audited dependency locks
Docs/             Feature guides, format contracts, and validation workflows
Tools/            Engine, editor, export, and dependency maintenance utilities
Tests/Engine/     Engine unit and regression tests
Tests/Editor/     Editor unit and regression tests
Tests/Fixtures/   Checked-in validation inputs and fixture instructions
Tests/Tools/      Benchmarks, stress/soak runners, and evidence collectors
```

## Troubleshooting

- **Linux fails to build or run:** Linux support is experimental and currently unverified. The Linux notes describe the intended porting workflow; use Windows for the recommended setup path.
- **Missing dependency files:** run `git submodule update --init --recursive`. Keep the pinned revisions; replacing them with system packages can invalidate build and runtime assumptions.
- **CMake cannot find Ninja or the compiler:** use a shell with the build tools on `PATH`. On Windows, use the x64 Visual Studio developer environment. For missing `rc.exe` or `mt.exe`, see the Windows SDK troubleshooting in [Testing](Docs/Testing.md#visual-studio-cannot-find-windows-sdk-tools).
- **The editor cannot start its renderer:** check Vulkan 1.3 and `VK_EXT_mesh_shader` support in the selected GPU's driver. Compiling the source does not guarantee the machine can run the interactive renderer.
- **Missing shaders, managed assemblies, or script tools:** rebuild the `install` target and launch from the installed `Build/Editor` directory. Close a running editor before replacing its installed binaries.
- **Dependency downloads fail:** check network access to the pinned archive sources. After caches are populated, `-DTERRAENGINE_DOTNET_SDK_MODE=OFF -DTERRAENGINE_NUGET_CACHE_MODE=OFF` requires those local caches and prevents automatic repair downloads; it does not fetch missing submodules.
- **A test needs an absent project or media file:** consult its fixture README or the relevant section of [Testing](Docs/Testing.md). Local demo assets and prior validation reports are not guaranteed to be published.
- **An older project no longer loads:** consult the subsystem's format and migration guide. Some scene and terrain format changes intentionally have no compatibility reader.

## Contributing

Bug reports and focused contributions are welcome through [Issues](https://github.com/Terra854/TerraEngine/issues) and [pull requests](https://github.com/Terra854/TerraEngine/pulls). Include your host, toolchain, GPU/driver where relevant, reproduction steps, and useful logs.

Agentic coding tools are allowed. The human contributor remains responsible for the entire change, including understanding and reviewing generated code, validating its correctness, and ensuring it follows the project's contribution guidelines. Using an agent does not transfer that responsibility to the tool.

Read [AGENTS.md](AGENTS.md) before changing code. In particular:

- Use C++20, RAII, clear comments, and tab indentation.
- Keep dependencies self-contained under `External/` and shader source in `Engine/Shaders/` or `Editor/Shaders/`.
- Declare scheduler resources and execution boundaries; use typed transactional writes for cross-system mutations.
- Add regression coverage for threading, mutation conflicts, rollback, and visibility timing when those behaviors change.
- Compile the changed code, run the applicable checks, and complete `cmake --build <build-dir> --target install` before finishing a code change.
- Use an area-first commit subject such as `Renderer: Fix shadow cache invalidation.` with a descriptive bullet-point body. PR descriptions should reference the contributor guide and report validation and any deviations.

Keep generated projects and validation output out of commits. Public documentation should link to files actually included in the repository.

### Pull Request Guidelines

- **Keep the scope focused.** Address one clear problem or feature. Separate unrelated cleanup, formatting, and dependency updates so each change can be reviewed on its own.
- **Explain the result.** Use a descriptive title and describe the problem, the resulting behavior, and any architectural tradeoffs. Link related issues when applicable. For bug fixes, include reproduction steps and the expected behavior after the fix.
- **Prepare the change for review.** Review the complete diff, remove temporary debugging code and generated output, and update affected documentation. Use a draft PR while implementation or required validation is still incomplete.
- **Report validation accurately.** List the exact build, test, and install commands you ran and their results. Code changes must compile, pass the applicable checks, and complete the CMake install step. Distinguish automated checks from manual testing, and explicitly identify skipped checks, missing fixtures, or unavailable hardware. Windows is the recommended validation platform; Linux remains experimental.
- **Cover consequential behavior.** Include regression tests for changed rendering or threading behavior and, where applicable, scheduler resource declarations, flush-boundary visibility, mutation conflicts, and atomic rollback. Add screenshots or captures when they help reviewers assess an editor or rendering change.
- **Confirm contributor-guide compliance.** Reference [AGENTS.md](AGENTS.md) in the PR description and confirm self-contained dependencies, separated shader sources, clear comments, and completion of the install step for code changes. Explain any deviation and provide a remediation plan and timeline. Call out API or asset-format breaks and document any required migration or recooking steps.
- **Own the submission.** Agentic coding tools are allowed, but the human contributor must understand and stand behind the entire change, respond to review feedback, and validate revisions before requesting another review. Submit only code and assets you have permission to contribute.

## License and third-party notices

Copyright 2026 Terra854 and TerraEngine contributors.

TerraEngine's original code and documentation are licensed under the **Apache License, Version 2.0**. See [LICENSE](LICENSE) for the full terms. Commercial use, modification, and redistribution are permitted subject to the license's conditions.

Unless explicitly stated otherwise, contributions submitted for inclusion in TerraEngine are provided under the same license.

Third-party libraries and media retain their respective licenses. Consult the notices in `External/` (after initializing submodules) and beside the relevant test fixtures. The install tree also includes third-party notices for the dependencies covered by its packaging rules.
