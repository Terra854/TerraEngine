[CmdletBinding()]
param(
	[string]$OutputDirectory = 'out/evidence/045/phase-gate',
	[string]$BuildDirectory = 'out/build/RelWithDebInfo',
	[string]$InstalledRoot = 'out/build/RelWithDebInfo/Build',
	[string]$CodecInputMesh = '',
	[string]$HardwareImage = '',
	[string]$ComputeImage = '',
	[string]$HybridImage = '',
	[string]$IndexedImage = '',
	[string]$FullReferenceEvidence = '',
	[string]$MillionDecisionEvidence = '',
	[string]$BenchmarkBaseline = '',
	[string]$BenchmarkCandidate = '',
	[string]$MigrationInput = '',
	[string]$PackageEvidence = '',
	[string]$RuntimeAdapter = '',
	[switch]$SkipBuildInstallPackage,
	[switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$utf8NoBom = [Text.UTF8Encoding]::new($false)

function Resolve-RepositoryPath([string]$Path)
{
	if([string]::IsNullOrWhiteSpace($Path))
	{
		return ''
	}
	if([IO.Path]::IsPathRooted($Path))
	{
		return [IO.Path]::GetFullPath($Path)
	}
	return [IO.Path]::GetFullPath((Join-Path $repositoryRoot $Path))
}

function Write-JsonFile([string]$Path, $Value)
{
	[IO.Directory]::CreateDirectory((Split-Path -Parent $Path)) | Out-Null
	[IO.File]::WriteAllText(
		$Path,
		($Value | ConvertTo-Json -Depth 64),
		$utf8NoBom)
}

function Get-Sha256([string]$Path)
{
	return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.
		ToLowerInvariant()
}

function Get-OptionalProperty($Value, [string]$Name)
{
	if($null -eq $Value)
	{
		return $null
	}
	# Gate rows are built as ordered dictionaries while parsed JSON arrives as
	# PSCustomObject. Support both representations so in-memory command results
	# participate in the same fail-closed rollups as reloaded evidence.
	if($Value -is [Collections.IDictionary])
	{
		if($Value.Contains($Name))
		{
			return $Value[$Name]
		}
		return $null
	}
	$property = $Value.PSObject.Properties[$Name]
	if($null -eq $property)
	{
		return $null
	}
	return $property.Value
}

function Get-CommandIdentity([string]$Command)
{
	$resolved = Get-Command $Command -ErrorAction Stop
	$path = [string]$resolved.Source
	if([string]::IsNullOrWhiteSpace($path))
	{
		$path = [string]$resolved.Path
	}
	if([string]::IsNullOrWhiteSpace($path) -or
		-not (Test-Path -LiteralPath $path -PathType Leaf))
	{
		throw "Command '$Command' does not resolve to a hashable executable."
	}
	return [ordered]@{
		path = [IO.Path]::GetFullPath($path)
		sha256 = Get-Sha256 $path
	}
}

function Get-TextSha256([string]$Text)
{
	$sha = [Security.Cryptography.SHA256]::Create()
	try
	{
		$bytes = $utf8NoBom.GetBytes($Text)
		return ([BitConverter]::ToString(
			$sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
	}
	finally
	{
		$sha.Dispose()
	}
}

function Invoke-GateCommand(
	[int]$Phase,
	[string]$Id,
	[string]$Command,
	[string[]]$Arguments,
	[hashtable]$Environment = @{},
	[bool]$RequirePassRecords = $false)
{
	$phaseRoot = Join-Path $outputRoot ("phase-{0}" -f $Phase)
	$commandRoot = Join-Path $phaseRoot 'commands'
	[IO.Directory]::CreateDirectory($commandRoot) | Out-Null
	$stdoutPath = Join-Path $commandRoot "$Id.stdout.log"
	$stderrPath = Join-Path $commandRoot "$Id.stderr.log"
	$evidencePath = Join-Path $commandRoot "$Id.json"
	$identity = Get-CommandIdentity $Command
	$executionId = [Guid]::NewGuid().ToString('D')
	$startedUtc = [DateTime]::UtcNow
	$stopwatch = [Diagnostics.Stopwatch]::StartNew()
	$previousErrorPreference = $ErrorActionPreference
	$previousEnvironment = @{}
	# Child scripts own their JSON files, but they intentionally do not assume
	# that a nested phase directory already exists. Prepare every declared
	# output destination before launch so a valid comparison cannot fail merely
	# because this is the first release-gate run in a clean evidence directory.
	for($argumentIndex = 0;
		$argumentIndex + 1 -lt $Arguments.Count;
		++$argumentIndex)
	{
		if($Arguments[$argumentIndex] -eq '-Output')
		{
			$outputParent = Split-Path -Parent (
				[string]$Arguments[$argumentIndex + 1])
			if(-not [string]::IsNullOrWhiteSpace($outputParent))
			{
				[IO.Directory]::CreateDirectory($outputParent) | Out-Null
			}
		}
		elseif($Arguments[$argumentIndex] -eq '-OutputDirectory')
		{
			[IO.Directory]::CreateDirectory(
				[string]$Arguments[$argumentIndex + 1]) | Out-Null
		}
	}
	foreach($key in $Environment.Keys)
	{
		$previousEnvironment[$key] = [Environment]::GetEnvironmentVariable(
			$key, [EnvironmentVariableTarget]::Process)
		[Environment]::SetEnvironmentVariable(
			$key, [string]$Environment[$key],
			[EnvironmentVariableTarget]::Process)
	}
	try
	{
		# Windows PowerShell can convert native stderr into an ErrorRecord.
		# Preserve the process exit code and hashed stderr as evidence instead of
		# aborting before the fail-closed command record is written.
		$ErrorActionPreference = 'Continue'
		& $identity.path @Arguments 1> $stdoutPath 2> $stderrPath
		$exitCode = $LASTEXITCODE
	}
	finally
	{
		$ErrorActionPreference = $previousErrorPreference
		$stopwatch.Stop()
		foreach($key in $Environment.Keys)
		{
			[Environment]::SetEnvironmentVariable(
				$key, $previousEnvironment[$key],
				[EnvironmentVariableTarget]::Process)
		}
	}
	$stdoutText = [string](Get-Content -Raw -LiteralPath $stdoutPath)
	$selectedPassRecords = @([regex]::Matches(
		$stdoutText, '(?m)^\[PASS\]\s+')).Count
	$publishedArtifacts = [Collections.Generic.List[object]]::new()
	for($argumentIndex = 0;
		$argumentIndex + 1 -lt $Arguments.Count;
		++$argumentIndex)
	{
		$candidate = ''
		if($Arguments[$argumentIndex] -eq '-Output')
		{
			$candidate = [string]$Arguments[$argumentIndex + 1]
		}
		elseif($Arguments[$argumentIndex] -eq '-OutputDirectory')
		{
			$candidate = Join-Path (
				[string]$Arguments[$argumentIndex + 1]) 'experiment-manifest.json'
		}
		if(-not [string]::IsNullOrWhiteSpace($candidate) -and
			(Test-Path -LiteralPath $candidate -PathType Leaf))
		{
			$publishedArtifacts.Add([ordered]@{
				path = [IO.Path]::GetFullPath($candidate)
				bytes = (Get-Item -LiteralPath $candidate).Length
				sha256 = Get-Sha256 $candidate
			})
		}
	}
	$acceptedExit = $exitCode -eq 0 -and
		(-not $RequirePassRecords -or $selectedPassRecords -gt 0)
	$record = [ordered]@{
		schemaVersion = 2
		status = if($acceptedExit) { 'passed' } else { 'failed' }
		identity = [ordered]@{
			executionId = $executionId
			phase = $Phase
			gate = $Id
			startedUtc = $startedUtc.ToString('o')
		}
		command = [ordered]@{
			path = $identity.path
			sha256 = $identity.sha256
			arguments = $Arguments
			argumentsSha256 = Get-TextSha256 ($Arguments -join "`0")
		}
		result = [ordered]@{
			exitCode = $exitCode
			elapsedMilliseconds = $stopwatch.Elapsed.TotalMilliseconds
			selectedPassRecords = $selectedPassRecords
			passRecordsRequired = $RequirePassRecords
			publishedArtifacts = $publishedArtifacts
			stdout = [ordered]@{
				path = $stdoutPath
				sha256 = Get-Sha256 $stdoutPath
				bytes = (Get-Item -LiteralPath $stdoutPath).Length
			}
			stderr = [ordered]@{
				path = $stderrPath
				sha256 = Get-Sha256 $stderrPath
				bytes = (Get-Item -LiteralPath $stderrPath).Length
			}
		}
	}
	Write-JsonFile $evidencePath $record
	return $record
}

function Publish-Unavailable(
	[int]$Phase,
	[string]$Id,
	[string]$Reason,
	[string]$RequestedPath = '')
{
	$path = Join-Path $outputRoot (
		"phase-{0}/gates/{1}.json" -f $Phase, $Id)
	$record = [ordered]@{
		schemaVersion = 2
		status = 'unavailable'
		identity = [ordered]@{
			phase = $Phase
			gate = $Id
		}
		requestedPath = $RequestedPath
		reason = $Reason
		note = 'Unavailable evidence never satisfies a release gate.'
	}
	Write-JsonFile $path $record
	return $record
}

function Test-AcceptedJsonEvidence(
	[int]$Phase,
	[string]$Id,
	[string]$Path,
	[string[]]$AcceptedStatuses)
{
	$resolvedPath = Resolve-RepositoryPath $Path
	if([string]::IsNullOrWhiteSpace($resolvedPath) -or
		-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf))
	{
		return Publish-Unavailable $Phase $Id (
			"A real JSON artifact is required for the $Id gate.") $Path
	}
	try
	{
		$document = Get-Content -Raw -LiteralPath $resolvedPath |
			ConvertFrom-Json
	}
	catch
	{
		return Publish-Unavailable $Phase $Id (
			"The supplied artifact is not readable JSON: $($_.Exception.Message)") $Path
	}
	$status = [string](Get-OptionalProperty $document 'status')
	$identity = Get-OptionalProperty $document 'identity'
	$executionId = [string](Get-OptionalProperty $identity 'executionId')
	if([string]::IsNullOrWhiteSpace($executionId))
	{
		$executionId = [string](Get-OptionalProperty $identity 'captureId')
	}
	$accepted = $status -in $AcceptedStatuses -and
		-not [string]::IsNullOrWhiteSpace($executionId)
	$record = [ordered]@{
		schemaVersion = 2
		status = if($accepted) { 'accepted' } else { 'incomplete' }
		identity = [ordered]@{
			phase = $Phase
			gate = $Id
			sourceExecutionId = $executionId
		}
		artifact = [ordered]@{
			path = $resolvedPath
			bytes = (Get-Item -LiteralPath $resolvedPath).Length
			sha256 = Get-Sha256 $resolvedPath
			publishedStatus = $status
		}
		reason = if($accepted) { '' } else {
			'The artifact must publish an accepted status and a non-empty execution identity.'
		}
	}
	Write-JsonFile (Join-Path $outputRoot (
		"phase-{0}/gates/{1}.json" -f $Phase, $Id)) $record
	return $record
}

function Test-BenchmarkCapture([string]$Path, [string]$Id, [int]$Phase = 9)
{
	$resolvedPath = Resolve-RepositoryPath $Path
	if([string]::IsNullOrWhiteSpace($resolvedPath) -or
		-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf))
	{
		return Publish-Unavailable $Phase $Id (
			"Measured benchmark capture '$Id' is required.") $Path
	}
	$document = Get-Content -Raw -LiteralPath $resolvedPath | ConvertFrom-Json
	$identity = Get-OptionalProperty $document 'identity'
	$executionId = [string](Get-OptionalProperty $identity 'executionId')
	$measured = @((Get-OptionalProperty $document 'rawSamples') |
		Where-Object {
			[string](Get-OptionalProperty $_ 'availability') -eq 'measured' -and
			[string](Get-OptionalProperty $_ 'executionId') -eq $executionId -and
			[double](Get-OptionalProperty $_ 'durationNanoseconds') -gt 0
		})
	$comparability = Get-OptionalProperty $document 'baselineAndComparability'
	$publishedStatus = [string](Get-OptionalProperty $comparability 'status')
	$requiredHashFields = @(
		'engineHash', 'sourceHash', 'diffHash', 'executableHash',
		'shaderHash', 'assetHash', 'buildRecipeHash')
	$invalidHashFields = @($requiredHashFields | Where-Object {
		[string](Get-OptionalProperty $identity $_) -notmatch '^[0-9a-fA-F]{64}$'
	})
	$integrity = Get-OptionalProperty $document 'integrity'
	$integrityHash = [string](Get-OptionalProperty $integrity 'hash')
	$accepted = -not [string]::IsNullOrWhiteSpace($executionId) -and
		$measured.Count -ge 5 -and
		$publishedStatus -in @('measured', 'comparable') -and
		$invalidHashFields.Count -eq 0 -and
		$integrityHash -match '^[0-9a-fA-F]{64}$'
	$record = [ordered]@{
		schemaVersion = 2
		status = if($accepted) { 'accepted' } else { 'incomplete' }
		identity = [ordered]@{
			phase = $Phase
			gate = $Id
			sourceExecutionId = $executionId
		}
		artifact = [ordered]@{
			path = $resolvedPath
			bytes = (Get-Item -LiteralPath $resolvedPath).Length
			sha256 = Get-Sha256 $resolvedPath
			measuredSamples = $measured.Count
			comparabilityStatus = $publishedStatus
			invalidIdentityHashFields = $invalidHashFields
			integrityHash = $integrityHash
		}
		reason = if($accepted) { '' } else {
			'Benchmark evidence requires one execution identity, at least five matching measured samples, measured/comparable status, and real 64-hex artifact/integrity hashes.'
		}
	}
	Write-JsonFile (Join-Path $outputRoot (
		"phase-{0}/gates/{1}.json" -f $Phase, $Id)) $record
	return $record
}

function Publish-TestRecordEvidence(
	[int]$Phase,
	[string]$Id,
	[object[]]$CommandRows,
	[string[]]$RequiredPassNames)
{
	# Do not call this collection `$matches`: PowerShell's case-insensitive
	# automatic `$Matches` variable is overwritten by every successful `-match`
	# expression and would replace the list with a regex capture dictionary.
	$passRecords = [Collections.Generic.List[object]]::new()
	foreach($requiredName in $RequiredPassNames)
	{
		$matchedCommand = $null
		foreach($commandRow in $CommandRows)
		{
			$commandResult = Get-OptionalProperty $commandRow 'result'
			if($null -eq $commandResult)
			{
				continue
			}
			$commandStdout = Get-OptionalProperty $commandResult 'stdout'
			$stdout = [string](Get-OptionalProperty $commandStdout 'path')
			if(Test-Path -LiteralPath $stdout -PathType Leaf)
			{
				$text = Get-Content -Raw -LiteralPath $stdout
				if($text -match (
					'(?m)^\[PASS\]\s+' + [regex]::Escape($requiredName) + '\s*$'))
				{
					$matchedCommand = $commandRow
					break
				}
			}
		}
		$matchedIdentity = Get-OptionalProperty $matchedCommand 'identity'
		$matchedResult = Get-OptionalProperty $matchedCommand 'result'
		$matchedStdout = Get-OptionalProperty $matchedResult 'stdout'
		$commandExecutionId = ''
		$stdoutSha256 = ''
		if($null -ne $matchedCommand)
		{
			$commandExecutionId = [string](
				Get-OptionalProperty $matchedIdentity 'executionId')
			$stdoutSha256 = [string](
				Get-OptionalProperty $matchedStdout 'sha256')
		}
		$passRecords.Add([ordered]@{
			test = $requiredName
			status = if($null -ne $matchedCommand) { 'passed' } else { 'missing' }
			commandExecutionId = $commandExecutionId
			stdoutSha256 = $stdoutSha256
		})
	}
	$accepted = @($passRecords | Where-Object {
		$_.status -ne 'passed'
	}).Count -eq 0
	$record = [ordered]@{
		schemaVersion = 2
		status = if($accepted) { 'accepted' } else { 'incomplete' }
		identity = [ordered]@{
			executionId = [Guid]::NewGuid().ToString('D')
			phase = $Phase
			gate = $Id
		}
		requiredPassRecords = $passRecords
		reason = if($accepted) { '' } else {
			'Every named full-reference or million-decision test must publish a selected PASS record.'
		}
	}
	Write-JsonFile (Join-Path $outputRoot (
		"phase-{0}/gates/{1}.json" -f $Phase, $Id)) $record
	return $record
}

$outputRoot = Resolve-RepositoryPath $OutputDirectory
$buildRoot = Resolve-RepositoryPath $BuildDirectory
$installedTree = Resolve-RepositoryPath $InstalledRoot
$engineTests = Join-Path $buildRoot 'TerraEngine-EngineTests.exe'
$editorTests = Join-Path $buildRoot 'TerraEngine-EditorTests.exe'
$codecRunner = Join-Path $installedTree (
	'Tools/TerraEngineVirtualGeometryCodecExperiment.exe')
$assetTool = Join-Path $installedTree 'Tools/TerraEngineVirtualGeometryAssetTool.exe'
$benchmarkCompare = Join-Path $PSScriptRoot 'CompareVirtualGeometryPerformance.ps1'
$imageCompare = Join-Path $PSScriptRoot 'CompareVisibilityImages.ps1'
$backendMatrix = Join-Path $PSScriptRoot 'RunBackendParityMatrix.ps1'

if($DryRun)
{
	foreach($requiredScript in @($benchmarkCompare, $imageCompare, $backendMatrix))
	{
		if(-not (Test-Path -LiteralPath $requiredScript -PathType Leaf))
		{
			throw "Release-gate helper missing: $requiredScript"
		}
	}
	Write-Output 'validated Vulkan-only phase 0-9 release-gate orchestration (dry run)'
	exit 0
}

[IO.Directory]::CreateDirectory($outputRoot) | Out-Null
$rows = [Collections.Generic.List[object]]::new()

$testGates = @(
	@{ Phase = 0; Id = 'capability-tests'; Executable = $engineTests; Filter = 'VirtualGeometry.Capability;VirtualGeometry.Diagnostics;VirtualGeometry.GpuTiming;VulkanFrameDiagnostics' },
	@{ Phase = 1; Id = 'codec-editor-tests'; Executable = $editorTests; Filter = 'VirtualGeometry;ClusterDag;Codec;PageFormat;Determinism' },
	@{ Phase = 1; Id = 'codec-runtime-tests'; Executable = $engineTests; Filter = 'VirtualGeometry;ClusterDag;Codec;PageFormat;Determinism' },
	@{ Phase = 2; Id = 'traversal-million-decision-tests'; Executable = $engineTests; Filter = 'VirtualGeometry.Traversal;VirtualGeometry.Compaction;VirtualGeometry.Culling;VirtualGeometry.Indirect;VirtualGeometry.Scheduler;VirtualGeometry.GpuDriven' },
	@{ Phase = 3; Id = 'visibility-tests'; Executable = $engineTests; Filter = 'VirtualGeometry.Visibility;VirtualGeometry.Picking;VirtualGeometry.Material;VulkanImageReadback' },
	@{ Phase = 3; Id = 'visibility-editor-tests'; Executable = $editorTests; Filter = 'VirtualGeometry.Editor.Automation.ValidationRoutes;VirtualGeometry.Editor.Automation.AuthoritativePick;VirtualGeometry.Editor.Feature045.LiveHarnessLocksValidationRoutes' },
	@{ Phase = 4; Id = 'streaming-tests'; Executable = $engineTests; Filter = 'VirtualGeometry.PageStreaming;VirtualGeometry.PageResidency;VirtualGeometry.Publication;Resource.Streaming' },
	@{ Phase = 5; Id = 'hybrid-raster-tests'; Executable = $engineTests; Filter = 'VirtualGeometry.ComputeRaster;VirtualGeometry.HybridRaster;VirtualGeometry.Visibility' },
	@{ Phase = 6; Id = 'views-materials-tests'; Executable = $engineTests; Filter = 'VirtualGeometry.Shadow;VirtualGeometry.MultiView;VirtualGeometry.Material;VirtualGeometry.Occlusion;VirtualGeometry.Capture' },
	@{ Phase = 6; Id = 'views-editor-tests'; Executable = $editorTests; Filter = 'VirtualGeometry.Editor.Diagnostics.Foundation.ShadowSelectionsRemainSeparate;VirtualGeometry.Editor.Diagnostics.VisualizerIdentitySurvivesGpuPacketSnapshots;VirtualGeometry.Editor.Diagnostics.OverlayLabelsGpuPacketSources' },
	@{ Phase = 7; Id = 'animation-runtime-tests'; Executable = $engineTests; Filter = 'AnimatedVirtualGeometry;DeformableVirtualGeometry;Renderer.AnimationView;ShaderGeometryContract' },
	@{ Phase = 7; Id = 'animation-editor-tests'; Executable = $editorTests; Filter = 'SkinnedMeshCook;AnimatedVirtualPageFormat;VirtualGeometry' },
	@{ Phase = 8; Id = 'aggregate-portability-tests'; Executable = $engineTests; Filter = 'VirtualGeometry.Aggregate;VirtualGeometry.Backend;VirtualGeometry.Capability' },
	@{ Phase = 9; Id = 'production-editor-tests'; Executable = $editorTests; Filter = 'VirtualGeometry.Editor.Automation.Production;VirtualGeometry.Editor.PackageImport;VirtualGeometry.Editor.Export.Benchmark;VirtualGeometry.Editor.AssetPipeline.Migration' }
)
foreach($gate in $testGates)
{
	if(-not (Test-Path -LiteralPath $gate.Executable -PathType Leaf))
	{
		$rows.Add((Publish-Unavailable $gate.Phase $gate.Id (
			"Required test executable is missing: $($gate.Executable)") $gate.Executable))
		continue
	}
	$rows.Add((Invoke-GateCommand $gate.Phase $gate.Id $gate.Executable @() @{
		TERRAENGINE_TEST_FILTER = $gate.Filter
	} $true))
}

if([string]::IsNullOrWhiteSpace($CodecInputMesh) -or
	-not (Test-Path -LiteralPath (Resolve-RepositoryPath $CodecInputMesh) -PathType Leaf))
{
	$rows.Add((Publish-Unavailable 1 'codec-page-size-experiment' `
		'A real compiled mesh is required to run the 32/64/128 KiB codec experiment.' `
		$CodecInputMesh))
}
elseif(-not (Test-Path -LiteralPath $codecRunner -PathType Leaf))
{
	$rows.Add((Publish-Unavailable 1 'codec-page-size-experiment' `
		"Installed codec experiment executable is missing: $codecRunner" $codecRunner))
}
else
{
	$codecOutput = Join-Path $outputRoot 'phase-1/codec'
	$rows.Add((Invoke-GateCommand 1 'codec-page-size-experiment' 'powershell' @(
		'-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
		(Join-Path $PSScriptRoot 'RunCodecExperiment.ps1'),
		'-InputMesh', (Resolve-RepositoryPath $CodecInputMesh),
		'-ExperimentExecutable', $codecRunner,
		'-OutputDirectory', $codecOutput)))
}

$routeImages = [ordered]@{
	hardware = $HardwareImage
	compute = $ComputeImage
	hybrid = $HybridImage
	indexed = $IndexedImage
}
foreach($route in @('compute', 'hybrid', 'indexed'))
{
	$baselinePath = Resolve-RepositoryPath $routeImages.hardware
	$candidatePath = Resolve-RepositoryPath $routeImages[$route]
	$id = "route-parity-hardware-$route"
	if([string]::IsNullOrWhiteSpace($baselinePath) -or
		[string]::IsNullOrWhiteSpace($candidatePath) -or
		-not (Test-Path -LiteralPath $baselinePath -PathType Leaf) -or
		-not (Test-Path -LiteralPath $candidatePath -PathType Leaf))
	{
		$rows.Add((Publish-Unavailable 3 $id (
			"Matched hardware and $route route images are required.") (
			"$HardwareImage|$($routeImages[$route])")))
		continue
	}
	$routePhase = if($route -eq 'hybrid') { 5 } else { 3 }
	$output = Join-Path $outputRoot "phase-$routePhase/parity/$id.json"
	$rows.Add((Invoke-GateCommand $routePhase $id 'powershell' @(
		'-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $imageCompare,
		'-Baseline', $baselinePath, '-Candidate', $candidatePath,
		'-BaselineRoute', 'hardware', '-CandidateRoute', $route,
		'-Output', $output)))
}

if([string]::IsNullOrWhiteSpace($FullReferenceEvidence))
{
	$rows.Add((Publish-TestRecordEvidence 7 'full-reference' $rows @(
		'AnimatedVirtualGeometry.MorphParity.PositionNormalTangentBeforeSkin')))
}
else
{
	$rows.Add((Test-AcceptedJsonEvidence 7 'full-reference' `
		$FullReferenceEvidence @('accepted', 'complete', 'measured', 'passed', 'validated')))
}
if([string]::IsNullOrWhiteSpace($MillionDecisionEvidence))
{
	$rows.Add((Publish-TestRecordEvidence 7 'million-decision' $rows @(
		'VirtualGeometry.GpuDriven.DeterministicMillionReplay',
		'AnimatedVirtualGeometry.Runtime.MillionDecisionExtremePoseVisibilityHasNoFalseRejections')))
}
else
{
	$rows.Add((Test-AcceptedJsonEvidence 7 'million-decision' `
		$MillionDecisionEvidence @('accepted', 'complete', 'measured', 'passed', 'validated')))
}

$matrixOutput = Join-Path $outputRoot 'phase-8/vendor/backend-parity.json'
$matrixArguments = @(
	'-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $backendMatrix,
	'-Matrix', (Join-Path $PSScriptRoot 'hardware-matrix.json'),
	'-Runner', (Join-Path $installedTree 'Tools/TerraEngineVirtualGeometryBenchmark.exe'),
	'-Output', $matrixOutput,
	'-Workloads', (Join-Path $PSScriptRoot 'workloads.json'),
	'-Quality', (Join-Path $PSScriptRoot 'quality-profiles.json'))
if(-not [string]::IsNullOrWhiteSpace($RuntimeAdapter))
{
	$matrixArguments += @('-RuntimeAdapter', $RuntimeAdapter)
}
$rows.Add((Invoke-GateCommand 8 'vulkan-vendor-matrix' 'powershell' `
	$matrixArguments))

$baselineRecord = Test-BenchmarkCapture $BenchmarkBaseline 'benchmark-baseline'
$candidateRecord = Test-BenchmarkCapture $BenchmarkCandidate 'benchmark-candidate'
$rows.Add((Test-BenchmarkCapture $BenchmarkBaseline 'baseline-capture' 0))
$rows.Add($baselineRecord)
$rows.Add($candidateRecord)
if($baselineRecord.status -eq 'accepted' -and $candidateRecord.status -eq 'accepted')
{
	$comparisonOutput = Join-Path $outputRoot 'phase-9/benchmark/comparison.json'
	$rows.Add((Invoke-GateCommand 9 'benchmark-comparison' 'powershell' @(
		'-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $benchmarkCompare,
		'-BaselineDirectory', (Split-Path -Parent (
			Resolve-RepositoryPath $BenchmarkBaseline)),
		'-CandidateDirectory', (Split-Path -Parent (
			Resolve-RepositoryPath $BenchmarkCandidate)),
		'-Output', $comparisonOutput)))
}
else
{
	$rows.Add((Publish-Unavailable 9 'benchmark-comparison' `
		'Accepted baseline and candidate benchmark captures are required.'))
}

$migrationPath = Resolve-RepositoryPath $MigrationInput
if([string]::IsNullOrWhiteSpace($migrationPath) -or
	-not (Test-Path -LiteralPath $migrationPath -PathType Leaf) -or
	-not (Test-Path -LiteralPath $assetTool -PathType Leaf))
{
	$rows.Add((Publish-Unavailable 9 'migration' `
		'A real migration input and the installed asset tool are required.' `
		$MigrationInput))
}
else
{
	$migrationCopy = Join-Path $outputRoot 'phase-9/migration/input.json'
	[IO.Directory]::CreateDirectory((Split-Path -Parent $migrationCopy)) | Out-Null
	Copy-Item -LiteralPath $migrationPath -Destination $migrationCopy -Force
	$rows.Add((Invoke-GateCommand 9 'migration' $assetTool @(
		'--migrate', '--target', $migrationCopy, '--confirm')))
}

if(-not $SkipBuildInstallPackage)
{
	$rows.Add((Invoke-GateCommand 9 'install' 'cmake' @(
		'--build', $buildRoot, '--config', 'RelWithDebInfo',
		'--target', 'install')))
	$inventoryPath = Join-Path $outputRoot 'phase-9/package/install-inventory.json'
	$inventory = @()
	if(Test-Path -LiteralPath $installedTree -PathType Container)
	{
		$inventory = @(Get-ChildItem -LiteralPath $installedTree -File -Recurse |
			Sort-Object FullName | ForEach-Object {
				[ordered]@{
					path = $_.FullName.Substring($installedTree.Length).
						TrimStart('\', '/').Replace('\', '/')
					bytes = $_.Length
					sha256 = Get-Sha256 $_.FullName
				}
			})
	}
	Write-JsonFile $inventoryPath ([ordered]@{
		schemaVersion = 2
		status = if($inventory.Count -gt 0) { 'accepted' } else { 'incomplete' }
		identity = [ordered]@{
			executionId = [Guid]::NewGuid().ToString('D')
			root = $installedTree
		}
		files = $inventory
	})
	$rows.Add((Test-AcceptedJsonEvidence 9 'installed-package-inventory' `
		$inventoryPath @('accepted')))
	$rows.Add((Test-AcceptedJsonEvidence 9 'package' `
		$inventoryPath @('accepted')))
}
else
{
	$rows.Add((Publish-Unavailable 9 'install' `
		'Build/install/package execution was explicitly skipped.'))
	$rows.Add((Test-AcceptedJsonEvidence 9 'package' `
		$PackageEvidence @('accepted', 'complete', 'passed', 'validated')))
}

# The quickstart gate is a hash-bound rollup of the phase-specific test
# commands above. This explicitly rejects an exit-zero harness that selected no
# tests, and gives the installed workflow one stable artifact to cite.
$quickstartCommands = @($rows | Where-Object {
	$resultValue = Get-OptionalProperty $_ 'result'
	$null -ne (Get-OptionalProperty $_ 'command') -and
	$null -ne (Get-OptionalProperty $resultValue 'passRecordsRequired') -and
	[bool](Get-OptionalProperty $resultValue 'passRecordsRequired')
})
$quickstartAccepted = $quickstartCommands.Count -eq $testGates.Count -and
	@($quickstartCommands | Where-Object {
		$_.status -ne 'passed' -or
		[int]$_.result.selectedPassRecords -le 0
	}).Count -eq 0
$quickstartPath = Join-Path $outputRoot 'phase-9/quickstart/validation.json'
$quickstartDocument = [ordered]@{
	schemaVersion = 2
	status = if($quickstartAccepted) { 'accepted' } else { 'incomplete' }
	identity = [ordered]@{
		executionId = [Guid]::NewGuid().ToString('D')
		scope = 'feature-045-quickstart-phases-0-9'
	}
	commandCount = $quickstartCommands.Count
	requiredCommandCount = $testGates.Count
	commands = @($quickstartCommands | ForEach-Object {
		[ordered]@{
			phase = $_.identity.phase
			gate = $_.identity.gate
			executionId = $_.identity.executionId
			executableSha256 = $_.command.sha256
			argumentsSha256 = $_.command.argumentsSha256
			stdoutSha256 = $_.result.stdout.sha256
			selectedPassRecords = $_.result.selectedPassRecords
			status = $_.status
		}
	})
	note = 'Selected PASS records, rather than the harness registry total, determine quickstart test coverage.'
}
Write-JsonFile $quickstartPath $quickstartDocument
$rows.Add((Test-AcceptedJsonEvidence 9 'quickstart' `
	$quickstartPath @('accepted')))

$scenarioFiles = @(Get-ChildItem -LiteralPath (
	Join-Path $PSScriptRoot 'PhaseScenarios') -Filter '*.json' -File)
foreach($phase in 0..9)
{
	$scenario = @($scenarioFiles | ForEach-Object {
		Get-Content -Raw -LiteralPath $_.FullName | ConvertFrom-Json
	} | Where-Object { [int]$_.phase -eq $phase })
	if($scenario.Count -ne 1)
	{
		throw "Expected exactly one release manifest for phase $phase."
	}
	$expectedGates = @($scenario[0].releaseGates)
	$phaseRows = @()
	foreach($gateId in $expectedGates)
	{
		$matching = @($rows | Where-Object {
			$identity = Get-OptionalProperty $_ 'identity'
			[int](Get-OptionalProperty $identity 'phase') -eq $phase -and
			[string](Get-OptionalProperty $identity 'gate') -eq [string]$gateId
		})
		$phaseRows += [ordered]@{
			gate = [string]$gateId
			status = if($matching.Count -eq 1) {
				[string]$matching[0].status
			} elseif($matching.Count -eq 0) {
				'missing'
			} else {
				'ambiguous'
			}
			evidence = $matching
		}
	}
	$phaseAccepted = @($phaseRows | Where-Object {
		$_.status -notin @('passed', 'accepted')
	}).Count -eq 0
	Write-JsonFile (Join-Path $outputRoot (
		"phase-{0}/phase-release-result.json" -f $phase)) ([ordered]@{
		schemaVersion = 2
		phase = $phase
		status = if($phaseAccepted) { 'accepted' } else { 'incomplete' }
		gates = $phaseRows
	})
}

$summary = [ordered]@{
	schemaVersion = 2
	status = if(@($rows | Where-Object {
		[string](Get-OptionalProperty $_ 'status') -notin @('passed', 'accepted')
	}).Count -eq 0) { 'accepted' } else { 'incomplete' }
	generatedUtc = [DateTime]::UtcNow.ToString('o')
	backendScope = @('Vulkan')
	gates = $rows
	note = 'A gate passes only with a hashed executable/artifact and a real execution identity. Unsupported vendor lanes remain unavailable.'
}
Write-JsonFile (Join-Path $outputRoot 'release-gates.json') $summary
$summary | ConvertTo-Json -Depth 64
if($summary.status -ne 'accepted')
{
	exit 2
}
