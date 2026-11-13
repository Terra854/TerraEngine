# FBX Fixtures

These tests use the already-vendored Assimp FBX fixtures under
`External/assimp/test/models/FBX` instead of duplicating binary model blobs in
the TerraEngine test tree. That keeps the feature self-contained, preserves the
upstream fixture provenance, and avoids adding or modifying `External/`.

Primary fixtures used by the editor tests:

- `box.fbx`: one static mesh with a material slot.
- `phong_cube.fbx`: material-factor coverage.
- `maxPbrMaterial_metalRough.fbx`: PBR material-field coverage.
- `box_orphant_embedded_texture.fbx`: embedded-texture diagnostics coverage.
- `animation_with_skeleton.fbx`: supported skeleton and animation import. The
  registered `ReportsImportedAnimationAndSkeletonSummary` regression requires
  `FbxAnimationImportSummary` and Skeleton/AnimationClip manifest children, and
  rejects the legacy `UnsupportedAnimationIgnored`/`UnsupportedSkeletonIgnored`
  diagnostics. Cooked children use `.teskel` skeleton and `.teanim` clip files;
  their exact package-relative paths come from the generated manifest.

See [FbxImportDiagnosticsTests.cpp](../../FbxImportDiagnosticsTests.cpp) for the
registered assertions. The importer also preserves distinct material sections
for same-named skinned meshes. Optional Quaternius duplicate-Body/bind-pose
regressions read `out/validation/animation-model-sources/quaternius/BaseCharacter.fbx`;
this downloaded corpus is separate from the bundled Assimp fixtures. Missing
optional corpus data does not prove those regressions executed.

The source fixtures are [under the bundled Assimp tree](../../../../../External/assimp/test/models/FBX).
No new download or third-party checkout is required for the primary list above.
