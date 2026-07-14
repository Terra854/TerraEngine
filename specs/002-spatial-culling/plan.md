# Implementation Plan: Spatial Partitioning and Object Culling

**Branch**: `002-spatial-culling` | **Date**: 2026-04-22 | **Spec**: [spec.md](C:/Users/benti/Desktop/TerraEngine/specs/002-spatial-culling/spec.md)
**Input**: Feature specification from `C:/Users/benti/Desktop/TerraEngine/specs/002-spatial-culling/spec.md`

## Summary

Add an engine-level spatial query and visibility layer so TerraEngine can avoid scanning every renderable for every camera view, while also creating a reusable query surface for editor picking, diagnostics, and future broadphase systems. The design adds conservative world-space bounds, a shared spatial index owned by engine systems, task-parallel candidate collection, render-system integration before draw command generation, diagnostics, validation scenes/tests, and documentation. Existing meshlet/task-shader and clustered-light culling remain downstream optimizations.

## Technical Context

**Language/Version**: C++20 for native engine code; HLSL shader files remain unchanged for this feature unless later validation proves a shader-side diagnostic is required.
**Primary Dependencies**: Existing bundled dependencies only: EnTT for object/component storage, GLM for math, nlohmann/json for scene/config data, TerraEngine ThreadPool for task execution, existing CMake build/install flow.
**Storage**: Runtime in-memory spatial index plus optional scene/debug JSON for validation fixtures; no persistent database.
**Testing**: CMake build/install verification, focused native validation executable or tool-driven validation path, synthetic validation scenes, and renderer diagnostics comparison.
**Target Platform**: TerraEngine desktop/editor/runtime targets currently built by the repository, with Windows as the primary verified environment.
**Project Type**: C++ game engine/editor/runtime.
**Performance Goals**: Large-scene validation with at least 10,000 renderable objects; camera visibility queries reduce candidate submission to no more than 20% of total objects when no more than 10% are visible; visibility query work fits inside a 60 FPS frame budget on the baseline development machine.
**Constraints**: No new global/system dependencies; no `External/` changes; all new engine source must be C++20; comments must explain intent and tricky behavior; indentation uses a single tab level; CMake install tree must be produced after code changes; correctness-first culling that includes uncertain objects.
**Scale/Scope**: Object-level spatial partitioning for renderable objects first, shared query contracts for area/sphere/ray/frustum queries, diagnostics, dynamic update correctness, and validation coverage. Narrow-phase physics and GPU shader culling changes are out of scope for the initial implementation.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- PASS: The design preserves task-based concurrency by using the existing `ThreadPool` for candidate collection and validation workloads; any sequential index mutation will be documented as a short critical section with query work parallelized outside mutation.
- PASS: No `External/` changes are planned. The feature uses existing bundled dependencies and engine utilities only.
- PASS: No shader source changes are planned. If future diagnostics require shader work, all shader source remains under `C:/Users/benti/Desktop/TerraEngine/Engine/Shaders/`.
- PASS: The implementation plan includes explicit compile and install verification: `cmake --build <build-dir> --target install`, plus validation scene/tool runs before completion.

## Project Structure

### Documentation (this feature)

```text
C:/Users/benti/Desktop/TerraEngine/specs/002-spatial-culling/
|-- plan.md
|-- research.md
|-- data-model.md
|-- quickstart.md
|-- contracts/
|   `-- spatial-query-contract.md
`-- tasks.md
```

### Source Code (repository root)

```text
C:/Users/benti/Desktop/TerraEngine/
|-- Engine/
|   |-- Scene/
|   |   |-- SpatialBounds.h
|   |   |-- SpatialIndex.h
|   |   |-- SpatialIndex.cpp
|   |   |-- SpatialQuery.h
|   |   `-- SpatialDiagnostics.h
|   |-- Renderer/
|   |   |-- RenderSystem.cpp
|   |   |-- RenderSystem.h
|   |   `-- RenderTypes.h
|   |-- Resource/
|   |   |-- ResourceManager.h
|   |   `-- ResourceManagerMesh.cpp
|   `-- Core/Object/
|       |-- ObjectManager.cpp
|       `-- ObjectManager.h
|-- Tools/
|   `-- SpatialValidation/
|       |-- README.md
|       `-- SpatialValidation.cpp
|-- Docs/
|   `-- SpatialPartitioningAndCulling.md
`-- CMakeLists.txt
```

**Structure Decision**: Add the spatial feature under `Engine/Scene/` because it describes scene-object placement and query behavior shared by renderer, editor, and future systems. Integrate renderer candidate filtering in `Engine/Renderer/`, mesh bounds sourcing in `Engine/Resource/`, lifecycle hooks through `Engine/Core/Object/`, validation tooling under `Tools/`, and user-facing reproducibility docs under `Docs/`.

## Complexity Tracking

No constitution violations are expected. No extra tracking table is required.

## Phase 0 Research

Research output is captured in [research.md](C:/Users/benti/Desktop/TerraEngine/specs/002-spatial-culling/research.md). Key resolved decisions:

- Use conservative world AABBs as the common query bound.
- Use a loose octree for broad 3D scene partitioning, with fallback handling for very large or invalid bounds.
- Rebuild or batch-update index membership from authoritative scene snapshots at deterministic points in the frame.
- Use task-parallel query result filtering while keeping structural index mutation serialized and short.
- Avoid new third-party dependencies and avoid shader changes in the first implementation.

## Phase 1 Design

Design outputs:

- [data-model.md](C:/Users/benti/Desktop/TerraEngine/specs/002-spatial-culling/data-model.md)
- [contracts/spatial-query-contract.md](C:/Users/benti/Desktop/TerraEngine/specs/002-spatial-culling/contracts/spatial-query-contract.md)
- [quickstart.md](C:/Users/benti/Desktop/TerraEngine/specs/002-spatial-culling/quickstart.md)

## Post-Design Constitution Check

- PASS: Query collection and validation are planned around task-based execution; mutation is scoped to deterministic update points to avoid data races.
- PASS: No `External/` changes are introduced by the design.
- PASS: The design does not require embedded shader strings or shader relocation.
- PASS: Quickstart and later tasks must include `cmake --build <build-dir> --target install` and validation runs before implementation is considered complete.

## Phase 2 Planning Handoff

The next `/speckit.tasks` step should decompose work into test-first slices:

1. Bounds extraction and validation.
2. Spatial index insertion, removal, update, and deterministic queries.
3. Render-system candidate filtering.
4. Diagnostics and validation tooling.
5. Documentation and mandatory build/install verification.
