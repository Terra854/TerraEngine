# Importing Animated Models

TerraEngine imports `.gltf`, `.glb`, and `.fbx` files as model packages. One package may produce several skeletons, deformable meshes, morph targets, clips, materials, textures, and a generated prefab. Import is a transaction: either the complete compatible generation is published, or the previous generation remains active.

## Preferred editor workflow

1. Copy or move the source package and its referenced files under the project `Assets` directory.
2. Let the Asset Database discover the package, or request reimport from the Asset Browser.
3. Wait for the process-isolated model package compiler to finish. Cancellation is polled at bounded staging points.
4. Inspect the package summary and its read-only generated children in the Asset Browser.
5. Open the generated prefab and verify its skeleton, default clip metadata, deformable meshes, morph targets, materials, and bind-pose result in Scene and Game views.
6. Save, move/rename, reload, and reimport the package as needed. Stable semantic GUIDs keep compatible references intact.

Generated animation children are package-owned and read-only. Edit the source package or a dedicated authored animation asset rather than renaming or overwriting a generated child in isolation.

## Headless compiler workflow

The editor normally supplies these arguments. For a focused diagnostic import, use the installed compiler:

```powershell
TerraEngineModelPackageCompiler `
	--source "C:\Project\Assets\Characters\Hero.glb" `
	--asset-root "C:\Project\Assets" `
	--build-root "C:\Project\Build\Assets" `
	--source-relative "Characters/Hero.glb" `
	--package-guid "00112233445566778899aabbccddeeff" `
	--shader-guid "00000000000000000000000000000001"
```

The compiler emits newline-delimited JSON progress records and writes the normal model-package sidecar. A zero exit code means that every generated mesh named by the published result also exists in the build tree. Prefer the editor/MCP import surface for normal authoring because it owns discovery, cancellation, refresh, and presentation of diagnostics.

## Supported source content

| Content | glTF/GLB | FBX |
|---|---|---|
| Nested node hierarchy and local transforms | fastgltf node graph | Assimp node graph |
| Multiple skins/skeletons | Supported | Supported when represented by imported bone sets |
| Inverse bind matrices | Preserved and validated | Derived/preserved from Assimp bone offsets |
| Multiple `JOINTS_n` / `WEIGHTS_n` sets | Decoded and merged | Assimp vertex weights merged by bone |
| Step, linear, cubic clip interpolation | Preserved where authored | Preserved when Assimp exposes equivalent keys; unsupported source metadata is diagnosed |
| Morph position deltas | Preserved, including morph-only meshes | Preserved from Assimp animation meshes |
| Named sockets | Preserved through semantic node paths | Preserved through semantic node paths |
| Materials and textures | Package children and assignments | Package children and assignments |
| Units, handedness, up/forward axes | Normalized once | Normalized once from FBX/Assimp metadata |
| Root-motion/event metadata | Preserved when representable by the importer contract | Preserved when representable by the importer contract |

The importers do not invent missing animation semantics. Unsupported interpolation, malformed sparse data, missing bone targets, non-finite transforms, or incomplete metadata is reported with a stable code and source path.

## Generated package contents

The package result contains an `AnimationImportManifest` with:

- package GUID, importer/version/settings, source hash, and coordinate conversion;
- stable semantic subasset key, GUID, kind, display name, source path, cooked path, content hash, and compatibility fingerprint for every generated child;
- dependency GUIDs and a deterministic dependency fingerprint;
- candidate, published, and last-known-good generation state;
- structured diagnostics and whether the previous generation was preserved.

The generated prefab owns renderable child objects and, for animated packages, an authored root `AnimatorComponent`. Its requested execution policy is `Auto`, its skeleton override points to the primary generated skeleton, and transient runtime pose/state is never serialized. Top-level animation-binding metadata identifies the primary skeleton, default clip, full clip set, deformable meshes, and morph targets without pretending that a clip is an animation controller. The Prefab manifest record depends on every referenced generated child, so dangling or incomplete bindings reject publication.

Asset Database summaries expose counts, generation, dependency fingerprint, warnings/errors, and package-child metadata without creating loose child sidecars. Runtime aliases point directly at cooked source-free paths.

## Stable identity

Subasset identity comes from the package GUID plus a normalized semantic role/path. It does not use display name or array position. This preserves references when:

