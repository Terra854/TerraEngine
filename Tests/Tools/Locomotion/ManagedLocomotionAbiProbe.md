# Managed locomotion ABI layout probe

This offline probe loads the built `TerraEngine.CSharp` assembly and checks the
CLR's x64 layout for the copied locomotion ABI v1 records. It does not load the
native engine, start Physics, invoke a gameplay callback, or prove runtime ABI
behavior.

Build the engine/managed assembly first, then use the repository-bundled .NET
SDK (not `dotnet` from `PATH`). Run from the repository root:

```powershell
$dotnet = "External/dotnet/sdk/win-x64/10.0.303/dotnet.exe"
$assembly = (Resolve-Path "out/build/RelWithDebInfo/Build/Binaries/Managed/RelWithDebInfo/TerraEngine.CSharp.dll").Path
$probeOut = "out/validation/managed-locomotion-abi"
$probeIntermediate = Join-Path (Get-Location) "out/validation/managed-locomotion-abi-obj/"
$env:DOTNET_ROOT = (Resolve-Path "External/dotnet/sdk/win-x64/10.0.303").Path
$env:DOTNET_MULTILEVEL_LOOKUP = "0"
$env:DOTNET_CLI_HOME = Join-Path (Get-Location) "out/build/RelWithDebInfo/DotNetCliHome"
$env:DOTNET_ADD_GLOBAL_TOOLS_TO_PATH = "0" # Keep disposable CLI homes out of the user PATH.
$env:NUGET_PACKAGES = Join-Path (Get-Location) "out/build/RelWithDebInfo/NuGetPackages"
New-Item -ItemType Directory -Force $probeOut | Out-Null

& $dotnet build Tests/Tools/Locomotion/ManagedLocomotionAbiProbe.csproj `
  --configfile NuGet.Config -c RelWithDebInfo `
  -p:TerraEngineAssemblyPath=$assembly `
  -p:BaseIntermediateOutputPath=$probeIntermediate `
  -o $probeOut
if ($LASTEXITCODE -ne 0) { throw "Managed locomotion ABI probe compilation failed." }

& $dotnet $probeOut/ManagedLocomotionAbiProbe.dll
if ($LASTEXITCODE -ne 0) { throw "Managed locomotion ABI layout probe failed." }
```

A successful run verifies 11 record strides, 22 member offsets, 7 wire enum
values, and 3 reflection-only cache behavior checks. The cache checks invoke a
private managed method directly and do not represent native or engine runtime
acceptance. Keep the probe output and intermediate directories under `out/`;
the project has no package dependencies and does not modify engine source.
