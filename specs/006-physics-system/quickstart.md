# Quickstart Validation Guide: Complete Physics System

## Purpose

This guide defines the end-to-end validation sequence the completed feature must satisfy. It is a runbook, not implementation code. Until the corresponding tasks land, physics-specific targets, filters, fixtures, and tools described below are expected future outputs.

Refer to:

- [Implementation plan](plan.md)
- [Data model](data-model.md)
- [Runtime contract](contracts/runtime-api.md)
- [Serialization/assets](contracts/serialization-assets.md)
- [Managed API](contracts/managed-api.md)
- [Automation API](contracts/automation-api.md)
- [Editor authoring](contracts/editor-authoring.md)
- [Export parity](contracts/export-parity.md)

## Prerequisites

- Clean checkout including all pinned submodules after the provider task is approved and implemented.
- Windows x64 Visual Studio developer environment for first-release acceptance.
- CMake/Ninja and the repository-bundled toolchain/dependencies.
- No globally installed physics SDK.
- A disposable project/work directory outside committed fixtures for live editor and export validation.
- Reference hardware/build configuration recorded for performance results.

## 1. Provider Selection Evidence

Before accepting permanent provider integration, run the selection spike for the approved Jolt 5.6 candidate and the unapproved PhysX 5.8 research fallback using the exact revisions in [research.md](research.md). Jolt must use its CPU-only configuration; GPU compute and hair simulation are outside scope.

The evidence must prove:

- Network-disabled configure, build, and install.
- No global/system package dependency.
- No provider-private recurring worker pool.
- Required shape, CCD, filter, material, query, constraint, character, and debug coverage.
- Repeated command-stream ordering and lifecycle safety.
- Recorded build size/time, tick percentiles, allocation/memory, worker utilization, barriers, and hashes.

Do not add or retain `External/JoltPhysics` permanently if a hard spike gate fails. Record the blocker and evaluate the fallback rather than weakening a requirement.

## 2. Configure and Compile

From a Visual Studio developer shell:

```powershell
cmake --preset Debug
cmake --build --preset Debug --target TerraEngine TerraEngine-Editor TerraEnginePhysicsColliderCompiler TerraEngine-EngineTests TerraEngine-EditorTests TerraEngine-Export
```

Expected outcome:

- The native engine/editor, managed bridge, headless collider compiler, test harnesses, and export tool compile.
- Jolt samples, viewer, tests, and unrelated tools are not normal TerraEngine build targets.
- No provider headers are required by gameplay-facing public consumers.

## 3. Focused Engine Validation

Run the physics-focused harness slice:

```powershell
$env:TERRAENGINE_TEST_FILTER = "Physics;SystemScheduler.Physics;PhysicsTransaction;PhysicsSerialization;Physics.Managed;Physics.Scripting"
./out/build/Debug/TerraEngine-EngineTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

Expected coverage:

- Units/math conversion, scale rejection, global gravity inheritance, per-body `useGravity` disable/enable behavior, and custom directional gravity applied through mass-independent acceleration on fixed ticks.
- Stable handle allocation and stale generation rejection.
- Component defaults/reflection/serialization.
- Collider recipes, hashing, materials, layers, and filtering.
- Fixed-step accumulation, clamp, pause, and 100 consecutive exact Next requests: each produces one configured fixed physics tick, one fixed/ordinary callback pass with that duration, one publication, one render, then paused.
- Scheduler resources/workloads and After-Physics visibility.
- Coordinated transaction success, conflict, cancellation, and rollback.
- Rigid body, CCD, sleep, trigger, constraint, character, query, and event fixtures.
- Managed commands, wrapper invalidation, event exception isolation, and exported ABI parity.

## 4. Focused Editor Validation

```powershell
$env:TERRAENGINE_TEST_FILTER = "Editor.Physics;Editor.Automation.Physics;Physics.Prefab;Physics.Undo;Physics.Export"
./out/build/Debug/TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

Expected coverage:

- Inspector authoring for repeated same-type collider/constraint records.
- Stable record IDs through reorder, undo/redo, prefab apply/revert, duplication, and UUID remap.
- Project Physics settings and layer rename/reference stability.
- Scene View debug packets with paused and renderer-unavailable cases.
- Diagnostics window and profiler scopes/counters.
- Play Mode capture, fresh world, pause/single-step, job join, and exact authored restoration.
- Automation discovery, bounds, generations, approvals, impact fingerprints, idempotency, durable jobs, and absence of project lifecycle capabilities.
- Cooked asset atomic publication, last-known-good behavior, export validation, and evidence.

## 5. CTest Regression Gates

Run the focused cross-cutting set first:

```powershell
ctest --test-dir out/build/Debug -C Debug -R "Physics|TransactionalMutation|SystemScheduler|Editor\.Automation|Editor\.Export|SceneRuntimeParity" --output-on-failure
```

Then run the complete project-owned suite:

