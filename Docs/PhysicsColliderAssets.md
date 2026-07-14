# Physics collider assets

Collider assets separate authored geometry from immutable runtime shape data.
Recipes and the envelope contract are provider-neutral; the payload inside a
`.tepcoll` envelope is opaque Jolt 5.6.0 `ShapeWithChildren` data. A recipe
records source geometry, shape kind, local transform, material GUID, and
provenance. Cooking normalizes that recipe, computes a stable hash, and emits a
versioned cooked envelope. Installed runtimes load only `.tepcoll` and do not
need the source model or `.tecollider` recipe.

## Recipes and supported scale

Primitive recipes (box, sphere, capsule, cylinder, and tapered capsule) are
preferred for gameplay objects. Convex recipes accept finite, non-degenerate
vertex sets. Triangle-mesh recipes require finite vertices and valid indexed
triangles. Compound recipes contain bounded child recipes and must not recurse
through a cycle. Empty geometry, duplicate/degenerate triangles, non-finite
values, excessive depth, and child-count/vertex-count limits are rejected with
diagnostics before provider work begins.

Rich collider recipes accept only finite, positive, uniform source scale. That
scale is baked during cooking; negative or non-uniform values are rejected
before provider work. A scene instance of cooked rich geometry must use an
identity hierarchy scale because changing it requires a new recipe and recook.
Primitive colliders retain their documented runtime scaling rules.

## Headless cooking

The collider compiler runs without a renderer or interactive editor. From a
clean checkout, build and install the RelWithDebInfo preset, then invoke:

```powershell
cmake --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target install
out\build\RelWithDebInfo\Build\Editor\TerraEnginePhysicsColliderCompiler.exe `
  --input <recipe.tecollider> `
  --output <collider.tepcoll> `
  --asset-id <32-character-guid> `
  [--settings <ProjectSettings\PhysicsSettings.json>]
```

Cancellation is cooperative and bounded. A cancelled job must not replace the
last published output. Publication is atomic: write and validate a temporary
envelope, then rename/commit it only after checksum and provider compatibility
checks pass. At runtime, identical recipe hashes reuse one immutable cache
entry. Changing source content or the approved provider/cook revision publishes
a new immutable generation; existing readers remain valid until they release
their old generation.

## Envelope, provenance, and recovery

Every cooked envelope carries a magic value, schema version, non-empty payload,
checksum, recipe hash, source/provenance record, provider revision, and cook
revision. This feature is pinned to Jolt Physics **5.6.0**, commit
`e77f175595e64cb44218cc9d9d56fc365ad0e36a`. Runtime loading rejects malformed,
future-version, checksum-mismatched, provider-mismatched, or empty envelopes.

If a recook fails, the last-known-good envelope remains available and its
diagnostic is surfaced to the editor. Do not delete that file manually while a
recovery is in progress; fix the source geometry or scale, rerun the cook, and
verify the new checksum before exporting.

## Moving and renaming assets

Asset paths are organizational metadata. Move or rename a collider or material
without changing its stable GUID. Scene references and cache keys use the GUID,
so a reload/reimport should resolve the same asset and preserve collider
behavior. Deleting and recreating a file with a new GUID is a different asset
and intentionally causes dependent scenes to report a missing reference.

## Validation and stress scope

From a clean checkout, follow [Docs/Testing.md](Testing.md) to configure, build,
run the focused physics tests, and install. The install step is required so the
runtime, editor compiler, cooked assets, and documentation are verified from
the produced tree rather than from source directories.

US2 validation includes a large cooked-mesh round trip and fixed-step
simulation; results are recorded in
`specs/042-physics-system/evidence/us2-validation.md`. The later whole-feature
stress lane records the required 10,000 mixed bodies and 1,000 active bodies at
60 Hz, including hardware and build details. Asset-level mesh coverage does not
by itself prove that full-scene performance target.
