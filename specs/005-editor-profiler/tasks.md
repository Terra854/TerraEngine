# Tasks: Editor Profiler

**Input**: Design documents from `C:\Users\benti\Desktop\TerraEngine\specs\005-editor-profiler\`
**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `contracts\profiler-ui-contract.md`, `quickstart.md`

**Tests**: Included because the feature touches rendering diagnostics and task/threading behavior, and the TerraEngine guide requires stress or regression coverage for rendering/threading changes.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel with other marked tasks in the same phase because it touches different files and has no incomplete dependency.
- **[Story]**: Maps task to a user story from `spec.md`.
- All task descriptions include exact repository paths.

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Create feature scaffolding and wire it into the build without implementing behavior.

- [X] T001 Create profiler source directories `C:\Users\benti\Desktop\TerraEngine\Engine\Profiling` and `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Profiling`
- [X] T002 [P] Create placeholder profiler documentation in `C:\Users\benti\Desktop\TerraEngine\Docs\EditorProfiler.md`
- [X] T003 Add profiler source/header entries to `C:\Users\benti\Desktop\TerraEngine\CMakeLists.txt` for `TerraEngine-Engine` and `TerraEngine-Editor`
- [X] T004 [P] Create profiler window stubs in `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Windows\ProfilerWindow.h` and `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Windows\ProfilerWindow.cpp`
- [X] T005 [P] Create editor profiler service stubs in `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Profiling\EditorProfilerService.h` and `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Profiling\EditorProfilerService.cpp`
- [X] T006 [P] Create project capture store stubs in `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Profiling\ProfilerCaptureStore.h` and `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Profiling\ProfilerCaptureStore.cpp`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core profiler data structures, lifecycle, and project-boundary safety that all user stories depend on.

**CRITICAL**: No user story work can begin until this phase is complete.

- [X] T007 Define capture enums, settings, metadata, module summaries, frame records, and sample records in `C:\Users\benti\Desktop\TerraEngine\Engine\Profiling\ProfilerCapture.h`
- [X] T008 Implement capture data validation helpers and bounded frame-buffer operations in `C:\Users\benti\Desktop\TerraEngine\Engine\Profiling\ProfilerCapture.cpp`
- [X] T009 Define RAII instrumentation scope helpers for named samples in `C:\Users\benti\Desktop\TerraEngine\Engine\Profiling\ProfilerScope.h`
- [X] T010 Implement thread-local sample collection, frame begin/end capture, selection pinning, and session state transitions in `C:\Users\benti\Desktop\TerraEngine\Engine\Profiling\Profiler.h` and `C:\Users\benti\Desktop\TerraEngine\Engine\Profiling\Profiler.cpp`
- [X] T011 Implement project-root canonicalization and containment checks in `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Profiling\ProfilerCaptureStore.cpp`
- [X] T012 Implement editor service lifecycle, active-project binding, finish-capture save/open orchestration, background task dispatch, and project-close cleanup in `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Profiling\EditorProfilerService.cpp`
- [X] T013 Add profiler service ownership to `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Core\EditorContext.h`
- [X] T014 Register and update the profiler service and profiler window in `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Subsystems\MainEditorSubsystem.cpp` and `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Subsystems\MainEditorSubsystem.h`
- [X] T015 Add beginner-friendly comments explaining profiler frame lifecycle, capture ownership, and project path safety in `C:\Users\benti\Desktop\TerraEngine\Engine\Profiling\Profiler.cpp` and `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Profiling\ProfilerCaptureStore.cpp`

**Checkpoint**: Foundation ready; user story implementation can begin.

---

## Phase 3: User Story 1 - Inspect Editor Performance During a Session (Priority: P1) MVP

**Goal**: Users can open the editor profiler, record live frames, pause/resume, finish a capture, clear capture, select a frame, and inspect a basic timing breakdown.

**Independent Test**: Open the editor with a project, open the Profiler window, record several frames, pause, select a frame, finish the capture, and verify frame timing plus categorized activity remain visible without using a standalone runtime build.

### Tests for User Story 1

- [X] T016 [P] [US1] Add live recording regression coverage for frame begin/end, pause/resume, finish, clear, and selected-frame pinning in `C:\Users\benti\Desktop\TerraEngine\Tests\Profiling\ProfilerSessionTests.cpp`
- [X] T017 [P] [US1] Add editor smoke-test instructions for live capture and selected-frame inspection in `C:\Users\benti\Desktop\TerraEngine\Docs\EditorProfiler.md`

### Implementation for User Story 1

- [X] T018 [US1] Implement Profiler window toolbar controls for record, pause, resume, finish capture, clear, and status messages in `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Windows\ProfilerWindow.cpp`
- [X] T019 [US1] Implement live frame timeline/list rendering and frame selection in `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Windows\ProfilerWindow.cpp`
- [X] T020 [US1] Add engine frame boundary instrumentation around engine system update work in `C:\Users\benti\Desktop\TerraEngine\Engine\Core\MainEngine.cpp`
- [X] T021 [US1] Remove editor update/UI/window profiling so captures report opened-project engine work only
- [X] T022 [US1] Display selected-frame summary, frame duration, completeness, buffer use, and unavailable-data messages in `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Windows\ProfilerWindow.cpp`
- [X] T023 [US1] Wire Profiler window menu visibility through existing Windows menu registration in `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Subsystems\MainEditorSubsystem.cpp`

**Checkpoint**: User Story 1 is independently functional and testable as the MVP.

---

## Phase 4: User Story 2 - Compare Performance Across Categories (Priority: P2)

**Goal**: Users can switch between CPU/engine and task execution modules and see module-specific summaries and selected-frame details. Rendering work is included in CPU/Engine unless dispatched to the thread pool.

**Independent Test**: Record a session that exercises engine systems, rendering, and background tasks, then confirm main-thread rendering appears under CPU/Engine, render thread-pool work appears under Task Execution, and missing data is clearly marked unavailable.

### Tests for User Story 2

- [X] T024 [P] [US2] Add module aggregation regression coverage for CPU/engine and task execution summaries in `C:\Users\benti\Desktop\TerraEngine\Tests\Profiling\ProfilerModuleTests.cpp`
- [X] T025 [P] [US2] Add task/render stress validation instructions for module switching in `C:\Users\benti\Desktop\TerraEngine\Docs\EditorProfiler.md`

### Implementation for User Story 2

- [X] T026 [US2] Add CPU/engine module aggregation and highest-cost contributor sorting in `C:\Users\benti\Desktop\TerraEngine\Engine\Profiling\Profiler.cpp`
- [X] T027 [US2] Feed existing render stats snapshots into the CPU/Engine profiler module in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RenderSystem.cpp` and `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RenderSystem.h`
- [X] T028 [US2] Remove the placeholder memory module until real allocator or GPU memory telemetry exists
- [X] T029 [US2] Add task execution counters and worker/main-thread distinction hooks in `C:\Users\benti\Desktop\TerraEngine\Engine\ThreadPool\ThreadPool.h` and `C:\Users\benti\Desktop\TerraEngine\Engine\ThreadPool\ThreadPool.cpp`
- [X] T030 [US2] Implement module selector tabs and per-module summary/detail panes in `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Windows\ProfilerWindow.cpp`
- [X] T031 [US2] Show unavailable, incomplete, or unsupported module states in `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Windows\ProfilerWindow.cpp`

