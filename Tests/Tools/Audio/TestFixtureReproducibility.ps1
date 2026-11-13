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

# Tests run the actual generator in a private copied repository layout. Only its
# controlled generation input changes; no reference generator is reimplemented.
function New-AudioCaseRoot([string]$Name) {
	$caseRoot = Join-Path $privateRoot $Name
	$scriptPath = Join-Path $caseRoot 'Tests/Tools/Audio/GenerateAudioFixtures.ps1'
	$fixtureRoot = Join-Path $caseRoot 'Tests/Fixtures/Audio/Generated'
	[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($scriptPath)) | Out-Null
	[IO.Directory]::CreateDirectory($fixtureRoot) | Out-Null
	Copy-Item -LiteralPath (Join-Path $SourceRoot 'Tests/Tools/Audio/GenerateAudioFixtures.ps1') -Destination $scriptPath
	foreach ($file in Get-ChildItem -LiteralPath (Join-Path $SourceRoot 'Tests/Fixtures/Audio/Generated') -File) {
		Assert-Contract ($file.Length -le 1MB) 'Audio unit fixture unexpectedly exceeds its bounded copy size.'
		Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $fixtureRoot $file.Name)
	}
	return [pscustomobject]@{root=$caseRoot;script=$scriptPath;fixtures=$fixtureRoot}
}
function Assert-GeneratorRejects($Case, [string]$Reason) {
	$result = Invoke-ProductionScript $Case.script @('-VerifyCommitted')
	Assert-Contract (-not $result.timedOut) 'Generator verification exceeded its bounded child watchdog.'
	Assert-Contract ($result.exitCode -ne 0) $Reason
}
try {
	Invoke-Case 'Audio.FixtureReproducibility.UnchangedGeneratorMatchesCommitted' {
		$case = New-AudioCaseRoot 'baseline'
		$result = Invoke-ProductionScript $case.script @('-VerifyCommitted')
		Assert-Contract ($result.exitCode -eq 0 -and -not $result.timedOut) "Actual committed baseline failed: $($result.stderr)"
	}
	Invoke-Case 'Audio.FixtureReproducibility.ChangedGeneratorSampleIsRejected' {
		$case = New-AudioCaseRoot 'changed-sample'
		$text = [IO.File]::ReadAllText($case.script)
		$original = '@(0,1000,-1000,0,500,-500,0,0)'
		Assert-Contract ($text.Split($original, [StringSplitOptions]::None).Count -eq 2) 'Generator sample mutation anchor changed; fixture setup requires review.'
		[IO.File]::WriteAllText($case.script, $text.Replace($original, '@(0,1001,-1000,0,500,-500,0,0)'))
		Assert-GeneratorRejects $case 'Changed actual generator bytes were accepted without refreshing committed fixtures/manifest.'
	}
	Invoke-Case 'Audio.FixtureReproducibility.ChangedGeneratorInventoryIsRejected' {
		$case = New-AudioCaseRoot 'changed-inventory'
		$text = [IO.File]::ReadAllText($case.script)
		$anchor = '$manifest=@(); Get-ChildItem'
		Assert-Contract ($text.Split($anchor, [StringSplitOptions]::None).Count -eq 2) 'Generator inventory mutation anchor changed; fixture setup requires review.'
		$addition = 'Wav (Join-Path $tmp ''review_extra.wav'') 1 8000 1 @(17)' + [Environment]::NewLine + '    ' + $anchor
		[IO.File]::WriteAllText($case.script, $text.Replace($anchor, $addition))
		Assert-GeneratorRejects $case 'A newly generated fixture was accepted against the old committed inventory.'
	}
	Invoke-Case 'Audio.FixtureReproducibility.CommittedByteCorruptionIsRejected' {
		$case = New-AudioCaseRoot 'corrupt-committed'
		$wav = Join-Path $case.fixtures 'mono_pcm16.wav'
		$bytes = [IO.File]::ReadAllBytes($wav)
		Assert-Contract ($bytes.Length -gt 44) 'Committed WAV fixture is unexpectedly truncated.'
		$bytes[44] = $bytes[44] -bxor 1
		[IO.File]::WriteAllBytes($wav, $bytes)
		Assert-GeneratorRejects $case 'Actual verification must reject committed byte corruption.'
	}
	Invoke-Case 'Audio.FixtureReproducibility.HiddenExtraFileIsRejected' {
		# Exercise the production inventory against a real hidden filesystem entry.
		$case = New-AudioCaseRoot 'hidden-extra'
		$extra = Join-Path $case.fixtures '.unexpected.wav'
		[IO.File]::WriteAllBytes($extra, [byte[]](1,2,3))
		if($IsWindows) { [IO.File]::SetAttributes($extra, [IO.FileAttributes]::Hidden) }
		Assert-GeneratorRejects $case 'Hidden extra fixtures must participate in the exact inventory.'
	}
	$exitCode = Complete-UnitScript 'AudioFixtureReproducibility'
} finally { Remove-PrivateRoot }
exit $exitCode
