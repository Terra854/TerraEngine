# Tasks: Shadow Memory Management

**Input**: Design documents from `C:\Users\benti\Desktop\TerraEngine\specs\001-shadow-memory-management\`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/shadow-diagnostics.md, quickstart.md
**Tests**: Included because FR-014 explicitly requires repeatable validation scenes or tests.
**Organization**: Tasks are grouped by user story so each story can be implemented and validated independently.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel with other marked tasks in the same phase because it touches different files or depends only on completed earlier phases.
- **[Story]**: User story label for traceability. Setup, Foundational, and Polish tasks do not use story labels.
- Every task includes an exact repository path.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Establish documentation and validation scaffolding used by all stories.

- [X] T001 Create `C:\Users\benti\Desktop\TerraEngine\Docs\ShadowMemoryManagement.md` with feature goals, current memory problem summary, validation scenarios, and CMake install requirement.
- [X] T002 Create `C:\Users\benti\Desktop\TerraEngine\Tools\ShadowMemoryValidation\README.md` describing how validation captures prove shadow memory, cache, and atlas behavior.
- [X] T003 Add `C:\Users\benti\Desktop\TerraEngine\Tools\ShadowMemoryValidation\CMakeLists.txt` and wire it from `C:\Users\benti\Desktop\TerraEngine\CMakeLists.txt` only if this repository already builds tools from CMake.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Define shared renderer data and diagnostics required before any story can be implemented.

**CRITICAL**: No user story work should begin until this phase is complete.

- [X] T004 Extend shadow diagnostic enums and counters in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RenderTypes.h` for configured capacity, active shadow count, allocated 2D region count, allocated point region count, estimated bytes, cache reuse, cache refresh, cache invalidation, degraded shadows, allocation failures, degradation reason, refresh reason, and quality tier.
- [X] T005 Add shadow storage region and cache-entry runtime structs in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContextImpl.h` with beginner-friendly comments explaining ownership, GPU lifetime safety, and invalidation intent.
- [X] T006 Refactor existing shadow assignment diagnostics in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContext.cpp` so each shadow-eligible retained light reports assigned state, resource kind, logical slot, degradation reason, refresh reason, and estimated bytes.
- [X] T007 Update lighting diagnostics aggregation in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContextResources.cpp` so the new shadow diagnostic fields appear in latest frame budget and lighting diagnostic snapshots.
- [X] T008 [P] Document the shadow diagnostics contract in `C:\Users\benti\Desktop\TerraEngine\Docs\ShadowMemoryManagement.md` using `C:\Users\benti\Desktop\TerraEngine\specs\001-shadow-memory-management\contracts\shadow-diagnostics.md` as the source.
- [X] T009 [P] Add initial validation capture schema notes to `C:\Users\benti\Desktop\TerraEngine\Tools\ShadowMemoryValidation\README.md` for the fields required by `C:\Users\benti\Desktop\TerraEngine\specs\001-shadow-memory-management\contracts\shadow-diagnostics.md`.

**Checkpoint**: Diagnostics and runtime data shapes are ready for story implementation.

---

## Phase 3: User Story 1 - Raise Shadow Capacity Without Memory Spikes (Priority: P1) MVP

**Goal**: Increasing configured shadow capacity does not reserve every shadow resource kind for every possible slot.

**Independent Test**: Configure capacity 32 with zero active shadows and with four active 2D shadows; verify allocated shadow regions and estimated memory remain proportional to active assignments and no point regions are allocated for 2D-only scenes.

### Tests for User Story 1

- [X] T010 [P] [US1] Add a validation scenario description for capacity 32 with zero active shadow-casting lights in `C:\Users\benti\Desktop\TerraEngine\Tools\ShadowMemoryValidation\README.md`.
- [X] T011 [P] [US1] Add a validation scenario description for capacity 32 with four active 2D shadow-casting lights and zero point shadows in `C:\Users\benti\Desktop\TerraEngine\Tools\ShadowMemoryValidation\README.md`.
- [X] T012 [US1] Create `C:\Users\benti\Desktop\TerraEngine\Tools\ShadowMemoryValidation\ShadowMemoryValidation.cpp` to read renderer diagnostic captures or runtime snapshots and fail when SC-001, SC-002, or SC-006 memory conditions are not met.

### Implementation for User Story 1

- [X] T013 [US1] Replace the single `activeShadowResourceSlots` capacity model in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContextImpl.h` with typed active counts for directional 2D, spot 2D, area 2D, shared 2D if introduced, and point cube resources.
- [X] T014 [US1] Change shadow winner output in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContext.cpp` from a single logical slot pool to typed resource demand while preserving existing light priority, stability, and direct-light fallback behavior.
- [X] T015 [US1] Refactor `EnsureShadowRenderResources` in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContextResources.cpp` to allocate only the typed resources required by current active assignments instead of allocating directional, spot, area, and point resources for every configured slot.
- [X] T016 [US1] Update descriptor writes in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContextResources.cpp` so 2D and point shadow bindings are populated only for live typed resources and leave unused bindings safe for shader reads.
- [X] T017 [US1] Update shadow pass framebuffer creation in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContext.cpp` to iterate live typed resources instead of `activeShadowResourceSlots`.
- [X] T018 [US1] Add GPU lifetime-safe release or reuse of no-longer-needed typed shadow resources in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContextResources.cpp`.
- [X] T019 [US1] Update memory estimation diagnostics in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContextResources.cpp` so estimated bytes match the live typed resources.
- [X] T020 [US1] Update `C:\Users\benti\Desktop\TerraEngine\Docs\ShadowMemoryManagement.md` with expected memory behavior for capacity 8 vs 32, 2D-only scenes, and point-light scenes.
- [X] T021 [US1] Run the User Story 1 validation scenarios from `C:\Users\benti\Desktop\TerraEngine\specs\001-shadow-memory-management\quickstart.md` and record results in `C:\Users\benti\Desktop\TerraEngine\Docs\ShadowMemoryManagement.md`.

