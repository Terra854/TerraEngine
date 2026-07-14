# Spec 042 Final Acceptance (T153)

Validation date: 2026-07-14  
Configuration: `RelWithDebInfo`, Windows x64

## Acceptance result

**PASS for FR-001 through FR-042 and SC-001 through SC-013.** FR-043 remains
explicitly outside the initial release scope.

The acceptance used three independent integrated paths:

1. The final post-install complete CTest suite passed 47/47 in 33.48 seconds.
2. The installed Vulkan editor completed the canonical 10,000-body live run,
   authoring walkthrough, managed reload, automation, 100 paused Next steps,
   profiler capture, Play Mode exit, and orderly process shutdown.
3. A fresh headless cook/export/launch run produced matching editor and
   packaged-runtime physics invariants with no source-asset or global physics
   dependency at runtime.

The installed-editor command was:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File Tools/Physics/RunInstalledEditorPhysicsStress.ps1 `
  -InstallRoot out/build/RelWithDebInfo/Build `
  -WorkRoot out/validation/t153-installed-editor-live-final `
  -ObjectCount 10000 -ActiveDynamicCount 1000 `
  -ExplicitStepCount 100 -WorkerCount max -McpPort 43391 `
  -RunAuthoringWalkthrough -RunUs6Loopback -AllowLiveStress
```

It passed in 23.287 seconds with 32 workers, 1,000 dynamic, 1,000 kinematic,
and 8,000 static bodies. Physics publication p50/p95/p99 was
59.41/84.87/92.74 ms. Authoring create/edit/undo/redo and prefab
apply/revert completed and cleaned up. Managed compilation/reload, runtime
discovery, idempotent custom-gravity acceleration, queries, and event reads
completed. The authored scene SHA-256 was identical before and after Play Mode,
and `forcedTermination` was false.

The export parity run at `out/validation/t153-final-export-parity-5` passed with
identical editor and runtime values: 4 bodies, 1 character, 1 constraint, 90
fixed steps, 1 query hit, event presence, 2 scene transitions, gravity -15.5
m/s^2, a 20 ms fixed step, broad-phase group 7, cooked-collider use, orderly
shutdown, and zero queued or active jobs after drain. The durable records are
under `evidence/export-parity/`.

## Functional-requirement traceability

| Requirement | Status | Reproducible evidence |
|---|---|---|
| FR-001 | Pass | `Engine.PhysicsUS1`, installed-editor 100-step run, and `us1-validation.md` cover body modes, fixed-step bounds, pause, Next, callbacks, publication, and restoration. |
| FR-002 | Pass | `Engine.PhysicsUS2` and `us2-validation.md` cover every required primitive, convex, compound, mesh, and multi-collider path. |
| FR-003 | Pass | `Physics.Scheduler.ColliderOnlyObjectsBecomeStaticAndBodiesWithoutCollidersAreExcluded` verifies implicit static geometry and the actionable exclusion diagnostic. |
| FR-004 | Pass | US1 simulation, material/filter/settings tests, and the live idempotent acceleration command cover gravity, layers, masks, triggers, sleeping, limits, interpolation, collision modes, and custom gravity. |
| FR-005 | Pass | Component reflection, serialization, inspector, and settings tests verify SI units, default gravity, and editor angle conventions. |
| FR-006 | Pass | Collider recipe and cooked-shape validation reject unsupported scale, transform, ownership, and geometry combinations. |
| FR-007 | Pass | `Engine.PhysicsUS4` covers all eight required constraint families, limits, springs, motors, breakage, and collision policy. |
| FR-008 | Pass | `Engine.PhysicsUS4` covers provider-neutral character configuration, movement, grounding, stairs/platform support, jump, resize, and recovery. |
| FR-009 | Pass | `Engine.PhysicsUS3` and the determinism stress cover all query families, filters, dimensions, batch/result/history limits, and 10,000-ray batches. |
| FR-010 | Pass | US3 query/provider tests verify complete hit context, generations/ticks, and stable tie ordering. |
| FR-011 | Pass | US3 event tests verify canonical ordered collision/contact/activation/sleep/trigger/break packets, filters, bounds, and overflow reporting. |
| FR-012 | Pass | US3 stale-generation tests and 100 stale-race repetitions reject replaced object, component, body, and world identities. |
| FR-013 | Pass | Command-queue, gameplay, managed, editor, and automation tests verify typed deferred admission and deterministic producer merging. |
| FR-014 | Pass | Scheduler and transaction tests verify worker isolation and visibility only after the declared Physics publication boundary. |
| FR-015 | Pass | Transaction, command, prefab, provider-staging, and serialization rejection tests verify atomic conflict, stale, invalid, cancellation, and rollback behavior. |
| FR-016 | Pass | `Engine.PhysicsUS5`, diagnostics tests, and the live seven-entry scheduler snapshot expose phases, resources, worker safety, and protected boundaries. |
| FR-017 | Pass | Job-adapter, backend, cooking, and shared-thread-pool tests cover engine-owned task execution and inactive-world suppression. |
| FR-018 | Pass | `Engine.PhysicsStress` compares backend and sorted-packet hashes across process launches and 1/2/4/max workers for the promised same-build scope. |
| FR-019 | Pass | Provider-boundary, ABI, serialization, automation, and export tests keep authored/runtime contracts provider-neutral. |
| FR-020 | Pass | `backend-selection.md` records comparison evidence, the Jolt 5.6.0 pin, limitations, and rationale. |
| FR-021 | Pass | Dependency-contract, setup, build, and install audits verify repository-pinned Jolt and no installed provider samples/source dependency. |
| FR-022 | Pass | Collider compiler/cooking tests cover validation, asynchronous/headless use, recipe deduplication, atomic publication, provenance, cancellation, and diagnostics. |
| FR-023 | Pass | Material/collider move, migration, GUID, cook, and export tests preserve stable project identity without runtime source models. |
| FR-024 | Pass | Component/settings/material/layer/constraint/character serialization tests verify versioned schemas, defaults, migrations, and transient-state exclusion. |
| FR-025 | Pass | Serialization and prefab tests cover save/reload, duplication, apply/revert, identity remapping, cycles, and atomic rejection. |
| FR-026 | Pass | `Editor.PhysicsUS5` covers inspectors, units/ranges, contextual validation, material/layer selection, multi-edit, handles, and fit-to-mesh. |
| FR-027 | Pass | Editor undo/prefab tests and the live walkthrough verify grouped undo/redo, override apply/revert, and protected runtime rebuild publication. |
| FR-028 | Pass | Play Mode tests, 1,000-cycle soak, and identical live scene hashes verify fresh worlds, drain, destruction, and exact authored restoration. |
| FR-029 | Pass | Scene View tests cover immutable debug packets, paused reads, shapes/contacts/constraints/characters/queries, selection, and renderer-unavailable safety. |
| FR-030 | Pass | Diagnostics/profiler tests and live capture expose timing, counts, caches, traffic, queues, rejection/stale causes, memory, and allocator high-water marks. |
| FR-031 | Pass | Managed ABI/integration tests and live reload/query/command execution verify native/managed parity and stale-wrapper safety. |
| FR-032 | Pass | Managed integration tests verify correlated per-callback exception isolation without disrupting later delivery or simulation. |
| FR-033 | Pass | `Editor.PhysicsUS6` contract tests verify discoverable, bounded capabilities scoped to an already-open native editor project. |
| FR-034 | Pass | MCP safety tests and live automation verify impact preflight/approval, generations, idempotency, durable jobs, and correlated diagnostics. |
| FR-035 | Pass | Export manifest, validator, cooker, package, and seeded corruption tests verify required physics content and fail before launch. |
| FR-036 | Pass | Fresh editor/export records match every startup, simulation, query, event, constraint, character, transition, and shutdown invariant. |
| FR-037 | Pass | Lifecycle stress, editor soak, managed reload, renderer failure, project close, and live shutdown verify stop/drain/join/invalidate/release ordering. |
| FR-038 | Pass | Invalid-authoring tests isolate affected records; fatal provider allocation regression contains simulation and still permits orderly cleanup. |
| FR-039 | Pass | Collider reimport tests verify failed recooks retain and clearly mark the last known-good result as stale. |
| FR-040 | Pass | Performance, cache, localized rebuild, sleeping-publication, bounded-history, and provider-allocation tests verify bounded/reused hot paths. |
| FR-041 | Pass | This matrix maps every required contract category to tests or durable acceptance evidence. |
| FR-042 | Pass | Final RelWithDebInfo configure/build/test/install and installed-tree verification are recorded in `full-regression.md` and `install-validation.md`. |