- the source file moves or is renamed inside the project;
- nodes, clips, or meshes are reordered without changing semantic identity;
- duplicate display names are present;
- the project closes and reloads; or
- a compatible reimport publishes a later generation.

A semantic change may intentionally create a different child GUID. A skeleton topology/reference-pose change also changes its compatibility fingerprint so old clips and deformable meshes are rejected rather than misbound.

## Coordinate conversion

Each importer determines source unit scale, handedness, up/forward axes, and inherited transform convention once. That conversion is applied consistently to node placement, bind/reference poses, inverse binds, animation translation and rotation, morph deltas, sockets, and root-motion metadata. Applying a second conversion downstream is a bug; generated artifacts are already in TerraEngine project convention.

Mirrored or negative-scale hierarchies are retained only when the resulting transforms and bind matrices remain finite and valid. Otherwise publication fails with the responsible source path.

## Skinning and morph rules

- At most four influences are retained per cooked vertex.
- Duplicate joint influences are merged before reduction.
- Negative, non-finite, or out-of-skeleton influences are rejected.
- Retained weights are normalized and quantized to an exact `UNORM8` sum of 255.
- Zero total weight is explicit identity deformation, not an implicit bind to joint zero.
- A palette partition contains at most 256 source joints. Deterministic topology-aware partitioning is attempted before rejection.
- A bound deformable mesh must carry the exact nonzero skeleton compatibility fingerprint.
- Canonical TEMF v1 stores dense position, normal, and tangent morph deltas in
  descriptor-owned deformable streams.

The hard source-package limits are 1,024 skins/skeletons and 4,096 clips; the normal operational defaults are 64 and 256. See [Animation Assets](AnimationAssets.md) and the [frozen limits contract](../../Docs/Animation/README.md) for all limits.

## Atomic staging and cancellation

Import prepares candidate outputs under a package staging directory, then validates the complete manifest and cross-references. Publication uses deterministic file order and publishes the generation marker last. Existing live files are backed up for rollback until every candidate file is in place.

Cancellation, stale source generation, parser error, cook error, manifest error, or filesystem publication failure removes staging data and restores any displaced last-known-good files. The package sidecar records the rejected candidate and preservation result; consumers never observe a half-old/half-new asset set.

## Diagnostics

Diagnostics include severity, stable code, source package/object path, message, corrective action, affected child, and last-known-good state. Common categories include:

| Category | Meaning and correction |
|---|---|
| Invalid hierarchy/bind data | Repair cycles, missing parents, non-finite transforms, or inverse binds in the source rig. |
| Invalid influence/missing joint | Normalize weights and restore or remove stale bone references. |
| Palette limit exceeded | Split a topology-connected deformable region or reduce the rig/influence set. |
| Missing/mismatched fingerprint | Reimport the complete package so skeleton, mesh, and clips share one generation. |
| Unsupported interpolation/morph metadata | Bake to a supported representation or remove unsupported channels. |
| Manifest/dependency rejection | Restore the missing generated child or correct prefab binding dependencies. |
| Candidate rejected/cancelled | The prior complete generation remains active; correct the first error and reimport. |

Warnings may publish a complete generation only when the affected content has a documented safe representation. Errors reject the candidate atomically.

## Clean validation

From a clean checkout in a Visual Studio developer shell:

```powershell
cmake --fresh --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target `
	TerraEngineModelPackageCompiler TerraEngine-EngineTests TerraEngine-EditorTests
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
	-R "Animation.AssetFormat|Animation.Codec|Editor.Animation.GltfImport|Editor.Animation.FbxImport|Editor.Animation.SubAssetIdentity|Editor.Animation.SkinnedMeshCook" `
	--output-on-failure
cmake --build --preset RelWithDebInfo --target install
```

Then import the deterministic fixtures through the installed editor, inspect
generated children and prefab bindings, inject a malformed or cancelled
reimport, and verify that the last-known-good bytes and GUIDs remain unchanged.

No source model is downloaded during configure, build, test, install, editor
startup, or normal import validation. Any licensed real-model acquisition is an
explicit user action outside the build.

Measured import and authoring timing, animation benchmarks, diagnostics overhead,
and engine-wide baseline comparisons require an explicit performance run.
Correctness and source-free import validation remain mandatory regardless of
whether performance measurement is scheduled.
