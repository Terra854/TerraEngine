# Physics Backend Selection Evidence

**Schema version**: 1  
**Decision owner**: TerraEngine physics feature 006  
**Approved implementation revision**: Jolt Physics v5.6.0, commit `e77f175595e64cb44218cc9d9d56fc365ad0e36a`

## Reproduction

Source acquisition is performed before measurement. Every measured configure,
build, and install receives an exact checkout path and runs with CMake package
registries and FetchContent downloads disabled.

```powershell
pwsh -NoProfile -File Tools/Physics/BackendSpike/RunBackendSpike.ps1 `
	-Candidate Jolt56 `
	-SourceRoot out/physics-spike/sources/JoltPhysics-5.6.0 `
	-WorkRoot out/physics-spike/results/Jolt56
```

## Hard gates

| Gate | Required evidence | Jolt 5.6 | PhysX 5.8 |
|---|---|---:|---:|
| Exact revision | Recorded Git commit equals the planned revision | PASS | Not run; unapproved fallback |
| Offline build | Configure/build/install performs no source or package download | PASS on Windows and Ubuntu/WSL | Not run; unapproved fallback |
| Hermetic output | No globally installed physics SDK or runtime is referenced | PASS; only checkout, compiler, CMake and local generator used | Not run; unapproved fallback |
| CPU-only scope | GPU compute and hair simulation are disabled and introduce no extra tool or runtime dependency | PASS with all four `JPH_USE_*` compute options disabled | N/A |
| Engine job compatibility | Jobs/barriers can use TerraEngine's shared pool without a recurring provider pool | PASS at interface/design gate; T017 supplies the production shared-pool adapter | Not run; unapproved fallback |
| Required runtime surface | Shapes, CCD, filters, materials, queries, constraints, characters, events, debug extraction | PASS by compiled API/source audit and scale harness; detailed behavior remains phase-tested | Not run; unapproved fallback |
| Friction behavior | New Jolt 5.6 friction behavior passes material, combine-mode, settling, and determinism fixtures | PASS for spike material/settling fixture; detailed combine fixtures are pinned by T031 | N/A |
| Gravity and LinearCast regressions | Gravity-force energy and LinearCast contact fixtures pass without regressions | PASS; falling-body gravity and 200 m/s thin-wall LinearCast probes pass | N/A |
| Deterministic publication | Repeated state and engine-owned sorted packet hashes match | PASS; three Windows launches produced `1075c77afda0a63f` | Not run; unapproved fallback |
| Lifecycle safety | World replacement, cancellation, drain, and shutdown complete without late callbacks | PASS for repeated create/run/remove/destroy process lifecycle; full race gate remains T137 | Not run; unapproved fallback |
| Large-world policy | Float and double-position behavior and cost are recorded | PASS; both modes build/run, initial integration selects float positions to match current authored `glm::vec3` precision | Not run; unapproved fallback |
| Performance | Correct provider is within 10% of the best passing finalist | PASS against feature threshold; fallback comparison waived by explicit Jolt approval | Not run; unapproved fallback |
| Redistribution | License and installed binary/source obligations are recorded | PASS; MIT source/license retention required | Not run; unapproved fallback |

Any failed Jolt 5.6 hard gate stops implementation. PhysX is a research
candidate only and cannot be selected without renewed planning and approval.

## Required measurements

| Candidate | Configure s | Build s | Install s | Source bytes | Build bytes | Install bytes | p50 tick ms | p95 tick ms | p99 tick ms | Peak memory bytes | Result hash |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| Jolt 5.6 Windows float | 1.2 | 18.0 | <1.0 | 36,352,173 | 100,826,850 | 23,549,147 | 0.429â€“0.476 | 0.544â€“0.664 | 0.649â€“0.781 | 15,355,904 observed working set | `1075c77afda0a63f` |
| Jolt 5.6 Windows double-position | 1.0 | 13.0 | <1.0 | 36,352,173 | Not retained | Not retained | 0.324 | 0.413 | 0.471 | Not sampled | `918272353099cbe7` |
| Jolt 5.6 Ubuntu/WSL float | 7.9 | 214.0 | <2.0 | 36,352,173 | 358,874,060 | 123,282,927 | 1.300 | 1.794 | 2.153 | Not sampled | `c8bf9c9c73edc765` |
| PhysX 5.8 | Not run | Not run | Not run | Not run | Not run | Not run | Not run | Not run | Not run | Not run | Unapproved fallback |

Reference hardware was an Intel Core i9-14900HX (24 cores, 32 logical workers)
with 32 GB RAM on Windows 11 10.0.26200 using MSVC 19.51.36247 and Ubuntu/WSL
using GCC 15.2. Both lanes used Release, default x64 SIMD, 10,000 mixed bodies,
1,000 active dynamics, 120 fixed ticks, a 200 m/s thin-wall `LinearCast`, and
10,000 downward rays. The disposable harness used Jolt's reference pool only
for provider measurement; TerraEngine production integration prohibits it and
is validated through the T017 shared-pool adapter. Raw Windows JSON is under
the ignored `out/physics-spike/results/Jolt56/results/` tree; the Linux result
is produced by the committed shell runner.

## Query-bound calibration

The selected values must support the 10,000-request acceptance batch while
keeping every input and retained history bounded.

| Setting | Calibrated value | Evidence |
|---|---:|---|
| Maximum cast distance | 100,000 m | Finite large-world cap; spike rays and later oversize rejection fixture |
| Maximum shape extent | 10,000 m | Broad-phase-safe authored/cooked input cap with explicit oversize rejection |
| Maximum hits per query | 4,096 | Bounded dense-contact result storage with truncation diagnostics |
| Maximum requests per batch | 10,000 | Windows and Linux acceptance batches completed |
| Retained result ticks | 8 | Bounded multi-consumer window; stress test verifies high-water behavior |

## Decision

Jolt Physics v5.6.0 at commit `e77f175595e64cb44218cc9d9d56fc365ad0e36a`
passes the provider-selection gate and is approved for permanent addition as a
pristine git submodule at `External/JoltPhysics`. The production build uses
CPU-only float-position mode, disables provider samples/tools/compute surfaces,
and replaces the disposable reference job pool with TerraEngine's shared task
adapter. Any revision, provider, source patch, or broader scope requires renewed
planning and explicit approval.
