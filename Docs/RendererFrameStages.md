# Renderer frame stages

`RenderSystem::BuildFrameExecution` in [RenderSystem.cpp](../Engine/Renderer/RenderSystem.cpp)
is one coroutine with four execution sections and exactly three explicit
`co_await std::suspend_always{}` boundaries. Compiled helpers divide the work;
they do not introduce additional scheduler phases or scene-mutation flushes.
`DeclareFrameWorkloads` remains the authority for descriptor identities,
dependencies, read/write resources and protected/worker classifications.

## Scheduler callbacks and suspension points

The five descriptors form this dependency chain in phase `Render`:

| Stable descriptor suffix (`RenderSystem/Render/…`) | Execution | What the callback actually does |
| --- | --- | --- |
| `RenderPublicationCapture` | Protected | Retires the prior continuation, captures inputs, resumes to suspension 1. |
| `GeometryRoutePrepare` | Worker eligible | Resumes route/snapshot/topology, spatial/base-command and shared light/header preparation to suspension 2. |
| `VirtualGeometryPrepare` | Worker eligible | Resumes per-view selection/grouping, deformation demand, dynamic pages and publication sealing to suspension 3. |
| `BackendCommit` | Protected | Resumes backend consumption, diagnostics and prior-frame feedback to coroutine completion. |
| `RenderPublication` | Protected | Marks the completed phase, destroys its continuation and observes terminal scratch diagnostics. It does not resume the coroutine. |

Worker-eligible callbacks may execute inline according to the existing workload
threshold. Each callback enters the same `RenderExecutionBoundary` gate. The
small `framePhaseMutex` protects the continuation handoff and readiness flags;
`Resume()` runs outside that mutex. Failure is retained in the coroutine promise
and rethrown by `RenderFrameExecution::Resume` to the owning scheduler callback.

## Domains and their owners

The private declarations under [Engine/Renderer/Frame](../Engine/Renderer/Frame)
keep independent domain owners and explicit borrowed views. They are not one
replacement frame context passed to every operation.

| Domain | Concrete files and symbols | Owner and borrow boundary |
| --- | --- | --- |
| Protected capture | `RenderFrameCapture.{h,cpp}`: `CaptureFrameRoots`, `CaptureFrameRenderableInputs`, `CaptureFrameSnapshotHeader`, `CaptureFrameFixtureInputs` | Separate coroutine locals retain scene-change, animation-presentation and GPU-plan roots; shader cohort owners, copied terrain/sky/fixture values and snapshot storage remain separate. Collection views borrow only during synchronous protected collection. |
| Retained state and scratch | `RenderFrameState.h`, `RenderFrameCaches.h`, `RenderScratchEpisode.h` | `RenderSystemLifetimeState` owns caches and the continuation. `RenderScratchEpisode` owns the scratch resource, its lease and PMR collections. |
| Snapshot and routes | `RenderSnapshotPreparation.cpp`: `FrameGeometryInputs`, `AssembleFrameSnapshotRows`; `RenderTopologyPreparation.cpp`: `PrepareFramePersistentTopology` | Per-unique-mesh memo tables and copied route facts borrow renderer-retained immutable mesh owners. Transactional topology construction preserves rejection behavior. |
| Spatial, base commands and lights | `RenderSpatialPreparation.cpp`: `PrepareFrameSpatialInputs`, `PrepareFrameBaseCommands`; `RenderLightingPreparation.cpp`: `PrepareFrameLightCandidates`, `PrepareFrameHeader` | Cache rows remain renderer-owned. Workers read captured transforms/components, write disjoint hash ranges, then the coordinator updates caches. |
| Views and virtual geometry | `RenderViewPreparation.{h,cpp}`, `RenderViewBuilders.cpp`, `RenderViewGrouping.cpp`, `RenderVirtualGeometrySelection.{h,cpp}`, `RenderVirtualGeometryPackets.cpp` | `FrameVisibilityInputs` and streaming views borrow retained tables. `FrameVirtualGeometryPreparation` owns current selection evidence. `FrameViewBuildOutputs` owns ordered submissions and the canonical animation-visibility mask. |
| Deformation and publication | `RenderAnimatedGeometryPreparation.cpp`, `RenderDynamicPublication.cpp`, `RenderFrameSealing.{h,cpp}` | Demand is built after view visibility. Candidate publications own persistent/dynamic roots and transferred selection/presentation arrays. |
| Protected completion | `RenderBackendCommit.cpp`, `RenderDiagnosticsPublication.cpp`, `RenderStreamingFeedback.cpp` | Backend work retains the sealed shared publication. Completed diagnostics receive moved snapshot/evidence owners before feedback reads the completed snapshot. |

Raw mesh/material tables are backed by retained immutable resource owners.
`FrameWorldTransforms` borrows the renderer's transform cache. These borrows
require the execution gate and explicit continuation retirement before replacing
those caches; merely moving the declarations into a header does not establish
that lifetime guarantee.

The four `RenderSystemCapture.cpp`, `RenderSystemSettings.cpp`,
`RenderSystemDiagnostics.cpp` and `RenderSystemResources.cpp` files contain the
corresponding facade operations. The private `renderFramePublicationStore`
member remains owned by `RenderSystem` under its original name and type.

## Retirement and nested tasks

`RenderScratchEpisode` declares members in this order:

