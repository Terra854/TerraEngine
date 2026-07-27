# Motion Matching

TerraEngine motion matching searches authored animation samples for the pose that
best follows a gameplay trajectory while respecting contact, tag, transition,
and continuity rules. The search data is immutable after publication. Queries
name both an asset generation and a schema fingerprint, so a result from an old
database cannot silently select a sample after hot reload.

## Building a database

`MotionDatabaseCompiler` accepts either legacy precomputed samples or typed clip
samples. A typed sample supplies three trajectory horizons, six pose points,
three velocity points, facing, contacts, phase, tags, transition flags,
continuity, and authored exclusion regions. The build then:

1. validates clip/sample IDs, ticks, ranges, and finite source values;
2. extracts the fixed feature schema and derives clip-neighbour continuity;
3. computes deterministic per-feature normalization ranges;
4. quantizes every value to a signed 16-bit integer;
5. sorts by stable sample ID, creates resident block metadata, and fingerprints
   the schema and content; and
6. validates the complete staged asset before publishing it.

Cancellation is checked between stages and during large extraction loops. A
cancelled or invalid build leaves the output and report unchanged, preserving
the last-known-good database. The asynchronous compiler copies its complete
input into a worker job, so caller-owned spans never cross the job boundary.

## Fixed feature schema and storage

Every sample has exactly 48 quantized lanes. Their ranges are part of the schema
fingerprint and are shared by the compiler, CPU scorer, GPU upload layout, and
shaders:

| Block | Lanes | Meaning |
| --- | ---: | --- |
| trajectory | 0-11 | three future x/z positions and x/z facings |
| pose | 12-29 | six local pose points |
| velocity | 30-38 | three local velocity points |
| facing | 39-41 | current facing vector |
| contact | 42-43 | contact feature lanes |
| phase | 44 | normalized cycle phase |
| tag | 45 | tag-derived feature lane |
| transition | 46 | transition-derived feature lane |
| continuity | 47 | continuity-derived feature lane |

The compact CPU record is 160 bytes. GPU uploads expand its signed-16 feature
lanes to signed-32 lanes in a frozen 256-byte record. Resident blocks contain at
most 4,096 samples and record independent CPU/GPU offsets, sizes, and content
fingerprints. A database can contain at most 16,777,216 samples. Code must shard
larger authored sets instead of allocating past this hard limit.

## Query constraints and costs

Hard exclusions run before scoring. A sample is ineligible when it is explicitly
excluded, belongs to an excluded authored region, misses required tags, contains
excluded tags, misses required contacts, exceeds the circular phase window, or
violates required/excluded transition flags. These rules are never relaxed to
manufacture a result.

Eligible samples use integer-only authoritative costs:

- each feature contributes `abs(sample - query) * featureWeight`;
- circular phase distance uses `phaseWeight`;
- missing preferred tags and mismatched preferred contacts use their configured
  per-bit penalties;
- changing or breaking the preferred continuity group uses the declared
  continuity penalties; and
- source-clip and preferred-transition mismatches use their declared penalties.

Feature, block, and metadata totals use 64-bit accumulators. Equal totals resolve
by source clip ID, source tick, and finally stable sample ID. This ordering makes
the same quantized query deterministic across worker counts, replay, and
headless/GPU-enabled runs.

## Budgets and fallback

The normal result budget is top-K 32; the hard maximum is 256. Queries also have
a maximum number of samples to visit and a maximum number of exclusion details
to retain. Reaching a visit budget is reported and never causes an unbounded
second scan. If the database is empty or every visited sample is excluded, the
scorer returns `NotReady` with an explicit fallback reason so the graph can use
its authored complete fallback state.

When GPU work is unavailable, pending, cancelled, stale, or rejected, the same
CPU scorer performs a deterministic bounded fallback. This is a correctness
path, not a float approximation of GPU output.

## GPU scoring and readback

Renderer integration implements `MotionMatchGpuBackend`. The animation side
copies a query packet, plans one score dispatch per requested resident block,
and plans repeated 1,024-candidate tiled reductions until no more than top-K
candidates remain. Score and reduction shaders use the same hard exclusions,
integer component costs, and stable ordering as the CPU reference.

Submission returns a renderer ticket containing a submission ID, fence value,
readback capacity, and 160-byte candidate stride. `PollBackends` checks a fence
without waiting. Only a signalled, matching-generation, bounded readback becomes
visible. Cancellation or a generation/schema change invalidates the request
before candidate IDs cross into selection.

GPU totals are advisory: the selector accepts at most top-K stable IDs and then
revalidates and re-scores that bounded subset on the CPU. Invalid or duplicate
IDs reject the readback and trigger the full CPU fallback. No current-frame GPU
wait is permitted.

## Diagnostics

`MotionMatchDiagnostics` is a fixed-capacity circular history. Each copied
record contains database/query generation, query tick, selected source clip and
source tick, all 48 per-feature costs, nine block totals, metadata costs, hard
exclusion counts/details, candidate-budget exhaustion, selection source, GPU
status, and fallback reason. Snapshots are independent values; recording,
rollback, managed, and editor consumers never retain pointers into live search
state.

The frozen validation manifest describes a 250,000-sample database, all 48
features, top-K 32, and 100,000 deterministic generated queries. Current tests
validate layout, limits, state transitions, cancellation, stale generations,
hard exclusions, and CPU-authoritative selection without timing the workload.
The 99.9-percent frame target and CPU-reduction measurement remain deferred to
the final performance acceptance task.
