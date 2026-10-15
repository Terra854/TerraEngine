# TerraEngine memory-system qualification

TerraEngine continues to use ordinary C++20 standard-library containers and
the standard allocator by default. The memory qualification campaign did not justify an engine-wide
STL replacement. It retained one narrow optimization: the editor/resource
verification path always reuses an owned byte vector. The measured standard
fresh-allocation branch was removed after the reusable path was accepted.

## Production design

- `MemoryStrategyConfiguration` remains available for memory experiments that
  have not been promoted. The resource domain and its former environment
  selector were removed because resource staging is now unconditional.
- `ResourceManager::ReadFileBytesInto` fills caller-owned storage for loose
  editor files. Packaged/mounted content preserves the existing authoritative
  read path.
- The `asset.content.verify` diagnostic owns one vector for the operation,
  grows it when necessary, and reuses its capacity for later requests. It
  reports its built-in default source, allocation, reuse, retained capacity,
  and fallback counts.
- The shared foundation includes a checked resettable PMR arena, a tracking
  memory resource, and a bounded typed fixed-block pool. These are utilities,
  not authorization to migrate another subsystem without its own benchmark.

## Measured decisions

The immutable numerical baseline remains
the standalone benchmark workspace's `Baselines/baseline-v4/` directory. The final
candidate-era standard route passed all 18 workloads after targeted replication
of three noisy single-sample checks; thresholds were not changed.

Resource staging was retained after five counterbalanced pairs per scale and
worker mode (60 fresh processes total):

| Workload | Median primary change | Allocation reduction | Decision |
|---|---:|---:|---|
| resource-small | -13.20% (inside 20.18% noise) | 99.17% | Retain |
| resource-normal | +3.15% | 99.97% | Retain |
| resource-stress | +1.92% | 99.997% | Retain |

Every protected timing/RSS gate and content hash passed. Four ten-minute
runtime/editor candidate soaks and two ten-minute all-retained integration
processes completed with 575-578 verified periodic operations, one allocation
plus 127 reuses per operation, and zero hidden fallback.

After acceptance, reusable staging was promoted to the only repeated resource
verification implementation. The historical standard/candidate evidence
remains immutable; current production builds no longer expose the discarded
fresh-allocation selector.

The animation binding arena was discarded. Small and stress workloads passed,
but animation-normal observed hidden arena fallback. Per the predeclared gate,
one fallback disqualifies the route regardless of its 100% attributable
transient-allocation reduction. Its PMR containers, arena ownership, route
state, and candidate counters were removed from production; evidence remains in
the external benchmark folder.

During that qualification campaign, physics, rendering, scene, typed-pool
integration, and specialized dense lookup were left unchanged because the
baseline did not justify candidate complexity. Existing physics scratch
already retained capacity; other measured costs were either tiny or too noisy
to establish allocator pressure. Runtime failure-safety work is described
separately below.

## Runtime failure safety and renderer frame scratch

