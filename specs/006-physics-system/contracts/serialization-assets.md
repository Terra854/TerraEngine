# Serialization and Asset Contract

## Purpose

Define stable project-owned formats for physics authoring, project settings, materials, collider recipes, cooked payloads, prefab references, migrations, and export compatibility.

## General Rules

- JSON keys and enum strings are engine-owned and versioned.
- Provider enum integers, IDs, pointers, class layouts, and runtime serialization are forbidden.
- Unknown newer schema versions fail clearly; older known versions migrate explicitly.
- Missing optional fields receive documented defaults.
- Runtime handles, caches, contacts, accumulated forces, sleep state, and transient transforms are excluded.
- GUID and stable record identities survive ordinary rename/move/edit operations.

## Scene Component Payloads

Components continue to serialize through the existing reflected scene component object.

### RigidBodyComponent

Required top-level fields:

- `schemaVersion`
- `motionType`
- `massMode`, `mass`
- `linearDamping`, `angularDamping`, `useGravity`
- `allowSleeping`, `startAwake`, `continuousCollisionMode`
- `initialLinearVelocity`, `initialAngularVelocity`
- `maxLinearVelocity`, `maxAngularVelocity`
- `allowedDegreesOfFreedom`
- `interpolationMode`
- `collisionLayerId`, optional `collisionMaskOverride`
- `enabled`

`useGravity=true` selects the project world-gravity vector captured for each fixed tick; `useGravity=false` suppresses built-in gravity. Custom gravity vectors are commands and are not serialized as Rigidbody authoring fields.

`continuousCollisionMode` accepts the engine-owned symbolic values `Discrete` and `LinearCast` in schema version 1. Unknown values fail validation rather than being converted from provider enum integers.

### ColliderSetComponent

- `schemaVersion`
- ordered `colliders` array

Every collider record contains `recordId`, `recordVersion`, type, local pose, tagged shape payload, material GUID/default flag, trigger, layer override, event policy, query flags, enabled, and optional debug color.

Shape payloads:

- Box: half/full extents according to one documented convention.
- Sphere: radius.
- Capsule/cylinder: radius and segment/total height according to documented convention.
- Tapered capsule: top radius, bottom radius, height.
- Convex/mesh: cooked collider asset GUID and optional subresource selection.
- Compound: ordered child recipes/references with stable child IDs; cycles prohibited.

### ConstraintSetComponent

- `schemaVersion`
- ordered `constraints` array

Each record contains stable ID/version, type, connected object UUID or world-anchor marker, both local frames, connected-body collision policy, tagged limits/spring/motor fields, maximum force/torque, break force/torque, enabled, and reset generation.

### CharacterControllerComponent

Contains schema version, shape and dimensions, center/up, step/support/slope/contact settings, mass/strength, ground/stair behavior, layer/mask, optional gravity/jump defaults, rotation policy, and enabled.

## Project Physics Settings

Canonical authored path: `ProjectSettings/PhysicsSettings.json`.

The file contains:

- Schema version.
- Units declaration for validation/evidence.
- Gravity, fixed timestep, substep and accumulator limits.
- Solver and sleep settings.
- Ordered stable collision layer table and broad-phase mapping.
- Default material GUID.
- Query, batch, event, debug history, and diagnostics bounds.
- Provider-neutral advanced settings with safe ranges.

Exports embed the resolved settings and their content hash. Provider-specific experimental switches are not serialized unless promoted to a stable engine-owned setting.

## Physics Material Asset

Suggested authored extension: `.tephysmat`.

Payload:

- Schema version and asset GUID from `.meta`.
- Static friction, dynamic friction, restitution, density.
- Engine-owned friction and restitution combine modes.
- Optional stable surface tag.

Missing material behavior: use the configured default, emit one bounded warning per unique missing reference, and preserve the missing GUID for repair.

## Collider Recipe and Cooked Asset

Suggested authored recipe extension: `.tecollider`; compiled extension: `.tepcoll`.

### Recipe

- Schema version.
- Source asset GUID and selected mesh/submesh identities.
- Shape intent: convex, mesh, or compound.
- Weld, simplification, convex limits, material mapping, scale normalization, and platform profile settings.
- Canonical recipe hash.

### Compiled payload envelope

- Magic and format version.
- Endianness/platform compatibility marker.
- Asset GUID, source GUID/hash, recipe/settings hash.
- Provider family and exact cook revision used for the payload.
- Bounds, vertex/triangle/part/material counts.
- Payload size and checksum.
- Bounded cook warnings and provenance summary.

Provider-specific cooked bytes may exist inside the compiled envelope but never appear in scene JSON or public contracts. A provider/cook version mismatch causes validation failure or explicit recook; it never reinterprets unknown bytes.

## Atomic Cooking and Publication

1. Resolve source GUID and immutable source content.
2. Validate recipe, geometry, scale, limits, and cancellation.
3. Cook in worker/headless tool context.
4. Validate nonempty output, envelope, checksum, and bounds.
5. Write a staging file and report.
6. Under a protected asset publication boundary, verify source/settings fingerprints and atomically replace the compiled file.
7. Update asset database metadata and emit one publication result.

Failure or cancellation before step 6 leaves the prior published asset untouched. If the prior version remains usable it is marked last-known-good/stale with the failed source/settings fingerprint. Long operations expose durable jobs.

## Asset Database Rules

- Add append-only asset type values for PhysicsMaterial and CookedCollider; never renumber existing serialized values.
- `.meta` remains the source of asset GUID, importer name, source/compiled path, and settings.
- Moving an asset through supported organization paths moves metadata and preserves GUID.
- Deletion requires dependent-reference preflight and approval where invoked through automation.
- Equivalent collider recipes share immutable runtime shape instances, not authored asset identities.

## Prefab and Duplication Rules

- Prefab overrides address component name plus stable collider/constraint record ID and field path.
- Apply/revert operates on the complete record payload and remains undoable.
- Connected object references use UUIDs and participate in prefab/duplicate remap tables.
- Duplicating an object creates new object and record IDs while remapping intentional references within the duplicated graph.
- Moving/renaming assets retains GUID references.
- Invalid, missing, or cyclic reference remaps reject the complete prefab/transaction operation.

## Migration Baseline

Initial schema is version 1 for settings, components, materials, recipes, and cooked envelope. Every later migration requires:

- Old-version fixture.
- Deterministic migrated output.
- Default behavior test for absent fields.
- Round-trip save/reload test.
- Prefab override/remap test where fields or record structures change.
- Export validator compatibility decision.

## Export Manifest Fields

The resolved manifest includes a `physics` section with:

- Settings schema/version/path/hash.
- Stable collision layer table hash.
- Provider identifier, version, revision.
- Cook format version and supported platform marker.
- Default material GUID.
- Required physics material and cooked collider GUID/path/checksum inventories.
- Validation status and invariant-fixture version.

Package validation rejects absent settings, unresolved GUIDs, provider/cook mismatches, empty/placeholder payloads, checksum failures, and source-model-only collider dependencies.
