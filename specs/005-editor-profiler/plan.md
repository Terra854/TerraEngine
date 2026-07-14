# Implementation Plan: Editor Profiler

**Branch**: `005-editor-profiler` | **Date**: 2026-04-25 | **Spec**: [C:\Users\benti\Desktop\TerraEngine\specs\005-editor-profiler\spec.md](C:\Users\benti\Desktop\TerraEngine\specs\005-editor-profiler\spec.md)  
**Input**: Feature specification from `C:\Users\benti\Desktop\TerraEngine\specs\005-editor-profiler\spec.md`

**Note**: This template is filled in by the `/speckit.plan` command. See `C:\Users\benti\Desktop\TerraEngine\.specify\templates\plan-template.md` for the execution workflow.

## Summary

Add an editor-only profiler tool that records live engine frame timing, module summaries, named sample ranges, task activity, render statistics, and saved capture artifacts for the currently opened project. The implementation will add low-overhead engine instrumentation, an editor profiler service with bounded capture buffers, an ImGui editor window for recording and inspection, and project-scoped capture persistence under `<OpenedProjectRoot>/Profiler/`; finishing a recording immediately saves the capture, refreshes the list, and opens the newly saved artifact.

## Technical Context

**Language/Version**: C++20 for native engine/editor code; existing managed editor assembly targets .NET 10.0 but is not required for the first profiler window.  
**Primary Dependencies**: Existing CMake/Ninja build, ImGui/cimgui editor UI, Vulkan renderer diagnostics, nlohmann/json, C++ standard library filesystem/chrono/atomic containers, TerraEngine `ThreadPool`. No new third-party dependency is planned.  
**Storage**: Local project files only, written to, immediately reopened from, and listed from `<OpenedProjectRoot>/Profiler/`; proposed capture extension `.teprof` with JSON metadata and compact frame/sample payloads.  
**Testing**: CMake build target verification, `cmake --build <build-dir> --target install`, focused native unit/regression tests where test harness exists, editor manual smoke tests using a real opened project, and stress captures with 10,000+ frames.  
**Target Platform**: TerraEngine desktop editor, primarily Windows x64 based on the current build/install scripts and bundled Windows DXC/.NET runtime layout.  
**Project Type**: Desktop engine/editor application with native runtime/editor and hosted managed editor support.  
**Performance Goals**: Default capture overhead below 5% average frame-time increase; frame entries visible within 2 seconds for at least 95% of active recording; 10,000-frame captures remain navigable without editor stalls over 1 second.  
**Constraints**: Profiler UI and capture management are editor-only, but captured samples include only engine systems that run for the opened project; capture read/write must stay inside the opened project directory; ending a recording must save then open the new capture; loading uses an in-editor list from `<OpenedProjectRoot>/Profiler/`; no file dialog or cross-project browsing; no `External/` changes; no embedded shader source; comments must explain intent and tricky data-flow; implementation must preserve task-based concurrency.  
**Scale/Scope**: One profiler window, one capture service, two initial modules (CPU/engine and task execution), bounded rolling capture buffer, saved/reopened local captures, and instrumentation hooks in selected engine hot paths. Rendering work is shown under CPU/Engine unless it runs as thread-pool work.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- Task-based concurrency: PASS. Capture collection must keep per-frame writes lightweight and run serialization, capture list refresh, and expensive aggregation through `ThreadPool` tasks so the editor UI does not block on disk or heavy summarization.
- `External/` changes: PASS. No new dependencies are planned and no files under `C:\Users\benti\Desktop\TerraEngine\External` should be edited.
- Shader separation and installed-tree assumptions: PASS. No shader changes are planned. Any future profiler visualization shaders would need dedicated files under `Engine/Shaders/`, but this plan uses ImGui drawing only.
- Build/install verification: PASS. Implementation tasks must end with a successful compile and `cmake --build out/build/Debug --target install` or equivalent install step before completion.
- Editor system lifecycle: PASS. If the profiler is introduced as a system, it must inherit from `SystemsBase`; otherwise the editor profiler service should be owned by `MainEditorSubsystem`/`EditorContext` with explicit initialize/update/shutdown lifecycle.

## Project Structure

### Documentation (this feature)

```text
C:\Users\benti\Desktop\TerraEngine\specs\005-editor-profiler\
|-- plan.md
|-- research.md
|-- data-model.md
|-- quickstart.md
|-- contracts\
|   `-- profiler-ui-contract.md
`-- tasks.md
```

