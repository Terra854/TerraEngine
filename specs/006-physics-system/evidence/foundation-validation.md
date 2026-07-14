# Phase 2 foundational validation

**Date**: 2026-07-13  
**Configuration**: Windows Debug, Ninja, MSVC 19.51.36247

## Commands

```powershell
cmd.exe /s /c '"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && cmake --preset Debug && cmake --build --preset Debug --target TerraEngine-EngineTests'
$env:TERRAENGINE_TEST_FILTER='Engine.Physics'
.\out\build\Debug\TerraEngine-EngineTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
cmd.exe /s /c '"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && cmake --build --preset Debug --target install'
```

## Results

- `TerraEngine-EngineTests` compiled successfully with the private static Jolt
  target and TerraEngine shared engine.
- All 19 focused `Engine.Physics` tests passed; the harness reported 713
  registered tests and zero failures in the filtered run.
- Generation reuse/stale rejection passed for world, body, constraint,
  character, query, and shape registries.
- Settings defaults, stable layer IDs, units, and calibrated bounds passed.
- Public non-backend headers contain no Jolt include or namespace reference.
- The real Jolt adapter initialized, created an empty world, simulated one fixed
  tick, drained, destroyed, and shut down successfully.
- Sixty-four provider jobs completed through the existing TerraEngine thread
  pool and barrier; the adapter reported zero recurring provider threads.
- Bounded queue, cancellation, idempotency, stale-world, and deterministic
  merge tests passed.
- Coordinated scene/provider commit, late scene conflict, provider inverse,
  cancellation, and zero-partial-visibility tests passed.
- The install target completed successfully and installed the backend-neutral
  physics contracts under `Build/include/TerraEngine/Physics`.

## Compile corrections

The first compile identified that direct Jolt headers require `Jolt/Jolt.h` to
be included first. Enabling the external profiler also makes the readable
broad-phase layer name part of Jolt's adapter interface. Both integration
details were implemented in TerraEngine-owned adapter files; provider source
remained pristine. A second compile found the missing concrete ray/cast-result
includes and the profile layer-name override. The final compile, focused test,
and install commands passed.
