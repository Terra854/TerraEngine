# Ragdolls

TerraEngine ragdolls keep gameplay physics authoritative in Jolt while the animation stack owns skeleton mapping and the final visual blend. CPU and GPU animation never mutate a live Jolt body directly and never depend on a complete-pose GPU readback.

## Authoring data

`RagdollProfileAsset` (`.teragdoll`) is a versioned, skeleton-fingerprint-bound asset. It stores stable joint-to-body mappings, primitive or immutable cooked colliders, mass/inertia intent, damping and gravity, collision filters, constraints, limits, motors, blend policies, and recovery candidates. The profile validator rejects incompatible skeleton/reference-pose identities, missing required body coverage, duplicate mappings, invalid or cyclic topology, unsupported collision layers/masks, implausible mass distribution, corrupt motor/limit values, and incompatible recovery clips before publication. A failed load or hot reload retains the last compatible generation.

## Protected physics lifecycle

Animation, managed gameplay, editor preview, and automation submit copied `RagdollPhysicsRequest` values. Create, rebuild, activate, deactivate, and teardown become visible only at `PhysicsCommandCommit`; producer or worker code never changes `PhysicsWorld` directly.

Create/rebuild/activate require one complete generation-stamped animation pose. `PrepareRagdollActivation` maps every stable joint through the profile's joint-to-body transform and preserves linear and angular velocity. Physics stages all shapes, bodies, constraints, and engine-owned constraint registrations off-world. Any invalid mapping, stale world/profile/skeleton generation, capacity failure, or provider rejection rolls back the whole batch. On success, provider staging commits first, old constraints retire before old bodies, and one immutable correlated result snapshot is published.

Every active fixed step publishes a copied `PhysicsPoseSnapshot`. Publication verifies the physics world, profile, skeleton, publication, body-slot, constraint, object, and component generations and requires every mapped body from the same extraction step. Animation presentation consumes that immutable value after Physics; it never reads provider-owned mutable state. A stale or incomplete extraction leaves the prior complete pose intact.

## Full and partial blending

The CPU reference blend and visual GPU stage use the same composition order:

1. per-joint weight;
2. inherited branch weight;
3. layer weight;
4. mask weight;
5. local activation/recovery time ramp;
6. caller-wide time weight.

Weights are finite and clamped. Quaternion blending follows the shortest hemisphere and normalizes the result. `KinematicAnchor` keeps a boundary animation-driven, `MatchAnimation` prevents a boundary from becoming more simulated than its parent, and `MotorToAnimation` emits the remaining animation fraction as a motor target. Animation-driven kinematic bodies always retain zero physics blend. Missing, stale, corrupt, or renderer-unavailable GPU records pass through the animation pose and report bounded diagnostics; Jolt authority is unchanged.

## Recovery

Recovery candidates name a compatible clip/skeleton/reference pose, required contacts and posture, pose and velocity envelopes, root/contact alignment, blend duration, and clearance dimensions. Selection consumes copied current-step pose and clearance values, uses stable candidate ID as the deterministic tie-break, and can accept an explicit candidate. Moving-support recovery carries the validated support velocity into root alignment over the declared handoff duration. Stale clearance, blocked space, excessive pose error, excessive velocity, or disallowed support motion publishes a structured blocked/not-ready result and never teleports the character.

## Rollback, replay, recording, and networking

`RagdollState` retains a bounded fixed-tick history containing lifecycle, complete physics pose, per-body blend weights, event watermark, recording sequence, retarget and motion-match generations, root-motion accumulation, contacts, recovery transition, rollback/replay generations, and network sequence. A deterministic hash covers authoritative values. Restore and transport decoding stage complete values and reject incompatible world/profile/skeleton/publication/body generations before replacing the last-good state. Historical event watermarks prevent one-shot delivery from repeating after correction or replay seek.

## Failure diagnosis

Inspect the copied lifecycle/result/pose diagnostics for the correlation ID and generation that failed. Common corrective actions are:

- regenerate the profile after a skeleton/reference-pose change;
- add every required deforming joint and connect each body to one valid root;
- use an enabled collision layer and a symmetric allowed mask;
- reduce body/constraint counts below reported bounds;
- provide a complete current-step pose and clearance result;
- retry only with the latest world/profile/body generations;
- keep recovery pending until space and velocity checks pass.

Run the focused native tests with `TERRAENGINE_TEST_FILTER=Animation.Ragdoll.`. Installed validation additionally exercises activation, impacts, partial weights, motors, moving support, blocked recovery, rollback, managed controls, editor preview, and renderer-loss fallback from the installed tree.
