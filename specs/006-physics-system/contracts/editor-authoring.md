# Editor Authoring and Diagnostics Contract

## Inspector

The Inspector presents familiar physics entries while storage uses collection components.

### Rigid body

- Motion type and contextual mass controls.
- Damping, `Use Gravity`, sleep, CCD, velocities, limits, axis locks, interpolation, and filtering.
- `Use Gravity` is enabled by default and clearly indicates that the project world-gravity vector applies. The Inspector does not expose a per-body gravity vector; custom gravity is supplied by fixed-tick gameplay acceleration commands.
- Units and valid ranges adjacent to numeric fields.
- Inline validation for missing colliders, unsupported hierarchy/scale, invalid mass, and layer references.

### Collider set

- Add Box, Sphere, Capsule, Cylinder, Tapered Capsule, Convex Hull, Mesh, and Compound records.
- Reorder, duplicate, remove, enable, and multi-edit safe common fields.
- Preserve stable record ID across ordinary edits/reorder; duplicate creates a new record ID.
- Shape-specific handles and fit-to-render-mesh action.
- Material, trigger, layer override, event, and query controls.
- Cook status, warnings, provenance, and last-known-good stale state for cooked geometry.

### Constraint set

- Add all required constraint record types.
- Pick connected object/world anchor and edit both frames.
- Contextual limits, springs, motors, collision, and break thresholds.
- Visual validation for missing/deleted endpoints and invalid axes/ranges.

### Character

- Shape, size, up, step, slope, support, contact, strength, filtering, gravity/jump, and rotation policy.
- Shape handles and inline warnings for unsupported resize or overlap conditions.

### Play Mode runtime state

Read-only fields include handle/world generation, awake/grounded state, velocity, mass, contacts, broad-phase layer, support identity, and tick. Runtime state is read from immutable snapshots.

## Edit, Undo, Prefab, and Rebuild Semantics

- All final edits use reflected component payload transactions and grouped undo/redo.
- Drag handles may preview editor-local geometry but enqueue one final authored transaction and one deferred runtime rebuild at release.
- No ImGui/Inspector callback directly rebuilds the provider world.
- Prefab overrides address stable collider/constraint record IDs and field paths.
- Apply/revert and add/remove record operations are atomic and remap connected UUIDs through prefab composition.
- Multi-selection edits are enabled only when the field has identical semantic meaning and the entire group preflights.
- A failed runtime rebuild leaves authored state and active physics consistent according to transaction policy and displays an actionable diagnostic.

## Project Settings

Add a Physics category for:

- Gravity.
- Fixed timestep, substep, and accumulator limits.
- Stable layer matrix and broad-phase mapping.
- Default material.
- Solver and sleep settings.
- Query/event/history bounds.
- Debug and diagnostic defaults.
- Safe backend-neutral advanced settings.

Changes serialize to `ProjectSettings/PhysicsSettings.json`, participate in undo where project settings support it, trigger bounded impact preflight, and are included in export evidence.

## Scene View

Visualization modes:

- Collider wireframe and optional translucent fill.
- Body center of mass, principal axes, awake/sleep/motion colors.
- Broad-phase bounds.
- Contact points, normals, and impulses.
- Constraint frames, limits, error, and broken state.
- Character support plane, ground normal/state, step/slope guides.
- Recent query rays/shapes/hits with bounded lifetime.

Scene View consumes immutable `PhysicsDebugDrawPacket` data. It never calls the provider while renderer work records. When rendering is unavailable, packet publication and physics continue; drawing becomes a no-op with diagnostics rather than a physics failure.

## Physics Diagnostics Window

An opt-in `Window` menu item shows:

- World generation, tick, accumulator, substeps, clamped/dropped time.
- Static/dynamic/kinematic/active/sleeping body counts.
- Shape and cooked-cache entries, hits/misses, bytes.
- Constraint, character, query, event, contact and pair counts.
- Dropped/filtered events and result expiry.
- Stale handle/result/command rejections and idempotent duplicates.
- Command queue depth and failed preflight reasons.
- Prepare, commit, simulate, extract, and publish timing.
- Provider memory and temporary allocator high-water marks.
- Scheduler phase/resource/workload/visibility declarations and fallbacks.

The window reads one immutable snapshot and does not activate extra simulation work. Named physics CPU/task scopes and counters feed the existing Profiler whether the window is open or closed.

## Play Mode Lifecycle

Enter:

1. Capture authored scene through existing editor flow.
2. Flush pending authoring transactions.
3. Build a fresh physics world generation from an immutable snapshot.
4. Start scripting/runtime only after world readiness or fail entry with diagnostics.

Pause/single-step:

- Pause freezes accumulation and fixed-step advancement.
- Next consumes exactly one configured fixed physics tick, including command commit, fixed and ordinary simulation callbacks using that explicit duration, result publication, and one resulting rendered frame.
- Next bypasses wall-clock editor delta and accumulator catch-up, then returns to paused state.
- Debug/diagnostic snapshots remain readable while paused.

Exit:

1. Stop accepting physics/script work.
2. Cancel/drain and join physics jobs.
3. Stop event delivery and destroy the runtime world.
4. Invalidate wrappers/results.
5. Restore the captured authored scene.

Runtime transforms and state never dirty the authored scene. Repeated enter/exit, script reload, scene unload, project close, renderer failure, and application shutdown use the declared lifecycle boundary and are regression-tested.

## Accessibility and Diagnostics

- Fields display units, acceptable ranges, and concise reason/action messages.
- Colors are supplemented by labels/icons and configurable palettes.
- Large lists are filterable and bounded.
- Diagnostics include object/record identity, world/tick, correlation ID, and proposed remediation without provider-only codes.