1. Strong `shared_ptr<RenderFrameScratch>` owner.
2. `RenderFrameScratch::Lease`.
3. PMR camera vector.
4. PMR light vector.

Reverse destruction therefore destroys both borrowing vectors before releasing
the lease and backing owner. Workers do not reset the unsynchronized scratch
resource. `MarkCompleted` stays at the no-output early completion or final
protected completion; completion does not itself reclaim a borrowed episode.

`RunRenderChunkedWork` in `RenderPreparationTasks.h` owns its future vector and
`ScopedTaskSettlement`. Child ranges borrow the callback and its inputs.
`GetAll()` settles admitted children before normal return; the guard also
settles them if enqueueing, inline range zero, or a child result throws. This
applies to base/light hashing, shadow selection and dynamic publication pages.

`FinalizeFramePublication` separately owns shadow-stream futures. It calls
`GetAll()` before sealing. Its failure path calls `Wait()` before resetting
`shadow.instanceInputs`, so another admitted task cannot still write those rows
while the caller discards them. No such child crosses a coroutine suspension.

`RetireFrameExecutionAtProtectedBoundary` moves the continuation out under
`framePhaseMutex`, releases that lock, then destroys it under the outer execution
gate. Lifecycle paths invoke this before replacing caches or lifetime state.
The final scheduler callback likewise destroys outside the phase mutex. The
scratch member order is an additional guarantee, not a substitute for explicit
retirement before cache replacement.

## Existing worker-time streaming snapshot reads

There is one narrow acquisition exception to the captured-input convention.
`AssembleFrameSnapshotRows` can lazily call `FrameGeometryInputs::Runtime`, which
calls `BuildVirtualGeometryRuntimeFromMesh` in `RenderSnapshotPreparation.cpp`.
That helper retains the original worker-time calls to
`VirtualGeometryStreamingSystem::Instance().ResidencySnapshot()` and
`.PublishedPages()`.

These const getters return owned value copies under their existing synchronization.
The helper consumes those copied residency/publication values. They are two
separate reads; this is not an atomic paired-root acquisition. Their timing was
preserved during extraction. This exception does not authorize worker access to
live ECS/component storage, mutable `ResourceManager` lookups or RHI commands.

## Publication and protected backend order

`BeginFramePublication` captures the expected previous generation and exact
renderer session. `PrepareFrameDynamicPages` settles page jobs before attaching
its immutable root. `PrepareFrameBackendWork` and `LowerFrameViewPublications`
transfer backend work and ordered view arrays into that candidate.

In the third resume, `FinalizeFramePublication` finishes shadow streams,
`SealRenderFramePublication` validates the candidate and
`renderFramePublicationStore.TryPublish` performs the atomic exchange. A rejected
candidate leaves the prior publication intact. Backend preparation-cache work
uses only a successful sealed root. This atomic publication happens before
suspension 3; it is distinct from the fourth, protected backend callback.

`CommitFrameBackend` preserves this order: record profiler evidence, publish
copied visible streaming touches, set global culling, `BeginFrame`, consume the
publication, render its ordered views, correlate capture-target metadata, hand
off readbacks, then `EndFrame` and capability observations. Publication consumption
follows `BeginFrame` because beginning a backend frame first retires and resets its
reused slot. Diagnostics then exchange completed snapshot owners; streaming
feedback is submitted for a later `StreamingPrepare` phase, followed by residency
cleanup, capture processing/draining and override-camera cleanup.

## Capture completion versus physical retirement

[RenderSystemCapture.cpp](../Engine/Renderer/RenderSystemCapture.cpp) preserves
separate client-terminal and physical-owner bookkeeping. Internal admission IDs
associate a result with its exact operation while public request IDs keep their
existing behavior. Terminal delivery storage is reserved before admission, and
delivery is committed before marking that client terminal.

Synthetic jobs own copied inputs and use the initialized task pool. They retain
no raw renderer/scene/arena capture and no strong `Task` after accepted enqueue.
A ready future or a client timeout does not release a physically running job's
capacity; retirement waits for the work owner to disappear. Clear/shutdown join
CPU work outside renderer locks.

Native readback first requires supported per-request ownership queries. Its
physical charge is released only when the same backend reports `Released` for
the exact admission. Clear can revoke logical delivery while native ownership
remains charged. Failed idle proof retains the RHI for shutdown retry and prevents
replacement. The exceptional final destructor path moves an unproven context
into the preallocated process-lifetime owner for OS release; normal shutdown
releases the backend normally.

## Validation boundaries

The extraction's isolated translation-unit compilations and source/span checks
support review. They do not establish installed runtime parity, multithreaded
failure behavior, GPU correctness or performance. Production-stage scratch and
publication contract tests exercise real renderer callbacks; integrated testing
must also retain the original/candidate replay and installed evidence required
by the feature's acceptance workflow.

Use [Testing.md](Testing.md) for the maintained build/install and CTest workflow.
A typical configured-checkout sequence is:

```powershell
cmake --preset RelWithDebInfo -DBUILD_TESTING=ON
cmake --build --preset RelWithDebInfo --target install --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Compile/install failed.' }
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R '^Engine\.FrameStageContracts$' --output-on-failure
```

Run the applicable complete suites and installed replay/acceptance checks after
integration. A structural ownership map describes reviewed owners, borrowers,
last consumers and settlement points; its line counts and brace accounting are
not a lifetime proof.
