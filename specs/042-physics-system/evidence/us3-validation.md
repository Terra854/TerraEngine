# User Story 3 Validation Evidence

Date: 2026-07-14  
Configuration: Windows x64, Debug, Visual Studio 2026 toolchain  
Provider: Jolt Physics 5.6.0, revision `e77f175595e64cb44218cc9d9d56fc365ad0e36a`

## Red-to-green findings

- The first integrated scheduler run rejected the new gameplay workloads because the query and event resources were declared by `PhysicsSystem` but absent from `DefaultSchedulerResources`. Adding the provider-neutral resources to the scheduler catalog made the declaration check pass.
- Replacement-world regressions initially exposed imprecise stale-query classification and a retained fixture body. Query reads now report `StaleWorld` for a replaced world and every 100-cycle fixture releases its replacement body.
- Provider command review exposed an object-slot reuse gap: Jolt validated the component generation but not the object generation. `JoltPhysicsBackend::ApplyCommand` now validates both before touching provider state, and the provider suite rejects a delayed stale-object command.

## Focused compilation and tests

The integrated engine test target compiled successfully:

```powershell
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
cmake --build out/build/Debug --target TerraEngine-EngineTests --config Debug --parallel 4
```

The complete User Story 3 CTest filter passed:

```powershell
ctest --test-dir out/build/Debug -C Debug -R '^Engine\.PhysicsUS3$' --output-on-failure
```

Result: `1/1` passed in 1.04 seconds. The registered filter covers command ordering and modes, all query families and limits, event ordering/filtering/retention, provider queries and callbacks, and 100-cycle stale publication replacement regressions.

The direct provider filter also passed with 775 checks and zero failures, including:

- `Physics.Provider.Jolt56.AllQueryKindsFiltersAndBounds`
- `Physics.Provider.Jolt56.CallbackBridgePublishesCollisionTriggerAndActivationEvents`

## Install validation

The required install target completed successfully after the gameplay stress target was added:

```powershell
cmake --build out/build/Debug --target install --config Debug --parallel 4
```

The install produced the engine library, public gameplay/query/event headers, `Docs/PhysicsGameplay.md`, and `Build/Engine/TerraEnginePhysicsGameplayStress.exe`. No global Jolt or other runtime dependency was used.

## Contract coverage

- Concurrent commands merge by effective tick, producer ID, producer sequence, and protected-boundary sequence.
- Force, acceleration, impulse, point impulse, velocity change, explicit velocity, teleport, kinematic target, wake, and sleep use typed deferred records.
- Query dimensions, cast distance, batch count, result count, retention, and history are bounded.
- Query publication and event delivery validate world, object, component, record, and body generations.
- Jolt callbacks only populate bounded engine-owned collectors; scene and gameplay publication occurs after the provider join boundary.
- Diagnostics publish command/query/event counts, hit/truncation/filter/drop/stale/rejection counts, and query/event timing.

