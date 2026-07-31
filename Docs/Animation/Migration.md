# Animation Migration Guide

animation production validation replaces ad-hoc clip playback and mutable pose access with versioned assets, authored `AnimatorComponent` data, generation-safe runtime instances, typed deferred commands, and immutable snapshots. Migration should preserve the old content until the converted dependency set has validated and published atomically.

## Project and asset migration

1. Reimport source glTF and FBX packages to create stable skeleton, clip, mesh, material, prefab, and animation subassets.
2. Validate compatibility fingerprints before assigning clips, controllers, retarget profiles, motion databases, or ragdoll profiles.
3. Convert embedded or string-named animation references to project UUID plus runtime generation handles.
4. Move controller logic into `.teanimctrl` assets and reusable masks into `.teanimmask` assets.
5. Cook retarget, motion-matching, and ragdoll data into `.teretarget`, `.temotiondb`, and `.teragdoll` assets rather than retaining editor/source paths at runtime.
6. Save and reload the scene, duplicate any connected prefabs, and verify that stable semantic subasset identities survive move, rename, and reimport.

The importer retains the last-known-good generated package if validation fails. Do not delete the previous generation until every dependent asset has switched successfully.

## Component migration

Replace legacy playback fields with an `AnimatorComponent` that stores only authored configuration:

- controller or clip asset identity;
- `Auto`, CPU, or GPU execution preference;
- root-motion policy;
- initial typed parameters;
- optional retarget, motion-database, and ragdoll profiles.

Transient pose buffers, graph state, event cursors, command receipts, physics bodies, and GPU addresses must not be serialized. Runtime state is created and retired by the animation lifecycle system using the object, component, scene, asset, and publication generations.

Older serialized components are migrated to `Auto` mode. Missing references remain visible as authored missing values so a user can repair them; migration must not silently substitute an unrelated asset.

## Native gameplay migration

- Submit parameter, trigger, playback, procedural, retarget, motion, ragdoll, rollback, and replay work through the typed animation command APIs.
- Supply a unique producer sequence and correlation ID. Commands are merged deterministically and become visible only after the declared protected boundary.
- Read copied receipts and snapshots. Never retain a pointer or span into an animator instance, pose buffer, event ring, physics pose, or renderer packet.
- Route scene, hierarchy, attachment, character, and physics changes through their transactional or typed deferred mutation queues.
- Reacquire handles after scene unload, component removal, asset reload, Play Mode restoration, physics-world replacement, or managed-domain reload.

## Managed C# migration

Use `TerraEngine.Animation.Animator` and the records under `TerraEngine.Animation`. The managed API mirrors native statuses and required-count reads; a zero-length destination is the supported way to query the required bounded count.

Dispose animation event subscriptions explicitly. Domain reload invalidates wrappers and subscription tokens, contains callback exceptions, and clears retained managed delivery state. Reacquire the component and assets after reload rather than retrying with an old generation.

## Rendering and shader migration

Remove embedded shader strings and place animation shaders under `Engine/Shaders/Animation/`. Render code should consume `RenderFrameSnapshot` animation packets and immutable pose/morph generations. Custom render paths must preserve positions, normals, tangents, morphs, dynamic bounds, shadows, captures, and deformable virtual-geometry fallbacks.

Authoritative gameplay must not depend on GPU bit identity. CPU Deterministic/Fallback mode owns rollback, replication, events, root motion, motion selection, and other gameplay-observable results.

## Export and deployment migration

Add the transitive skeleton, mesh, clip, controller, mask, retarget, motion database, ragdoll, material, texture, shader, and managed-runtime dependencies to the export profile. The runtime resolves cooked animation classes from TEPAK and must not access source, editor, or build-tree paths.

Validate the installed launcher both normally and with `--headless-cpu-fallback`. A package is ready only when installed-editor, exported runtime, and headless authoritative results agree and missing/corrupt dependencies fail before publication.

## Migration acceptance checklist

- The old and converted projects produce the intended state, event, curve, morph, root-motion, socket, and gameplay behavior.
- Failed conversion or reload leaves the complete prior generation active.
- Scene/prefab save-reload and Play Mode restoration contain no transient runtime data.
- CPU modes agree across supported worker counts; GPU visual output remains within the documented tolerance.
- Installed editor, MCP automation, TEPAK runtime, and headless runtime use only installed/cooked artifacts.
- Full build, CTest, and CMake install complete from a clean source copy using repository-contained dependencies.

See [Troubleshooting](Troubleshooting.md) when a migration reports a stale generation, incompatibility, fallback, rejected publication, or export failure.