**Checkpoint**: User Stories 1 and 2 both work independently.

---

## Phase 5: User Story 3 - Use Instrumentation Markers to Find Hotspots (Priority: P3)

**Goal**: Users can inspect named nested sample ranges, repeated sample aggregation, and task/thread context for selected frames.

**Independent Test**: Record a session containing known named operations and confirm the profiler displays names, nesting, durations, ordering, relative cost, repeated occurrences, and worker-task separation.

### Tests for User Story 3

- [X] T032 [P] [US3] Add nested sample and repeated-marker regression coverage in `C:\Users\benti\Desktop\TerraEngine\Tests\Profiling\ProfilerMarkerTests.cpp`
- [X] T033 [P] [US3] Add known-marker validation steps for engine hot paths and task execution in `C:\Users\benti\Desktop\TerraEngine\Docs\EditorProfiler.md`

### Implementation for User Story 3

- [X] T034 [US3] Implement nested sample stack handling and parent/child validation in `C:\Users\benti\Desktop\TerraEngine\Engine\Profiling\Profiler.cpp`
- [X] T035 [US3] Add named profiler scopes to engine system update and task execution paths while excluding editor UI/window code from recorded samples
- [X] T036 [US3] Add named render and game-view sample scopes around render submission and stats capture in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RenderSystem.cpp` and `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Windows\GameViewWindow.cpp`
- [X] T037 [US3] Add task labels and sample context propagation for `ThreadPool` work items in `C:\Users\benti\Desktop\TerraEngine\Engine\ThreadPool\ThreadPool.h` and `C:\Users\benti\Desktop\TerraEngine\Engine\ThreadPool\ThreadPool.cpp`
- [X] T038 [US3] Implement selected-frame marker tree, repeated sample grouping, and cost sorting in `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Windows\ProfilerWindow.cpp`

**Checkpoint**: User Stories 1, 2, and 3 work independently.

---

## Phase 6: User Story 4 - Save and Reopen Profiling Evidence (Priority: P4)

**Goal**: Users can finish a capture and have it immediately saved to `<OpenedProjectRoot>/Profiler/`, opened as the active inspected artifact, selectable from an in-editor list, and reopened later without accessing files outside the active project.

**Independent Test**: Record a session, finish the capture, verify it is saved into the opened project's `Profiler\` folder and automatically opened as the active artifact, close and reopen the editor/project, load the capture from the profiler list, and verify frame summaries, module details, markers, and metadata are restored.

### Tests for User Story 4

- [X] T039 [P] [US4] Add capture store path-containment regression coverage for save/list/load rejection outside the active project in `C:\Users\benti\Desktop\TerraEngine\Tests\Profiling\ProfilerCaptureStoreTests.cpp`
- [X] T040 [P] [US4] Add capture serialization and automatic save/open round-trip regression coverage for metadata, frames, modules, and markers in `C:\Users\benti\Desktop\TerraEngine\Tests\Profiling\ProfilerSerializationTests.cpp`

### Implementation for User Story 4

- [X] T041 [US4] Implement `.teprof` metadata and payload serialization in `C:\Users\benti\Desktop\TerraEngine\Engine\Profiling\ProfilerCapture.cpp`
- [X] T042 [US4] Implement project-scoped save behavior that creates `<OpenedProjectRoot>\Profiler\` in `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Profiling\ProfilerCaptureStore.cpp`
- [X] T043 [US4] Implement project-scoped capture listing that filters valid captures and ignores unsupported files in `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Profiling\ProfilerCaptureStore.cpp`
- [X] T044 [US4] Implement project-scoped capture loading with current-format validation and non-destructive failure handling in `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Profiling\ProfilerCaptureStore.cpp`
- [X] T045 [US4] Dispatch finish-capture save, automatic reopen, manual load, and list refresh operations through `ThreadPool` in `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Profiling\EditorProfilerService.cpp`
- [X] T046 [US4] Add capture list, refresh, automatic save/open progress, and load-error UI without file-dialog loading in `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Windows\ProfilerWindow.cpp`
- [X] T047 [US4] Clear live capture state and invalidate pending old-project capture list results on project close in `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Subsystems\MainEditorSubsystem.cpp`

**Checkpoint**: All user stories are independently functional.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Hardening, documentation, performance validation, and required repository verification.

- [X] T048 [P] Add complete configure/build/install/user validation instructions to `C:\Users\benti\Desktop\TerraEngine\Docs\EditorProfiler.md`
- [X] T049 [P] Add PR compliance notes for self-contained dependencies, shader separation, comments, compile, and install verification to `C:\Users\benti\Desktop\TerraEngine\Docs\EditorProfiler.md`
- [X] T050 Review all touched C++ files for single-tab indentation and beginner-friendly intent comments in `C:\Users\benti\Desktop\TerraEngine\Engine\Profiling`, `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Profiling`, and `C:\Users\benti\Desktop\TerraEngine\Editor\TerraEngine-Editor\Windows\ProfilerWindow.cpp`
- [ ] T051 Run profiler overhead validation against a representative editor project and record results in `C:\Users\benti\Desktop\TerraEngine\Docs\EditorProfiler.md`
- [ ] T052 Run 10,000-frame capture stress validation and record results in `C:\Users\benti\Desktop\TerraEngine\Docs\EditorProfiler.md`
- [X] T053 Run `cmake --preset Debug` from `C:\Users\benti\Desktop\TerraEngine`
- [X] T054 Run `cmake --build out\build\Debug` from `C:\Users\benti\Desktop\TerraEngine` and fix any compile failures before closing the feature
- [X] T055 Run `cmake --build out\build\Debug --target install` from `C:\Users\benti\Desktop\TerraEngine` and verify the install tree is produced before closing the feature
- [ ] T056 Execute the manual smoke test from `C:\Users\benti\Desktop\TerraEngine\specs\005-editor-profiler\quickstart.md`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies; can start immediately.
- **Foundational (Phase 2)**: Depends on Setup completion; blocks all user stories.
- **User Story 1 (Phase 3)**: Depends on Foundational; MVP scope.
- **User Story 2 (Phase 4)**: Depends on Foundational and benefits from US1 UI/session plumbing.
- **User Story 3 (Phase 5)**: Depends on Foundational and benefits from US1 selected-frame detail plus US2 module aggregation.
- **User Story 4 (Phase 6)**: Depends on Foundational and can be implemented after live capture data exists, with finish-capture behavior chaining save and open.
- **Polish (Phase 7)**: Depends on all desired user stories being complete.

### User Story Dependencies

- **US1 (P1)**: No dependency on later stories; delivers MVP live profiler.
- **US2 (P2)**: Can start after Foundational, but final UI integration is easier after US1.
- **US3 (P3)**: Can start after Foundational, but marker display depends on selected-frame UI from US1.
- **US4 (P4)**: Can start after Foundational, but meaningful automatic save/open round-trip validation needs capture data from US1/US2/US3.

### Within Each User Story

- Tests and docs-first validation tasks should be written before implementation tasks in the same story.
- Data structures and services should be implemented before UI rendering.
- Background task dispatch should be implemented before large save/load/stress validation.
- Story checkpoint validation should pass before moving to the next priority when working sequentially.

---

## Parallel Opportunities

- Setup stubs T002, T004, T005, and T006 can run in parallel after T001.
- US1 tests/docs T016 and T017 can run in parallel before US1 implementation.
- US2 tests/docs T024 and T025 can run in parallel before module implementation.
- US3 tests/docs T032 and T033 can run in parallel before marker implementation.
- US4 tests T039 and T040 can run in parallel before persistence implementation.
- Documentation polish T048 and T049 can run in parallel after implementation behavior is known.

---

## Parallel Example: User Story 1

```text
Task: "Add live recording regression coverage for frame begin/end, pause/resume, clear, and selected-frame pinning in C:\Users\benti\Desktop\TerraEngine\Tests\Profiling\ProfilerSessionTests.cpp"
Task: "Add editor smoke-test instructions for live capture and selected-frame inspection in C:\Users\benti\Desktop\TerraEngine\Docs\EditorProfiler.md"
```

## Parallel Example: User Story 2

```text
Task: "Add module aggregation regression coverage for CPU/engine and task execution summaries in C:\Users\benti\Desktop\TerraEngine\Tests\Profiling\ProfilerModuleTests.cpp"
Task: "Add task/render stress validation instructions for module switching in C:\Users\benti\Desktop\TerraEngine\Docs\EditorProfiler.md"
```

## Parallel Example: User Story 4

```text
Task: "Add capture store path-containment regression coverage for save/list/load rejection outside the active project in C:\Users\benti\Desktop\TerraEngine\Tests\Profiling\ProfilerCaptureStoreTests.cpp"
Task: "Add capture serialization and automatic save/open round-trip regression coverage for metadata, frames, modules, and markers in C:\Users\benti\Desktop\TerraEngine\Tests\Profiling\ProfilerSerializationTests.cpp"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1 setup.
2. Complete Phase 2 foundational capture data, lifecycle, project binding, and window/service registration.
3. Complete Phase 3 User Story 1 live capture.
4. Stop and validate live recording, pause/resume, finish capture, clear, selected-frame inspection, and basic timing display.
5. Run compile verification before considering the MVP complete.

### Incremental Delivery

1. Add US1 for live frame capture and selected-frame inspection.
2. Add US2 for category-specific module summaries and details.
3. Add US3 for named nested markers and task/thread context.
4. Add US4 for project-scoped finish-capture save/open plus list/load under `<OpenedProjectRoot>/Profiler/`.
5. Run stress validation, compile, install, and quickstart smoke checks.

### Parallel Team Strategy

1. One developer completes foundational engine profiling data structures.
2. One developer completes editor service/window scaffolding.
3. After the foundation is stable, split work by story: live capture UI, module aggregation, marker instrumentation, and persistence.
4. Keep `ProfilerWindow.cpp`, `EditorProfilerService.cpp`, and `ProfilerCaptureStore.cpp` ownership coordinated to avoid overlapping edits.

## Notes

- Do not modify `C:\Users\benti\Desktop\TerraEngine\External` for this feature.
- Do not add shader code for this feature; if future profiler visualization requires shaders, place them under `C:\Users\benti\Desktop\TerraEngine\Engine\Shaders`.
- Capture loading must never use arbitrary file dialogs or cross-project paths.
- Every touched C++ file must preserve single-tab indentation and include comments explaining intent, inputs, outputs, and tricky logic.
