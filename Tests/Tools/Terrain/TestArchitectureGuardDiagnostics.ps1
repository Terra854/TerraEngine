# Bounded script-contract regression; prerequisites: pwsh7 and this checkout.
# All synthetic raw metrics are parser inputs, not native/GPU measurements.
[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$SourceRoot,
	[Parameter(Mandatory=$true)][string]$EvidenceDirectory
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$EvidenceDirectory = [IO.Path]::GetFullPath($EvidenceDirectory)
[IO.Directory]::CreateDirectory($EvidenceDirectory) | Out-Null
$caseRows = [Collections.Generic.List[object]]::new()
$childRows = [Collections.Generic.List[object]]::new()
$privateOwner = [Guid]::NewGuid().ToString('N')
$privateParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([IO.Path]::DirectorySeparatorChar)
$privateRoot = Join-Path $privateParent ("TerraEngine-ReviewScript-$privateOwner")
[IO.Directory]::CreateDirectory($privateRoot) | Out-Null
[IO.File]::WriteAllText((Join-Path $privateRoot '.review-owner'), $privateOwner)

function Assert-Contract([bool]$Condition, [string]$Message) {
	if (-not $Condition) { throw $Message }
}
function Invoke-Case([string]$Name, [scriptblock]$Body) {
	try {
		& $Body | Out-Null
		$caseRows.Add([pscustomobject]@{name=$Name;status='passed-unit-contract';reason='Actual production script/producer contract executed; no performance acceptance claimed.'})
	} catch {
		$caseRows.Add([pscustomobject]@{name=$Name;status='failed-contract';reason=$_.Exception.Message})
	}
}
function Invoke-ProductionScript([string]$Script, [string[]]$ScriptArguments = @()) {
	$pwsh = (Get-Command pwsh -ErrorAction Stop).Source
	return Invoke-BoundedChild $pwsh (@('-NoLogo','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',$Script) + $ScriptArguments)
}
function Invoke-BoundedChild([string]$Executable, [string[]]$ChildArguments) {
	$startInfo = [Diagnostics.ProcessStartInfo]::new()
	$startInfo.FileName = $Executable
	$startInfo.UseShellExecute = $false
	$startInfo.CreateNoWindow = $true
	$startInfo.RedirectStandardOutput = $true
	$startInfo.RedirectStandardError = $true
	foreach ($argument in $ChildArguments) { $startInfo.ArgumentList.Add($argument) }
	$child = [Diagnostics.Process]::new()
	$child.StartInfo = $startInfo
	try {
		[void]$child.Start()
		$stdout = $child.StandardOutput.ReadToEndAsync()
		$stderr = $child.StandardError.ReadToEndAsync()
		$timedOut = -not $child.WaitForExit(30000)
		if ($timedOut) { $child.Kill($true); [void]$child.WaitForExit(5000) }
		$outText = if ($stdout.Wait(5000)) { $stdout.Result } else { '<stdout incomplete>' }
		$errText = if ($stderr.Wait(5000)) { $stderr.Result } else { '<stderr incomplete>' }
		$result = [pscustomobject]@{exitCode=if($timedOut){124}else{$child.ExitCode};stdout=$outText;stderr=$errText;timedOut=$timedOut}
		$childRows.Add([pscustomobject]@{executable=$Executable;arguments=$ChildArguments;result=$result})
		return $result
	} finally { $child.Dispose() }
}
function Write-UnitJson([string]$Path, $Value) {
	[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path)) | Out-Null
	[IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 24), [Text.UTF8Encoding]::new($false))
}
function Read-UnitJson([string]$Path) { Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json }
function Has-UnitProperty($Value, [string]$Name) { return $null -ne $Value -and $null -ne $Value.PSObject.Properties[$Name] }
function Complete-UnitScript([string]$Suite) {
	$failed = @($caseRows | Where-Object status -eq 'failed-contract').Count
	$unverified = @($caseRows | Where-Object status -eq 'unverified-prerequisite').Count
	$result = [ordered]@{suite=$Suite;qualification='parser/script-unit-inputs; not native GPU/performance measurement';selectedCases=$caseRows.Count;failedCases=$failed;unverifiedCases=$unverified;cases=$caseRows.ToArray();executions=$childRows.ToArray()}
	Write-UnitJson (Join-Path $EvidenceDirectory 'result.json') $result
	[Console]::Out.WriteLine(($result | ConvertTo-Json -Depth 12))
	if ($failed -gt 0) { return 1 }
	if ($unverified -gt 0) { return 2 }
	return 0
}
function Remove-PrivateRoot {
	# Verify the exact exclusively owned absolute target immediately before removal.
	$resolved = (Resolve-Path -LiteralPath $privateRoot).Path.TrimEnd([IO.Path]::DirectorySeparatorChar)
	$parent = [IO.Directory]::GetParent($resolved).FullName.TrimEnd([IO.Path]::DirectorySeparatorChar)
	if ($parent -ne $privateParent -or [IO.File]::ReadAllText((Join-Path $resolved '.review-owner')) -ne $privateOwner) {
		throw 'Refusing cleanup outside the exclusively owned temporary root.'
	}
	Remove-Item -LiteralPath $resolved -Recurse -Force
}

