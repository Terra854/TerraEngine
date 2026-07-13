# Physics editor automation

Physics automation is available only from the already-open project in the
native TerraEngine editor. It is a bounded semantic API: it does not create,
open, close, select, or otherwise control projects; it does not expose a
physics-provider pointer; and it never accepts an arbitrary file path or shell
command.

## Discovery and lifecycle

The `physics` automation provider self-registers its tools. Each tool is
version `1`, has a closed JSON-object schema, and describes its required
project, scene, Play Mode, permission, and scheduler boundary in discovery.
All responses are correlated with the request and include bounded diagnostics.
Runtime reads also include their source `worldGeneration` and physics `tick`.

The provider exposes the following groups:

- World reads: `physics.state`, `physics.settings.get`,
  `physics.layers.list`, `physics.material.inspect`, `physics.body.inspect`,
  `physics.character.inspect`, `physics.contacts.list`, and
  `physics.diagnostics.get`.
- Bounded queries: `physics.raycast`, `physics.shape_cast`,
  `physics.overlap`, and `physics.query_job.get`.
- Project authoring: `physics.settings.preflight`, `physics.settings.set`,
  `physics.layer.create`, `physics.layer.update`, `physics.material.create`,
  `physics.material.update`, and `physics.collider.cook`.
- Play Mode commands: `physics.body.apply_force`,
  `physics.body.apply_impulse`, `physics.body.set_velocity`,
  `physics.body.teleport`, `physics.character.move`, and
  `physics.character.jump`.

All physics tools require a project that is already open. Body, character,
contact, and query tools also require an active scene and an active or paused
physics world. A renderer is never required for diagnostics or physics
automation.

## Reads and bounded queries

Reads use immutable published physics snapshots. They never lock or traverse a
live Jolt world. `physics.contacts.list` requires `maximumContacts`; query
requests require `maximumHits`, and query schemas bound results to 256 hits,
batches to 1,024 requests, ray distance to 100,000 units, and shape extents to
10,000 units. Invalid, non-finite, negative, or over-limit values are rejected
instead of being silently clamped.

Small queries run at the declared protected query boundary. Over-budget query
or cooking work returns a durable receipt:

```json
{
  "correlationId": "query-42",
  "accepted": true,
  "idempotencyDisposition": "new",
  "jobId": "physics-job-17",
  "diagnostics": []
}
```

Poll `physics.query_job.get` (or the general `jobs.get`) with the job ID.
Jobs retain a bounded terminal result across reconnects and support cooperative
cancellation. A cancelled cook is discarded before publication; it cannot
replace a previously valid collider asset with a partial result.

## Identity, retries, and stale targets

Every runtime command contains a target identity and exact generation data:

```json
{
  "target": {
    "objectId": "object-uuid",
    "componentGeneration": 7,
    "worldGeneration": 12
  },
  "worldGeneration": 12,
  "idempotencyKey": "client-retry-001",
  "correlationId": "jump-request-001"
}
```

Collider and constraint-record operations additionally carry `recordId` and
`recordGeneration`. If an object, component, record, scene, or world was
recreated after the request was prepared, the command is rejected as stale and
cannot affect the replacement.

All runtime commands, cooking requests, and asset-publication mutations require
a nonempty `idempotencyKey`. The service begins the shared idempotency record
using the capability, target generations, payload, and requested tick policy as
the fingerprint. An exact retry returns its original receipt; reusing a key for
different work is rejected; completion occurs only after a protected command
acceptance or atomic asset publication. A duplicate never applies a second
impulse, force, teleport, jump, or asset publication.

## Settings, layers, materials, and approvals

Use `physics.settings.preflight` before changing settings which can rebuild a
world or affect referenced bodies. It returns a bounded reference-impact
summary and an `impactFingerprint` tied to the active project and scene
generations. Submit the result to the editor approval flow, then pass the
approved token back to `physics.settings.set`:

```json
{
  "settings": {
    "gravity": [0.0, -9.81, 0.0],
    "fixedTimeStep": 0.016666667,
    "debugVisualizationEnabled": true,
    "retainedDebugTicks": 2,
    "maxDebugPrimitives": 100000
  },
  "worldGeneration": 12,
  "projectGeneration": 55,
  "idempotencyKey": "settings-55-1",
  "approval": {
    "approvalId": "approval-123",
    "impactFingerprint": "impact-abc",
    "projectGeneration": 55
  }
}
```

Settings, layer updates, material updates, and cooking revalidate the
fingerprint, approval/session binding, payload, and target generations at the
protected commit boundary. Any change after preflight invalidates the approval
and requires a new one. Layer and material writes use atomic project-owned
publication; reference-impact failures leave live settings and assets unchanged.

`physics.settings.set` is constrained to engine-owned settings, including
gravity, fixed time step, substeps, and bounded debug retention. Provider
private values and Jolt enums are intentionally absent.

## Runtime command example

The following queues one impulse for the next eligible physics boundary. It
does not write an object transform or provider body from the automation thread.

```json
{
  "target": {
    "objectId": "crate-uuid",
    "componentGeneration": 4,
    "worldGeneration": 12
  },
  "worldGeneration": 12,
  "vector": [0.0, 5.0, 0.0],
  "applyOnNextStep": true,
  "idempotencyKey": "crate-impulse-1",
  "correlationId": "tutorial-crate"
}
```

Force, impulse, velocity, teleport, move, and jump commands publish through
`physics_command_commit`; authoring changes use
`post_editor_automation_flush`; snapshot reads use
`immutable_physics_snapshot`. This makes queueing, visibility, and retry
behavior visible in diagnostics without exposing provider internals.

## Errors and diagnostics

Callers should handle structured categories such as `validation`, `permission`,
`approval_required`, `stale_target`, `unavailable`, `conflict`, `canceled`,
`timeout`, and `internal`. Diagnostics are bounded and carry correlation,
world/tick, truncation, overflow, target-generation, and durable-job context
where applicable. Provider-native errors, machine paths, and unapproved source
data are not part of this public contract.
