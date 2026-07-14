# US6 Managed and Automation Validation

Date: 2026-07-14  
Configuration: RelWithDebInfo, Windows x64  
Scope: T114-T128

## Result

PASS. The versioned native/NativeAOT ABI, managed physics surface, protected
event delivery, and self-registering editor automation provider compile and
pass their focused integrated gates. No provider-owned type, pointer, worker
pool, or mutable world is exposed through managed or MCP contracts.

## Implementation checks

- Fixed-width ABI layout/version assertions cover handles, receipts, queries,
  snapshots, event packets, and the append-only NativeAOT VTable tail.
- Body and character handles are validated against published snapshots;
  rejected query receipts are initialized and caller-owned buffers are copied.
- Managed immediate and scheduled queries preserve numeric correlations.
- Wrapper reload invalidation, event subscription snapshots, and per-callback
  exception isolation are covered by the managed integration filter.
- Runtime MCP commands require exact object/component/world generations,
  idempotency, and explicit `applyOnNextStep=true` while paused.
- `physics.body.apply_force` now distinguishes ordinary `Force` from
  mass-independent `Acceleration`; unsupported modes fail before admission.
- Contact reads apply target/layer/tick bounds and aggregate stable retained
  packets from a separate bounded diagnostics history. Gameplay subscription
  filtering remains unchanged even when automation observes contacts.
- Physics diagnostics expose immutable scheduler phase/read/write/boundary and
  worker-safety declarations.

## RelWithDebInfo build and test evidence

The orchestrator serialized all shared build-tree use. The integrated affected
target build completed with exit code 0:

```powershell
cmake --build --preset RelWithDebInfo --target TerraEngine-EngineTests TerraEngine-EditorTests TerraEngine-Export TerraEngine.Managed.Output TerraEngine.Export.Generator --parallel 4
cmake --build --preset RelWithDebInfo --target TerraEngine.Editor.Managed TerraEngine.Managed.Output --parallel 4
```

The managed self-contained TerraEngine and TerraEngine.Editor publications
completed with zero build errors. Direct focused runs passed:

- `Engine.PhysicsManagedAbi,Engine.PhysicsManagedIntegration`: 8 matching tests,
  0 failures.
- `Editor.MCP.Physics`: 11 matching tests, 0 failures after the retained raw
  diagnostics-history regression was added (`484 tests, 0 failures` reported by
  the filtered harness).

Final combined CTest command and result:

```powershell
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
  -R "^(Engine\.PhysicsUS6|Editor\.PhysicsUS6)$" `
  --output-on-failure --no-tests=error
```

Result: 2/2 passed (`Engine.PhysicsUS6` and `Editor.PhysicsUS6`) in 1.14 s.

## Install gate

The required installed-tree gate completed successfully after the final code
changes:

```powershell
cmake --build --preset RelWithDebInfo --target install --parallel 4
```

The canonical runnable tree was refreshed at
`out/build/RelWithDebInfo/Build`, including the native editor, managed
RelWithDebInfo publications, export/compiler tools, physics collider compiler,
stress executables, headers, Jolt notices, and required runtime dependencies.

## Red/fix evidence

Review and installed testing exposed two observable missing contracts before
the final pass: automation force commands hardcoded ordinary force, and contact
tick ranges consulted only the newest subscription-filtered packet. The final
tests and live loopback fail without those corrections. They now pass with a
typed acceleration mapping and a separate bounded raw diagnostics history,
without weakening gameplay event filters.