## Success-criteria traceability

| Criterion | Status | Reproducible evidence |
|---|---|---|
| SC-001 | Pass | The installed-editor authoring walkthrough completed create/edit/undo/redo/prefab operations and ran static/dynamic/kinematic interaction in the 23.287-second automated acceptance. |
| SC-002 | Pass | `Physics.Serialization.AcceptanceFixturePreservesPhysicsAndPrefabApplyRevertParity` verifies 100% scene and prefab parity for required physics properties. |
| SC-003 | Pass | `performance-validation.md` records the canonical 10,000-body/1,000-active 1/2/4/32-worker matrix and reference hardware; all fixed-tick budgets passed. |
| SC-004 | Pass | `Engine.PhysicsStress` completed 100 identical 10,000-ray batches with stable hashes and bounded post-warm-up allocation growth. |
| SC-005 | Pass | The stress registration completed 100 remove/recreate, scene reload, script reload, and shutdown races with zero stale effects. |
| SC-006 | Pass | Transactional mutation, command cancellation/idempotency, prefab, constraint-target, and invalid-collider suites leave zero partial live or physics state. |
| SC-007 | Pass | Fixed-step tests and the live 100-step run produced one tick/publication per Next and remained paused; no autonomous paused ticks occurred. |
| SC-008 | Pass | All required fixture categories in this matrix are passing; full CTest is 47/47. |
| SC-009 | Pass | Fresh editor and exported-runtime invariant JSON records match 100%. |
| SC-010 | Pass | Export validation tests detect every seeded missing, corrupt, empty, and placeholder physics dependency before launch. |
| SC-011 | Pass | Engine lifecycle and editor Play Mode soak each completed 1,000 cycles with zero active jobs, late callbacks, stale publications, or authored changes. |
| SC-012 | Pass | Scene View diagnostics remain readable while paused and renderer-unavailable lifecycle/performance fixtures pass without physics failure. |
| SC-013 | Pass | The documented RelWithDebInfo configure/build/test/install path succeeds using the repository-pinned provider and produces the verified installed runtime tree. |

## Explicitly excluded scope

FR-043 remains out of scope: this run does not claim ragdolls, vehicles, soft
bodies, buoyancy, force fields, cross-platform deterministic lockstep,
networking rollback, fracture, fluids, GPU rigid bodies, runtime provider
switching, or arbitrary user callbacks inside physics workers.
