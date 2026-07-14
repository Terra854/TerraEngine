# Sanitizer Validation (T146)

Status: completed capability audit with no eligible Spec 042 sanitizer lane.
This record documents the Windows/MSVC boundary without claiming sanitizer
coverage that the repository and required build configuration do not provide.

## Repository sanitizer capability

The repository provides a provider-only Jolt `ReleaseASAN` BackendSpike. It is
not a sanitizer build of TerraEngine, the editor, the exported runtime, Vulkan
presentation, or managed NativeAOT assemblies. Spec 042 requires every build,
test, install, stress, and live-validation gate to use `RelWithDebInfo`
exclusively, so this differently configured provider spike is not an eligible
completion lane and was deliberately not invoked.

```powershell
pwsh -NoProfile -File Tools/Physics/BackendSpike/RunBackendSpike.ps1 `
  -Candidate Jolt56 `
  -Jolt56Source out/physics-spike/sources/JoltPhysics-5.6.0 `
  -WorkRoot out/physics-backend-spike `
  -RunId jolt56-windows-asan `
  -Configuration ReleaseASAN `
  -FixtureRepeats 3
```

The provider spike uses the repository-pinned Jolt revision
`e77f175595e64cb44218cc9d9d56fc365ad0e36a`. The command above is retained only
to make the excluded capability auditable; it is not a result from this run.

## Unsupported or excluded lanes

- No full-engine or editor ASAN configuration is currently defined for this
  Windows/MSVC project.
- No full-engine ThreadSanitizer (TSan) lane is supported on Windows/MSVC.
- Vulkan presentation, managed NativeAOT, exported-runtime startup, and the
  complete CTest lifecycle suite are not covered by the provider spike.
- A normal RelWithDebInfo build is not sanitizer evidence.

## Orchestrator results

Capability inspection was performed on 2026-07-14 with Windows 11 Home
`10.0.26200` x64, MSVC `19.51.36247`, and CMake `4.2.0`. Repository search
confirmed that `/fsanitize=address` exists only in
`Tools/Physics/BackendSpike/CMakeLists.txt` behind the `ReleaseASAN`
configuration. No ASan or TSan option exists in the main CMake presets or the
RelWithDebInfo engine/editor targets.

Result: **no sanitizer executable was run**. The only available ASan command
would violate the user's RelWithDebInfo-only mandate and would still omit the
engine lifecycle and callback collectors named by T146. Windows/MSVC provides
no full-engine TSan lane. Lifecycle and callback-drain correctness therefore
remain covered by the RelWithDebInfo deterministic stress, 100 stale-race
repetitions, 1,000 lifecycle/Play Mode cycles, zero-job shutdown checks, and
the full regression suite; these are correctness evidence, not sanitizer
evidence. Adding a supported main-project RelWithDebInfo-compatible sanitizer
lane remains future build-system work and must not be represented as passed
sanitizer coverage.
