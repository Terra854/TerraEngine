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

Physics, rendering, scene, typed-pool integration, and specialized dense lookup
remain unchanged because baseline evidence did not justify candidate
complexity. Existing physics scratch already retains capacity; other measured
costs were either tiny or too noisy to establish allocator pressure.

## Adoption rule

Do not replace STL containers or the process allocator engine-wide. A future
candidate must target one measured lifetime/domain, preserve the standard
route, and use the external installed-build benchmark. Retention requires at
least a 5% primary improvement or 50% comparable allocation-work reduction,
exact correctness, no hidden fallback, and no protected regression beyond
`max(2%, measured A/A noise)`. Neutral, unavailable, or regressing candidates
must be removed.
