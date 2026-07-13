# Editor Automation API Contract

## Scope and Registration

Physics capabilities self-register through a dedicated `PhysicsAutomationProvider`. Generic component/reflection automation remains the preferred path for ordinary component creation and property editing, but semantic runtime commands and bounded queries use this provider because they require request context, generations, approvals, idempotency, jobs, and correlation.

Physics automation operates only inside the native-editor-owned active project/session. It never creates, opens, closes, or selects projects and exposes no arbitrary command or filesystem execution.

## Common Envelope

Every request uses the existing automation envelope and carries:

- Capability name and schema version.
- Correlation ID.
- Active project/session identity.
- Optional scene/world identity.
- Target UUID/component/record IDs and generations where applicable.
- Idempotency key for runtime or publication mutations.
- Arguments and requested bounds.
- Approval token and impact fingerprint where required.

Every response includes:

- Success/failure category and bounded diagnostics.
- Correlation ID.
- World generation and physics tick for runtime data.
- Target-generation/stale diagnostics.
- Truncation/overflow flags.
- Mutation receipt or durable job ID where applicable.

## Read-only Capabilities

### `physics.state`

Returns active/inactive/paused/failed state, world generation, tick, accumulator/substeps, settings hash, and bounded summary counts.

### `physics.settings.get`

Returns the resolved project-owned settings and schema/version; provider-private settings are excluded.

### `physics.layers.list`

Returns stable layer IDs, names, masks, broad-phase groups, enabled state, and reference counts where available.

### `physics.body.inspect`

Requires object UUID and optional component/world generations. Returns authored identity plus immutable runtime state, contacts summary, handle generation, and stale status.

### `physics.contacts.list`

Returns bounded, ordered published contacts filtered by target/layer/tick range. Never traverses the live provider world.

### `physics.diagnostics.get`

Returns the immutable diagnostics snapshot, scheduler declarations, queue/rejection counts, timing/memory summaries, and bounded recent failures.

### `physics.raycast`, `physics.shape_cast`, `physics.overlap`

Validate finite bounded dimensions, distance, filters, hit limits, world generation, and active scene. Small requests may execute at a protected query boundary; larger work returns a durable job. Results are ordered and tick-stamped.

### `physics.query_job.get`

Returns durable query/cook job status and, when complete, immutable bounded result metadata. Existing generic `jobs.get` remains canonical; this capability may be a typed physics convenience rather than a second job store.

## Mutating Capabilities

### Project authoring

- `physics.settings.set`
- `physics.layer.create`
- `physics.layer.update`
- `physics.material.create`
- `physics.material.update`
- `physics.collider.cook`

These require ProjectWrite permission, active project, preflight fingerprint, and atomic publication. Global settings changes and broad/bulk changes require HighImpact approval. Layer/material deletion, if added later, requires Destructive approval and reference-impact remediation.

### Runtime commands

- `physics.body.apply_force`
- `physics.body.apply_impulse`
- `physics.body.teleport`
- `physics.body.set_velocity`
- `physics.character.move`
- `physics.character.jump`

Requirements:

- Active project, scene, and physics world.
- Play Mode state permitted by the command. Force/impulse/move/jump require playing; paused requests may be accepted only with explicit `applyOnNextStep=true`. Teleport and velocity follow the same boundary semantics.
- Exact object/component/record/world generations.
- `physics.body.apply_force` accepts an engine-owned application mode; custom gravity uses bounded mass-independent acceleration and follows fixed-tick command ordering.
- Nonempty idempotency key and fingerprint over capability, target generations, payload, and effective tick policy.
- Typed enqueue only; no direct ObjectManager or provider mutation.

## Approval and Impact Binding

High-impact operations return a preflight result containing:

- Referencing scenes/prefabs/assets/objects.
- Expected rebuild/recook/body counts.
- Settings/layer/material changes.
- Failure and rollback policy.
- Stable impact fingerprint and expiry/session binding.

Execution requires an approval token bound to the exact fingerprint, capability, project/session, requester, and expiry. A changed target generation or payload invalidates approval and requires new preflight. Bridge-level generic approval alone is insufficient without handler-side fingerprint verification.

## Idempotency

Runtime impulses, forces, teleports, velocity changes, jumps, and asset publication call the shared idempotency store explicitly:

1. Begin with key and request fingerprint.
2. Return the prior completed receipt for an exact duplicate.
3. Reject a reused key with a different fingerprint.
4. Complete only after protected command acceptance or atomic asset publication.
5. Record failed/cancelled state according to existing store policy.

Duplicate requests never apply a physical action or publication twice.

## Durable Jobs

Collider cooking and over-budget synchronous query batches return durable jobs with:

- Job ID, correlation ID, kind, project/world generation.
- Queued/running/completed/failed/cancelled state.
- Progress counters and bounded logs.
- Cooperative cancellation.
- Result identity/checksum or immutable query result handle.

Cancellation before commit leaves no partial asset/world change. Completed jobs remain reconnectable for the configured bounded retention period.

## Bounds and Failure Categories

Bounds come from PhysicsSettings and include maximum ray distance, shape extents, hits, batch size, retained results, event/contact range, and cook input size. Reject nonfinite, negative, malformed, unsupported, or over-budget requests; do not silently expand bounds.

Failure categories include project/scene/world unavailable, permission denied, approval required/invalid, stale generation, invalid arguments, limit exceeded, duplicate conflict, preflight conflict, cancellation, job expiry, provider failure, and internal failure. Results never leak native paths outside approved project-relative diagnostics.

## Capability Discovery Metadata

Each descriptor declares:

- Read-only versus ProjectWrite.
- `requiresProject=true` for all physics capabilities.
- `requiresScene=true` for scene/body/query/character capabilities.
- Allowed editor/Play Mode states.
- Approval class and impact-preflight support.
- Idempotency requirement.
- Durable-job behavior.
- Execution boundary (`immutable_physics_snapshot`, `physics_command_commit`, `post_physics_publish`, or `post_editor_automation_flush`).
- Argument/result schema versions and configured bounds.
