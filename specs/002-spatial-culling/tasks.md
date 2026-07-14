# Tasks: Spatial Partitioning and Object Culling

**Input**: Design documents from `C:/Users/benti/Desktop/TerraEngine/specs/002-spatial-culling/`
**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `contracts/spatial-query-contract.md`, `quickstart.md`

**Tests**: Required by the feature specification. Validation tasks are listed before implementation work within each user story and should fail before the matching implementation is complete.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel because it touches separate files or has no dependency on incomplete tasks.
- **[Story]**: User story label for traceability.
- Every task includes an exact repository file path.

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Create the feature's source, validation, and documentation locations without changing runtime behavior.

- [x] T001 Create the spatial source file placeholders in `C:/Users/benti/Desktop/TerraEngine/Engine/Scene/SpatialBounds.h`, `C:/Users/benti/Desktop/TerraEngine/Engine/Scene/SpatialQuery.h`, `C:/Users/benti/Desktop/TerraEngine/Engine/Scene/SpatialDiagnostics.h`, `C:/Users/benti/Desktop/TerraEngine/Engine/Scene/SpatialIndex.h`, and `C:/Users/benti/Desktop/TerraEngine/Engine/Scene/SpatialIndex.cpp`
- [x] T002 [P] Create the validation tool directory and stub files in `C:/Users/benti/Desktop/TerraEngine/Tools/SpatialValidation/SpatialValidation.cpp` and `C:/Users/benti/Desktop/TerraEngine/Tools/SpatialValidation/README.md`
- [x] T003 [P] Create the feature documentation stub in `C:/Users/benti/Desktop/TerraEngine/Docs/SpatialPartitioningAndCulling.md`
- [x] T004 Add the new spatial engine files and `SpatialValidation` executable target to `C:/Users/benti/Desktop/TerraEngine/CMakeLists.txt`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Define shared data structures, bounds derivation, and diagnostics needed by every user story.

**CRITICAL**: No user story work can begin until this phase is complete.

- [x] T005 Define `SpatialAABB`, `SpatialSphere`, `SpatialRay`, `SpatialFrustum`, bounds-state enums, finite-value validation, expansion, transform, overlap, and ray helper functions in `C:/Users/benti/Desktop/TerraEngine/Engine/Scene/SpatialBounds.h`
- [x] T006 Define `SpatialObjectRecord`, category flags, query filter options, query result structs, and deterministic sort options in `C:/Users/benti/Desktop/TerraEngine/Engine/Scene/SpatialQuery.h`
- [x] T007 [P] Define `SpatialDiagnosticsSnapshot` and query/update timing counters in `C:/Users/benti/Desktop/TerraEngine/Engine/Scene/SpatialDiagnostics.h`
- [x] T008 Add local mesh bounds fields and beginner-friendly intent comments to the in-memory `Mesh` representation in `C:/Users/benti/Desktop/TerraEngine/Engine/File Formats/TerraEngineMeshFile.h`
- [x] T009 Populate mesh local bounds from TEMF meshlet bounds, including invalid/empty fallback states, in `C:/Users/benti/Desktop/TerraEngine/Engine/Resource/ResourceManagerMesh.cpp`
- [x] T010 Expose read-only mesh bounds accessors needed by render and validation code in `C:/Users/benti/Desktop/TerraEngine/Engine/Resource/ResourceManager.h`
- [x] T011 Add object lifecycle dirty-notification hooks for create, remove, active-state, static-state, and parenting changes in `C:/Users/benti/Desktop/TerraEngine/Engine/Core/Object/ObjectManager.h` and `C:/Users/benti/Desktop/TerraEngine/Engine/Core/Object/ObjectManager.cpp`
- [x] T012 Add `spatialObjectId`, fallback bounds policy comments, and reflection metadata required for spatial participation to `C:/Users/benti/Desktop/TerraEngine/Engine/Components/MeshRenderComponent.h`

**Checkpoint**: Foundation ready; story implementation can begin.

---

## Phase 3: User Story 1 - Render Large Scenes Efficiently (Priority: P1) MVP

**Goal**: Rendering can request a conservative camera-visible candidate set before draw command generation.

**Independent Test**: Load or synthesize at least 10,000 distributed renderables, move a camera through a small visible region, and verify that the candidate set is reduced while visible output stays correct.

### Tests for User Story 1