```powershell
ctest --test-dir out/build/Debug -C Debug --output-on-failure
```

Expected outcome: all applicable tests pass. Failures in non-physics systems are investigated because scheduler, lifecycle, asset, managed, and export changes are cross-cutting.

## 6. Stress, Ordering, and Lifecycle

Run the committed physics fixture manifest through the dedicated stress runner or harness filter:

```powershell
$env:TERRAENGINE_TEST_FILTER = "Physics.Stress;Physics.Ordering;Physics.Lifecycle;Physics.LargeWorld"
./out/build/Debug/TerraEngine-EngineTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

Required evidence:

- At least 10,000 mixed bodies and 1,000 active dynamics at 60 fixed updates/second on recorded hardware.
- At least 10,000 batched rays with bounded memory and identical ordering over 100 repeated runs.
- Identical engine packet hashes for repeated launches and supported worker counts from the same state.
- 100 remove/recreate/reload/shutdown race repetitions with zero stale publication.
- 1,000 scene load/unload and Play Mode enter/exit cycles with zero active jobs, late callbacks, or authored changes.
- Dense stacks, contacts, triggers, constraint chains, and character/moving-platform fixtures within documented tolerances.

Where supported, repeat the bounded lifecycle suite under ASan/TSan. Sanitizer evidence supplements rather than replaces normal acceptance.

## 7. Install Tree

The feature is not complete until installation succeeds:

```powershell
cmake --build --preset Debug --target install
```

Verify the installed tree contains:

- Editor and exported runtime with the selected provider objects.
- Managed physics assemblies/NativeAOT bindings.
- Headless physics collider compiler where intended.
- Physics documentation and required runtime assets/settings.
- No Jolt samples, viewer, source assets, or global SDK references.

Use `out/build/Debug/Build` as the canonical installed runtime root where the current project workflow produces it.
TerraEngine install destinations are intentionally rooted at the build tree's
`Build` directory; `out/install/Debug` is the preset's conventional prefix but
is not the runtime tree consumed by the installed-editor validation workflow.

## 8. Installed Editor and Automation Loopback

Launch the installed editor with isolated preferences and open a disposable project using native editor controls. Enable the loopback bridge and writes explicitly.

Validate:

1. From a clean project, time creation and first run of the static/dynamic/kinematic falling-body scene and confirm completion in under 10 minutes.
2. Create the remaining trigger, compound, cooked mesh, constraint, and character fixtures; create/move/rename/reload a PhysicsMaterial asset; then edit, multi-edit, undo/redo, prefab apply/revert, duplicate, save, and reload.
3. Enter Play Mode, inspect runtime state, pause, press Next 100 consecutive times, and verify each request produces exactly one configured fixed physics tick, one fixed/ordinary callback pass, one publication, one rendered result, and a return to paused state before exiting.
4. Enable collider/contact/constraint/character/query visualization and open Physics Diagnostics.
5. Discover physics automation capabilities; confirm project create/open/close are absent.
6. Run one bounded query and one durable batch/cook job.
7. Apply an idempotent impulse twice and prove it occurs once.
8. Exercise one approved high-impact settings/layer change and one denied/stale-fingerprint change.
9. Reload scripts and prove old wrappers fail safely.
10. Disable rendering or use the renderer-unavailable fixture and prove physics cleanup remains safe.

Expected outcome: no direct provider identifiers appear in UI/API/log contracts; all results include correlation/world/tick context; exiting Play Mode restores exact authored state.

## 9. Headless Export and Parity

After installation, extend and run the existing baseline workflow against a disposable physics project:

```powershell
pwsh -NoProfile -File Tools/ExportRuntimeBaseline/RunBaseline.ps1 `
  -InstallRoot out/build/Debug/Build `
  -WorkRoot out/ci/physics-export-baseline
```

Then run the exported physics fixture without source models or any globally installed physics SDK.

Verify the six durable evidence files:

- `export-result.json`
- `resolved-manifest.json`
- `content-inventory.json`
- `validation-report.json`
- `progress.jsonl`
- `package-validation.txt`

Expected outcome:

- Settings, layers, material GUIDs, cooked collider GUIDs, provider revision, and cook version are present and checksummed.
- Every required payload is nonempty and not a placeholder.
- Seeded missing/corrupt/empty/provider-mismatch assets fail before launch.
- Editor Play Mode and export invariant records match for bodies, stepping, filtering, queries, events, constraints, characters, scene transitions, and shutdown.
- Export exits with zero active physics jobs or callbacks.

## Acceptance Record

Archive or link:

- Exact source revisions and user-approved dependency record.
- Configure/build/test/install logs.
- Provider spike and performance results with hardware/build settings.
- Focused and complete CTest results.
- Stress/soak/order/lifecycle hashes and diagnostics.
- Installed editor loopback transcript/screenshots where useful.
- Export evidence and editor/export invariant comparison.

The feature is ready only when all required records pass or a specification-level blocker is documented without silently reducing scope.
