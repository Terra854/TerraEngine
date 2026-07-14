# Tasks: Per-View Buffer Snapshots

**Input**: Design documents from `C:\Users\benti\Desktop\TerraEngine\specs\004-per-view-buffer-snapshots\`
**Prerequisites**: [plan.md](plan.md), [spec.md](spec.md), [research.md](research.md), [data-model.md](data-model.md), [contracts/render-snapshot-contract.md](contracts/render-snapshot-contract.md), [quickstart.md](quickstart.md)

**Tests**: Required by FR-014 and FR-015. Validation tasks are included and should be created before the implementation they verify.

**Organization**: Tasks are grouped by user story so each story can be implemented and validated independently after the shared foundation is complete.

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Prepare documentation and validation scaffolding used by all stories.

- [X] T001 Create `Tools/RenderSnapshotValidation/` directory with placeholder `README.md` and `RenderSnapshotValidation.cpp`
- [X] T002 Add `Tools/RenderSnapshotValidation/CMakeLists.txt` for the new validation executable target
- [X] T003 Register `Tools/RenderSnapshotValidation` in `CMakeLists.txt` so the executable builds and installs with the existing install tree
- [X] T004 [P] Add renderer snapshot verification instructions to `Tools/RenderSnapshotValidation/README.md`
- [X] T005 [P] Create renderer snapshot validation overview in `Docs/RenderSnapshotValidation.md`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Define shared snapshot data, ownership, diagnostics, and RHI contracts that all user stories depend on.

**CRITICAL**: No user story work should begin until this phase is complete.

- [X] T006 Add `RenderOutputKind`, `RenderOutputPriority`, `RenderSnapshotState`, `RenderSnapshotFallbackReason`, and `RenderSnapshotFallbackResult` types in `Engine/Renderer/RenderTypes.h`
- [X] T007 Add `RenderOutputRequest`, `RenderSnapshotHandle`, `RenderSnapshotOwnerSummary`, `RenderSnapshotFallbackEvent`, and `RenderSnapshotDiagnosticsSnapshot` structs in `Engine/Renderer/RenderTypes.h`
- [X] T008 Add beginner-friendly comments in `Engine/Renderer/RenderTypes.h` explaining snapshot ownership, lifetime, inputs, outputs, and fallback intent
- [X] T009 Extend `RHIContext` with snapshot-aware render output entry points and diagnostics accessors in `Engine/Renderer/RHI/RHIContext.h`
- [X] T010 Extend `VulkanContext` public API with snapshot-aware render output overloads and diagnostics accessors in `Engine/Renderer/RHI/Vulkan/VulkanContext.h`
- [X] T011 Add snapshot pool, per-frame budget, owner summaries, fallback event storage, and frame lifetime state to `Engine/Renderer/RHI/Vulkan/VulkanContextImpl.h`
- [X] T012 Add descriptor/buffer range bookkeeping for snapshot-owned lighting, culling, clustering, indirect draw, and diagnostics storage in `Engine/Renderer/RHI/Vulkan/VulkanContextImpl.h`
- [X] T013 Add `RenderSnapshotValidation` placeholder scenarios and command-line parsing in `Tools/RenderSnapshotValidation/RenderSnapshotValidation.cpp`

**Checkpoint**: Shared types, interfaces, and validation target scaffolding exist; story work can start.

---

## Phase 3: User Story 1 - Independent Editor View Rendering (Priority: P1) MVP

**Goal**: Scene and Game views render in the same frame using their own lighting, culling, shadow, and draw data.

**Independent Test**: Open Scene and Game views with different cameras and shadow-relevant lighting; verify each view keeps its own visible objects, selected lights, cluster contents, and shadows.

### Tests for User Story 1

- [X] T014 [P] [US1] Add a dual-view isolation validation scenario in `Tools/RenderSnapshotValidation/RenderSnapshotValidation.cpp`
- [X] T015 [P] [US1] Document expected dual-view validation output in `Tools/RenderSnapshotValidation/README.md`

### Implementation for User Story 1

- [X] T016 [US1] Refactor `RenderSystem::Update` in `Engine/Renderer/RenderSystem.cpp` to build explicit Scene and Game `RenderOutputRequest` records from existing `FrameRenderData`
- [X] T017 [US1] Add output owner metadata to Scene and Game submissions in `Engine/Renderer/RenderSystem.cpp`
- [X] T018 [US1] Implement snapshot acquisition and release helpers in `Engine/Renderer/RHI/Vulkan/VulkanContext.cpp`
- [X] T019 [US1] Move lighting payload upload from frame-global ownership to snapshot-owned ranges in `Engine/Renderer/RHI/Vulkan/VulkanContext.cpp`
- [X] T020 [US1] Move frame culling data and meshlet counter ownership from frame-global slots to snapshot-owned ranges in `Engine/Renderer/RHI/Vulkan/VulkanContext.cpp`
- [X] T021 [US1] Move indirect draw command upload from frame-global ownership to snapshot-owned ranges in `Engine/Renderer/RHI/Vulkan/VulkanContext.cpp`
- [X] T022 [US1] Capture per-output shadow assignment metadata inside the active snapshot in `Engine/Renderer/RHI/Vulkan/VulkanContext.cpp`
- [X] T023 [US1] Update command recording in `Engine/Renderer/RHI/Vulkan/VulkanContext.cpp` to bind snapshot-owned lighting, culling, cluster, shadow, and indirect draw resources
- [X] T024 [US1] Preserve Scene and Game diagnostic compatibility while sourcing data from snapshot owners in `Engine/Renderer/RHI/Vulkan/VulkanContext.cpp`
- [X] T025 [US1] Run `Tools/RenderSnapshotValidation/RenderSnapshotValidation.cpp` dual-view scenario and fix any US1 correctness failures

**Checkpoint**: Scene/Game dual-view correctness is independently demonstrable.

---

## Phase 4: User Story 2 - Remove Broad Editor Serialization (Priority: P1)

**Goal**: Normal warmed-up dual-view editor rendering no longer depends on broad between-view synchronization for correctness.

**Independent Test**: Run both editor views and confirm diagnostics/frame capture show no mandatory broad synchronization between view recordings while Scene/Game correctness remains intact.

### Tests for User Story 2

- [X] T026 [P] [US2] Add validation assertions for no broad fallback synchronization during normal dual-view frames in `Tools/RenderSnapshotValidation/RenderSnapshotValidation.cpp`
- [X] T027 [P] [US2] Add a warmed-up dual-view fallback counter expectation to `Tools/RenderSnapshotValidation/README.md`

### Implementation for User Story 2

- [X] T028 [US2] Remove the normal editor `vkQueueSubmit` plus wait-idle path before the next editor view records in `Engine/Renderer/RHI/Vulkan/VulkanContext.cpp`
- [X] T029 [US2] Replace broad editor serialization with snapshot dependency checks and narrow safety fallback reporting in `Engine/Renderer/RHI/Vulkan/VulkanContext.cpp`
- [X] T030 [US2] Ensure `EndFrame` submits recorded snapshot-backed command buffers without relying on later view uploads in `Engine/Renderer/RHI/Vulkan/VulkanContext.cpp`
- [X] T031 [US2] Add fallback synchronization counters and reasons to `RenderSnapshotDiagnosticsSnapshot` population in `Engine/Renderer/RHI/Vulkan/VulkanContext.cpp`
- [X] T032 [US2] Update `Tools/RenderSnapshotValidation/RenderSnapshotValidation.cpp` to fail if normal dual-view frames use broad fallback synchronization
- [ ] T033 [US2] Run dual-view editor manual verification from `specs/004-per-view-buffer-snapshots/quickstart.md`

**Checkpoint**: Dual-view rendering remains correct without normal broad editor serialization.

---

## Phase 5: User Story 3 - Diagnose Snapshot Usage and Regressions (Priority: P2)

**Goal**: Developers can inspect snapshot owners, active counts, pressure, isolation status, and fallback reasons.

**Independent Test**: Enable diagnostics during dual-view rendering and confirm active owners, resource pressure, fallback events, and isolation status are visible.

### Tests for User Story 3

- [X] T034 [P] [US3] Add diagnostics schema validation to `Tools/RenderSnapshotValidation/RenderSnapshotValidation.cpp`
- [X] T035 [P] [US3] Add diagnostics expected-field documentation to `Tools/RenderSnapshotValidation/README.md`

### Implementation for User Story 3

- [X] T036 [US3] Add snapshot diagnostics storage and thread-safe copy access in `Engine/Renderer/RHI/Vulkan/VulkanContextImpl.h`
- [X] T037 [US3] Implement `GetLatestRenderSnapshotDiagnostics` or equivalent diagnostics accessor in `Engine/Renderer/RHI/Vulkan/VulkanContext.cpp`
- [X] T038 [US3] Surface snapshot diagnostics through `RHIContext` and `RenderSystem` in `Engine/Renderer/RHI/RHIContext.h`, `Engine/Renderer/RenderSystem.h`, and `Engine/Renderer/RenderSystem.cpp`
- [X] T039 [US3] Include owner summaries, target kind, target size, active count, peak count, and fallback events in `Engine/Renderer/RHI/Vulkan/VulkanContext.cpp`
- [X] T040 [US3] Add editor-facing diagnostic plumbing without changing existing Game/Scene stat callers in `Engine/Renderer/RenderSystem.cpp`
- [X] T041 [US3] Run diagnostics validation in `Tools/RenderSnapshotValidation/RenderSnapshotValidation.cpp` and fix mismatches

**Checkpoint**: Snapshot usage and fallback regressions are diagnosable without frame-capture tools.

---

## Phase 6: User Story 4 - Prepare Future Multi-Camera Targets (Priority: P2)

**Goal**: The snapshot model supports a main camera and at least one offscreen camera target in the same frame without cross-output data contamination.

**Independent Test**: Run the validation scenario with a primary camera output and synthetic offscreen camera output; confirm both outputs have independent snapshot ownership and isolation checks pass.

### Tests for User Story 4

- [X] T042 [P] [US4] Add a synthetic offscreen camera-output scenario in `Tools/RenderSnapshotValidation/RenderSnapshotValidation.cpp`
- [X] T043 [P] [US4] Add snapshot budget exhaustion and optional-output skip scenarios in `Tools/RenderSnapshotValidation/RenderSnapshotValidation.cpp`
- [X] T044 [P] [US4] Document future multi-camera validation commands in `Tools/RenderSnapshotValidation/README.md`

### Implementation for User Story 4

- [X] T045 [US4] Generalize snapshot owners beyond `Game` and `Scene` in `Engine/Renderer/RenderTypes.h` and `Engine/Renderer/RHI/Vulkan/VulkanContext.h`
- [X] T046 [US4] Add render output target descriptors for future offscreen camera outputs in `Engine/Renderer/RenderTypes.h`
- [X] T047 [US4] Update Vulkan offscreen target handling to accept validation-only offscreen camera outputs in `Engine/Renderer/RHI/Vulkan/VulkanContext.cpp`
- [X] T048 [US4] Ensure outputs with different target sizes or aspect ratios receive independent culling and clustered-lighting payloads in `Engine/Renderer/RHI/Vulkan/VulkanContext.cpp`
- [X] T049 [US4] Implement required-output fallback and optional-output skip behavior for exhausted snapshot budgets in `Engine/Renderer/RHI/Vulkan/VulkanContext.cpp`
- [X] T050 [US4] Connect the synthetic offscreen output validation path to the renderer through `Tools/RenderSnapshotValidation/RenderSnapshotValidation.cpp`
- [X] T051 [US4] Run multi-camera validation from `specs/004-per-view-buffer-snapshots/quickstart.md`

**Checkpoint**: Future RenderTexture-style camera outputs have a validated snapshot ownership path.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Final docs, cleanup, performance checks, and required build/install verification.

- [X] T052 [P] Update `specs/004-per-view-buffer-snapshots/quickstart.md` if final executable names or paths differ from the plan
- [X] T053 [P] Update `Docs/RenderSnapshotValidation.md` with final dual-view and multi-camera verification steps
- [X] T054 Review comments in touched files under `Engine/Renderer/` and `Tools/RenderSnapshotValidation/` for beginner-friendly intent, inputs, outputs, and tricky lifetime logic
- [X] T055 Verify all touched C++ files use single-tab indentation where indentation is required
- [X] T056 Run `cmake -S C:\Users\benti\Desktop\TerraEngine -B C:\Users\benti\Desktop\TerraEngine\out\build\render-snapshots -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=C:/Users/benti/Desktop/TerraEngine/out/build/render-snapshots/install`
- [X] T057 Run `cmake --build C:\Users\benti\Desktop\TerraEngine\out\build\render-snapshots --config Debug --target install`
- [X] T058 Run `C:\Users\benti\Desktop\TerraEngine\out\build\render-snapshots\Build\Tools\RenderSnapshotValidation\RenderSnapshotValidation.exe`
- [ ] T059 Run manual Scene/Game editor verification from `specs/004-per-view-buffer-snapshots/quickstart.md`
- [X] T060 Record final validation results and any fallback deviations in `specs/004-per-view-buffer-snapshots/quickstart.md`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1 Setup**: No dependencies.
- **Phase 2 Foundational**: Depends on Phase 1 and blocks all user stories.
- **Phase 3 US1**: Depends on Phase 2. This is the MVP correctness slice.
- **Phase 4 US2**: Depends on US1 because broad synchronization should only be removed after snapshot-backed correctness exists.
- **Phase 5 US3**: Depends on Phase 2 and can proceed in parallel with US1/US2 after shared diagnostics types exist, but final validation depends on US1/US2 behavior.
- **Phase 6 US4**: Depends on Phase 2 and benefits from US1 snapshot implementation; can proceed once owner abstraction is stable.
- **Phase 7 Polish**: Depends on all selected user stories.

### User Story Dependencies

- **US1 Independent Editor View Rendering**: MVP; no dependency on other stories after foundation.
- **US2 Remove Broad Editor Serialization**: Depends on US1.
- **US3 Diagnose Snapshot Usage and Regressions**: Can start after foundation; final checks depend on US1 and US2.
- **US4 Prepare Future Multi-Camera Targets**: Can start after foundation; should integrate with US1 snapshot ownership.

### Parallel Opportunities

- T004 and T005 can run in parallel with CMake scaffolding after T001.
- T014 and T015 can run in parallel.
- T026 and T027 can run in parallel.
- T034 and T035 can run in parallel.
- T042, T043, and T044 can run in parallel.
- T052 and T053 can run in parallel after implementation settles.

## Parallel Execution Examples

### User Story 1

```text
Task: "Add a dual-view isolation validation scenario in Tools/RenderSnapshotValidation/RenderSnapshotValidation.cpp"
Task: "Document expected dual-view validation output in Tools/RenderSnapshotValidation/README.md"
```

### User Story 3

```text
Task: "Add diagnostics schema validation to Tools/RenderSnapshotValidation/RenderSnapshotValidation.cpp"
Task: "Add diagnostics expected-field documentation to Tools/RenderSnapshotValidation/README.md"
```

### User Story 4

```text
Task: "Add a synthetic offscreen camera-output scenario in Tools/RenderSnapshotValidation/RenderSnapshotValidation.cpp"
Task: "Add snapshot budget exhaustion and optional-output skip scenarios in Tools/RenderSnapshotValidation/RenderSnapshotValidation.cpp"
Task: "Document future multi-camera validation commands in Tools/RenderSnapshotValidation/README.md"
```

## Implementation Strategy

### MVP First

1. Complete Phase 1 and Phase 2.
2. Complete Phase 3 for Scene/Game snapshot-backed correctness.
3. Validate US1 independently with `RenderSnapshotValidation` dual-view scenario.
4. Stop before removing broad synchronization unless US1 correctness passes.

### Incremental Delivery

1. US1: Isolate Scene/Game data and prove correctness.
2. US2: Remove normal broad editor serialization and prove fallback counters stay clear.
3. US3: Expose diagnostics for ongoing regression detection.
4. US4: Add future offscreen camera-output validation and budget behavior.
5. Polish: Build, install, run validation, and manually verify editor behavior.

### Validation Gate

The feature is not complete until:

- `cmake --build <build-dir> --config Debug --target install` succeeds.
- `RenderSnapshotValidation.exe` passes.
- Manual Scene/Game editor verification passes.
- Any shader changes, if introduced, remain in `Engine/Shaders/`.
