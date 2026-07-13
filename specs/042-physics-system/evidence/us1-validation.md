# US1 Validation Evidence

Date: 2026-07-13  
Provider: pristine Jolt Physics 5.6.0, commit `e77f175595e64cb44218cc9d9d56fc365ad0e36a`

Jolt 5.5 is used only by the provider comparison evidence and is not linked into TerraEngine.

## Red-to-green observations

- The first strengthened US1 run exposed three failures: an incomplete fixed-step fixture, a globally misordered `After Physics` flush, and a compact static-support fixture that allowed a dynamic body to fall from its edge.
- The scheduler now reserves a unique global order block for each system and flush, so all seven Physics workloads finish before `After Physics`.
- Physics Transform writes retain transaction receipts until the protected flush. Rejected writes do not advance `lastPublishedPose`, so sleeping bodies retry instead of silently losing publication.
- The rigid-body fixtures now use calibrated support geometry and validate material restitution, layer/sensor filtering, sleep/wake, kinematic targets, and a real Discrete-versus-LinearCast thin-geometry tunnelling outcome.

## Integrated compile

The x64 Visual Studio 2026 developer environment was selected explicitly:

```powershell
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
cmake --build out\build\Debug --config Debug --target TerraEngine-EngineTests TerraEngine-Editor TerraEngine-EditorTests --parallel 8
```

Result: engine library, editor executable, engine tests, and editor tests compiled and linked successfully.

## Focused validation

```powershell
ctest --test-dir out/build/Debug -C Debug --output-on-failure -R "(Engine.Physics(Foundation|US1|DependencyContract)|Editor.PhysicsUS1)"
```

Result: 4/4 tests passed.

- `Engine.PhysicsDependencyContract`: passed.
- `Engine.PhysicsFoundation`: passed.
- `Engine.PhysicsUS1`: passed.
- `Editor.PhysicsUS1`: passed.

The US1 group covers component contracts and serialization, PhysicsMaterial JSON, fixed-step bounds and 100 consecutive paused tokens, transaction conflict/rollback, seven-workload scheduling, `After Physics` visibility, Jolt-backed body/material/filter/CCD/sleep behavior, and exact authored restoration across fresh Play worlds.

## Install checkpoint

```powershell
cmake --build out\build\Debug --config Debug --target install --parallel 8
```

Result: passed. The canonical install tree was refreshed at `out/build/Debug/Build`, including the installed editor/runtime, provider-neutral physics headers, and `ThirdPartyNotices/JoltPhysics-LICENSE.txt`.

## Remaining demonstration

T048 remains open for the final installed-editor loopback. It will be completed with the broader final stress fixture so the same installed build exercises gravity, collision, paused Next, transform visibility, restoration, and a large object count.
