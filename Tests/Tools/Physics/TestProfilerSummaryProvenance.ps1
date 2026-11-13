# Bounded protocol regression only. Fixture numbers are NOT native performance evidence.
[CmdletBinding()]
param(
	[string]$RepoRoot = (Join-Path $PSScriptRoot '../../..'),
	[string]$WorkRoot,
	[string]$StressProducer,
	[string]$NavigationExecutable,
	[string]$NavigationManifest
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
if([string]::IsNullOrWhiteSpace($WorkRoot)) { $WorkRoot = Join-Path $script:RepoRoot ('out/validation/evidence-contract-fixtures/' + [guid]::NewGuid().ToString('N')) }
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)
$prefix = (Join-Path $script:RepoRoot 'out').TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
if(-not $WorkRoot.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture work root must remain below repository out/.' }
[IO.Directory]::CreateDirectory($WorkRoot) | Out-Null
$script:Utf8NoBom = [Text.UTF8Encoding]::new($false)
$script:Cases = [Collections.Generic.List[object]]::new()
$script:FunctionProvenance = [Collections.Generic.List[object]]::new()
function Get-ActualProducerFunctions {
	param([string]$Path, [string[]]$Names)
	$tokens = $null; $errors = $null
	$ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
	if(@($errors).Count) { throw "Producer parse failed: $($errors[0].Message)" }
	$definitions = @()
	foreach($name in $Names) {
		$nodes = @($ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name}, $true))
		if($nodes.Count -ne 1) { throw "Required production callable seam '$name' was not found exactly once in '$Path'." }
		$definitions += $nodes[0].Extent.Text
		$sha=[Security.Cryptography.SHA256]::Create()
		try {$astHash=([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($nodes[0].Extent.Text)))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
		$script:FunctionProvenance.Add([ordered]@{ source=$Path; sourceSha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash; function=$name; astSha256=$astHash; startLine=$nodes[0].Extent.StartLineNumber; endLine=$nodes[0].Extent.EndLineNumber })
	}
	# Execute exact AST extents, with no source replacement or copied calculation.
	# Dot-source this returned block in caller scope; never dot-source the monolith.
	return [scriptblock]::Create(($definitions -join "`n"))
}
function Assert-Protocol([bool]$Condition,[string]$Message) { if(-not $Condition) { throw $Message } }
function Invoke-ProtocolCase([string]$Name,[scriptblock]$Body) {
	try { & $Body; $script:Cases.Add([ordered]@{name=$Name;status='passed'}); Write-Output "[PASS] $Name" }
	catch { $script:Cases.Add([ordered]@{name=$Name;status='failed';error=$_.Exception.Message}); Write-Output "[FAIL] $Name : $($_.Exception.Message)" }
}
function Complete-ProtocolTests {
	$failures = @($script:Cases | Where-Object {$_.status -ne 'passed'}).Count
	[IO.File]::WriteAllText((Join-Path $WorkRoot 'result.json'), ([ordered]@{fixtureProtocolOnly=$true;nativePerformanceMeasured=$false;selected=$script:Cases.Count;failures=$failures;cases=@($script:Cases.ToArray());functionProvenance=@($script:FunctionProvenance.ToArray())}|ConvertTo-Json -Depth 32),$script:Utf8NoBom)
	Write-Output "$($script:Cases.Count) selected protocol cases, $failures failures; native performance NOT measured."
	if($failures) { exit 1 }
}

$animation = Join-Path $script:RepoRoot 'Tests/Tools/Animation/CaptureAnimationPerformance.ps1'
if([string]::IsNullOrWhiteSpace($StressProducer)) { $StressProducer=Join-Path $script:RepoRoot 'Tests/Tools/Physics/RunInstalledEditorPhysicsStress.ps1' }
. (Get-ActualProducerFunctions $animation @('Assert-ChildPath','Get-RelativeRepoPath','Write-Json','Read-Json','Get-RequiredProperty','Get-RequiredFiniteNumber','Get-Percentile','New-Metric','Convert-PhysicsRun'))
$timing = [ordered]@{ frameMedianMs=11;frameP95Ms=19;frameP99Ms=29;frameWorstMs=41;timingStreams=@(
	[ordered]@{id='engine_cpu';meanMs=7},[ordered]@{id='viewport_cpu';p95Ms=13},[ordered]@{id='viewport_gpu';p95Ms=17}) }
