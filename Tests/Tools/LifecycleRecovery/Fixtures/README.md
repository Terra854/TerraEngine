# Lifecycle recovery acceptance inputs

Build and install with `BUILD_TESTING=ON`, then invoke the installed
`RunInstalledLifecycleRecoveryAcceptance.ps1 -InstallRoot <Build> -AllowLiveAcceptance`.
Use a fresh work directory outside `<Build>`; export output and runtime inputs
must be separate. Existing evidence is never reused or removed.

Native fixtures are created by the named production-path tests: 1,000 disk
changes, 100 gated scene/prefab interleavings, 100 controlled imports and 1,000
controlled exports. Nested-work cases must enter the actual animation/Vulkan
groups. The wrapper enforces every named case floor and retains watchdog logs.

The installed Correctness managed project and ExportRuntimeBaseline/PhysicsExportRuntime
authored fixtures supply live Save As, managed callback, real NativeAOT export,
package/readiness, and normal shutdown checks. Both installed default editor
layout files are copied into each disposable editor project. The normal editor
approval UI remains required for protected live actions.

All fixture inputs, runners, tools and matching binaries must appear in
`correctness-artifacts.json`. Missing artifacts, source runner invocation,
failed readiness, zero cases, denied/missing approval or forced process cleanup
are failures; they cannot produce a passing summary.
