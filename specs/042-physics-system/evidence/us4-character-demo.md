# User Story 4 Installed Constraint and Character Demonstration

Status: passed installed-runtime validation on 2026-07-14.

## Fixture

`TerraEnginePhysicsConstraintCharacterStress` is a deterministic, headless executable installed beside `TerraEnginePhysicsGameplayStress` and `TerraEngine.dll`. It uses TerraEngine's provider-neutral backend requests and extraction records to create:

- all eight required base constraint families plus Jolt's optional gear constraint;
- limits, springs, and motors on each configured axis;
- a 48-body / 47-link dense point-constraint chain;
- four virtual characters (two capsules and two cylinders) over 180 fixed 60 Hz steps, covering movement, jumping, resize, teleport, and support-state extraction; and
- the provider-neutral one-shot constraint-break and monotonic-reset policy.

The executable emits one JSON invariant record and returns nonzero for invalid requests, provider errors, non-finite body/constraint/character state, missing extracted records, failed support extraction, failed break/reset policy, unhealthy backend state, or teardown failure.

## Local build-tree validation

Command (worker-local scope, Visual Studio developer environment):

```powershell
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && cmake --build out\build\Debug --target TerraEnginePhysicsConstraintCharacterStress'
& 'out/build/Debug/TerraEnginePhysicsConstraintCharacterStress.exe'
```

Outcome: target compiled and linked successfully. Two consecutive build-tree
runs returned exit code `0` with identical invariant output:

```json
{"tool":"physics_constraint_character_stress","passed":true,"stage":"complete","provider":{"name":"Jolt","version":"5.6.0","revision":"e77f175595e64cb44218cc9d9d56fc365ad0e36a"},"fixedSteps":180,"bodies":49,"constraints":{"families":8,"gear":1,"denseChainLinks":47,"total":58,"limitsMotorsSprings":true},"characters":{"total":4,"capsules":2,"cylinders":2,"supported":3,"movementJumpResizeTeleport":true},"breakReset":true,"stateHash":"5132756894446993244","teardown":true}
```

The executable currently links directly into `out/build/Debug`, so the
build-tree path for the local run is
`out/build/Debug/TerraEnginePhysicsConstraintCharacterStress.exe`.

## Installed-runtime validation

The orchestrator ran the required install and then launched the executable twice from the installed Engine directory:

```powershell
cmake --build out/build/Debug --target install --config Debug --parallel 4
Push-Location out/build/Debug/Build/Engine
& '.\TerraEnginePhysicsConstraintCharacterStress.exe'
& '.\TerraEnginePhysicsConstraintCharacterStress.exe'
Pop-Location
```

Both independent installed processes returned exit code `0` and emitted identical JSON, including state hash `5132756894446993244`. The exact shared record was:

```json
{"tool":"physics_constraint_character_stress","passed":true,"stage":"complete","provider":{"name":"Jolt","version":"5.6.0","revision":"e77f175595e64cb44218cc9d9d56fc365ad0e36a"},"fixedSteps":180,"bodies":49,"constraints":{"families":8,"gear":1,"denseChainLinks":47,"total":58,"limitsMotorsSprings":true},"characters":{"total":4,"capsules":2,"cylinders":2,"supported":3,"movementJumpResizeTeleport":true},"breakReset":true,"stateHash":"5132756894446993244","teardown":true}
```

The fixture requires every extracted pose, velocity, impulse magnitude, ground/support value, and provider diagnostic used by the hash to be finite. It also requires every family and character operation to succeed, at least one character to publish support, break/reset semantics to pass, and teardown to leave no live provider world. All invariants passed; no tolerance exception or retained provider error was observed.
