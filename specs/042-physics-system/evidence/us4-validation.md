# User Story 4 validation

Date: 2026-07-14  
Configuration: Windows x64, Debug, Visual Studio 2026 toolchain  
Provider: Jolt Physics 5.6.0 at `e77f175595e64cb44218cc9d9d56fc365ad0e36a`

## Integrated compile and focused tests

The affected engine test target and installed stress fixture compiled successfully:

```powershell
cmake -S . -B out/build/Debug
cmake --build out/build/Debug --target TerraEngine-EngineTests TerraEnginePhysicsConstraintCharacterStress --config Debug --parallel 4
ctest --test-dir out/build/Debug -C Debug -R '^Engine\.PhysicsUS4$' --output-on-failure
```

CTest result: `1/1` test passed in 0.14 seconds. The filter executed 12 focused cases with zero failures:

- ConstraintSet CRTP, reflection, stable record IDs, endpoint/range validation, and deterministic duplicate rejection.
- CharacterController CRTP, reflection, defaults, traversal validation, and command/state boundaries.
- Constraint and character schema round trips, runtime-state exclusion, defaults, and atomic malformed-data rejection.
- All eight required Jolt constraint families, limits, springs, motors, solver-lambda extraction, staging rollback, and optional gear references.
- Capsule/cylinder character movement, grounding, jump, resize, teleport, support publication, and teardown.
- Exactly-once break publication, persistent broken state, monotonic reset generations, and bounded immutable character packets.
- Gear dependency regressions proving unrelated hinges remain editable while referenced hinges require gear-first teardown.

The initial integrated run exposed Jolt lock-order assertions during constraint creation. Constraint construction now uses Jolt's no-lock body lookup only while TerraEngine holds the protected, step-joined backend boundary. Repeated focused runs then completed without provider assertions.

## Atomic mutation and scheduler boundary

`PhysicsSystem` now stages every prepared body, shape, constraint, and character in one provider token. Engine constraint registrations are provisional under the protected system lock. Any rejected member removes those registrations, rolls back provider records, releases all new generation handles, and leaves the prior runtime maps intact. Successful publication retires gears before hinges, constraints before bodies, and then swaps all runtime records at the protected commit boundary.

The provider-heavy implementation and explicitly exported `PhysicsSystem` implementation are compiled through private static archives. This keeps Jolt and internal transaction templates out of Windows' automatic export scan while preserving the public DLL ABI; the regenerated engine export definition contains 62,786 entries, below MSVC's 65,535 import-library ceiling.

## Install

The required repository install command passed:

```powershell
cmake --build out/build/Debug --target install --config Debug --parallel 4
```

The canonical tree contains:

- `Build/Engine/TerraEngine.dll`
- `Build/Engine/TerraEnginePhysicsConstraintCharacterStress.exe`
- installed provider-neutral physics headers for constraints and characters
- `Build/ThirdPartyNotices/JoltPhysics-LICENSE.txt`

Jolt headers, source, samples, and tools are not installed as public/runtime dependencies.

## Result

US4 focused compile, tests, install, and installed-runtime fixture passed. See `us4-character-demo.md` for the repeated installed-process invariant evidence.