$raw = [ordered]@{start=[ordered]@{request='raw-start'};stop=[ordered]@{jobId='raw-job'};job=[ordered]@{state='succeeded'};artifact=[ordered]@{name='fixture.teprof';artifactId='fixture-artifact'};summary=[ordered]@{data=[ordered]@{summary=$timing}};succeeded=$true}
$evidence = [ordered]@{artifactBytes=1234;durationGatePassed=$true;sizeGatePassed=$true;metricAvailability=[ordered]@{gatePassed=$true};formatIdentifier='TerraEngineProfilerJournalV1';fixtureOnly=$true}
Invoke-ProtocolCase 'Profiler.NormalizationConsumesActualRawAndEvidenceSchema' {
	$root=Join-Path $WorkRoot 'normalize';$dest=Join-Path $root 'normalized'
	[IO.Directory]::CreateDirectory((Join-Path $root 'project/Profiler'))|Out-Null
	# A marker tests artifact binding/path provenance only, not binary .teprof decoding.
	[IO.File]::WriteAllText((Join-Path $root 'project/Profiler/fixture.teprof'),'fixture marker, not a native capture',$script:Utf8NoBom)
	$summary=[ordered]@{success=$true;forcedTermination=$false;explicitStepPublication=@([ordered]@{durationMs=2},[ordered]@{durationMs=5});processMemorySamples=@([ordered]@{workingSetBytes=100;privateBytes=80});profilerCapture=$raw;profilerEvidence=$evidence;determinismHash='fixture-protocol';monotonicDurationMs=100}
	Write-Json (Join-Path $root 'evidence/summary.json') $summary
	$path=Convert-PhysicsRun -WorkRoot $root -Destination $dest -RunKind fixture -RunIndex 1 -Control ([pscustomobject]@{id='fixture';settingsHash='fixture'})
	$normalized=Read-Json $path
	Assert-Protocol ($normalized.metrics.frameP95Ms.value -eq 19 -and $normalized.metrics.frameP99Ms.value -eq 29 -and $normalized.metrics.frameWorstMs.value -eq 41) 'Actual normalizer must retain distinct raw percentiles.'
	Assert-Protocol ($normalized.facts.profilerMetricGatePassed -and $normalized.metrics.profilerBytes.value -eq 1234) 'Actual normalizer must consume profilerEvidence gate/size fields.'
}
foreach($flow in @('normal','error')) {
	Invoke-ProtocolCase "Profiler.FinalRewriteRetainsBothObjects.$flow" {
		. (Get-ActualProducerFunctions $StressProducer @('Set-PhysicsProfilerSummaryFields'))
		$summary=([ordered]@{success=($flow -eq 'normal');profilerCapture=$raw;profilerEvidence=$evidence}|ConvertTo-Json -Depth 32|ConvertFrom-Json)
		$beforeRaw=$summary.profilerCapture|ConvertTo-Json -Depth 32 -Compress
		$beforeEvidence=$summary.profilerEvidence|ConvertTo-Json -Depth 32 -Compress
		Set-PhysicsProfilerSummaryFields -Summary $summary -RawCapture $summary.profilerCapture -ProfilerEvidence $summary.profilerEvidence
		Assert-Protocol (($summary.profilerCapture|ConvertTo-Json -Depth 32 -Compress) -ceq $beforeRaw) 'Final production rewrite must retain raw capture/job/artifact/summary independently.'
		Assert-Protocol (($summary.profilerEvidence|ConvertTo-Json -Depth 32 -Compress) -ceq $beforeEvidence) 'Final production rewrite must retain normalized evidence independently.'
	}
}
Complete-ProtocolTests