- [x] T013 [P] [US1] Add validation cases for 10,000 distributed renderables, frustum inclusion, boundary inclusion, and invalid-bound fallback in `C:/Users/benti/Desktop/TerraEngine/Tools/SpatialValidation/SpatialValidation.cpp`
- [x] T014 [P] [US1] Document the large-scene rendering validation procedure and expected candidate-count thresholds in `C:/Users/benti/Desktop/TerraEngine/Tools/SpatialValidation/README.md`

### Implementation for User Story 1

- [x] T015 [US1] Implement loose-octree node storage, insertion, removal, large-object fallback storage, and deterministic object ordering in `C:/Users/benti/Desktop/TerraEngine/Engine/Scene/SpatialIndex.h` and `C:/Users/benti/Desktop/TerraEngine/Engine/Scene/SpatialIndex.cpp`
- [x] T016 [US1] Implement frustum query traversal, conservative fallback inclusion, rejected-count diagnostics, and stable result sorting in `C:/Users/benti/Desktop/TerraEngine/Engine/Scene/SpatialIndex.cpp`
- [x] T017 [US1] Build world-space renderable records from `Transform` and `MeshRenderComponent` snapshots before draw command generation in `C:/Users/benti/Desktop/TerraEngine/Engine/Renderer/RenderSystem.cpp`
- [x] T018 [US1] Replace renderable draw-command generation input with the spatial visible candidate set while preserving the existing task-parallel draw-command build in `C:/Users/benti/Desktop/TerraEngine/Engine/Renderer/RenderSystem.cpp`
- [x] T019 [US1] Add spatial candidate counters to frame data and render stats structures in `C:/Users/benti/Desktop/TerraEngine/Engine/Renderer/RenderTypes.h`
- [x] T020 [US1] Expose latest spatial diagnostics through `RenderSystem` accessors in `C:/Users/benti/Desktop/TerraEngine/Engine/Renderer/RenderSystem.h` and `C:/Users/benti/Desktop/TerraEngine/Engine/Renderer/RenderSystem.cpp`
- [x] T021 [US1] Wire the US1 validation scenarios to fail on incorrect visibility, excessive candidates, or missing fallback inclusion in `C:/Users/benti/Desktop/TerraEngine/Tools/SpatialValidation/SpatialValidation.cpp`

**Checkpoint**: User Story 1 is independently functional and is the MVP.

---

## Phase 4: User Story 2 - Query Nearby Scene Objects (Priority: P2)

**Goal**: Engine systems can request area, sphere, and ray candidates through a shared spatial query contract.

**Independent Test**: Place objects in known world-space regions and verify area, sphere, and ray queries return expected object identities while excluding unrelated or inactive objects by default.

### Tests for User Story 2

- [x] T022 [P] [US2] Add area-query validation cases for inside, outside, boundary-overlapping, inactive, and diagnostic-included objects in `C:/Users/benti/Desktop/TerraEngine/Tools/SpatialValidation/SpatialValidation.cpp`
- [x] T023 [P] [US2] Add sphere-query and point-query validation cases for radius zero, invalid radius, and category filtering in `C:/Users/benti/Desktop/TerraEngine/Tools/SpatialValidation/SpatialValidation.cpp`
- [x] T024 [P] [US2] Add ray-query validation cases for nearest-first sorting, invalid rays, maximum distance, and broad-phase candidate behavior in `C:/Users/benti/Desktop/TerraEngine/Tools/SpatialValidation/SpatialValidation.cpp`

### Implementation for User Story 2

- [x] T025 [US2] Implement AABB area query traversal, active-state filtering, category filtering, fallback inclusion, and deterministic ordering in `C:/Users/benti/Desktop/TerraEngine/Engine/Scene/SpatialIndex.cpp`
- [x] T026 [US2] Implement sphere and point query traversal with non-finite and negative-radius rejection in `C:/Users/benti/Desktop/TerraEngine/Engine/Scene/SpatialIndex.cpp`
- [x] T027 [US2] Implement ray query broad-phase traversal, candidate distance calculation, maximum-distance filtering, and nearest-first sorting in `C:/Users/benti/Desktop/TerraEngine/Engine/Scene/SpatialIndex.cpp`
- [x] T028 [US2] Publish the shared spatial query contract comments and examples in `C:/Users/benti/Desktop/TerraEngine/Engine/Scene/SpatialQuery.h`
- [x] T029 [US2] Update `C:/Users/benti/Desktop/TerraEngine/specs/002-spatial-culling/contracts/spatial-query-contract.md` if implementation behavior differs from the planned query contract

