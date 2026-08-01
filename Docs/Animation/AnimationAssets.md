# Animation Assets

TerraEngine cooks animated model packages into source-independent runtime assets. The runtime never parses glTF or FBX files: it reads checked `.teskel`, `.teanim`, and `.temf` artifacts from the project build or installed/exported content tree.

This document describes the first frozen animation asset family used by animation production validation. Later controller, mask, retarget, motion-database, and ragdoll formats use the same common envelope and compatibility rules.

## Common identity and envelope

Every cooked animation asset has:

- a stable 128-bit asset GUID;
- a versioned binary envelope with magic, major/minor version, header size, endianness, payload size, content hash, dependency table, and chunk table;
- a nonzero compatibility fingerprint when it binds to a skeleton generation;
- checked 64-bit offset/count arithmetic before allocation or payload access; and
- deterministic table ordering so identical input and settings produce identical bytes.

Generated package GUIDs are derived from the package GUID plus a normalized semantic role and source path. Display names and source array positions are deliberately excluded. This is why moving, renaming, reordering, or reimporting compatible source content does not change scene, prefab, controller, or gameplay references.

Unknown major versions and unknown required chunks are rejected. Optional minor chunks may be skipped only when their recorded range is valid. Rejected data never partially replaces the last-known-good generation.

## Skeleton assets (`.teskel`)

A skeleton asset contains the parent-before-child hierarchy, stable joint IDs, display names, normalized semantic paths, local bind/reference transforms, inverse bind matrices, optional semantic roles, sockets, source unit/axis metadata, a reference-pose hash, and the skeleton compatibility fingerprint.

Important rules:

- The hierarchy has one logical root and is acyclic.
- Every transform and inverse bind value is finite; rotations are normalized.
- Duplicate display names are allowed, but stable semantic IDs and paths must remain distinct.
- Socket identity is semantic and survives compatible display-name changes.
- A consumer may provide an expected fingerprint. A mismatch is rejected before the asset enters the runtime cache.

The version-1 hard limit is 4,096 joints in one skeleton; the normal operational default is 256.

## Animation clips (`.teanim`)

A clip stores fixed integer-tick duration and sample rate, independently seekable compressed segments, joint tracks, morph-weight tracks, named custom curves, ordered events, root-motion metadata, compression tolerances, and the compatible skeleton identity.

The engine-owned codec uses:

- constant/default-track elision;
- segmented random access;
- quantized scalar and vector tracks;
- smallest-three quaternion encoding;
- step, linear, and smooth cubic interpolation metadata; and
- the same checked bit contract for CPU reference decoding and later GPU decoding.

Runtime validation opens every declared codec track before publication. A bad segment range, decoder version, encoded stride, timestamp ordering, or fingerprint rejects the complete clip. Default authoring tolerances are 0.5 mm translation, 0.1 degrees rotation, 0.05% scale, and 0.0005 absolute morph weight; a source may request tighter limits.

The version-1 hard limits include 24 hours per clip, 65,536 channels, 16,777,216 total keys, and 1,048,576 authored events. See [the frozen runtime limits](../../Docs/Animation/README.md) for the complete table.

## Deformable TEMF

Static and deformable meshes use the same canonical TEMF v1 production
directory. A skinned or morphed mesh declares descriptor-owned skin, palette,
morph, and demanded-page blocks; it does not append an extension or carry a
second reader contract. Cooked data from earlier layouts is rejected with a
recook-required diagnostic.

Canonical deformable metadata records:

- skinning vertex count and 8-byte skinning-stream range;
- deterministic palette partition and per-vertex palette tables;
- dense morph position, normal, and tangent records with stable target IDs;
- source skeleton joint count and compatibility fingerprint;
- conservative whole-mesh deformation bounds; and
- fail-open virtual-geometry flags that disable unproven cone culling and require complete fallback coverage.

Each skinned vertex stores four palette-local `uint8` joint indices and four `UNORM8` weights. The cooker merges duplicate joints, rejects negative/non-finite values and missing joints, retains the four highest weights with a stable joint-ID tie break, renormalizes them, and distributes quantization remainder deterministically so every non-identity record sums exactly to 255. It reports discarded weight when influence reduction changes fidelity.

Four zero weights encode identity deformation. The runtime must not sample a palette for that record. A morph-only mesh may therefore carry a zero skeleton fingerprint; any payload containing a nonzero retained skin weight must carry the nonzero fingerprint of its cooked skeleton.

Topology-connected vertices share one palette-local index space. Partitions are created deterministically and contain at most 256 source joints. A connected region that cannot fit is rejected with an actionable diagnostic instead of silently truncating joint indices.

Morph streams store finite position, normal, and tangent deltas. Each demanded
page carries only the deltas and identities needed by that independently
validated page.

## Runtime validation and loading

`ResourceManager` exposes read-only validation commands and source-free loaders:

```cpp
auto skeletonResult = resources.ValidateSkeletonAssetFile(path, expectedFingerprint);
auto clipResult = resources.ValidateAnimationClipAssetFile(path, expectedFingerprint);
auto meshResult = resources.ValidateDeformableMeshFile(
	path, expectedJointCount, expectedFingerprint);

TerraEngine::UUID skeleton = resources.LoadSkeleton(path, expectedFingerprint);
TerraEngine::UUID clip = resources.LoadAnimationClip(path, expectedFingerprint);
TerraEngine::UUID mesh = resources.LoadDeformableMesh(
	path, expectedJointCount, expectedFingerprint);
```

Validation reports the detected version, actual and expected fingerprint, relevant counts, stable diagnostic code, and a corrective message. It does not reserve UUIDs or mutate a live cache. Loading repeats the same checks, verifies the cooked GUID, and publishes only a complete immutable cache entry.

Use `LoadMesh` for a canonical static TEMF and `LoadDeformableMesh` when the
canonical deformable descriptor set is mandatory.

## Compatibility and publication

A topology or reference-pose change creates an incompatible skeleton fingerprint. Meshes and clips cooked against the old fingerprint cannot bind to the new skeleton. Package import stages all mutually dependent artifacts, validates their GUIDs, dependencies, fingerprints, and ranges, then publishes the package generation atomically. Failed parse, cook, validation, cancellation, or publication leaves the prior generation intact.

Runtime generations remain immutable until their frame, GPU, and managed references retire. A newly loaded generation never aliases an old generation-safe handle.

## Reproducible verification

From a Visual Studio developer shell:

```powershell
cmake --fresh --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine-EngineTests TerraEngine-EditorTests
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
	-R "Animation.AssetFormat|Animation.Codec|Editor.Animation.SkinnedMeshCook" `
	--output-on-failure
cmake --build --preset RelWithDebInfo --target install
```

All dependencies come from the repository; configuring, building, testing, and installing do not download animation models or require a global package manager. The install step is mandatory because editor/runtime validation uses installed binaries and assets.

These commands are correctness checks. Candidate benchmarks, workflow timing,
diagnostics-overhead measurements, and performance comparisons require a
separate measured release run. Deferring measurement does not waive any
performance threshold.
