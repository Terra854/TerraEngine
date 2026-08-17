# Terrain automation

Terrain automation accepts the versioned `terrain-edit-v1.schema.json` envelope
at the editor boundary. The installed validation runner is
`Tests/Tools/Terrain/RunTerrainValidation.ps1`; it checks the installed tree
before any operation is attempted.

## Installed validation

```powershell
$install = (Resolve-Path 'out/build/RelWithDebInfo/Build').Path
& (Join-Path $install 'Tests/Tools/Terrain/RunTerrainValidation.ps1') `
    -InstallRoot $install `
    -OutputDirectory (Join-Path $install 'out/terrain-validation') `
    -DryRun
```

Dry-run verifies the editor, terrain compiler, fixture generator, benchmark,
schema, terrain manifest, recovery manifest, and workload manifest. It records
metrics as unavailable because no provider or job was executed. Missing
artifacts are an error and return a non-zero exit code.

Live mode requires an explicitly supplied automation adapter. The adapter is a
small installed executable boundary, not a replacement terrain implementation.
For each requested operation it receives:

```text
--operation <create|inspect|edit|rebuild|validate|import|export|save|parity>
--schema <installed terrain-edit-v1.schema.json>
--fixture <installed terrain manifest>
--recovery-fixture <installed save/recovery manifest>
--output <receipt path>
```

It must publish JSON with `status: "executed"` and the matching operation name.
The runner rejects missing receipts, non-zero adapter exits, and receipts for a
different operation. It never converts an unavailable capability into passing
evidence.

## Request and safety semantics

Every mutation identifies the terrain asset and instance, uses canonical signed
world coordinates or explicit bounds, supplies expected root/area generations
and hashes, and includes correlation and idempotency identities. Budgets cover
areas, samples, prepared bytes, and workload class. Destructive or large work
also carries the approval fingerprint produced by protected preflight.

The provider lowers valid JSON into typed terrain requests before worker work.
Rejected schema, coordinate, budget, approval, stale-generation, shared-asset,
or layer-generation requests do not partially mutate authored state. Accepted
large imports, exports, erosion, spline work, rebuilds, saves, and validation
may return durable cancellable jobs. Cancellation discards private work before
the protected publication boundary.

Receipts retain operation, correlation, idempotency, transaction, and job
identities; outcome and structured error category; old/new root and area
generations; affected bounds; history/dirty visibility; derived packet status;
budget actuals; timings; warnings; conflicts; and repair actions. A repeated
completed idempotency key returns its original receipt, while a changed
fingerprint is rejected.

## Error and job categories

The contract uses `InvalidSchema`, `AmbiguousCoordinates`, `OutOfBounds`,
`BudgetExceeded`, `ApprovalRequired`, `SharedAssetChoiceRequired`,
`AreaUnavailable`, `StaleGeneration`, `LayerStackChanged`,
`UnsupportedHeightfieldOperation`, `Cancelled`, `DerivedBuildFailed`,
`PersistenceFailed`, and `InternalInvariant`. Durable jobs expose progress,
reconnect, cancellation, and a terminal receipt. A failed derived rebuild
retains the last-known-good derived publication and reports the affected
location as stale.

## Canonical editor capabilities

The editor advertises only these installed terrain routes: `terrain.asset.inspect`,
`terrain.create`, `terrain.import_heightmap`, `terrain.session.begin`,
`terrain.session.get`, `terrain.session.cancel`, `terrain.session.commit`,
`terrain.stroke.apply`, `terrain.layers.set`, `terrain.holes.set`,
`terrain.operation.start`, `terrain.rebuild`, `terrain.job.get`,
`terrain.job.cancel`, `terrain.diagnostics.get`, `terrain.validate`,
`terrain.export`, and `terrain.save`. Unsupported service prerequisites return
an explicit unavailable result; capability discovery does not imply that an
export pipeline or live physics scheduler has been injected.

## Parity

Parity compares the same bounded create, import, sculpt, layer, hole,
procedural, rebuild, and save inputs through visible and automation paths.
Comparison covers canonical authored hashes, area generations, history grouping,
dirty/save identity, diagnostics, cancellation, and final receipts. UI/session
identities and worker timing may differ; authored results and visibility safety
may not. A parity adapter must publish measured comparison details before the
runner can report `executed`.