**Checkpoint**: User Story 2 works independently through validation-tool queries.

---

## Phase 5: User Story 3 - Keep Dynamic Scenes Correct (Priority: P3)

**Goal**: Spatial results remain correct when objects move, change state, reparent, appear, or disappear.

**Independent Test**: Repeatedly move, enable, disable, create, delete, and reparent objects while validating that query results match the current scene state with no stale, missing, or duplicate entries.

### Tests for User Story 3

- [x] T030 [P] [US3] Add dynamic movement and region-transfer validation cases for at least 1,000 moving objects in `C:/Users/benti/Desktop/TerraEngine/Tools/SpatialValidation/SpatialValidation.cpp`
- [x] T031 [P] [US3] Add create, destroy, active-state, static-state, and duplicate-result validation cases in `C:/Users/benti/Desktop/TerraEngine/Tools/SpatialValidation/SpatialValidation.cpp`
- [x] T032 [P] [US3] Add parent-child transform update validation cases in `C:/Users/benti/Desktop/TerraEngine/Tools/SpatialValidation/SpatialValidation.cpp`

### Implementation for User Story 3

- [x] T033 [US3] Implement dirty-object tracking, batch remove/reinsert, and frame-stable update application in `C:/Users/benti/Desktop/TerraEngine/Engine/Scene/SpatialIndex.h` and `C:/Users/benti/Desktop/TerraEngine/Engine/Scene/SpatialIndex.cpp`
- [x] T034 [US3] Integrate ObjectManager lifecycle dirty hooks with the spatial index update path in `C:/Users/benti/Desktop/TerraEngine/Engine/Renderer/RenderSystem.cpp`
- [x] T035 [US3] Ensure parent-child world transform changes refresh child spatial membership without stale results in `C:/Users/benti/Desktop/TerraEngine/Engine/Renderer/RenderSystem.cpp`
- [x] T036 [US3] Add deterministic duplicate-removal and destroyed-object exclusion safeguards in `C:/Users/benti/Desktop/TerraEngine/Engine/Scene/SpatialIndex.cpp`
- [x] T037 [US3] Add dynamic update counters and stale-entry diagnostics to `C:/Users/benti/Desktop/TerraEngine/Engine/Scene/SpatialDiagnostics.h` and `C:/Users/benti/Desktop/TerraEngine/Engine/Renderer/RenderTypes.h`

**Checkpoint**: User Story 3 dynamic correctness is independently validated.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Documentation, diagnostics presentation, performance checks, and mandatory verification.

- [x] T038 [P] Add end-user documentation, assumptions, limitations, and validation instructions to `C:/Users/benti/Desktop/TerraEngine/Docs/SpatialPartitioningAndCulling.md`
- [x] T039 [P] Expand `C:/Users/benti/Desktop/TerraEngine/Tools/SpatialValidation/README.md` with clean-checkout configure, build, install, and validation commands
- [x] T040 Update `C:/Users/benti/Desktop/TerraEngine/specs/002-spatial-culling/quickstart.md` with the final validation target name, executable location, and any corrected commands
- [x] T041 Review all touched C++ files for required beginner-friendly intent comments and single-tab indentation in `C:/Users/benti/Desktop/TerraEngine/Engine/Scene/SpatialIndex.cpp`
- [x] T042 Run the spatial validation executable and record pass/fail notes in `C:/Users/benti/Desktop/TerraEngine/Docs/SpatialPartitioningAndCulling.md`
- [x] T043 Run `cmake -S C:/Users/benti/Desktop/TerraEngine -B C:/Users/benti/Desktop/TerraEngine/out/build/spatial-culling -DCMAKE_BUILD_TYPE=Debug` and fix configure failures in `C:/Users/benti/Desktop/TerraEngine/CMakeLists.txt`
- [x] T044 Run `cmake --build C:/Users/benti/Desktop/TerraEngine/out/build/spatial-culling --config Debug --target install` and fix compile or install failures in the affected files
- [x] T045 Verify no shader sources were embedded in C++ and no `External/` files were modified by reviewing `C:/Users/benti/Desktop/TerraEngine/Engine/Shaders/` and `C:/Users/benti/Desktop/TerraEngine/External/`
- [x] T046 Update `C:/Users/benti/Desktop/TerraEngine/specs/002-spatial-culling/tasks.md` by checking off completed tasks only after implementation and verification succeed

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies.
- **Foundational (Phase 2)**: Depends on Setup completion and blocks all user stories.
- **User Story 1 (Phase 3)**: Depends on Foundational completion; provides the MVP.
- **User Story 2 (Phase 4)**: Depends on Foundational completion; can proceed in parallel with US1 after shared index interfaces exist, but final validation benefits from US1 traversal work.
- **User Story 3 (Phase 5)**: Depends on Foundational completion; can proceed after mutation/query contracts are stable, and should be validated after US1/US2 query behavior exists.
- **Polish (Phase 6)**: Depends on the selected user stories being complete.

