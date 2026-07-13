# US2 rich collider asset validation

Validation date: 2026-07-14 (Asia/Singapore)

## Environment

- Configuration: CMake/Ninja Debug, MSVC x64 developer environment.
- Host: Windows 11 Home 64-bit, build 10.0.26200.
- CPU: AMD Ryzen 9 7950X, 16 cores / 32 logical processors.
- Provider: Jolt Physics 5.6.0 at exact revision
  `e77f175595e64cb44218cc9d9d56fc365ad0e36a`.
- Payload contract: `TerraEngine.Jolt.ShapeWithChildren`, version 1.

## Red-to-green findings

The first focused simulation pass intentionally exposed two provider-boundary
failures: a collinear zero-area mesh triangle reached Jolt, and a mutated opaque
payload could reach Jolt restoration. The backend now performs an explicit
triangle-area check and verifies the envelope-compatible FNV-1a payload checksum
before deserialization. Both regression assertions pass.

The first integrated `Engine.PhysicsUS2` run also found two invalid compound test
fixtures: value-initialized rotation was the valid identity rotation, and the
cycle fixture retained convex vertices after changing kind. The fixtures were
corrected to use a zero quaternion and a geometry-free compound. The production
cycle check was ordered ahead of depth exhaustion so a real cycle reports the
specific `CompoundCycle` diagnostic.

## Focused suites

Command:

```powershell
ctest --test-dir out/build/Debug -R '^(Engine|Editor)\.PhysicsUS2$' --output-on-failure
```

Result: 2/2 CTest entries passed.

- Engine coverage: four recipe tests, three cooked-envelope tests, and four
  cooked-shape simulation/cache tests.
- Editor coverage: five atomic compiler/cancellation/cache/reimport tests and three
  physics-material publication/move/migration tests.
- Simulation includes convex cook/restore, static triangle mesh with dynamic
  ownership rejection, a 64x64-cell large mesh supporting a dynamic body,
  compound restoration, corrupt/degenerate/oversized/scale rejection, and
  immutable cache deduplication/replacement/stale/reader-retirement behavior.

## Real headless compiler

The built compiler printed the documented usage, cooked
`Tests/Fixtures/Physics/Convex.tecollider` to a 647-byte `.tepcoll`, and reported
the exact approved Jolt revision. A second identical invocation returned
`cache_reused`; SHA-256 remained
`6D052B4D7D3E4E373636C31B17096093C670F5C56090883E77B91A160B0EE17C` and the
output write timestamp did not change. The runtime restoration tests consume
only the opaque cooked payload, proving simulation does not require source
geometry.

## Compilation and install

The following x64 targets compiled successfully:

- `TerraEngine-Engine`
- `TerraEngine-Editor`
- `TerraEnginePhysicsColliderCompiler`
- `TerraEngine-EngineTests`
- `TerraEngine-EditorTests`

`cmake --build --preset Debug --target install -j 8` completed successfully.
The installed build tree contains the editor, physics compiler, physics public
headers, Jolt license notice, and `Docs/PhysicsColliderAssets.md` under
`out/build/Debug/Build`.

This is the US2 asset-level checkpoint. The mandatory 10,000-body / 1,000-active
whole-scene performance and installed-editor loopback evidence belongs to the
later integrated stress tasks and is not inferred from the large-mesh test.
