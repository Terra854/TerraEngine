<#
.SYNOPSIS
Runs CMake arguments from a Visual Studio x64 developer environment with a complete Windows SDK.

.DESCRIPTION
Some Visual Studio 18 installations have VsDevCmd omit Windows SDK and UCRT paths. Its
`-winsdk` selector can also report an installed SDK as incomplete, so this helper imports
the compiler environment first, then validates and appends one installed SDK explicitly.
It installs no software and restores the caller's environment after CMake exits.

.EXAMPLE
pwsh -File Tests/Tools/Locomotion/InvokeLocomotionBuild.ps1 --version

.EXAMPLE
pwsh -File Tests/Tools/Locomotion/InvokeLocomotionBuild.ps1 --build --preset RelWithDebInfo --target TerraEngine-EngineTests -j 4
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true, Position = 0, ValueFromRemainingArguments = $true)]
	[string[]]$CMakeArguments,

	[string]$DevCmdPath,
	[string]$SdkVersion
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-DevCommandPath {
	param([string]$RequestedPath)

	if(-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
		$resolved = Resolve-Path -LiteralPath $RequestedPath -ErrorAction Stop
		return $resolved.Path
	}

	if(-not [string]::IsNullOrWhiteSpace($env:VSINSTALLDIR)) {
		$candidate = Join-Path $env:VSINSTALLDIR 'Common7\Tools\VsDevCmd.bat'
		if(Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
	}

	# Prefer a simple known location before asking the Visual Studio locator utility.
	$knownCandidate = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat'
	if(Test-Path -LiteralPath $knownCandidate -PathType Leaf) { return $knownCandidate }

	$vswhereCandidates = @(
		${env:ProgramFiles(x86)} + '\Microsoft Visual Studio\Installer\vswhere.exe',
		$env:ProgramFiles + '\Microsoft Visual Studio\Installer\vswhere.exe'
	) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) -and (Test-Path -LiteralPath $_ -PathType Leaf) }
	foreach($vswhere in $vswhereCandidates) {
		$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
		if($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace(($installation -join ''))) {
			$candidate = Join-Path ($installation -join '').Trim() 'Common7\Tools\VsDevCmd.bat'
			if(Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
		}
	}
	throw 'Could not find VsDevCmd.bat. Pass -DevCmdPath with the installed Visual Studio developer command script.'
}

function Resolve-WindowsSdkRoot {
	$root = $null
	try {
		$root = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -Name KitsRoot10 -ErrorAction Stop).KitsRoot10
	} catch {
		$root = 'C:\Program Files (x86)\Windows Kits\10\'
	}
	if(-not (Test-Path -LiteralPath $root -PathType Container)) {
		throw "Windows 10 SDK root was not found: $root"
	}
	return (Resolve-Path -LiteralPath $root).Path
}

function Test-WindowsSdkVersion {
	param([string]$Root, [string]$Version)

	$includeRoot = Join-Path $Root "Include\$Version"
	$libraryRoot = Join-Path $Root "Lib\$Version"
	$requiredPaths = @(
		(Join-Path $includeRoot 'ucrt\stddef.h'),
		(Join-Path $includeRoot 'ucrt\crtdbg.h'),
		(Join-Path $includeRoot 'shared\basetsd.h'),
		(Join-Path $includeRoot 'um\Windows.h'),
		(Join-Path $libraryRoot 'ucrt\x64\ucrt.lib'),
		(Join-Path $libraryRoot 'um\x64\kernel32.lib')
	)
	if(@($requiredPaths | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) }).Count -ne 0) { return $false }
	$versionedBin = Join-Path $Root "bin\$Version\x64"
	$sharedBin = Join-Path $Root 'bin\x64'
	return (Test-Path -LiteralPath $versionedBin -PathType Container) -or (Test-Path -LiteralPath $sharedBin -PathType Container)
}

function Resolve-WindowsSdkVersion {
	param([string]$Root, [string]$RequestedVersion)

	if(-not [string]::IsNullOrWhiteSpace($RequestedVersion)) {
		if(-not (Test-WindowsSdkVersion -Root $Root -Version $RequestedVersion)) {
			throw "Requested Windows SDK $RequestedVersion is incomplete under $Root. Required UCRT/UM headers, x64 libraries, and SDK bin directory must all exist."
		}
		return $RequestedVersion
	}

	$includeRoot = Join-Path $Root 'Include'
	$versions = Get-ChildItem -LiteralPath $includeRoot -Directory | Where-Object {
		$_.Name -match '^\d+\.\d+\.\d+\.\d+$'
	} | Sort-Object { [version]$_.Name } -Descending
	foreach($versionDirectory in $versions) {
		if(Test-WindowsSdkVersion -Root $Root -Version $versionDirectory.Name) { return $versionDirectory.Name }
	}
	throw "No complete x64 Windows SDK was found under $Root."
}