### User Story Dependencies

- **US1 Render Large Scenes Efficiently**: MVP; no dependency on US2 or US3 after Phase 2.
- **US2 Query Nearby Scene Objects**: Independent query expansion after Phase 2; reuses the same index.
- **US3 Keep Dynamic Scenes Correct**: Independent dynamic correctness slice after Phase 2; depends on query behavior to validate movement and lifecycle changes.

### Parallel Opportunities

- T002 and T003 can run in parallel after T001.
- T007 can run in parallel with T005 and T006 once file placeholders exist.
- T013 and T014 can run in parallel.
- T022, T023, and T024 can run in parallel because they add separate validation scenarios.
- T030, T031, and T032 can run in parallel because they cover separate dynamic validation scenarios.
- T038 and T039 can run in parallel with late implementation cleanup.

---

## Parallel Example: User Story 1

```text
Task: "Add validation cases for 10,000 distributed renderables, frustum inclusion, boundary inclusion, and invalid-bound fallback in C:/Users/benti/Desktop/TerraEngine/Tools/SpatialValidation/SpatialValidation.cpp"
Task: "Document the large-scene rendering validation procedure and expected candidate-count thresholds in C:/Users/benti/Desktop/TerraEngine/Tools/SpatialValidation/README.md"
```

## Parallel Example: User Story 2

```text
Task: "Add area-query validation cases for inside, outside, boundary-overlapping, inactive, and diagnostic-included objects in C:/Users/benti/Desktop/TerraEngine/Tools/SpatialValidation/SpatialValidation.cpp"
Task: "Add sphere-query and point-query validation cases for radius zero, invalid radius, and category filtering in C:/Users/benti/Desktop/TerraEngine/Tools/SpatialValidation/SpatialValidation.cpp"
Task: "Add ray-query validation cases for nearest-first sorting, invalid rays, maximum distance, and broad-phase candidate behavior in C:/Users/benti/Desktop/TerraEngine/Tools/SpatialValidation/SpatialValidation.cpp"
```

## Parallel Example: User Story 3

```text
Task: "Add dynamic movement and region-transfer validation cases for at least 1,000 moving objects in C:/Users/benti/Desktop/TerraEngine/Tools/SpatialValidation/SpatialValidation.cpp"
Task: "Add create, destroy, active-state, static-state, and duplicate-result validation cases in C:/Users/benti/Desktop/TerraEngine/Tools/SpatialValidation/SpatialValidation.cpp"
Task: "Add parent-child transform update validation cases in C:/Users/benti/Desktop/TerraEngine/Tools/SpatialValidation/SpatialValidation.cpp"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1 and Phase 2.
2. Complete Phase 3 for object-level render candidate filtering.
3. Run `SpatialValidation` for US1 cases.
4. Run `cmake --build C:/Users/benti/Desktop/TerraEngine/out/build/spatial-culling --config Debug --target install`.
5. Stop and review candidate-count diagnostics before adding US2 and US3.

### Incremental Delivery

1. Deliver US1 to reduce render candidates and preserve visible output.
2. Deliver US2 to expose shared area, sphere, and ray query behavior.
3. Deliver US3 to harden dynamic scene correctness.
4. Finish polish, documentation, and install verification.

### Notes

- Do not modify files under `C:/Users/benti/Desktop/TerraEngine/External/`.
- Do not embed shader strings in C++; any future shader changes must stay under `C:/Users/benti/Desktop/TerraEngine/Engine/Shaders/`.
- Every touched C++ file must include comments explaining intent and tricky logic.
- The feature is incomplete until the CMake install target succeeds.
