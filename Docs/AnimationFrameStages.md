# Animation frame stages

`AnimationSystem::DeclareFrameWorkloads` describes the existing nine callbacks
in the `Animation` scheduler phase. Capture copies live inputs, workers prepare
owned results, and a protected authority exchange makes one sealed result
visible. The extraction changes where this code lives; it adds no scheduler
phase, callback, dependency, or scene-mutation flush.

## Files and private ownership

The current source closure contains these 13 files under `Engine/Animation/`.
The public API remains in `AnimationSystem.h`; the private `Impl` definition is
in `Frame/AnimationFrameRuntime.h`. Stage bodies are compiled translation units,
with ordinary small value/container helpers and required templates retained in
the private header.

| File | Main responsibility |
| --- | --- |
| `AnimationSystem.h` | Public system, playback and observation interface |
| `AnimationSystem.cpp` | System lifecycle entry points and nine workload declarations |
| `AnimationSystemBindings.cpp` | Playback binding, runtime-record construction and controller lookup |
| `AnimationSystemDiagnostics.cpp` | Diagnostics, statistics and inspection |
| `AnimationSystemLifecycle.cpp` | Lifecycle invalidation, frame reset, cancellation and workload configuration helpers |
| `Frame/AnimationFrameRuntime.h` | Private types, composed storage and cross-file method declarations |
| `Frame/AnimationFrameCapture.cpp` | Protected input/command capture and compact admission proof |
| `Frame/AnimationFramePlanning.cpp` | Complete/compact planning and single plan finalization |
| `Frame/AnimationCompleteEvaluation.cpp` | Complete evaluation, pose selection and owned publication preparation |
| `Frame/AnimationCompactEvaluation.cpp` | Compact GPU-visual evaluation |
| `Frame/AnimationAuthoritySealing.cpp` | Ordered authority rows, pages, sidecars and sealed candidate |
| `Frame/AnimationRuntimeRootPreparation.cpp` | Copy-on-write broad and compact runtime-root candidates |
| `Frame/AnimationAuthorityPublication.cpp` | Protected exchange, dependent cache work and command/specialist finalization |

The five composed groups in
[`AnimationFrameRuntime.h`](../Engine/Animation/Frame/AnimationFrameRuntime.h)
own the existing storage rather than making another copy of the crowd:

| Member and type | Owned state and reuse boundary |
| --- | --- |
| `frameInputs` / `FrameInputs` | Captured inputs, copied commands/statuses, consumer read view, compact columns and their strong runtime roots; retained through planning, evaluation and sealing |
| `frameStorage` / `FrameStorage` | Complete work items, scheduler plan/range ledger, scratch allocation/lease and shared pose reservation; reset only after earlier borrowers settle |
| `evaluationState` / `FrameEvaluationState` | Indexed completions, deterministic order, cancellation, counters and the single plan-finalization gate |
| `authorityState` / `AuthorityState` | Fallible sealed candidate, prepared presentation/diagnostics and the published commit token |
| `persistentReuse` / `PersistentReuse` | Alternating candidate buffers, retained page/sidecar/presentation pools, evaluation cache, memory controller and history |

Live authority is separate: `Impl::authorityPublisher` and
`Impl::authorityPublicationGeneration` are direct long-lived members, outside
`authorityState`. Live runtime stores, session/declaration epochs and lifecycle
locks also remain separate from candidate storage. A candidate does not own or
replace these synchronization boundaries.

## Declared execution order

All rows below belong to the same `Animation` phase. Names are the actual
workload names from
[`DeclareFrameWorkloads`](../Engine/Animation/AnimationSystem.cpp); the last
column names the private implementation callback.

| # | Workload | Execution | Depends on | Callback |
| --- | --- | --- | --- | --- |
| 1 | `CaptureAnimationInputs` | Protected snapshot | — | `CaptureAnimationInputsStage` |
| 2 | `PlanAnimationEvaluation` | Worker ranges | 1 | `PlanAnimationEvaluationStage` |
| 3 | `FinalizeAnimationWorkPlan` | Single worker | 2 | `FinalizeAnimationWorkPlanStage` |
| 4 | `EvaluateCompleteAnimation` | Worker ranges | 3 | `EvaluateCompleteAnimationStage` |
| 5 | `EvaluateCompactGpuVisualAnimation` | Worker ranges | 3 | `EvaluateCompactAnimationStage` |
| 6 | `SealAnimationAuthority` | Single worker | 4 and 5 | `SealAnimationAuthorityStage` |
| 7 | `PublishAnimationAuthority` | Protected commit | 6 | `PublishAnimationAuthorityStage` |
| 8 | `CommitPublishedAnimationEvaluationCache` | Single worker | 7 | `CommitPublishedAnimationCacheStage` |
| 9 | `FinalizePublishedAnimationAuthority` | Protected commit | 8 | `FinalizePublishedAnimationStage` |

The system remains after `ScriptingSystem` and before `PhysicsSystem`, and
requires flushed scene mutations before protected capture. `SchedulerDeclaration`
and the workload descriptors retain their typed read/write resources and
protected/worker classifications. Workers consume captured data and write
declared plan/completion ranges; they do not mutate live scene/component storage.
The final three descriptors retain their same-frame visibility requirement.
Paused Play Mode without a simulation step and dormant membership retain their
existing early-return behavior.

