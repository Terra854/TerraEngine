# US5 validation audit

## Scope and status

This is a Phase 7 acceptance audit for T091--T103 plus the focused US5
validation lanes. It records existing source/test presence and parent-run
RelWithDebInfo results; it does **not** mark any task complete or replace the
separate T104--T111 performance evidence.

## T091--T103 surface audit

| Tasks | Required surface | Evidence found | CTest coverage |
| --- | --- | --- | --- |
| T091 / T096 | Inspector record operations, units/ranges, multi-edit, deferred rebuild | `Tests/Editor/Physics/PhysicsInspectorTests.cpp`; `Editor/TerraEngine-Editor/Windows/InspectorWindow.cpp` | `Editor.PhysicsUS5` includes `Editor.PhysicsInspector` |
| T092 / T098 | Grouped undo/redo, stable records, prefab apply/revert, UUID remap, atomic rejection | `Tests/Editor/Physics/PhysicsPrefabUndoTests.cpp`; `Editor/TerraEngine-Editor/Subsystems/UndoRedoSystem.cpp`; `Engine/Scene/PrefabComposition.cpp` | `Editor.PhysicsUS5` includes `Editor.PhysicsPrefabUndo` |
| T093 / T099 | Physics Project Settings, layer impact preflight, bounded debug settings, protected publication | `Tests/Editor/Physics/PhysicsProjectSettingsTests.cpp`; `Editor/TerraEngine-Editor/Project/ProjectSettingsMetadata.h`; `Editor/TerraEngine-Editor/Windows/ProjectSettingsWindow.cpp` | `Editor.PhysicsUS5` includes `Editor.PhysicsProjectSettings` |
| T094 / T097 / T100 / T101 | Immutable Scene View debug packets, pause retention, bounded history, handles/preview, renderer-unavailable behavior | `Tests/Editor/Physics/PhysicsSceneViewTests.cpp`; `Editor/TerraEngine-Editor/Windows/SceneViewWindow.cpp`; `Editor/TerraEngine-Editor/Windows/SceneViewOverlayTypes.cpp`; `Engine/Physics/PhysicsDiagnostics.cpp`; `Engine/Renderer/RenderSystem.cpp` | `Editor.PhysicsUS5` includes `Physics.SceneView` |
| T095 / T102 / T103 | Immutable diagnostics view, scheduler declarations, profiler scopes/counters, capture serialization, no extra debug work from reads | `Tests/Editor/Physics/PhysicsDiagnosticsWindowTests.cpp`; `Editor/TerraEngine-Editor/Windows/PhysicsDiagnosticsWindow.cpp`; `Engine/Profiling/ProfilerCapture.cpp`; `Editor/TerraEngine-Editor/Windows/ProfilerWindow.cpp` | `Editor.PhysicsUS5` includes `Physics.Diagnostics`; `Engine.PhysicsUS5` covers `Physics.Scheduler,Engine.Profiler` |

The five editor test sources are listed in `Tests/Editor/CMakeLists.txt` and
registered from `Tests/Editor/EditorTests.cpp`. The exact filtered CTest
registrations are:

```text
Editor.PhysicsUS5        Editor.PhysicsInspector,Editor.PhysicsPrefabUndo,
                         Editor.PhysicsProjectSettings,Physics.SceneView,
                         Physics.Diagnostics
Engine.PhysicsUS5        Physics.Scheduler,Engine.Profiler
Editor.PhysicsPerformance Physics.Editor.Performance
Engine.PhysicsPerformance Physics.Performance
```

`Physics.Editor.Performance.TenThousandObjectCreationLoadPlayModeAndPausedNextRemainObservableWithoutRenderer`
also explicitly shuts down `RenderSystem` before submitting debug packets. It
therefore supplies focused renderer-unavailable regression coverage, rather
than assuming an RHI remains alive from another fixture.

## Parent-run RelWithDebInfo validation

The parent session ran the following integrated test-target build:

```powershell
cmake --build --preset RelWithDebInfo --target TerraEngine-EngineTests TerraEngine-EditorTests --parallel 4
```

Result: exit code 0. The initial run completed 77 compile/link steps before
the final edits; the later incremental rerun also exited 0.

Focused US5/performance CTest command:

```powershell
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R "^(Engine\.PhysicsFoundation|Engine\.PhysicsUS5|Engine\.PhysicsPerformance|Editor\.PhysicsUS5|Editor\.PhysicsPerformance|Engine\.VirtualGeometryPerformanceGate)$" --output-on-failure --no-tests=error
```

Result: 6/6 passed, 0 failed, 0.76 s.

Exact focused regression filter:

```text
TERRAENGINE_TEST_FILTER=Engine.Physics.JobAdapter.OneWorkerNestedPumpRunsInline,Renderer.FailureContainment.DeviceLostLatchesFrameSubmission
```

Result: 2/2 passed.

Install gate command, run from the Visual Studio x64 environment:

```powershell
cmake --build --preset RelWithDebInfo --target install --parallel 4
```

Result: exit code 0; the staged install tree is
`out/build/RelWithDebInfo/Build`.

The renderer-unavailable cases were also rerun directly with
`TERRAENGINE_TEST_FILTER` set to
`Physics.SceneView.RendererUnavailableStateKeepsDiagnosticsReadable` and
`Physics.Editor.Performance.TenThousandObjectCreationLoadPlayModeAndPausedNextRemainObservableWithoutRenderer`.
Both passed (2/2); the latter shuts down `RenderSystem` before it submits debug
packets, so this is the exact unavailable-renderer condition rather than a
no-camera approximation.

The audit establishes source presence, focused CTest registration, passing
results, and the RelWithDebInfo install gate. The separate installed-editor
authoring, prefab, Next, diagnostics, restoration, and shutdown acceptance is
recorded in `us5-editor-demo.md`.