**Checkpoint**: User Story 1 is independently functional when high capacity no longer causes memory growth without active shadow demand.

---

## Phase 4: User Story 2 - Reuse Stable Shadow Results (Priority: P2)

**Goal**: Stable lights and unchanged shadow casters reuse valid shadow results, while changed lights or casters invalidate and refresh promptly.

**Independent Test**: Render a static scene for multiple frames and verify cache reuse after warm-up; move a caster or light and verify affected shadows refresh on the next rendered frame.

### Tests for User Story 2

- [X] T022 [P] [US2] Add static-scene cache reuse validation instructions to `C:\Users\benti\Desktop\TerraEngine\Tools\ShadowMemoryValidation\README.md`.
- [X] T023 [P] [US2] Add moving-light and moving-caster invalidation validation instructions to `C:\Users\benti\Desktop\TerraEngine\Tools\ShadowMemoryValidation\README.md`.
- [X] T024 [US2] Extend `C:\Users\benti\Desktop\TerraEngine\Tools\ShadowMemoryValidation\ShadowMemoryValidation.cpp` to fail when static scenes do not reach the target cache reuse rate or when changed inputs are reused stale.

### Implementation for User Story 2

- [X] T025 [US2] Add light shadow signatures in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContext.cpp` covering transform, type, range, cone, area size, shadow strength, shadow bias, and view-dependent directional projection inputs.
- [X] T026 [US2] Add conservative shadow caster signatures in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContext.cpp` covering shadow-casting draw count, mesh IDs, material IDs, model matrices, and material shadow behavior.
- [X] T027 [US2] Implement shadow cache entry state transitions in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContextResources.cpp` for assigned, cached, invalid, refreshed, and pending release states.
- [X] T028 [US2] Update shadow pass recording in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContext.cpp` to skip redraws for valid cache entries and record refresh reasons for rendered entries.
- [X] T029 [US2] Add cache invalidation and refresh diagnostics in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RenderTypes.h` and populate them from `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContext.cpp`.
- [X] T030 [US2] Ensure cache reuse never bypasses refresh on uncertain signatures by adding explicit fallback-to-refresh comments and checks in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContext.cpp`.
- [X] T031 [US2] Update `C:\Users\benti\Desktop\TerraEngine\Docs\ShadowMemoryManagement.md` with cache validity rules, invalidation triggers, and troubleshooting diagnostics.
- [X] T032 [US2] Run the User Story 2 validation scenarios from `C:\Users\benti\Desktop\TerraEngine\specs\001-shadow-memory-management\quickstart.md` and record results in `C:\Users\benti\Desktop\TerraEngine\Docs\ShadowMemoryManagement.md`.

