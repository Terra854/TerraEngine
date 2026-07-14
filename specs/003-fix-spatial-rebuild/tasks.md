# Tasks: Fix Spatial Rebuild

**Input**: Design documents from `C:\Users\benti\Desktop\TerraEngine\specs\003-fix-spatial-rebuild\`
**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `contracts/spatial-refresh-contract.md`, `quickstart.md`
**Tests**: Required by the specification through regression/stress validation for spatial refresh correctness and diagnostics.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel because it touches different files or does not depend on incomplete tasks.
- **[Story]**: Maps to a user story from `spec.md`.
- Every task includes an exact repository path.

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Confirm the current feature context and affected files before changing behavior.

- [X] T001 Review current per-update spatial rebuild call site and record construction in `Engine/Renderer/RenderSystem.cpp`
- [X] T002 Review current spatial index rebuild, query, fallback, and dirty tracking behavior in `Engine/Scene/SpatialIndex.h` and `Engine/Scene/SpatialIndex.cpp`
- [X] T003 [P] Review current spatial diagnostics fields and editor/runtime consumers in `Engine/Scene/SpatialDiagnostics.h` and `Engine/Renderer/RenderTypes.h`
- [X] T004 [P] Review existing spatial validation coverage in `Tools/SpatialValidation/SpatialValidation.cpp`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Add the shared spatial refresh model and comparison behavior required by every user story.

**CRITICAL**: No user story work can begin until this phase is complete.

- [X] T005 Extend spatial diagnostics refresh-mode/count fields in `Engine/Scene/SpatialDiagnostics.h`
- [X] T006 Add spatial record comparison helpers for culling-relevant fields in `Engine/Scene/SpatialIndex.cpp`
- [X] T007 Add spatial index refresh API declarations for latest-record snapshots and explicit full-refresh requests in `Engine/Scene/SpatialIndex.h`
- [X] T008 Implement reusable diagnostics reset/update helpers for unchanged, incremental, and full refreshes in `Engine/Scene/SpatialIndex.cpp`
- [X] T009 Update any render statistics structures that mirror spatial diagnostics in `Engine/Renderer/RenderTypes.h`

**Checkpoint**: Foundation ready - spatial refresh types and diagnostics can be used by user stories.

---

## Phase 3: User Story 1 - Stable Scenes Avoid Full Spatial Refresh (Priority: P1) MVP

**Goal**: Stable renderable scenes reuse the existing spatial organization instead of rebuilding the whole index every update.

**Independent Test**: Run a large static spatial validation workload for several refreshes and verify diagnostics report unchanged refreshes after warm-up while query results remain valid.

### Tests for User Story 1

- [X] T010 [P] [US1] Add stable-scene refresh reuse validation in `Tools/SpatialValidation/SpatialValidation.cpp`
- [X] T011 [P] [US1] Document stable-scene validation expectations in `Tools/SpatialValidation/README.md`

### Implementation for User Story 1

- [X] T012 [US1] Implement `SpatialIndex` unchanged-snapshot detection that skips node rebuilds when records are equivalent in `Engine/Scene/SpatialIndex.cpp`
- [X] T013 [US1] Preserve and report indexed/static/dynamic/fallback counts on unchanged refreshes in `Engine/Scene/SpatialIndex.cpp`
- [X] T014 [US1] Replace unconditional `spatialIndex.Rebuild(spatialRecords)` with change-aware refresh invocation in `Engine/Renderer/RenderSystem.cpp`
- [X] T015 [US1] Run `Tools/SpatialValidation/SpatialValidation.exe` from the build tree and fix US1 failures in `Engine/Scene/SpatialIndex.cpp` or `Tools/SpatialValidation/SpatialValidation.cpp`

**Checkpoint**: User Story 1 is independently functional when stable scenes no longer trigger repeated full refreshes and existing queries still pass validation.

---

## Phase 4: User Story 2 - Culling Results Stay Correct After Changes (Priority: P1)

**Goal**: Moved, added, removed, inactive, fallback, and resource-changed renderables update spatial query results correctly without forcing a full rebuild for every frame.

**Independent Test**: Run dynamic spatial validation scenarios that move, add, remove, disable, and fallback objects, then confirm old regions are cleared and new regions are queryable.

### Tests for User Story 2

- [X] T016 [P] [US2] Add incremental move/add/remove validation scenarios in `Tools/SpatialValidation/SpatialValidation.cpp`
- [X] T017 [P] [US2] Add fallback-bound and inactive-object incremental validation scenarios in `Tools/SpatialValidation/SpatialValidation.cpp`

### Implementation for User Story 2

- [X] T018 [US2] Implement added-entry insertion and removed-entry deletion in `Engine/Scene/SpatialIndex.cpp`
- [X] T019 [US2] Implement updated-entry removal from old node/fallback membership and reinsertion into new membership in `Engine/Scene/SpatialIndex.cpp`
- [X] T020 [US2] Implement full-refresh fallback when root bounds or ambiguous state make incremental refresh unsafe in `Engine/Scene/SpatialIndex.cpp`
- [X] T021 [US2] Ensure renderer-generated spatial records capture transform, active, static, asset, bounds, category, and revision changes in `Engine/Renderer/RenderSystem.cpp`
- [X] T022 [US2] Run dynamic spatial validation and fix stale-hit or missing-hit failures in `Engine/Scene/SpatialIndex.cpp`

**Checkpoint**: User Story 2 is independently functional when changed renderables produce correct query results without requiring every stable frame to rebuild.

---

## Phase 5: User Story 3 - Diagnostics Reveal Spatial Refresh Cost (Priority: P2)

**Goal**: Developers can see whether each spatial update was unchanged, incremental, or full and how many entries changed.

**Independent Test**: Run validation and inspect diagnostics for stable, partial-change, and full-refresh cases.

### Tests for User Story 3

- [X] T023 [P] [US3] Add diagnostics assertions for unchanged, incremental, and full refresh modes in `Tools/SpatialValidation/SpatialValidation.cpp`

### Implementation for User Story 3

- [X] T024 [US3] Populate refresh mode and added/updated/removed/unchanged/full counts in `Engine/Scene/SpatialIndex.cpp`
- [X] T025 [US3] Propagate new spatial refresh diagnostics into game and scene render stats in `Engine/Renderer/RenderSystem.cpp`
- [X] T026 [US3] Expose new spatial refresh diagnostic fields through render stats structures in `Engine/Renderer/RenderTypes.h`
- [X] T027 [US3] Update developer verification notes for diagnostics in `Tools/SpatialValidation/README.md`

**Checkpoint**: User Story 3 is independently functional when diagnostics distinguish unchanged, incremental, and full refresh behavior.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Final validation, documentation, and build compliance across all stories.

- [X] T028 [P] Update feature quickstart with final diagnostic field names in `specs/003-fix-spatial-rebuild/quickstart.md`
- [X] T029 Audit beginner-friendly comments and single-tab indentation in `Engine/Scene/SpatialIndex.cpp`, `Engine/Scene/SpatialIndex.h`, and `Engine/Renderer/RenderSystem.cpp`
- [X] T030 Build and install with `cmake --build C:\Users\benti\Desktop\TerraEngine\out\build\spatial-culling --config Debug --target install`
- [X] T031 Run installed spatial validation at `C:\Users\benti\Desktop\TerraEngine\out\build\spatial-culling\Build\Tools\SpatialValidation\SpatialValidation.exe`
- [X] T032 Review `git diff` for unintended `External/`, shader, or unrelated file changes in `C:\Users\benti\Desktop\TerraEngine`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies.
- **Foundational (Phase 2)**: Depends on Phase 1; blocks all user stories.
- **User Story 1 (Phase 3)**: Depends on Phase 2 and is the MVP.
- **User Story 2 (Phase 4)**: Depends on Phase 2; can be developed after or in parallel with US1 once the refresh API exists, but final validation should include US1 behavior.
- **User Story 3 (Phase 5)**: Depends on Phase 2; diagnostics are most meaningful after US1 and US2 refresh paths exist.
- **Polish (Phase 6)**: Depends on all desired user stories being complete.

### User Story Dependencies

- **US1 (P1)**: No dependency on other user stories; delivers the MVP.
- **US2 (P1)**: No dependency on US3; depends only on the foundational refresh API and should preserve US1 behavior.
- **US3 (P2)**: Depends on refresh outcomes produced by US1 and US2 for complete diagnostic coverage.

### Parallel Opportunities

- T003 and T004 can run in parallel with T001 and T002 during setup.
- T010 and T011 can run in parallel before US1 implementation.
- T016 and T017 can run in parallel because both extend validation scenarios in the same file only if coordinated; otherwise sequence them to avoid conflicts.
- T023 and T028 can run in parallel with implementation once diagnostic names are settled.

---

## Parallel Example: User Story 1

```text
Task: "T010 [P] [US1] Add stable-scene refresh reuse validation in Tools/SpatialValidation/SpatialValidation.cpp"
Task: "T011 [P] [US1] Document stable-scene validation expectations in Tools/SpatialValidation/README.md"
```

## Parallel Example: User Story 2

```text
Task: "T016 [P] [US2] Add incremental move/add/remove validation scenarios in Tools/SpatialValidation/SpatialValidation.cpp"
Task: "T021 [US2] Ensure renderer-generated spatial records capture transform, active, static, asset, bounds, category, and revision changes in Engine/Renderer/RenderSystem.cpp"
```

## Parallel Example: User Story 3

```text
Task: "T023 [P] [US3] Add diagnostics assertions for unchanged, incremental, and full refresh modes in Tools/SpatialValidation/SpatialValidation.cpp"
Task: "T027 [US3] Update developer verification notes for diagnostics in Tools/SpatialValidation/README.md"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1 setup review.
2. Complete Phase 2 foundational refresh API and diagnostics fields.
3. Complete Phase 3 US1 unchanged-refresh behavior.
4. Run `SpatialValidation.exe` and confirm stable scenes avoid repeated full refreshes.
5. Stop and validate before expanding incremental mutation behavior.

### Incremental Delivery

1. Deliver US1 to remove repeated full refreshes for stable scenes.
2. Deliver US2 to handle changed renderables incrementally while preserving culling correctness.
3. Deliver US3 to expose developer-facing refresh diagnostics.
4. Finish Phase 6 build/install and quickstart validation.

### Final Verification

1. Run `cmake --build C:\Users\benti\Desktop\TerraEngine\out\build\spatial-culling --config Debug --target install`.
2. Run `C:\Users\benti\Desktop\TerraEngine\out\build\spatial-culling\Build\Tools\SpatialValidation\SpatialValidation.exe`.
3. Confirm no unintended dependency, shader, or unrelated source changes are present.