# Execute the real complete guard, with deliberately bounded missing-source and
# embedded-shader inputs. This checks its diagnostics, not runtime architecture.
function New-GuardRoot([string]$Name) {
	$caseRoot = Join-Path $privateRoot $Name
	foreach ($relative in @('Engine/Terrain','Engine/Renderer/Terrain','Engine/Renderer/RHI/Vulkan','Engine/Physics/Terrain','Editor/TerraEngine-Editor/Terrain')) {
		[IO.Directory]::CreateDirectory((Join-Path $caseRoot $relative)) | Out-Null
	}
	return $caseRoot
}
function Invoke-Guard([string]$Root) {
	$result = Invoke-ProductionScript (Join-Path $SourceRoot 'Tests/Tools/Terrain/VerifyTerrainArchitecture.ps1') @('-SourceRoot',$Root)
	Assert-Contract (-not $result.timedOut) 'Actual architecture guard exceeded its watchdog.'
	Assert-Contract ($result.exitCode -eq 1) "Expected the intended guard failure exit1, got $($result.exitCode): $($result.stderr)"
	try { $document = $result.stdout | ConvertFrom-Json } catch { throw "Actual guard did not emit its intended JSON: $($result.stderr)" }
	Assert-Contract ($null -ne $document -and (Has-UnitProperty $document 'status')) "Actual guard emitted no structured failure document: $($result.stderr)"
	Assert-Contract ($document.status -eq 'failed' -and $document.runtimeProof -eq 'not-provided-by-static-check') 'Static guard must retain truthful failed/runtime-proof classification.'
	return $document
}
function Write-EmbeddedInput([string]$Root, [string]$Relative) {
	# Intentionally malformed architecture fixture in a private validation tree.
	# It is data for the guard; no real engine/editor source is changed or built.
	[IO.File]::WriteAllText((Join-Path $Root $Relative), 'const char* injected = R"review(Texture2D<float4> reviewInput;)review";')
}
try {
	Invoke-Case 'Terrain.ArchitectureGuard.MissingSourcesProduceIntendedJson' {
		$root = New-GuardRoot 'missing-source-control'
		$document = Invoke-Guard $root
		Assert-Contract (($document.failures -join "`n").Contains('Missing source:')) 'Actual guard must retain actionable missing-source diagnostics.'
	}
	Invoke-Case 'Terrain.ArchitectureGuard.EmbeddedShaderReportsOffenderPath' {
		$root = New-GuardRoot 'single-offender'
		$relative = 'Engine/Terrain/ReviewInjected.cpp'
		Write-EmbeddedInput $root $relative
		$document = Invoke-Guard $root
		Assert-Contract (($document.failures -join "`n").Contains($relative)) 'Actual embedded-shader diagnostic must report the offending path.'
	}
	Invoke-Case 'Terrain.ArchitectureGuard.MultipleOffendersRemainStructured' {
		$root = New-GuardRoot 'multiple-offenders'
		$paths = @('Engine/Terrain/ReviewFirst.cpp','Engine/Renderer/RHI/Vulkan/ReviewSecond.hpp')
		foreach ($relative in $paths) { Write-EmbeddedInput $root $relative }
		$document = Invoke-Guard $root
		foreach ($relative in $paths) { Assert-Contract (($document.failures -join "`n").Contains($relative)) "Actual guard omitted offender: $relative" }
	}
	$exitCode = Complete-UnitScript 'TerrainArchitectureGuardDiagnostics'
} finally { Remove-PrivateRoot }
exit $exitCode