function Add-EnvironmentPath {
	param([string]$Name, [string[]]$Entries)

	$existing = [Environment]::GetEnvironmentVariable($Name, 'Process')
	$allEntries = @()
	if(-not [string]::IsNullOrWhiteSpace($existing)) { $allEntries += $existing -split ';' }
	$allEntries += $Entries
	$uniqueEntries = [System.Collections.Generic.List[string]]::new()
	$seenEntries = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
	foreach($entry in $allEntries) {
		if(-not [string]::IsNullOrWhiteSpace($entry) -and $seenEntries.Add($entry.TrimEnd('\'))) {
			$uniqueEntries.Add($entry.TrimEnd('\'))
		}
	}
	[Environment]::SetEnvironmentVariable($Name, ($uniqueEntries -join ';'), 'Process')
}

if($CMakeArguments.Count -eq 0) {
	throw 'Pass at least one CMake argument, such as --version or --build --preset RelWithDebInfo.'
}

$devCommand = Resolve-DevCommandPath -RequestedPath $DevCmdPath
$sdkRoot = Resolve-WindowsSdkRoot
$sdkVersionResolved = Resolve-WindowsSdkVersion -Root $sdkRoot -RequestedVersion $SdkVersion
$includeVersionRoot = Join-Path $sdkRoot "Include\$sdkVersionResolved"
$libraryVersionRoot = Join-Path $sdkRoot "Lib\$sdkVersionResolved"
$sdkBin = Join-Path $sdkRoot "bin\$sdkVersionResolved\x64"
if(-not (Test-Path -LiteralPath $sdkBin -PathType Container)) { $sdkBin = Join-Path $sdkRoot 'bin\x64' }

# Save the caller's environment so this wrapper is safe when dot-sourced or invoked by a larger script.
$savedEnvironment = @{}
Get-ChildItem Env: | ForEach-Object { $savedEnvironment[$_.Name] = $_.Value }
$originalNames = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach($name in $savedEnvironment.Keys) { [void]$originalNames.Add($name) }

try {
	# VsDevCmd emits the complete developer environment as KEY=VALUE records.
	$quotedDevCommand = $devCommand.Replace('"', '""')
	$commandText = "call `"$quotedDevCommand`" -arch=x64 -host_arch=x64 && set"
	$devEnvironmentOutput = & $env:ComSpec /d /s /c $commandText
	$devCommandExitCode = $LASTEXITCODE
	if($devCommandExitCode -ne 0) {
		throw "VsDevCmd.bat failed with exit code $devCommandExitCode."
	}
	foreach($line in $devEnvironmentOutput) {
		$separator = $line.IndexOf('=')
		if($separator -le 0) { continue }
		$name = $line.Substring(0, $separator)
		$value = $line.Substring($separator + 1)
		[Environment]::SetEnvironmentVariable($name, $value, 'Process')
	}

	$env:WindowsSdkDir = $sdkRoot.TrimEnd('\') + '\'
	$env:WindowsSDKVersion = $sdkVersionResolved.TrimEnd('\') + '\'
	$env:UniversalCRTSdkDir = $env:WindowsSdkDir
	$env:UCRTVersion = $sdkVersionResolved
	Add-EnvironmentPath -Name 'INCLUDE' -Entries @(
		(Join-Path $includeVersionRoot 'ucrt'),
		(Join-Path $includeVersionRoot 'shared'),
		(Join-Path $includeVersionRoot 'um'),
		(Join-Path $includeVersionRoot 'winrt'),
		(Join-Path $includeVersionRoot 'cppwinrt')
	)
	Add-EnvironmentPath -Name 'LIB' -Entries @(
		(Join-Path $libraryVersionRoot 'ucrt\x64'),
		(Join-Path $libraryVersionRoot 'um\x64')
	)
	Add-EnvironmentPath -Name 'PATH' -Entries @($sdkBin)

	Write-Host "Using Visual Studio developer command: $devCommand"
	Write-Host "Using Windows SDK: $sdkVersionResolved ($sdkRoot)"
	& cmake @CMakeArguments
	$cmakeExitCode = $LASTEXITCODE
	exit $cmakeExitCode
} finally {
	# Restore the process state even when setup or CMake fails.
	foreach($name in @((Get-ChildItem Env: | ForEach-Object Name))) {
		if(-not $originalNames.Contains($name)) {
			[Environment]::SetEnvironmentVariable($name, $null, 'Process')
		}
	}
	foreach($name in $savedEnvironment.Keys) {
		[Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
	}
}