**Checkpoint**: User Story 2 is independently functional when static scenes reuse cached shadows and changed inputs refresh within one frame.

---

## Phase 5: User Story 3 - Pack 2D Shadows Efficiently (Priority: P3)

**Goal**: Directional, spot, and area shadows use managed 2D storage regions with variable quality instead of one full-size image per 2D shadow.

**Independent Test**: Render many mixed-priority 2D shadows and verify storage grows by allocated region area, high-priority shadows retain quality, and lower-priority shadows degrade with diagnostics when storage is full.

### Tests for User Story 3

- [X] T033 [P] [US3] Add mixed-resolution 2D shadow packing validation instructions to `C:\Users\benti\Desktop\TerraEngine\Tools\ShadowMemoryValidation\README.md`.
- [X] T034 [P] [US3] Add storage-full degradation validation instructions to `C:\Users\benti\Desktop\TerraEngine\Tools\ShadowMemoryValidation\README.md`.
- [X] T035 [US3] Extend `C:\Users\benti\Desktop\TerraEngine\Tools\ShadowMemoryValidation\ShadowMemoryValidation.cpp` to verify 2D region counts, quality tiers, estimated bytes, and degradation reasons for over-budget 2D scenes.

### Implementation for User Story 3

- [X] T036 [US3] Add 2D shadow region metadata in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RenderTypes.h` for region coordinates, dimensions, quality tier, and atlas/shared-storage identifier.
- [X] T037 [US3] Implement 2D region allocation and eviction policy in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContextResources.cpp` using priority, visibility, stability, and available storage area.
- [X] T038 [US3] Update 2D shadow viewports, scissors, and framebuffer or render-target selection in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContext.cpp` so each 2D shadow renders into its assigned region.
- [X] T039 [US3] Update shader-visible 2D shadow sampling metadata in `C:\Users\benti\Desktop\TerraEngine\Engine\Shaders\Default\Default.frag.hlsl` so receivers sample the correct managed region.
- [X] T040 [US3] Mirror any changed shadow payload fields in `C:\Users\benti\Desktop\TerraEngine\Engine\Shaders\Lighting\ClusterLightCount.comp.hlsl`, `C:\Users\benti\Desktop\TerraEngine\Engine\Shaders\Lighting\ClusterLightFill.comp.hlsl`, and `C:\Users\benti\Desktop\TerraEngine\Engine\Shaders\Lighting\LightCull.comp.hlsl`.
- [X] T041 [US3] Add graceful 2D storage-full degradation decisions in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContext.cpp` while keeping non-shadowed retained lights active for direct lighting.
- [X] T042 [US3] Update `C:\Users\benti\Desktop\TerraEngine\Docs\ShadowMemoryManagement.md` with 2D storage region behavior, quality tiers, and degradation troubleshooting.
- [X] T043 [US3] Run the User Story 3 validation scenarios from `C:\Users\benti\Desktop\TerraEngine\specs\001-shadow-memory-management\quickstart.md` and record results in `C:\Users\benti\Desktop\TerraEngine\Docs\ShadowMemoryManagement.md`.

**Checkpoint**: User Story 3 is independently functional when many 2D shadows pack into managed storage and over-budget cases degrade predictably.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Final documentation, formatting, full validation, and required build/install verification.

