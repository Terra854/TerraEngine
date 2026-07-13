# Physics Backend Selection Evidence

**Schema version**: 3  
**Decision owner**: TerraEngine physics feature 042  
**Status**: Complete — Jolt Physics v5.6.0 passes every provider-selection hard gate.  
**Approved implementation revision**: Jolt Physics v5.6.0, commit `e77f175595e64cb44218cc9d9d56fc365ad0e36a`

This is the measured T003 decision record. Raw results are append-only under
`out/physics-backend-spike`; every accepted run below has an exact SHA-256.
The disposable probe establishes provider capabilities only. Engine-level
transaction, generation, publication, and callback-order guarantees remain
implementation requirements for later phases.

## Candidate and exact-revision manifest

| Candidate | Release | Exact commit | Git tree | License | Decision status |
|---|---|---|---|---|---|
| Jolt 5.5 | `v5.5.0` | `23dadd0e603f1b321142d4c74df07fce85064989` | `9e96cdd7a339a01b74f6e84d2fecfd233425a0b2` | MIT | Comparison only |
| Jolt 5.6 | `v5.6.0` | `e77f175595e64cb44218cc9d9d56fc365ad0e36a` | `9bc76dd82ae320398f894f69dd3c7735075a7b3e` | MIT | **PASS — selected and approved** |
| PhysX 5.8 | `110.0-omni-and-physx-5.8.0` | `fc1018a3745664a1db2b95ce03fb5e91eb585f2e` | `9354f87ca356c446d215b5167bc9ced62ee8748a` | BSD-3-Clause | Research candidate; offline configure failed |

The Jolt 5.6 accepted manifests record the upstream URL, tag, commit, tree,
source-directory acquisition timestamp and basis, clean tracked-tree status,
source bytes, run timestamp, compiler, CMake, CPU/OS, commands, timings, logs,
installed-tree file hashes, and dependency closure. Source acquisition occurred
before offline measurement.

## Reproduction and environment

Windows Release example:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
	-File Tools/Physics/BackendSpike/RunBackendSpike.ps1 `
	-Candidate Jolt56 `
	-Jolt56Source out/physics-spike/sources/JoltPhysics-5.6.0 `
	-WorkRoot out/physics-backend-spike `
	-RunId jolt56-windows-final-float `
	-Configuration Release `
	-FixtureRepeats 3 `
	-FixtureTimeoutSeconds 180
```

Linux Release example:

```bash
bash Tools/Physics/BackendSpike/RunBackendSpike.sh \
	--candidate jolt56 \
	--jolt56-source out/physics-spike/sources/JoltPhysics-5.6.0 \
	--work-root out/physics-backend-spike \
	--run-id jolt56-linux-final-float \
	--configuration Release \
	--fixture-repeats 3 \
	--fixture-timeout-seconds 180
```

