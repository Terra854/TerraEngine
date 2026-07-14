# Implementation Plan: Shadow Memory Management

**Branch**: `001-shadow-memory-management` | **Date**: 2026-04-21 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `C:\Users\benti\Desktop\TerraEngine\specs\001-shadow-memory-management\spec.md`

## Summary

Make high configured shadow counts memory-safe by replacing the current uniform shadow slot allocation with demand-driven typed shadow storage, then add cache validity tracking for reusable shadow results, and finally add managed atlas-style packing for 2D shadows. The first implementation slice must ensure that raising the configured shadow capacity does not reserve point cubemaps, directional maps, spot maps, and area maps for every possible slot when those resources are not active.

## Technical Context

**Language/Version**: C++20 engine code with HLSL shader files where shader-visible payloads change  
**Primary Dependencies**: Vulkan, VMA, GLM, EnTT, nlohmann/json, existing TerraEngine `ThreadPool`, existing renderer diagnostics  
**Storage**: Runtime GPU images, frame diagnostics snapshots, optional JSON validation captures already produced by renderer tooling  
**Testing**: CMake configure/build/install, renderer validation scenes or tests, existing cluster/shadow diagnostics capture path  
**Target Platform**: Desktop TerraEngine runtime/editor on supported Vulkan platforms  
**Project Type**: Native desktop engine renderer feature  
**Performance Goals**: 60 FPS-oriented frame behavior; configured shadow capacity from 8 to 32 must not increase shadow memory by more than 10% unless active shadow demand also increases; stable scenes should reuse at least 80% of eligible shadow frames after warm-up  
**Constraints**: All third-party dependencies remain vendored under `External/`; shader source remains under `Engine/Shaders/`; implementation must use task-friendly renderer architecture and avoid main-loop stalls; final work must pass compile plus install tree verification  
**Scale/Scope**: Up to 32 shadow-capable retained lights, mixed 2D and point shadows, per-view diagnostics for Game and Scene views

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- Task-based concurrency: PASS. Shadow assignment and cache bookkeeping remain CPU-side frame preparation work and must be structured so future expensive validation/hash gathering can be chunked through `ThreadPool`. The first memory allocation slice is lifecycle/resource-management heavy and does not introduce a new single-threaded per-object bottleneck.
- `External/` changes: PASS. No third-party dependency changes are planned.
- Shader separation and asset hygiene: PASS. Any shader-visible payload changes must update files under `Engine/Shaders/`; no embedded shader strings are planned. Runtime verification must use installed assets.
- Compile/build verification: PASS. The implementation handoff requires CMake build and `cmake --build <build-dir> --target install` or `cmake --install <build-dir>` before completion.

## Project Structure

### Documentation (this feature)

```text
C:\Users\benti\Desktop\TerraEngine\specs\001-shadow-memory-management\
|-- plan.md
|-- research.md
|-- data-model.md
|-- quickstart.md
|-- contracts\
|   `-- shadow-diagnostics.md
|-- checklists\
|   `-- requirements.md
`-- tasks.md
```

### Source Code (repository root)

```text
C:\Users\benti\Desktop\TerraEngine\
|-- Engine\
|   |-- Renderer\
|   |   |-- RenderTypes.h
|   |   `-- RHI\
|   |       `-- Vulkan\
|   |           |-- VulkanContext.cpp
|   |           |-- VulkanContextImpl.h
|   |           |-- VulkanContextResources.cpp
|   |           `-- VulkanContext*.cpp
|   `-- Shaders\
|       |-- Default\
|       |   `-- Default.frag.hlsl
|       `-- Lighting\
|           |-- ClusterLightCount.comp.hlsl
|           |-- ClusterLightFill.comp.hlsl
|           `-- LightCull.comp.hlsl
|-- Docs\
|   `-- ShadowMemoryManagement.md
`-- CMakeLists.txt
```

**Structure Decision**: This is a renderer/RHI feature inside the existing engine. Runtime data contracts live in `Engine/Renderer/RenderTypes.h`, Vulkan resource lifecycle changes live in `Engine/Renderer/RHI/Vulkan/`, and any shader payload interpretation changes stay in dedicated shader files under `Engine/Shaders/`.

## Complexity Tracking

No constitution violations are planned.

## Phase 0 Research Summary

See [research.md](research.md). Key decisions:

- Allocate shadow resources by actual assigned light type instead of by global capacity.
- Keep the existing shadow winner selection but change the resource request output from a single slot count to typed demand.
- Add cache entries with explicit invalidation reasons before attempting aggressive cache reuse.
- Treat atlas management as a 2D shadow storage manager with diagnostics and graceful degradation, not as a blind texture packing optimization.

## Phase 1 Design Summary

See [data-model.md](data-model.md) and [contracts/shadow-diagnostics.md](contracts/shadow-diagnostics.md).

Primary design objects:

- `ShadowAssignment`: selected per-frame shadow decision.
- `ShadowStorageRegion`: typed storage allocation for active shadow results.
- `ShadowCacheEntry`: reusable result with validity and refresh reason.
- `ShadowDiagnosticsSnapshot`: author/debug visibility into memory, caching, refresh, and degradation.

## Post-Design Constitution Check

- Task-based concurrency: PASS. The design keeps selection deterministic and allows cache/assignment analysis to be parallelized without touching the entity registry from worker tasks.
- `External/` changes: PASS. None required.
- Shader separation and asset hygiene: PASS. Shader changes, if needed for atlas UV/region metadata, are isolated to `Engine/Shaders/`.
- Compile/build verification: PASS. Quickstart requires configure, compile, install, and validation capture checks before completion.
