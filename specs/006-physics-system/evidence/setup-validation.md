# Phase 1 setup validation

**Date**: 2026-07-13  
**Provider**: Jolt Physics 5.6.0 commit
`e77f175595e64cb44218cc9d9d56fc365ad0e36a`

## Dependency identity

The permanent provider is a detached Git submodule at
`External/JoltPhysics`. `.gitmodules` names the approved upstream URL and does
not track a moving branch. Root CMake validates the exact Git object when
metadata is present and always validates the 5.6.0 version macros.

## Windows gate

Visual Studio 2026 Developer Command Prompt 18.7.0, MSVC 19.51.36247, Ninja,
and the Debug preset were used.

```powershell
cmd.exe /s /c '"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && cmake --preset Debug && cmake --build --preset Debug --target TerraEngine-EngineTests'
$env:TERRAENGINE_TEST_FILTER='Engine.Physics.Dependency'
.\out\build\Debug\TerraEngine-EngineTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
cmd.exe /s /c '"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && cmake --build --preset Debug --target install'
```

Results:

- Root configure and `TerraEngine-EngineTests` compilation passed.
- Both `Engine.Physics.Dependency` contract tests passed.
- The full install target passed after building 485 required targets/steps.
- `Build/Docs/Dependencies/JoltPhysics.md` and
  `Build/Docs/Licenses/JoltPhysics-LICENSE.txt` are present.
- The installed tree contains no Jolt samples, viewer, headers, or source.

## Linux provider gate

Ubuntu/WSL with GCC 15.2 and Unix Makefiles was used directly against the
permanent submodule:

```bash
bash Tools/Physics/BackendSpike/RunBackendSpike.sh \
	External/JoltPhysics out/physics-spike/linux-submodule
```

The core provider configure/build/install and harness passed. The final
10,000-body/1,000-active/10,000-ray run reported `gravityPassed=true`,
`linearCastPassed=true`, p50/p95/p99 tick times of 1.300/1.794/2.153 ms, ray
time 4.176 ms, and state hash `c8bf9c9c73edc765`.

## Notes

One initial Windows rerun found the build tree locked by a previous command
whose caller had timed out; after that process exited, the same command passed.
One Linux harness configure initially supplied a relative prefix and could not
find the already-installed provider package. The runner now resolves absolute
paths and passes the exact `Jolt_DIR`; the incremental rerun passed. Neither
issue changed provider source or required a network/package-manager fallback.