### Source Code (repository root)

```text
C:\Users\benti\Desktop\TerraEngine\
|-- Engine\
|   |-- Profiling\
|   |   |-- Profiler.h
|   |   |-- Profiler.cpp
|   |   |-- ProfilerCapture.h
|   |   |-- ProfilerCapture.cpp
|   |   `-- ProfilerScope.h
|   |-- Renderer\
|   |   `-- RenderSystem.*            # expose/record existing render stats into profiler samples
|   |-- ThreadPool\
|   |   `-- ThreadPool.*              # add optional task profiling counters without changing task API shape unless needed
|   `-- Core\
|       `-- MainEngine.*              # frame boundary instrumentation if required
|-- Editor\
|   `-- TerraEngine-Editor\
|       |-- Core\
|       |   `-- EditorContext.h       # profiler service pointer/state if editor-owned
|       |-- Subsystems\
|       |   `-- MainEditorSubsystem.* # create/update/shutdown profiler window/service
|       |-- Windows\
|       |   |-- ProfilerWindow.h
|       |   `-- ProfilerWindow.cpp
|       `-- Profiling\
|           |-- EditorProfilerService.h
|           |-- EditorProfilerService.cpp
|           |-- ProfilerCaptureStore.h
|           `-- ProfilerCaptureStore.cpp
|-- Docs\
|   `-- EditorProfiler.md
`-- CMakeLists.txt
```

**Structure Decision**: Add reusable, low-overhead capture primitives under `C:\Users\benti\Desktop\TerraEngine\Engine\Profiling` so engine code can emit samples without depending on editor UI. Keep project-scoped persistence and ImGui presentation under `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Profiling` and `...\Windows` because saving/loading is editor-only and depends on `ProjectManager::GetProjectRoot()`.

## Complexity Tracking

No constitution violations are planned. The feature intentionally avoids new dependencies, shader work, runtime/player UI, cross-project browsing, and arbitrary file-dialog loading.

## Phase 0: Research Summary

See [C:\Users\benti\Desktop\TerraEngine\specs\005-editor-profiler\research.md](C:\Users\benti\Desktop\TerraEngine\specs\005-editor-profiler\research.md).

Key decisions:

- Use native C++20 instrumentation with RAII scopes and frame-boundary capture.
- Keep live capture in bounded in-memory buffers with immutable frame snapshots for selected-frame inspection.
- Save/load captures only through `<OpenedProjectRoot>/Profiler/`, with capture completion chaining save, list refresh, and open-new-artifact behavior.
- Run save/load/list refresh and aggregation work on `ThreadPool`.

## Phase 1: Design Summary

See:

- [C:\Users\benti\Desktop\TerraEngine\specs\005-editor-profiler\data-model.md](C:\Users\benti\Desktop\TerraEngine\specs\005-editor-profiler\data-model.md)
- [C:\Users\benti\Desktop\TerraEngine\specs\005-editor-profiler\contracts\profiler-ui-contract.md](C:\Users\benti\Desktop\TerraEngine\specs\005-editor-profiler\contracts\profiler-ui-contract.md)
- [C:\Users\benti\Desktop\TerraEngine\specs\005-editor-profiler\quickstart.md](C:\Users\benti\Desktop\TerraEngine\specs\005-editor-profiler\quickstart.md)

The design centers on a `ProfilingSession` with bounded live frames, per-frame module summaries, nested instrumentation samples, capture metadata, and an automatic save/open transition when recording ends. The editor window talks to an editor service instead of reading files directly, and the service talks to a capture store that canonicalizes all paths against the active project root before any filesystem operation.

## Constitution Check (Post-Design)

- Task-based concurrency: PASS. Disk operations and expensive aggregation are assigned to `ThreadPool`; per-thread sample collection remains local and lightweight during frame execution.
- `External/` changes: PASS. No vendored dependency changes are required.
- Shader separation and installed-tree assumptions: PASS. No shader files or embedded shader strings are introduced; quickstart uses CMake install.
- Build/install verification: PASS. Quickstart and later tasks require configure, build, and install verification.
- Maintainability/comments: PASS. Planned implementation files must include beginner-friendly comments for frame capture lifecycle, path-boundary checks, lock-free or mutex-protected data transfer, and capture serialization format.
