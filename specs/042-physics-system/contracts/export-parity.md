# Export and Runtime Parity Contract

## Shared Runtime Requirement

Editor Play Mode and exported games register and execute the same native `PhysicsSystem`, backend adapter, fixed-step policy, collision filtering, query/event ordering, constraint/character logic, and lifecycle state machine. Editor-only UI and automation transport are not packaged.

## Required Export Content

- Physics runtime library/provider objects from the installed tree.
- Resolved `PhysicsSettings.json` and settings hash.
- Stable collision layer table and broad-phase mapping.
- Referenced physics material assets.
- Referenced nonempty cooked collider assets.
- Provider identifier/version/revision and cook format version.
- Startup scene physics dependency inventory.
- Managed physics API/NativeAOT bridge where scripting is enabled.

Source model geometry is not a runtime collider dependency. Debug UI, editor diagnostics window, source recipes not needed at runtime, and MCP transport are excluded.

## Manifest Contract

The resolved manifest `physics` section contains:

- `schemaVersion`
- `provider`: engine-facing identifier, semantic version, exact revision
- `cookFormatVersion`
- `settings`: package path, schema version, content hash
- `layersHash`
- `defaultMaterialGuid`
- `materials[]`: GUID, package path, checksum, size
- `colliders[]`: GUID, package path, checksum, size, source provenance hash, recipe hash
- `validation`: fixture/schema version and status

Asset type values are append-only. Manifest paths are normalized project/package paths and do not expose build-machine absolute paths.

## Validation Contract

Export fails before publication when:

- Physics settings or layer table are absent, malformed, unsupported, or inconsistent.
- A referenced material/collider GUID is unresolved.
- A cooked payload is missing, empty, placeholder-marked, corrupt, checksum-invalid, or built for an incompatible provider/cook format/platform.
- A runtime collider depends only on source model data.
- Startup scene physics data is invalid and cannot be safely disabled under policy.
- Required managed/native physics bindings are missing.

Diagnostics identify scene/object/record/asset GUID, failure category, expected/actual version or checksum, and remediation. A failed export does not publish a partial output tree.

## Evidence Contract

Extend existing durable evidence files:

- `export-result.json`: physics validation stage and final status.
- `resolved-manifest.json`: complete physics manifest section.
- `content-inventory.json`: physics settings/material/collider entries with size/checksum and `placeholderDetected=false`.
- `validation-report.json`: ordered physics dependency and fixture diagnostics.
- `progress.jsonl`: bounded physics cooking/staging/validation progress.
- `package-validation.txt`: human-readable provider/settings/asset/parity summary.

Evidence records provider and cook revision, settings/layer hashes, included GUIDs, validation fixture version, build configuration, and reference platform.

## Parity Fixture

The same source fixture runs in editor Play Mode and exported runtime. It covers:

- Static, dynamic, and kinematic body creation/initial state.
- Required primitive and cooked shape loading.
- Gravity, fixed timestep, substep clamp, pause/single-step where applicable.
- Layer/mask/trigger/material behavior.
- Representative CCD, sleep/wake, forces/impulses/teleport/velocity.
- Ordered rays, shape casts, overlaps, events, and contacts.
- Constraint limits/motors/springs/breaking.
- Character slope, stair, jump, moving-platform, resize, and grounding invariants.
- Scene transition, object/component removal, script reload boundary where supported, and shutdown.

Comparison uses stable invariant records and tolerances, not fragile exact floating-point snapshots. Records include tick, world generation, stable object/record IDs, event/query order, and final lifecycle state. All covered invariants must match.

## Headless and Installed Validation

1. Configure and build required engine, editor, compiler, export, and test targets.
2. Run focused then complete applicable CTest suites.
3. Run the install target.
4. Launch the installed editor/runtime tools, not intermediate binaries.
5. Cook/package a disposable physics fixture through the headless pipeline.
6. Validate the six evidence files and package inventory.
7. Run the exported fixture without source assets or global physics dependencies.
8. Compare invariant records with installed editor Play Mode.
9. Verify clean shutdown with zero active jobs/callbacks.

## Failure and Shutdown Parity

Both surfaces must safely handle missing/invalid authored bodies, stale handles, query expiry, failed recook with last-known-good data, world replacement, renderer unavailability, and fatal provider failure. The exported runtime may omit editor visualization, but simulation containment, logs, exit status, and resource cleanup follow the same native policy.