- [X] T044 [P] Review comments in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RenderTypes.h`, `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContext.cpp`, `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContextImpl.h`, and `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RHI\Vulkan\VulkanContextResources.cpp` for beginner-friendly intent explanations and single-tab indentation.
- [X] T045 [P] Update `C:\Users\benti\Desktop\TerraEngine\specs\001-shadow-memory-management\quickstart.md` with final validation command details discovered during implementation.
- [X] T046 Run all shadow memory validation scenarios documented in `C:\Users\benti\Desktop\TerraEngine\Tools\ShadowMemoryValidation\README.md` and fix any failures in the affected renderer files.
- [X] T047 Run `cmake -S C:\Users\benti\Desktop\TerraEngine -B C:\Users\benti\Desktop\TerraEngine\Build\Debug -DCMAKE_BUILD_TYPE=Debug` and resolve configuration failures.
- [X] T048 Run `cmake --build C:\Users\benti\Desktop\TerraEngine\Build\Debug --config Debug` and resolve compile failures.
- [X] T049 Run `cmake --build C:\Users\benti\Desktop\TerraEngine\Build\Debug --config Debug --target install` or `cmake --install C:\Users\benti\Desktop\TerraEngine\Build\Debug --config Debug` and verify the install tree is produced.
- [X] T050 Update `C:\Users\benti\Desktop\TerraEngine\Docs\ShadowMemoryManagement.md` with final measured memory, cache reuse, degradation, build, and install results.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1 Setup**: No dependencies.
- **Phase 2 Foundational**: Depends on Phase 1 and blocks all user stories.
- **Phase 3 User Story 1**: Depends on Phase 2 and is the MVP.
- **Phase 4 User Story 2**: Depends on Phase 2, but should be implemented after US1 in normal priority order because typed storage simplifies cache ownership.
- **Phase 5 User Story 3**: Depends on Phase 2 and is easiest after US1 because atlas regions build on typed 2D storage.
- **Phase 6 Polish**: Depends on all implemented user stories.

### User Story Dependencies

- **US1 (P1)**: No user-story dependency; deliver first.
- **US2 (P2)**: Can start after Phase 2, but practical integration depends on US1's storage-region ownership model.
- **US3 (P3)**: Can start after Phase 2, but practical integration depends on US1's typed 2D resource model.

### Within Each User Story

- Validation tasks precede implementation tasks.
- Data/payload structures precede resource lifecycle changes.
- Resource lifecycle changes precede render-pass recording changes.
- Diagnostics updates are required before story validation is considered complete.

---

## Parallel Opportunities

- T008 and T009 can run in parallel after T004-T007 define diagnostic fields.
- T010 and T011 can run in parallel before T012.
- T022 and T023 can run in parallel before T024.
- T033 and T034 can run in parallel before T035.
- T044 and T045 can run in parallel after story implementation.
- Separate developers can work on US2 and US3 after Phase 2 if they coordinate shared changes in `C:\Users\benti\Desktop\TerraEngine\Engine\Renderer\RenderTypes.h` and shader payload fields.

---

## Parallel Example: User Story 1

```text
Task: "T010 [P] [US1] Add zero-active-shadow validation instructions in C:\Users\benti\Desktop\TerraEngine\Tools\ShadowMemoryValidation\README.md"
Task: "T011 [P] [US1] Add four-active-2D-shadow validation instructions in C:\Users\benti\Desktop\TerraEngine\Tools\ShadowMemoryValidation\README.md"
```

## Parallel Example: User Story 2

```text
Task: "T022 [P] [US2] Add static-scene cache reuse validation instructions in C:\Users\benti\Desktop\TerraEngine\Tools\ShadowMemoryValidation\README.md"
Task: "T023 [P] [US2] Add moving-light and moving-caster invalidation validation instructions in C:\Users\benti\Desktop\TerraEngine\Tools\ShadowMemoryValidation\README.md"
```

## Parallel Example: User Story 3

```text
Task: "T033 [P] [US3] Add mixed-resolution 2D shadow packing validation instructions in C:\Users\benti\Desktop\TerraEngine\Tools\ShadowMemoryValidation\README.md"
Task: "T034 [P] [US3] Add storage-full degradation validation instructions in C:\Users\benti\Desktop\TerraEngine\Tools\ShadowMemoryValidation\README.md"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1 setup.
2. Complete Phase 2 foundational diagnostics and structs.
3. Complete Phase 3 typed demand-driven allocation.
4. Stop and validate SC-001, SC-002, and SC-006 before starting cache or atlas work.
5. Run compile and install verification if stopping after the MVP.

### Incremental Delivery

1. US1: Memory-safe high shadow capacity.
2. US2: Correct conservative shadow caching.
3. US3: Managed 2D shadow storage regions.
4. Polish: Full validation, compile, install, final docs.

### Quality Gates

- No changes to `C:\Users\benti\Desktop\TerraEngine\External\`.
- Shader changes only in `C:\Users\benti\Desktop\TerraEngine\Engine\Shaders\`.
- All touched code must include clear intent comments where logic is non-obvious.
- Final implementation must pass CMake configure, build, and install.



