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
- `animation_with_skeleton.fbx`: unsupported animation/skeleton diagnostics.
