# Terrain automation

Terrain automation validates versioned requests through the editor's typed
automation boundary. The installed validation runner is
`Tests/Tools/Terrain/RunTerrainValidation.ps1`; it checks the installed tree
before any operation is attempted.

Automation operates only on canonical `TERRAROOT1` assets. Legacy
`TERRAINROOT2`, creation JSON, unknown magic, and non-canonical roots return
`UnsupportedTerrainRoot` with recreate/recook guidance; no route migrates or
partially publishes them. Recreate from the source data or recook with the
current compiler before retrying the original request.

## Installed validation

```powershell
$install = (Resolve-Path 'out/build/RelWithDebInfo/Build').Path
& (Join-Path $install 'Tests/Tools/Terrain/RunTerrainValidation.ps1') `
    -InstallRoot $install `
    -OutputDirectory (Join-Path $install 'out/terrain-validation') `
    -DryRun
```

Dry-run verifies the editor, terrain compiler, fixture generator, benchmark,
terrain manifest, recovery manifest, and workload manifest. It records
metrics as unavailable because no provider or job was executed. Missing
artifacts are an error and return a non-zero exit code.

Live mode requires an explicitly supplied automation adapter. The adapter is a
small installed executable boundary, not a replacement terrain implementation.
For each requested operation it receives:

```text
--operation <create|inspect|edit|rebuild|validate|import|export|save|parity>
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

Layer-stack mutations must retain 1–64 definitions and 1–16 enabled layers,
with non-zero unique stable IDs and unique normalized names. Mapping is physical
asset-local Terrain-XZ or triplanar data (tile metres, offset metres, layer
rotation, and triplanar sharpness). Surface materials have Base Color, Normal,
packed ORM, and Emissive semantics. A request never asks the renderer to
truncate layers or silently accept a missing source without the layer's
fallback policy.

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
`terrain.operation.start`, `terrain.resize.preview`, `terrain.expand.start`,
`terrain.crop.start`, `terrain.rebuild`, `terrain.job.get`,
`terrain.job.cancel`, `terrain.diagnostics.get`, `terrain.validate`,
`terrain.export`, and `terrain.save`. Unsupported service prerequisites return
an explicit unavailable result; capability discovery does not imply that an
export pipeline or live physics scheduler has been injected.

Use `terrain.asset.inspect` before a write to capture the current root hash and
generation, complete enabled layer list with stable IDs, physical mapping, and
requested/effective sampler quality. Include the returned root identity, layer
stack generation, and every affected area observation in the typed edit
envelope. After commit or job completion, call `terrain.diagnostics.get` and
compare the receipt's published generations rather than polling mutable editor
objects.

### Resize envelopes

Call `terrain.resize.preview` with `kind: "expand"` and zero-or-positive area
counts for each side, or `kind: "crop"` and exact signed half-open sample bounds.
Expansion also accepts `policy` (`edge-blend`, `extend-edge`, or `flat-default`),
`transitionSamples`, and `holePolicy`. Poll the returned `jobId` through
`terrain.job.get` until it reports the ready proposal.

The ready result includes the root-bound `proposalFingerprint`, previous/proposed
bounds and area counts, policy, hole policy, transition size, physical dimensions,
ready generation, crop-loss object, warnings, and a human-readable diagnostic.
Commit exactly that proposal with `terrain.expand.start`, or with
`terrain.crop.start` plus `acknowledgeDataLoss: true`. A changed root, changed
settings, mismatched kind, missing hole choice, or different fingerprint is
rejected before publication. Start and terminal receipts retain transaction,
fingerprint, bounds, ready generation, outcome, published root identity, and the
`terrain_authoring_publish` visibility boundary.

Inspection and diagnostics report per-area desired, balanced, resident, and
actual LOD; projected error/hysteresis; stitch and hole-pin state; desired and
actual page plus own-ancestor fallback depth; payload byte/status information;
material/control generation touches; sampler fallback and descriptor pressure;
and stable root/layer/page/publication diagnostic codes. Missing fields or an
unavailable backend remain explicit unavailable evidence, never inferred
success.

## Publication timing

Automation handlers validate and lower JSON on the protected editor boundary,
then workers prepare immutable candidates. Accepted root/area, derived
material/control/geometry, and upload-complete page changes become visible only
at their declared protected scheduler boundaries. Read-only inspection captures
an immutable terrain snapshot. Workers never mutate live scene objects,
components, terrain/material registries, descriptor tables, or page visibility.
Stale, conflicting, malformed, capacity-exhausted, or cancelled candidates keep
the last-known-good publication.

## Parity

Parity compares the same bounded create, import, sculpt, layer, hole,
procedural, rebuild, and save inputs through visible and automation paths.
Comparison covers canonical authored hashes, area generations, history grouping,
dirty/save identity, diagnostics, cancellation, and final receipts. UI/session
identities and worker timing may differ; authored results and visibility safety
may not. A parity adapter must publish measured comparison details before the
runner can report `executed`.
