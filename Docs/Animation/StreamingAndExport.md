# Animation streaming and export

TerraEngine treats animation data as cooked, generation-owned runtime content. The runtime never reads authoring files to repair a missing export, and asynchronous work cannot publish into a newer generation by accident. This document describes the residency, streaming, hot-reload, TEPAK, headless, and parity contracts that preserve those guarantees.

## Resident animation assets

The residency tracker accounts for all seven cooked animation asset classes independently:

- skeletons;
- animation clips;
- animation controllers;
- animation masks;
- retarget profiles;
- motion databases; and
- ragdoll profiles.

Every request identifies the asset, asset class, byte size, priority, and request generation. `ResourceManager` performs the actual asynchronous I/O; `AnimationResidencyTracker` owns only bounded, worker-safe policy state. A completion or cancellation is accepted only for the matching generation. A stale worker completion is counted in diagnostics and cannot replace the active payload.

Residency is not equivalent to visibility. An offscreen animator may omit visual deformation work while still requiring authoritative data for events, root motion, physics, recording, replication, rollback, validation, or editor preview. These usages retain the generation explicitly. Budget eviction skips retained generations, gameplay-critical requests, and offscreen-authority requests. Ordinary unretained generations are selected deterministically by priority and last-touch order. Until a load succeeds, callers must use the reported safe fallback instead of accessing a partial payload.

Diagnostics expose the configured budget, resident, evictable, protected, and over-budget bytes, plus rejected requests, cancellations, stale completions, and per-asset-class state counts. A disabled or collapsed diagnostics view must consume a previously copied snapshot and must not walk live resource storage.

## Clip and motion-database streaming

Large animation clips stream as independently identified clip segments. Motion databases stream as independently identified search blocks. `AnimationStreamingCoordinator` bounds all four pressure dimensions:

- queued prefetch request count;
- total pending request count;
- total pending bytes; and
- tracked payload generations.

Prefetch windows are atomic: either every new block in a window is queued or none is. This prevents a partially advanced look-ahead horizon when the byte or request budget is exhausted. Gameplay-critical blocks can be retained until their authoritative consumer releases them. Cancellation and completion are generation checked, and resident-budget eviction cannot reclaim retained blocks.

Fallback behavior is explicit. A missing clip segment uses the reference pose or the last resident generation. A missing motion block uses the deterministic motion-search fallback. Diagnostics report fallback use, pending and resident bytes, retained gameplay payloads, rejected requests, and stale completions.

## Atomic hot reload

Hot reload is last-known-good and publishes only at a protected boundary. A candidate must:

1. parse successfully;
2. use a newer generation;
3. preserve the active compatibility fingerprint;
4. provide every required dependency at its expected generation and compatibility fingerprint; and
5. fit within the bounded asset, dependency-edge, instance, and retirement capacities.

Batch publication validates every candidate before changing active state. A corrupt, incompatible, stale, or dependency-incomplete batch is rejected atomically. Active instances remain bound to the preceding cooked generation and receive a structured reason.

Successful publication invalidates dependent assets and switches compatible active instances at one publication boundary. Frame packets, GPU work, managed handles, and other immutable consumers retain the generation they captured. Old generations are retired only after those references are released and their safe protected boundary has completed.

The editor source watcher schedules reimport work through `AssetDatabase`; it does not mutate a live runtime payload from its worker callback. Import success offers a candidate to the hot-reload publisher. Failure records last-known-good status and leaves the active generation unchanged.

## TEPAK dependency closure

Animation export starts from the selected startup content and resolves the complete transitive closure across animation assets, meshes, materials, textures, shaders, and package manifests. Cooked animation envelopes declare stable type codes and dependencies. The seven animation TEPAK classes are append-only so existing class identities and unknown-class fallback remain compatible:

| Code | Runtime class |
| ---: | --- |
| 8 | Skeleton |
| 9 | Animation clip |
| 10 | Animation controller |
| 11 | Animation mask |
| 12 | Retarget profile |
| 13 | Motion database |
| 14 | Ragdoll profile |

Before publication, the validator rejects missing dependencies, class mismatches, invalid envelopes, unsupported format or capability versions, corrupt payloads, path traversal, forbidden source/editor/build paths, and unavailable required CPU fallback data. It also verifies the emitted runtime manifest and records structured evidence for each checked asset and dependency edge. Export publication is atomic: a failed validation does not replace a previously valid archive.

The archive contains cooked content only. Source files and editor sidecars are never copied as a runtime fallback. The required animation compute shaders are included for rendered builds; CPU fallback remains part of the validated capability set for renderer loss and headless operation.

## Runtime mounting and reads

`ContentMount` publishes an immutable archive view. A remount is prepared and validated separately, then swapped atomically; concurrent readers therefore observe either the complete old archive or the complete new archive. A failed remount preserves the last-known-good mount.

`AnimationContentResolver` accepts only a mounted archive. It discovers the seven animation classes, validates logical paths, and performs bounded range reads. It returns structured statuses for an absent mount, missing content, a non-animation class, a forbidden runtime path, an out-of-range request, a read-limit violation, or an archive error. It cannot fall back to the project, source tree, editor tree, or build tree.

## Headless and installed-runtime parity

Headless startup uses `Engine::ConfigureHeadlessCpuFallback`. Window, input, UI, and render systems are omitted, while animation, physics, presentation handoff, transactional mutation, and managed gameplay bindings remain available. CPU deterministic evaluation supplies authoritative state without a GPU readback or renderer dependency. The installed managed animation assembly is resolved relative to the installed engine root, not the development tree.

The export-runtime fixture in `Tests/Fixtures/AnimationExportRuntime/` defines the required parity surface. Installed editor, rendered runtime, and headless runtime must agree on authoritative animation state, one-shot events, root motion, motion-match decisions, ragdoll state, deformation inputs, and conservative bounds. A headless run may omit pixels, but it may not omit or change authoritative gameplay results.

## Failure and recovery expectations

- Budget pressure evicts only unprotected generations and leaves an explicit safe fallback.
- Cancellation prevents later completion of that request from publishing.
- Incompatible or corrupt reload preserves active instances and the preceding generation.
- Old generations remain alive while frame, GPU, scripting, or instance references exist.
- Missing or corrupt TEPAK dependencies reject export or remount atomically.
- Runtime content resolution never escapes the mounted archive.
- Renderer loss selects CPU fallback without changing authoritative animation results.
- Project reload and remount discard stale asynchronous results by generation.

## Reproducible validation

From a Visual Studio developer shell, configure, build, test, and install the repository-contained implementation:

```powershell
cmake --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --parallel
ctest --preset RelWithDebInfo --output-on-failure
cmake --build --preset RelWithDebInfo --target install --parallel
```

Focused coverage lives in:

- `Tests/Engine/Animation/AnimationResidencyTests.cpp`;
- `Tests/Engine/Animation/AnimationHotReloadTests.cpp`;
- `Tests/Editor/Animation/AnimationExportTests.cpp`; and
- `Tests/Engine/Animation/AnimationRuntimeParityTests.cpp`.

The installed US12 workflow uses `Tools/Animation/RunAnimationPhaseGate.ps1` and stores evidence under `out/validation/animation/us12-export/`. It must exercise eviction/reactivation, reload success and failure, project reload, TEPAK export/remount, installed rendered and headless startup, managed ABI loading, and camera evidence. Correctness/readiness evidence may be collected without timing; performance acceptance is deferred to the final valid-performance environment.
