# Foundation Validation Evidence

Validation date: 2026-07-13

Provider baseline: Jolt Physics 5.6.0 at commit
`e77f175595e64cb44218cc9d9d56fc365ad0e36a`.

## Configure

Command:

```powershell
cmake --preset Debug
```

Result: PASS. CMake generated the Debug build with the vendored Jolt provider.

## Integrated Debug build

Command (from the repository's configured Visual Studio developer shell):

```powershell
cmake --build --preset Debug --parallel 16
```

Result: PASS. The Jolt provider, TerraEngine, editor, launcher, export tool, managed
assemblies, shaders, and engine/editor test binaries compiled and linked.

## Focused foundation tests

Command, rerun after installation:

```powershell
ctest --test-dir out/build/Debug -C Debug --output-on-failure -R "Engine.Physics(Foundation|DependencyContract)"
```

Result: PASS, 2/2 tests and 0 failures. This includes the provider dependency
contract and the physics handle, settings, backend boundary, job adapter, command
queue, and coordinated transaction foundation cases.

## Install

Command (from the repository's configured Visual Studio developer shell):

```powershell
cmake --build --preset Debug --target install --parallel 16
```

Result: PASS. The install tree was produced at `out/build/Debug/Build`.
Provider-neutral physics headers were installed below
`include/TerraEngine/Physics`, including `Backend/IPhysicsBackend.h`.
Provider-private Jolt headers and libraries were not installed. The only installed
Jolt-named artifact is `ThirdPartyNotices/JoltPhysics-LICENSE.txt`.

The generated `CMakeCache.txt` contains no retained
`USE_STATIC_MSVC_RUNTIME_LIBRARY` cache entry. TerraEngine forces that Jolt option
off before provider configuration so Jolt and TerraEngine use the same MSVC
runtime without exposing a persistent user cache switch.

## Integration defects found and corrected

- The engine test target initially lacked the provider-private include path used by
  the job-adapter test. The test target now links Jolt privately; public engine
  headers remain provider-neutral.
- Jolt initially selected the static MSVC runtime, causing an `MTd`/`MDd` link
  mismatch. The dependency contract now forces
  `USE_STATIC_MSVC_RUNTIME_LIBRARY=OFF` before adding the provider.
- Direct construction of the Jolt job adapter before installing the Jolt allocator
  callbacks caused a test crash. The adapter now enforces the allocator lifecycle
  precondition and tests construct the allocation adapter first.
- A stale scene read set with physics-only writes was not being validated. Physics
  transaction preflight now validates every non-empty scene read set, preserving
  zero-partial-visibility conflict behavior.

These fixes were included in the successful integrated build, focused tests, and
install run above.