Reference machine: AMD Ryzen 9 7950X, 16 physical/32 logical cores. Windows
used Windows 10.0.26200, SDK 10.0.26100.0, MSVC 19.51.36248.0, and CMake
4.3.2. Linux used WSL2 kernel 5.15.167.4, GCC 13.3.0, and CMake 3.28.3.
Provider and fixture were static Release builds; the fixture is C++20. The
accepted sanitizer lane used Jolt's `ReleaseASAN` configuration with
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1`.

All runs enforced loopback-only proxy variables, disabled CMake package
registries, and requested disconnected FetchContent. The Jolt core CMake tree
contains zero FetchContent, ExternalProject, or `file(DOWNLOAD)` primitives, so
the disconnected variables are intentionally unused rather than bypassed.

## Hard pass/fail gates

| Gate | Pass criterion | Jolt 5.5 comparison | Jolt 5.6 | PhysX 5.8 | Evidence |
|---|---|---|---|---|---|
| Exact revision | Immutable commit/tree and clean checkout recorded | PASS | **PASS** | PASS | Accepted manifests and raw hashes below |
| Offline/hermetic build | Configure/build/install with no downloads or package-registry hits | Windows PASS | **Windows + Linux PASS** | FAIL | PhysX direct configure requires absent Packman/freeglut artifacts |
| Hermetic output | No global physics/GPU SDK or provider runtime | PASS | **PASS** | Not reached | PE closure is only `KERNEL32.dll`; Linux `ldd`/`readelf` and installed configs are clean |
| Shared-pool compatibility | Jolt jobs/barriers use TerraEngine `ThreadPool`; no provider pool | PASS | **PASS** | Not measured | 1/2/4/32 pump limits; queued equals completed in every process |
| Required feature surface | Shapes, CCD, filters/materials, queries, constraint, character, events, debug callbacks | PASS | **PASS** | Not measured | All booleans true; actual `DebugRenderer` geometry callbacks observed |
| Lifecycle safety | Replacement, cancellation, drain, restart; no late callbacks/UAF/leak | PASS | **PASS** | Not measured | 100 replacements/process, four cancelled jobs drained, zero late callbacks; ASAN passes |
| Deterministic publication | Repeated engine packet/query and backend-state hashes match | PASS | **PASS** | Not measured | 52 accepted 5.6 processes across OS/configuration lanes |
| Large-world policy | Float/double error, far queries, memory, and cost recorded | Comparison only | **PASS** | Not measured | Float error 0.25 m at 10,000,000.25 m; double error 0; both far rays hit |
| Performance bound | Within 10% of best passing comparison on calibrated workload | Comparison baseline | **PASS** | Not measured | 5.6 median p95 was best at every worker limit |
| Redistribution | License/notice/source/binary obligations acceptable | PASS | **PASS** | Research only | MIT record in `Docs/Dependencies/JoltPhysics.md` |

The PhysX failure is a candidate hermeticity result, not a selected-provider
failure. Its public CMake path could not configure without Packman-supplied
`freeglut.dll`/`freeglutd.dll`. The approved stop rule applies if Jolt 5.6
fails; Jolt 5.6 passed, so T004 may proceed with the exact approved commit.

## Feature, lifecycle, determinism, and precision results

Each Release precision mode ran three separate processes at pump limits 1, 2,
4, and 32 on Windows and Linux (48 processes). ReleaseASAN ran one process at
each limit (4 more processes). Every accepted process reported:

- all required feature flags true, including real `DebugRenderer` callbacks;
- no Jolt-private thread pool, truthful pump concurrency, and equal queued and
  completed job counts;
- 100 create/update/drain/destroy world replacements, zero late callbacks,
  four stop-flag cancellations skipped, and successful restart/drain status;
- `StateRecorderImpl` restore/re-save equality and a stable backend-state hash;
- stable sorted packet, dense-query, and 10,000-query-batch hashes;
- 100 identical repetitions of the full 10,000-ray batch;
- balanced custom allocator counts and zero current bytes in Release; ASAN
  correctly reported its Jolt-mandated default allocator mode and no failures.

| Configuration | Processes | State hash | Packet hash | Dense hash | 10k batch hash | Peak process RSS |
|---|---:|---|---|---|---|---:|
| Windows float Release | 12 | `f25ad64130e71dd2` | `39729be241f30b15` | `7d9521c5f8c4e79e` | `a8942d6ef64d1783` | 15,626,240 B |
| Windows double Release | 12 | `2851e3334ec8dcb2` | `39729be241f30b15` | `7d9521c5f8c4e79e` | `a8942d6ef64d1783` | 16,646,144 B |
| Linux float Release | 12 | `f25ad64130e71dd2` | `39729be241f30b15` | `7d9521c5f8c4e79e` | `a8942d6ef64d1783` | 77,565,952 B |
| Linux double Release | 12 | `2851e3334ec8dcb2` | `39729be241f30b15` | `7d9521c5f8c4e79e` | `a8942d6ef64d1783` | 17,555,456 B |
| Linux float ReleaseASAN | 4 | `24660e07b6bf0e32` | `39729be241f30b15` | `7d9521c5f8c4e79e` | `a8942d6ef64d1783` | 836,562,944 B |

State hashes are compared only within an identical build/configuration. Packet
and query hashes are provider-neutral and matched across both operating systems.

## Performance and size metrics

The calibrated performance fixture contains 10,000 bodies, 1,000 active
dynamics, 30 warm-up ticks, and 120 measured ticks. The table uses the median
of three Windows Release runs at the 32-pump limit; per-limit raw values remain
in each run bundle.

| Candidate | Configure s | Build s | Install s | Source B | Build B | Install B | p50 ms | p95 ms | p99 ms | Peak Jolt allocation B | Peak RSS B | Pump job utilization | Barrier wait ms | State / packet hash |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| Jolt 5.5 | 4.844 | 3.655 | 0.782 | 34,605,797 | 198,693,593 | 74,494,913 | 0.5266 | 0.6612 | 0.7595 | 27,888,966 | 15,798,272 | 2.73% | 131.683 | `f25ad64130e71dd2` / `39729be241f30b15` |
| Jolt 5.6 | 4.108 | 3.678 | 0.820 | 36,600,709 | 194,141,439 | 72,336,874 | 0.4991 | 0.6334 | 0.6643 | 27,888,966 | 15,626,240 | 2.70% | 139.457 | `f25ad64130e71dd2` / `39729be241f30b15` |
| PhysX 5.8 | Configure failed | — | — | Recorded in failed run | — | — | — | — | — | — | — | — | — | — |

Pump job utilization is adapter-observed job execution divided by elapsed
process time and available pump lanes; caller-thread barrier work is not counted.
At pump limits 1/2/4/32, Jolt 5.6 median p95 values were 0.3069, 0.2873,
0.2705, and 0.6334 ms. Jolt 5.5 values were 0.3579, 0.3384, 0.3068, and
0.6612 ms. Jolt 5.6 was the best measured comparison at every limit and thus
comfortably satisfies the 10% bound.

## Query-bound calibration

| Setting | Accepted value | Evidence |
|---|---:|---|
| Maximum cast distance | 100,000 m | Boundary accepted; non-finite and 100,001 m rejected before provider allocation |
| Maximum shape extent | 1,000 m | Boundary body entered broad phase and was ray-hit; 1,001 m rejected |
| Maximum hits per query | 64 | Dense 96-hit ray sorted deterministically and truncated to 64 |
| Maximum requests per batch | 10,000 | Full batch completed and matched over 100 repetitions/process |
| Retained result ticks | 2 | Worst-case retained payload is bounded to 15,360,000 B with current `RayCastResult` size |

These initial limits are provider-selection inputs for `PhysicsSettings`. The
runtime adapter must validate finite inputs before calling Jolt, retain stable
engine-owned ordering, and publish an explicit rejected/truncated status.

## Raw artifact index

| Artifact | SHA-256 |
|---|---|
| `out/physics-backend-spike/jolt56-windows-final-float/Jolt56/evidence/raw-result.json` | `2dd6cc91a0aa6e96ea19ac2cf69a27ce7778593de0976697aa82a9074077b235` |
| `out/physics-backend-spike/jolt56-windows-final-double/Jolt56/evidence/raw-result.json` | `5bd27ae696686de353df22779ed5e5d3ef949a78c0fc8723e58605c4fedb9ebe` |
| `out/physics-backend-spike/jolt56-linux-final-float/jolt56/evidence/raw-result.json` | `688145f74f107bd540fbb045fd718540bced828b5b00f06f26892570064b84c2` |
| `out/physics-backend-spike/jolt56-linux-final-double/jolt56/evidence/raw-result.json` | `f3158688c8a3581cdb358e325ef9f89c1533339310e92c14e79535d0cfcad908` |
| `out/physics-backend-spike/jolt56-linux-final-asan/jolt56/evidence/raw-result.json` | `45d9104d8e66a4c2d97b7430afcec17a5d463f2d0483eef1b2230636f7557311` |
| `out/physics-backend-spike/jolt55-windows-final-comparison/Jolt55/evidence/raw-result.json` | `9d30119ca50e773844e25e3de357373f4369317d14ea4e558f111354f041f27e` |
| `out/physics-backend-spike/physx58-windows-release-3/PhysX58/logs/provider-configure.log` | `e526f0fe33b6422743056d6cfbe282e364e3d46adae2500b32295de68ef9529a` |

Each directory also contains checkout/environment manifests, exact command
records, configure/build/install logs, fixture JSON per worker/repeat, dependency
closure output, installed-tree hashes, and aggregate hashes.

## Decision

**Select Jolt Physics v5.6.0 at commit
`e77f175595e64cb44218cc9d9d56fc365ad0e36a`.** It passes every hard gate,
beats the 5.5 comparison in the calibrated p95 profiles, has acceptable MIT
redistribution terms, and requires no global SDK or private worker pool. T004
is authorized to add only this pristine revision under `External/JoltPhysics`.
Jolt 5.5 remains comparison evidence only; PhysX remains ineligible without a
new planning and approval cycle.