Runtime memory safety applies failure and lifetime contracts to existing
utilities; it does not qualify an engine-wide container replacement. Renderer
integration and installed validation tools are present. The final installed
campaign records 183/183 CTest groups, 54 scoped native cases with 500 cache/admission
repetitions, 1000 Vulkan repetitions, and 600 renderer lifecycle repetitions plus
editor loopback. Small/normal/stress parity and all 18 independent preparation
median/p95 comparisons passed with zero observed parity fallbacks. These results
apply to the pinned installation; see the [actual qualification receipts and
limitations](Testing.md#runtime-memory-safety-and-renderer-scratch). A passing
no-regression comparison does not establish a performance improvement.

The standalone `FrameArena` and `FrameAllocator` APIs remain supported after the
renderer migration. Alignment must use the actual backing address, including
deliberately misaligned backing, rather than align only the offset. Check array
multiplication and backing/fallback size arithmetic before allocating or
changing state. Rejected requests leave existing live ranges intact, and reset
requires every borrower to have retired. Tests use the same span-allocation
routine as production and a controlled base-plus-one span for deterministic
64/256-byte alignment coverage; allocator luck cannot establish sensitivity.

A failed `FixedBlockPool` constructor must return its reserved slot without
marking an object occupied or increasing successful-object/high-water counts.
Only successfully constructed objects are destroyed. Allocation or tracking
failure must preserve neighbor sentinels and allow a later valid allocation.

Task ownership participates in reclamation. Shutdown closes the current cache
generation and detaches owners before destruction outside the pool mutex;
late owners cannot repopulate a closed or later generation. If final dependency
admission fails, preserve the original exception and settle ancestors only
after their admitted sibling work has completed. Future readiness alone cannot
prove borrowed camera/light input has retired.

The central renderer design moves exactly the camera and light capture vectors
to `std::pmr::vector` using a renderer-owned `ReusableArenaResource` with persistent
16 MiB backing. A stable `RenderSystem`-owned `atomic_flag` rejects concurrent or
same-thread recursive execution before continuation, queue, backend, or cache
mutation. Acquire/release admission spans each callback or lifecycle operation;
it is not held while the coroutine is suspended between scheduler phases. A
separate small handoff mutex protects continuation pointers and readiness, and
is released before resuming or destroying the continuation.

`RenderFrameScratch` adds an exclusive episode lease with strong resource
ownership. The coroutine declares its lease before the camera/light vectors;
reverse destruction retires both vectors before releasing the lease. Its stable
owner is likewise declared before the continuation in renderer lifetime state,
so lifetime teardown destroys the continuation first. One protected coordinator
allocates; workers borrow completed collections, and all admitted borrowers
settle before collection destruction. Lease release marks the episode inactive
without resetting storage, including on worker exception paths. The next
protected capture or protected lifecycle retirement performs reclamation.

Keep strong ownership across suspended execution and terminal cleanup for
completion, exception, cancellation, scene clear, shutdown, and restart. A new
renderer lifetime receives a distinct identity. Snapshot completed, failed, and
cancelled episodes before protected reset: copied lifetime/episode/frame IDs,
capacity/use/peak/high-water, reuse/fallback deltas, reset count, and outcome keep
their terminal values after reclamation. The existing read-only
`diagnostics.get_summary` exposes them under `rendererScratch.latest` and
`rendererScratch.previous` (inside the automation response's `data`). A separate
`rendererScratch.reclamation` copy contains `lifetimeId`,
`totalFallbackAllocationCount`, `totalFallbackDeallocationCount`,
`outstandingFallbackAllocationCount`, `outstandingFallbackBytes`, and `resetCount`.
Actual committed fallback tracking and actual upstream deallocation update these
facts; repeated empty resets cannot fabricate release counts. Protected/quiescent
code copies them through `ReclamationSnapshot()`; automation reads copied native
facts and receives no arena pointers, capacity overrides, or allocator controls.

The scheduler declares capture writes, worker reads, protected backend reads,
and copied diagnostic publication for `RendererFrameScratch` through the existing
Render phases. The scoped change adds no scheduler phases or mutation flushes,
dependencies, or shader changes. Existing scene mutation visibility and rollback
rules still apply; their integrated checks remain acceptance requirements.

This ownership migration requires its own output/parity and frame-preparation
measurement gates. The earlier external resource-staging campaign does not
qualify renderer performance. Keep its historical measurements and acceptance
rules intact, preserve the runtime memory safety campaign's immutable reference/calibration,
and report fallbacks and unavailable metrics explicitly. See
[runtime-memory validation](Testing.md#runtime-memory-safety-and-renderer-scratch)
for installed filters, registration floors, and evidence requirements.

## Adoption rule

Do not replace STL containers or the process allocator engine-wide. A future
candidate must target one measured lifetime/domain, preserve the standard
route, and use the external installed-build benchmark. Retention requires at
least a 5% primary improvement or 50% comparable allocation-work reduction,
exact correctness, no hidden fallback, and no protected regression beyond
`max(2%, measured A/A noise)`. Neutral, unavailable, or regressing candidates
must be removed.