## Borrowers, nested children and retained readers

Four operations can admit nested children. Each reserves its future tracking
before admission, declares `ScopedTaskSettlement` after the local owners/views
captured by children, and calls `GetAll()` before accepting the batch:

| Nested group | Production operation | Borrowed owners/views |
| --- | --- | --- |
| `AnimationCompactCapture` | `ProveCompactCaptureRanges` in `AnimationFrameCapture.cpp` | Caller-retained `capturedRuntimeRoot` and `capturedCompactRuntimeRoot`, captured input/consumer views and disjoint admission bytes |
| `AnimationCompactRuntime` | `PrepareRuntimeRootCandidate` in `AnimationRuntimeRootPreparation.cpp` | `compactHandles`, `nextCompact`, captured compact columns and their retained roots |
| `AnimationHomogeneousRows` | `PrepareHomogeneousAuthorityRows` in `AnimationAuthoritySealing.cpp` | `rowPositions`, mutable pages, sidecars, specialist sidecars and prepared compact runtime root; strong caller-owned roots remain active through the call |
| `AnimationMixedRows` | `PrepareMixedAuthorityRows` in `AnimationAuthoritySealing.cpp` | `rowPositions`, `gpuPositions`, mutable pages, sidecars and compact captured roots |

[`ScopedTaskSettlement`](../Engine/ThreadPool/ScopedTaskSettlement.h) waits for
every admitted child during unwinding before the referenced locals are
destroyed. `GetAll()` observes all futures before rethrowing the first failure.
The guard is inside each borrowing operation; moving it outside that operation
would allow its local vectors or closures to expire too early.

Compact raw pointers have explicit strong owners.
`CompactGpuVisualCaptures::runtimeRecords[i]` points into the matching retained
`RuntimeRecordRoot`: candidate columns pair with
`persistentReuse.candidateCompactRuntimeCaptureRoot`, then active columns pair
with `frameInputs.compactRuntimeCaptureRoot` after the successful swap. The
corresponding scalar-state view pairs with
`candidateCompactRuntimeStateCaptureRoot` / `compactRuntimeStateCaptureRoot`.
Sealing and compact runtime preparation finish their dereferences before reuse;
retirement clears the columns before releasing the paired roots.

Published pose lifetime extends beyond frame reset.
`PublishEvaluatedCpuPose` in
[`AnimationCompleteEvaluation.cpp`](../Engine/Animation/Frame/AnimationCompleteEvaluation.cpp)
assigns `poseMemoryReservation` from the retained prior publication or the
current `frameStorage.framePoseReservation`. Shared published pose/authority
owners keep that reservation alive until their final reader releases it.
Page/sidecar/presentation pool reuse keeps the existing sole-ownership checks;
a held reader forces fresh storage rather than allowing mutation of its frame.

`frameStorage.frameScratch` is currently reserved plan storage with an
accounting lease. Plan finalization resizes it to `workPlan.totalScratchBytes`
and moves the staged lease into `frameScratchLease`; protected reset clears it
and releases that lease. **There is no `frameScratch.data()` or span borrower in
this extracted implementation.** The pose reservation has real retained
consumers; an evaluation borrow of these scratch bytes must not be inferred.

## Cancellation and the commit boundary

Before exchange, cancellation or stale session/declaration, lifecycle,
predecessor-root or qualified resource identities reject the candidate without
publishing its root. Sealing prepares fallible rows, roots and sidecars first.
`PublishAnimationAuthorityStage` checks the expected broad/compact runtime roots
under `runtimeOwnershipMutex`; `AnimationAuthorityPublisher::Publish` validates
the sealed authority predecessor/session/lifecycle receipt. Accepted publication
then stores the prepared runtime roots, marks the commit token ready and
advances the live authority generation.

After acceptance, graph cancellation cannot undo committed authority or ordinary
command state. `CommitPublishedAnimationCacheStage` intentionally does not
early-return on graph cancellation; it qualifies the accepted token/root
identities before attempting optional cache insertions. A failed optional cache
insertion remains a cache miss, not an authority rollback.

`FinalizePublishedAnimationStage` publishes committed ordinary command receipts
before settling remaining failures. Its cancelled path preserves those receipts
and cancels still-uncommitted work; it does not promise to apply every specialist
side effect after cancellation. The normal path qualifies the token, applies
sealed specialist batches, completes commands and publishes prepared diagnostics
and presentation. Presentation rejection does not roll back CPU authority.

## Verification status

Worker-local evidence includes isolated object compilation of the 11 affected
translation units and exact source-span/brace accounting of this 13-file
closure. Those checks support syntax and structure; they do not establish linked
runtime parity or lifetime acceptance. Production stage tests cover complete,
compact and mixed work, pinned readers, stale/lifecycle rejection, and graph
cancellation before and after exchange.

Final integrated compilation/install, installed stage tests, candidate/reference
replay and acceptance qualification remain the orchestrator's responsibility.
Use the configure/build/install and CTest workflow in [Testing.md](Testing.md);
the focused registered stage entry is `Engine.FrameStageContracts`. Keep actual
installed artifact hashes and receipts with any acceptance claim.
