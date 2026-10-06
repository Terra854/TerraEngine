# Managed navigation-agent contract probe

This probe is an offline, CLR-only check for the additive NavigationAgent v1
managed/native contract. It does not start the engine, load a native library,
exercise physics, or prove that a managed gameplay callback is scheduled.

## Prerequisites

Build the engine and install the managed assembly first. Use the repository
bundled SDK, not a `dotnet` executable from `PATH`:

```powershell
$dotnet = "External/dotnet/sdk/win-x64/10.0.303/dotnet.exe"
$assembly = (Resolve-Path "out/build/RelWithDebInfo/Build/Binaries/Managed/RelWithDebInfo/TerraEngine.CSharp.dll").Path
$probeOut = "out/validation/managed-agent-contract"
$probeIntermediate = Join-Path (Get-Location) "out/validation/managed-agent-contract-obj/"
$env:DOTNET_ROOT = (Resolve-Path "External/dotnet/sdk/win-x64/10.0.303").Path
$env:DOTNET_MULTILEVEL_LOOKUP = "0"
$env:DOTNET_CLI_HOME = Join-Path (Get-Location) "out/build/RelWithDebInfo/DotNetCliHome"
$env:DOTNET_ADD_GLOBAL_TOOLS_TO_PATH = "0" # Keep disposable CLI homes out of the user PATH.
$env:NUGET_PACKAGES = Join-Path (Get-Location) "out/build/RelWithDebInfo/NuGetPackages"
New-Item -ItemType Directory -Force $probeOut | Out-Null
```

The probe project has no package dependency. The build below performs restore
using the repository's offline NuGet configuration and keeps intermediate files
under `out/`.

## Build and run

```powershell
& $dotnet build Tests/Tools/Navigation/ManagedAgentContractProbe.csproj `
  --configfile NuGet.Config -c RelWithDebInfo `
  -p:TerraEngineAssemblyPath=$assembly `
  -p:BaseIntermediateOutputPath=$probeIntermediate `
  -o $probeOut
if ($LASTEXITCODE -ne 0) { throw "Managed contract probe compilation failed." }

& $dotnet $probeOut/ManagedAgentContractProbe.dll
if ($LASTEXITCODE -ne 0) { throw "Managed contract probe failed." }
```

The successful run reports the ten ABI structure sizes and eight fixed header/
offset checks (18 checks total), followed by the fixture compilation notice.
The expected sizes are 64, 16, 80, 176, 104, 72, 48, 72, 304, and 80 bytes
for the identity and v1 resolve/identity, command, receipt, status, and
guidance packets/queries respectively.

## Interpretation

A pass proves only that the referenced managed assembly exposes the expected
types with the expected CLR packing and that both the agent live fixture and
managed outcome fixture compile. It does
not prove native export availability, ABI behavior at runtime, physics
integration, navigation coverage, receipt completion, or callback timing.
