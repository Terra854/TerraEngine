# Physics editor automation (MCP)

The self-registering `physics` provider is available only in the already-open
native TerraEngine editor project. It cannot create/open/close/select projects,
run shells, accept arbitrary paths, or expose a provider/Jolt pointer. Every
tool is version 1 with a closed JSON schema, bounded diagnostics, correlation
ID, and an explicit scheduler visibility boundary.

## Capabilities, permissions, and availability

Read permission is used by `physics.state`, `physics.settings.get`,
`physics.layers.list`, `physics.material.inspect`, body/character inspection,
contacts, diagnostics, bounded `raycast`/`shape_cast`/`overlap`, and
`physics.query_job.get`. Scene reads and queries require an active scene and a
playing or paused world; settings/layers/material metadata can be read with an
open project and no scene. Renderer availability is never required.

Project-write permission is required for settings/layer changes and collider
cooking. Runtime body/character commands are typed scene mutations and also
require project-write permission, an active scene/world, and Play Mode
(`playing` or `paused`). Material creation is an atomic asset mutation without
approval; material updates, settings, layer create/update, and collider cooking
are `HighImpact` approval-gated operations.

## Reads and bounded queries

Reads consume immutable published snapshots (`immutable_physics_snapshot`),
not a live provider world. Query inputs reject non-finite, negative, or
out-of-range values. Limits are **64 hits per query/contact list**, **1,024
requests per batch**, **100,000 units ray distance**, and **10,000 units shape
extent**. Results are bounded and report `truncated`/`overflow` diagnostics.
Over-budget queries may return a durable `jobId`; poll it with
`physics.query_job.get` (or the general jobs API). Terminal receipts survive a
reconnect and support cooperative cancellation.

`physics.contacts.list` reads the engine-owned bounded diagnostics history,
not a managed gameplay subscription. Its inclusive `fromTick`/`toTick` range
is clamped to retained ticks and returned in stable tick order, so a quiet
latest tick does not hide an earlier retained contact and automation cannot
broaden or interfere with managed event filters.

```json
{
  "worldGeneration": 12,
  "origin": [0, 2, 0],
  "direction": [0, -1, 0],
  "distance": 200,
  "maximumHits": 8,
  "batchSize": 1,
  "correlationId": "ray-42"
}
```

## Identity, idempotency, and stale targets

Runtime commands carry `target.objectId`, `componentGeneration`, and
`worldGeneration`; collider/constraint records additionally carry
`recordId`/`recordGeneration`. Every command envelope also requires the
expected top-level `worldGeneration`, a non-empty `idempotencyKey`, and a
`correlationId`:

```json
{
  "target": {"objectId": "crate-uuid", "componentGeneration": 4, "worldGeneration": 12},
  "worldGeneration": 12,
  "vector": [0, 5, 0],
  "applyOnNextStep": true,
  "idempotencyKey": "crate-impulse-1",
  "correlationId": "tutorial-crate"
}
```

The service fingerprints capability, payload, target generations, and tick
policy. An exact retry returns the retained receipt (`idempotencyDisposition:
"replay"`); changing any fingerprint with the same key returns `"conflict"`.
Duplicates never apply a second force, impulse, teleport, move, or jump.
Stale object/component/record/world generations are rejected atomically.

`physics.body.apply_force` accepts `applicationMode: "Force"` (the default) or
`"Acceleration"`. Acceleration is mass-independent and is the automation
equivalent of managed `ForceMode.Acceleration`; use it each fixed tick for
custom gravity after disabling the body's authored built-in gravity:

```json
{
  "target": {"objectId": "crate-uuid", "componentGeneration": 4, "worldGeneration": 12},
  "worldGeneration": 12,
  "vector": [0, -9.81, 0],
  "applicationMode": "Acceleration",
  "applyOnNextStep": true,
  "idempotencyKey": "crate-custom-gravity-42",
  "correlationId": "gravity-tick-42"
}
```

## Approvals, fingerprints, and durable cooking

Call `physics.settings.preflight` before a settings change; for layer, material,
or cook writes, obtain the equivalent editor approval preflight for that
capability. The bounded result includes
`impactFingerprint`, `projectGeneration`, `sceneGeneration`, `referenceCount`,
affected object IDs, approval requirement, and expiry. Submit that fingerprint
to the editor approval flow, then pass this envelope to the mutation:

```json
{
  "settings": {"gravity": [0, -9.81, 0], "fixedTimeStep": 0.016666667,
    "maxSubsteps": 4, "retainedDebugTicks": 2, "maxDebugPrimitives": 100000},
  "projectGeneration": 55,
  "approval": {"approvalId": "approval-123", "impactFingerprint": "impact-abc",
    "projectGeneration": 55, "sceneGeneration": 8},
  "worldGeneration": 12,
  "idempotencyKey": "settings-55-1",
  "correlationId": "settings-change"
}
```

The commit boundary (`post_editor_automation_flush`) revalidates approval,
fingerprint, payload, session, and generations. Any intervening project/scene
change invalidates approval; rejected writes leave settings, layers, materials,
and assets unchanged. Collider cooking is durable and cancellable: cancellation
before publication discards the result and cannot replace a valid cooked asset
with partial data. Runtime commands publish at `physics_command_commit`; reads
remain snapshot-only. All receipts/logs include correlation and (for durable
work) job IDs, bounded diagnostics, tick/world generation, and truncation or
overflow context.

Structured errors use categories such as `validation`, `permission`,
`approval_required`, `stale_target`, `unavailable`, `conflict`, `canceled`,
`timeout`, and `internal`. Provider-native errors, machine paths, and arbitrary
source/process access are not part of the contract.
