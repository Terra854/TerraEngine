# Bounded script-contract regression; prerequisites: pwsh7 and this checkout.
# All synthetic raw metrics are parser inputs, not native/GPU measurements.
[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$SourceRoot,
	[Parameter(Mandatory=$true)][string]$EvidenceDirectory,
	[ValidateSet('Parser','Producer','All')][string]$Lane='All',
	[string]$TerrainBenchmarkExecutable,
	[string]$UnboundedValidationExecutable
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

# Synthetic values below exercise only actual production parser/comparison gates.
# They are never represented as an observed renderer, hardware or benchmark run.
function New-TerrainRaw([string]$Directory, [double]$Reduction) {
	[IO.Directory]::CreateDirectory($Directory) | Out-Null
	$paths = @()
	for ($run = 0; $run -lt 5; ++$run) {
		$path = Join-Path $Directory "unit-raw-$run.json"
		$document = [ordered]@{schemaVersion=2;status='available';route='terrain-1000km-edit-route';distanceKilometres=1000;qualification='PARSER UNIT INPUT, NOT NATIVE MEASUREMENT';rawMetrics=[ordered]@{
			previewNanoseconds=@(10,20,30);mappingEditNanoseconds=@(10,20,30);geometryRebuildCounts=@(0,0,0)
			cookNanoseconds=@(10,20,30);cpuPreparationNanoseconds=@(10,20,30);uploadNanoseconds=@(10,20,30);publicationNanoseconds=@(10,20,30);collisionNanoseconds=@()
			samplerCount=1;sampledAliasCount=1;visibleTextureTouchCount=1;visibleAreaCount=1;selectedNodeCount=if($Reduction -ge 75){20}else{100};residentPageCount=1;fallbackPageCount=0;seamRepairCount=0;allocationBytes=1024;geometryReductionPercent=$Reduction}}
		Write-UnitJson $path $document
		$paths += $path
	}
	return $paths
}
function Invoke-TerrainCapture([string[]]$Paths, [string]$Directory) {
	# A tiny child entrypoint passes the real string[] parameter without relying
	# on command-line array parsing. It executes the actual production script.
	$driver = Join-Path $privateRoot ('capture-driver-' + [Guid]::NewGuid().ToString('N') + '.ps1')
	$argumentFile = "$driver.json"
	Write-UnitJson $argumentFile @{script=(Join-Path $SourceRoot 'Tests/Tools/Terrain/CaptureTerrainEvidence.ps1');paths=$Paths;output=$Directory}
	[IO.File]::WriteAllText($driver, 'param([string]$Data);$a=Get-Content -Raw -LiteralPath $Data|ConvertFrom-Json;& $a.script -BenchmarkResult ([string[]]$a.paths) -CaptureDirectory $a.output;if($LASTEXITCODE){exit $LASTEXITCODE}')
	$result = Invoke-ProductionScript $driver @('-Data',$argumentFile)
	Assert-Contract ($result.exitCode -eq 0 -and -not $result.timedOut) "Actual terrain capture parser failed on bounded unit input: $($result.stderr)"
}
function Write-TerrainUnitManifest([string]$CaptureDirectory, [string]$Path) {
	$rows = @()
	foreach ($id in @('authoring','render-culling','streaming-decode-io','collision','save-recovery')) {
		$row = Read-UnitJson (Join-Path $CaptureDirectory "$id.json")
		$row | Add-Member -NotePropertyName id -NotePropertyValue $id
		$rows += $row
	}
	Write-UnitJson $Path @{schemaVersion=1;status='incomplete-unavailable';passing=$false;qualification='PARSER UNIT INPUT ONLY';captures=$rows}
}
function Write-UnboundedUnitEvidence([string]$Path, [double]$Memory, [string]$Revision='unit-rev-a', [string]$Hardware='unit-hardware-a') {
	$rows = @(); for($index=0;$index -lt 5;++$index){$rows += @{frameTimeNanoseconds=1000;memoryBytes=$Memory;run=$index+1}}
	Write-UnitJson $Path @{schemaVersion=2;status='measured';artifactFinalized=$true;qualification='PARSER UNIT INPUT, NOT NATIVE MEASUREMENT';identity=@{workloadId='unit-workload';contentHash='unit-content';cameraPathHash='unit-camera';hardwareIdentity=$Hardware;runtimeRevision=$Revision};rawSamples=$rows}
}
function Assert-MetricProvenance($Document, $Metrics, [string]$Name) {
	if (-not (Has-UnitProperty $Metrics $Name) -or $null -eq $Metrics.$Name) {
		Assert-Contract ((Has-UnitProperty $Document 'unavailable') -and (Has-UnitProperty $Document.unavailable $Name) -and
			-not [string]::IsNullOrWhiteSpace([string]$Document.unavailable.$Name)) "Unavailable metric requires its explicit reason: $Name"
		return
	}
	# Numeric producer contract: a numeric fact must identify the actual
	# source operation, not inherit an unobserved constant's available status.
	Assert-Contract ((Has-UnitProperty $Document 'measurementProvenance') -and (Has-UnitProperty $Document.measurementProvenance $Name)) "Numeric metric has no measured provenance: $Name"
	$origin = $Document.measurementProvenance.$Name
	Assert-Contract ($origin.status -eq 'measured' -and -not [string]::IsNullOrWhiteSpace([string]$origin.sourceOperation)) "Metric provenance is not an actual identified observation: $Name"
}
try {
	if ($Lane -in @('Parser','All')) {
		Invoke-Case 'Terrain.MeasurementProvenance.ReductionParserGateCanFail' {
			foreach ($kind in @('good','bad')) {
				$paths = New-TerrainRaw (Join-Path $EvidenceDirectory "parser-$kind-raw") $(if($kind -eq 'good'){80.0}else{0.0})
				$captures = Join-Path $EvidenceDirectory "parser-$kind-captures"
				Invoke-TerrainCapture $paths $captures
				$manifest = Join-Path $EvidenceDirectory "parser-$kind-manifest.json"
				Write-TerrainUnitManifest $captures $manifest
				$result = Invoke-ProductionScript (Join-Path $SourceRoot 'Tests/Tools/Terrain/ValidateTerrainEvidenceManifest.ps1') @('-Manifest',$manifest)
				Assert-Contract (-not $result.timedOut) 'Terrain manifest parser exceeded watchdog.'
				if($kind -eq 'good'){Assert-Contract ($result.exitCode -eq 0) "Control reduction should satisfy the parser gate: $($result.stderr)"}
				else{Assert-Contract ($result.exitCode -ne 0 -and ($result.stderr + $result.stdout).Contains('75%')) 'Changing unit selected-node/reduction facts must make the actual geometry-reduction gate fail.'}
			}
		}
		Invoke-Case 'Unbounded.MeasurementProvenance.SyntheticMemoryGrowthIsRejected' {
			$baseline = Join-Path $EvidenceDirectory 'unit-baseline.json';$candidate=Join-Path $EvidenceDirectory 'unit-memory-growth.json';$comparison=Join-Path $EvidenceDirectory 'unit-memory-comparison.json'
			Write-UnboundedUnitEvidence $baseline 100
			Write-UnboundedUnitEvidence $candidate 200
			$result = Invoke-ProductionScript (Join-Path $SourceRoot 'Tests/Tools/UnboundedScenes/CompareUnboundedScenePerformance.ps1') @('-Baseline',$baseline,'-Candidate',$candidate,'-Output',$comparison)
			$document = Read-UnitJson $comparison
			Assert-Contract ($result.exitCode -eq 2 -and $document.status -eq 'regression' -and $document.medianResidentMemoryRatio -eq 2) 'The real comparator must reject synthetic fixed-resident memory growth; this is parser evidence only.'
		}
		Invoke-Case 'Unbounded.MeasurementProvenance.RevisionAndHardwareMismatchAreRejected' {
			$baseline=Join-Path $EvidenceDirectory 'unit-baseline-identity.json';Write-UnboundedUnitEvidence $baseline 100
			foreach($field in @('runtimeRevision','hardwareIdentity')) {
				$candidate=Join-Path $EvidenceDirectory "unit-mismatch-$field.json";$comparison=Join-Path $EvidenceDirectory "unit-mismatch-$field-comparison.json"
				if($field -eq 'runtimeRevision'){Write-UnboundedUnitEvidence $candidate 100 'unit-rev-b'}else{Write-UnboundedUnitEvidence $candidate 100 'unit-rev-a' 'unit-hardware-b'}
				$result=Invoke-ProductionScript (Join-Path $SourceRoot 'Tests/Tools/UnboundedScenes/CompareUnboundedScenePerformance.ps1') @('-Baseline',$baseline,'-Candidate',$candidate,'-Output',$comparison)
				Assert-Contract ($result.exitCode -ne 0 -and ($result.stderr+$result.stdout).Contains("Evidence identity mismatch: $field")) "Actual comparison must reject the changed unit identity: $field"
			}
		}
	}
	if($Lane -in @('Producer','All')) {
		if([string]::IsNullOrWhiteSpace($TerrainBenchmarkExecutable)) {
			$caseRows.Add([pscustomobject]@{name='Terrain.MeasurementProvenance.ProducerCullingIsMeasuredOrUnavailable';status='unverified-prerequisite';reason='Supply an explicit matching terrain producer executable; parser fixtures cannot establish producer/native observations.'})
		} else {
			Invoke-Case 'Terrain.MeasurementProvenance.ProducerCullingIsMeasuredOrUnavailable' {
				$fixture=Join-Path $privateRoot 'existence-only-unit-input.terrain';[IO.File]::WriteAllText($fixture,'Producer contract input only; this is not a live terrain measurement.')
				$output=Join-Path $EvidenceDirectory 'terrain-producer-contract.json'
				$result=Invoke-BoundedChild $TerrainBenchmarkExecutable @('--fixture',$fixture,'--output',$output,'--duration-minutes','0')
				Assert-Contract ($result.exitCode -eq 0 -and -not $result.timedOut) 'Actual terrain producer contract invocation failed.'
				$raw=Read-UnitJson $output
				foreach($metric in @('visibleAreaCount','selectedNodeCount','residentPageCount','distantBaselineNodeCount','distantSelectedNodeCount','geometryReductionPercent')){Assert-MetricProvenance $raw $raw.rawMetrics $metric}
			}
		}
		if([string]::IsNullOrWhiteSpace($UnboundedValidationExecutable)) {
			$caseRows.Add([pscustomobject]@{name='Unbounded.MeasurementProvenance.ProducerMemoryAndIdentityAreObserved';status='unverified-prerequisite';reason='Supply an explicit matching unbounded validator executable; no native/performance approval follows from unit data.'})
		} else {
			Invoke-Case 'Unbounded.MeasurementProvenance.ProducerMemoryAndIdentityAreObserved' {
				$output=Join-Path $EvidenceDirectory 'unbounded-producer-contract.json'
				$result=Invoke-BoundedChild $UnboundedValidationExecutable @('--scenario','diagnostics','--runs','5','--duration-seconds','0','--workloads',(Join-Path $SourceRoot 'Tests/Tools/UnboundedScenes/workloads.json'),'--output',$output)
				Assert-Contract ($result.exitCode -eq 0 -and -not $result.timedOut) 'Actual unbounded producer contract invocation failed.'
				$raw=Read-UnitJson $output;$scenario=$raw.scenarios.diagnostics
				foreach($sample in $scenario.rawSamples){Assert-MetricProvenance $raw $sample 'memoryBytes'}
				foreach($field in @('hardwareIdentity','runtimeRevision')) {
					$value=if(Has-UnitProperty $scenario.identity $field){[string]$scenario.identity.$field}else{''}
					if([string]::IsNullOrWhiteSpace($value)){Assert-Contract ((Has-UnitProperty $raw 'unavailable') -and (Has-UnitProperty $raw.unavailable $field)) "Unavailable identity requires a reason: $field"}
					else{Assert-Contract ($value -notin @('cpu-deterministic','unbounded-scenes-v1')) "Actual producer retained a fixed identity placeholder: $field"}
				}
				Assert-Contract (-not (Has-UnitProperty $raw 'buildIdentity') -or $raw.buildIdentity -ne 'unbounded-scenes-v1') 'The actual producer build identity must not be a fixed revision placeholder.'
			}
		}
	}
	$exitCode=Complete-UnitScript 'BenchmarkMeasurementProvenance'
} finally { Remove-PrivateRoot }
exit $exitCode
