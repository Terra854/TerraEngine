# Phase 1 Setup Validation

**Date**: 2026-07-13  
**Result**: PASS  
**Provider**: Jolt Physics v5.6.0, commit `e77f175595e64cb44218cc9d9d56fc365ad0e36a`

## Scope

This gate verifies the permanent, pristine Jolt 5.6 submodule; the production
core-only CMake profile; provider-neutral public headers; the absence of a
physics-owned worker pool and shader surface; the canonical Debug compile; and
the required runtime install tree.

## Revision and source integrity

The parent repository and provider checkout reported the same approved commit:

```powershell
git ls-files --stage -- External/JoltPhysics
git -C External/JoltPhysics rev-parse HEAD
git -C External/JoltPhysics status --short
```

Result:

```text
160000 e77f175595e64cb44218cc9d9d56fc365ad0e36a 0 External/JoltPhysics
e77f175595e64cb44218cc9d9d56fc365ad0e36a
<clean>
```

The dependency contract repeats these checks at test time and also rejects
untracked provider files, forbidden network-fetch primitives, public Jolt type
exposure, direct physics-owned thread construction, physics shader additions,
and leaked Jolt cache options.

## Configure and compile

The canonical Ninja preset was configured successfully:

```powershell
cmake --preset Debug
```

Result: PASS. Build files were generated in `out/build/Debug`.

The first build invocation from a plain desktop PowerShell failed before
project compilation because that shell did not contain the Visual Studio SDK
and standard-library include environment. The same command was rerun in the
installed x64 Visual Studio developer shell:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1' `
	-Arch amd64 -HostArch amd64 -SkipAutomaticLocation
cmake --build --preset Debug --target TerraEngine-EngineTests --parallel 16
```

Result: PASS. Jolt built as `External/JoltPhysics/Joltd.lib`,
`TerraEngine.dll` linked successfully, and `TerraEngine-EngineTests.exe` linked
successfully. The environment-only failed attempt made no source changes.

## Focused dependency contract

```powershell
ctest --test-dir out/build/Debug `
	-R "^Engine\.PhysicsDependencyContract$" `
	--output-on-failure
```

Result: PASS, 1/1 tests, 0 failures. The final post-install execution completed
in 0.29 seconds.

The generated cache was also searched for the generic temporary entries
`ENABLE_INSTALL`, `ENABLE_ALL_WARNINGS`, `OVERRIDE_CXX_FLAGS`,
`TARGET_UNIT_TESTS`, `TARGET_SAMPLES`, and `TARGET_VIEWER`. None remained after
Jolt target configuration.

## Required install

The complete required install target ran in the same x64 developer shell:

```powershell
cmake --build --preset Debug --target install --parallel 16
```

Result: PASS. Native engine, editor, launcher, export, test, managed, compiler,
and shader build steps completed, followed by the canonical runtime install to
`out/build/Debug/Build`.

Existing Roslyn analyzer release-tracking warnings `RS2008` were emitted by
`TerraEngine.Export.Generator`; they are unrelated to the Jolt integration and
did not fail the build.

## Installed-tree audit

The canonical notice exists at:

```text
out/build/Debug/Build/ThirdPartyNotices/JoltPhysics-LICENSE.txt
```

Its SHA-256 is:

```text
8D1C7E0303333DFDAFCC6583C620EB163E9C6C53C7CEBCB53DB78EAA9807E76C
```

A recursive audit found no other Jolt-named file or Jolt directory in
`out/build/Debug/Build`, and no Jolt upstream artifact in
`out/install/Debug`. Therefore Jolt headers, static libraries, DLLs, PDBs,
samples, tests, viewer, and standalone tools are absent from the delivered
runtime while the required MIT notice is present.

## Gate conclusion

T001 through T008 pass. The exact Jolt 5.6 provider is measured, approved,
pinned, pristine, offline, privately linked, licensed, compiled, tested, and
installed without provider-facing runtime leakage. Foundational runtime work
may begin.
