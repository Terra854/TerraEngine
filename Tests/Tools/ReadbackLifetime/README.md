# Installed readback lifetime validation

Configure with `cmake --preset RelWithDebInfo -DBUILD_TESTING=ON`, then build and
install with `cmake --build --preset RelWithDebInfo --target install --parallel 2`
from a matching Visual Studio developer shell. Dependencies and PNG decoding use
the committed copies. Native acceptance additionally requires a working Vulkan
driver/desktop and the host diagnostic `VK_LAYER_KHRONOS_validation` layer; this
layer is an explicit diagnostic prerequisite, not a newly linked engine dependency.
The native gate also requires timeline semaphore support. Test builds enable that
optional feature only when validation is active; unsupported hosts report incomplete.
The real copy waits for value 1 at TRANSFER on a semaphore initially at 0. Only an
actual host signal to 1 releases it after the incomplete-fence and timeout checks.
Each frame renders the smaller dedicated capture first and the normal runtime
output second, exercising separate AO history ownership and valid presentation.

Invoke the installed runner with PowerShell 7 and a nonexistent work directory:

```powershell
& out/build/RelWithDebInfo/Build/Tests/Tools/ReadbackLifetime/RunInstalledReadbackLifetime.ps1 `
	-InstallRoot "$PWD/out/build/RelWithDebInfo/Build" `
	-WorkRoot "$PWD/out/validation/readback-fresh" -Mode All -Iterations 100
```

The native lane records 100 operations per timeout, resize and orderly-shutdown
scenario (300 total), plus a separately labelled C++ allocation-fault qualification.
The editor lane warms up, captures 100 changed-size images in one read-only owned
session, independently decodes the transmitted bytes, then closes normally.
Exit 0 means all requested lanes passed, 1 means failure, and 2 means an incomplete
prerequisite. Failed, killed, skipped or simulated work cannot establish acceptance.

`Engine.ReadbackLifetime` exercises the shared production controller and renderer
charge accounting. `Runtime.ReadbackLifetime.ProtocolContracts` checks synthetic
receipt rejection only. `TestReadbackLifetimeSensitivity.ps1` compiles three isolated
source copies with MSVC; see its usage header for compiler and fresh-work-root args.
Neither deterministic policy tests nor source wiring checks prove actual GPU timing.
