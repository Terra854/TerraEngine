# Quickstart: Editor Profiler

## Prerequisites

- Work from `C:\Users\benti\Desktop\TerraEngine`.
- Use the existing vendored dependencies under `External/`; do not install or require global third-party libraries.
- Use CMake presets from `CMakePresets.json`.

## Configure

```powershell
cmake --preset Debug
```

## Build

```powershell
cmake --build out/build/Debug
```

## Install

The install step is mandatory for TerraEngine tasks after code changes because the editor/runtime asset layout is produced during install.

```powershell
cmake --build out/build/Debug --target install
```

## Manual Editor Smoke Test

1. Start the installed editor from `C:\Users\benti\Desktop\TerraEngine\out\build\Debug\Build\Editor\TerraEngine-Editor.exe` or the installed editor path produced by the local generator.
2. Open or create a TerraEngine project.
3. Open the Profiler window from the editor window menu.
4. Start recording and interact with Scene View, Game View, the hierarchy, and build/profile-heavy editor operations.
5. Select a frame while recording continues and confirm the selected frame remains stable.
6. Switch between CPU/engine and task execution modules.
7. Finish the capture and confirm a file appears under `<OpenedProjectRoot>\Profiler\`.
8. Confirm the profiler automatically opens the newly saved capture as the active inspected artifact.
9. Refresh the capture list and reopen the saved capture from the list.
10. Confirm there is no arbitrary file dialog path for loading captures.
11. Close the project, open a different project, and confirm captures from the first project are not listed.

## Stress/Regression Checks

- Record at least 10,000 frames and verify timeline navigation and selected-frame detail remain responsive.
- Record while background tasks are active and verify task execution data is separated from main-thread engine activity.
- Finish a large capture and verify the automatic save/open workflow completes while the editor remains responsive.
- Place unsupported files in `<OpenedProjectRoot>\Profiler\` and verify they are ignored or clearly marked unsupported.
- Attempt to load a stale capture list entry after switching projects and verify the old project path is rejected.

## Completion Criteria

- `cmake --build out/build/Debug` succeeds.
- `cmake --build out/build/Debug --target install` succeeds and produces the expected install tree.
- The manual smoke test passes for an opened project.
- The profiler never reads or writes capture files outside the opened project directory.
