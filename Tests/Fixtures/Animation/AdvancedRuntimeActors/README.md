# Advanced runtime actor overlays

These four loose prefabs are deterministic, mesh-free runtime control fixtures for
the Spec 043 retargeting, motion-matching, and ragdoll installed-editor gates.
They use the production prefab document shape and the exact authored
`AnimatorComponent` schema-v1 fields emitted by the model package importer and
scene serializer. Runtime instance handles, effective modes, asset generations,
poses, and other transient state are intentionally absent.

## Deployment layout

Copy the contents of this directory into `Assets/AdvancedActors` in the
disposable gate project. Copy `Tests/Fixtures/Animation/AdvancedAssets` into
`Assets/Advanced` separately. The cooked animation envelopes own their durable
GUIDs, so the actor references remain valid even though the actor and
cooked-asset fixture families are deployed under separate project directories.

The checked-in `.meta` files give the loose prefabs deterministic AssetDatabase
GUIDs and use paths relative to the deployed `Assets` directory. Their
`lastModified` value is deliberately zero; the disposable project's first scan
updates only its copied sidecar timestamp while preserving the stable GUID.

## Actor roles

| Prefab | Prefab GUID | Root object UUID | Authored animation bindings |
|---|---|---|---|
| `SourceMotionActor.teprefab` | `5445414e303433414456414354000001` | `5445414e3034334143544f424a000001` | Source skeleton `...000001`, locomotion clip `...000003`, and motion database `...000006` |
| `SourceBuiltMotionActor.teprefab` | `5445414e303433414456414354000005` | `5445414e3034334143544f424a000004` | Source skeleton `...000001`, locomotion clip `...000003`, and the US8-built motion database `...ACT000004` |
| `TargetRetargetActor.teprefab` | `5445414e303433414456414354000002` | `5445414e3034334143544f424a000002` | Target skeleton `...000002`, source locomotion clip `...000003`, and retarget profile `...000005` |
| `TargetRagdollActor.teprefab` | `5445414e303433414456414354000003` | `5445414e3034334143544f424a000003` | Target skeleton `...000002`, target recovery clip `...000004`, and ragdoll profile `...000007` |

The source root starts at x = -1.5 m, the retarget root at the origin, and the
ragdoll root at x = +1.5 m. This keeps all diagnostic actors easy to select when
instantiated together without requiring a renderable mesh. All three request
`Auto`, extract root motion, update every frame on scaled time, dispatch events,
and retain automatic authority-aware culling. Live gate operations remain
responsible for retarget commands, motion queries, ragdoll activation/impacts,
and recovery requests.

`SourceBuiltMotionActor.teprefab` is intentionally separate from the ordinary
source actor. US8 instantiates it before starting Play Mode, after its pre-Play
build has atomically published `source-locomotion-built.temotiondb`. The
animation system therefore binds the same database UUID and generation later
submitted by the live `set_database` and `query` operations. Other scenarios
continue using `SourceMotionActor.teprefab` and the checked-in database, so a
missing US8 build output cannot invalidate their otherwise independent actors.

The retarget and ragdoll roles are deliberately separate. The binding
coordinator treats `skeletonOverrideAssetId` as the output skeleton, samples
`defaultClipAssetId`, and loads the retarget profile's source skeleton as an
independent dependency. A truthful retarget instance therefore combines the
target skeleton with the source locomotion clip and retarget profile. The target
recovery clip is already target-compatible, so it belongs on the ragdoll actor
without a retarget override.

## Compatibility and generation assumptions

The actor bindings intentionally store only durable asset UUIDs because that is
the production `AnimatorComponent` contract. The referenced cooked envelopes
carry the following compatibility fingerprints and dependency generations:

| Relationship | Compatibility fingerprint | Expected generation |
|---|---|---|
| Source skeleton, source clip, motion database | `0x043a000000001001` | 1 |
| Target skeleton, recovery clip, ragdoll profile | `0x043a000000002001` | 1 |
| Retarget profile | `0x043a000000007001` | 1 |

These values match `AdvancedAssets/manifest.json`. In particular, the retarget
profile depends on source and target skeleton generation 1, while the ragdoll
profile depends on target skeleton and recovery clip generation 1. Runtime
generation values are resolved by `ResourceManager` when the prefabs bind; they
must not be serialized into these authored documents.

## Motion-database authoring input

`source-locomotion.motiondb-authoring.json` is a production schema-v1
`MotionDatabaseAuthoringDocument`, not a gate-specific command payload. Its
field names and numeric representations match `AuthoringJson` and
`DecodeAuthoringDocument` in `Editor/AssetCompilers/MotionDatabaseCompiler.cpp`.
The document contains all 48 required feature weights, uses two samples per
streaming block to mirror the checked-in advanced database, and preserves the
same contact, tag, and exclusion ranges as the deterministic cooked fixture.

The authoring document references
`Assets/Advanced/source-locomotion.teanim`. `MotionDatabaseSourceResolver`
accepts this project-root-relative spelling and resolves it beneath the open
project's `Assets` directory. The source clip GUID is
`5445414e303433414456464958000003`, its derived source ID is
`0xbc034d0e92ea3b0b`, and its compatibility fingerprint is
`0x043a000000001001`.

Two FNV-1a fingerprints are intentionally distinguished:

- `0x3fad510f215d0684` is the production authoring resolver fingerprint of the
  envelope's 32-byte content hash. This is the decimal
  `sourceClipFingerprint` value stored in the authoring JSON and is required
  for a successful stale-identity check.
- `0x328ab144a83e7df9` is the whole cooked-envelope fingerprint recorded by
  `AdvancedAssets/manifest.json` and used by the already-cooked fixture. It
  describes a different byte scope and must not be substituted into the
  authoring sidecar.

The new output asset UUID `5445414e303433414456414354000004` is distinct
from the checked-in database UUID, so a build cannot alias two files to one
runtime resource. Invoke `animation.motion_database.build` with
`authoringPath` set to
`AdvancedActors/source-locomotion.motiondb-authoring.json` and `outputPath`
set to `AdvancedActors/source-locomotion-built.temotiondb`. Both arguments are
relative to `Assets`; the caller supplies its current automation
`expectedGeneration`, correlation ID, and idempotency key.
