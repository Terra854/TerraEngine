[CmdletBinding()]
param([string]$Project,[string]$Scene,[string]$Manifest=(Join-Path $PSScriptRoot '../FramePublication/workloads.json'),[string]$OutputDirectory,[string]$InstallRoot,[string]$EditorExecutable,[string]$CameraHash,[string]$EditorProcessName='TerraEngine-Editor',[string]$LiveAdapter=(Join-Path $PSScriptRoot '../VirtualGeometry/CaptureVirtualGeometryLiveVisual.ps1'),[int]$WarmupSeconds=30,[int]$DurationSeconds=60,[int]$Runs=5,[ValidateSet('DetailedRecording','MatchedRecordingComparison','MatchedWindowOpen')][string]$Mode='DetailedRecording',[int]$OpenProfilerAtSeconds=-1,[int]$McpPort=43155,[switch]$ProfilerBenchmark,[switch]$DryRun,[switch]$Resume)
$ErrorActionPreference='Stop'
function Require([bool]$ok,[string]$message){ if(-not $ok){ throw $message } }
function Get-PropertyValue($object,[string]$name){ if($null -eq $object){ return $null }; if($object -is [Collections.IDictionary] -and $object.Contains($name)){ return $object[$name] }; $property=$object.PSObject.Properties[$name]; if($null -eq $property){ return $null }; return $property.Value }
function Get-OptionalUInt64($object,[string]$name){ $value=Get-PropertyValue $object $name; if($null -eq $value -or [string]::IsNullOrWhiteSpace([string]$value)){ return $null }; try { return [UInt64]$value } catch { return $null } }
function Get-EngineCpuWindow($evidence,[double]$startMilliseconds,[double]$endMilliseconds){
	$samples=@(Get-PropertyValue $evidence 'samples' | Where-Object {
		$elapsed=[double](Get-PropertyValue $_ 'elapsedMilliseconds')
		$null -ne (Get-PropertyValue $_ 'engineCpuTimeMs') -and $elapsed -ge $startMilliseconds -and $elapsed -lt $endMilliseconds
	} | ForEach-Object { [double](Get-PropertyValue $_ 'engineCpuTimeMs') } | Where-Object {
		-not [double]::IsNaN($_) -and -not [double]::IsInfinity($_) -and $_ -ge 0.0
	} | Sort-Object)
	if($samples.Count -eq 0){ return $null }
	$p95Index=[Math]::Min($samples.Count-1,[Math]::Max(0,[int][Math]::Ceiling($samples.Count*0.95)-1))
	[ordered]@{ count=$samples.Count; meanMs=[double](($samples|Measure-Object -Average).Average); medianMs=[double]$samples[[int][Math]::Floor(($samples.Count-1)*0.5)]; p95Ms=[double]$samples[$p95Index]; minimumMs=[double]$samples[0]; maximumMs=[double]$samples[-1] }
}
function Test-BoundedSampleCadence($evidence,[int]$expectedMilliseconds){
	$samples=@(Get-PropertyValue $evidence 'samples' | Sort-Object { [double](Get-PropertyValue $_ 'elapsedMilliseconds') })
	if($samples.Count -lt 2){ return $false }
	$intervals=@()
	for($index=1;$index -lt $samples.Count;++$index){$intervals += ([double](Get-PropertyValue $samples[$index] 'elapsedMilliseconds')-[double](Get-PropertyValue $samples[$index-1] 'elapsedMilliseconds'))}
	$ordered=@($intervals|Sort-Object); $median=[double]$ordered[[int][Math]::Floor(($ordered.Count-1)*0.5)]
	return $median -ge ($expectedMilliseconds * 0.50) -and $median -le ($expectedMilliseconds * 1.50)
}
function Get-BoundedTimingBlocks($evidence,[int]$blockCount=6){
	# P95 of a single 60-second MCP sample stream is dominated by periodic
	# response bursts. Aggregate equal-duration block P95s instead, retaining raw
	# samples and failing closed when any block lacks enough valid observations.
	$samples=@(Get-PropertyValue $evidence 'samples' | Where-Object {
		$validFlag=Get-PropertyValue $_ 'valid'; $value=Get-PropertyValue $_ 'engineCpuTimeMs'
		($null -eq $validFlag -or [bool]$validFlag) -and $null -ne $value
	} | Sort-Object elapsedMilliseconds)
	$duration=[double](Get-PropertyValue $evidence 'durationSeconds'); if($duration -le 0 -or $samples.Count -eq 0){ return $null }
	$blockSeconds=$duration/$blockCount; $blocks=[Collections.Generic.List[object]]::new()
	for($block=0;$block -lt $blockCount;++$block){
		$start=$block*$blockSeconds*1000.0; $end=if($block -eq $blockCount-1){[double]::PositiveInfinity}else{($block+1)*$blockSeconds*1000.0}
		$values=@($samples|Where-Object {[double]$_.elapsedMilliseconds -ge $start -and [double]$_.elapsedMilliseconds -lt $end}|ForEach-Object {[double]$_.engineCpuTimeMs}|Where-Object {$_ -ge 0 -and -not [double]::IsNaN($_) -and -not [double]::IsInfinity($_)}|Sort-Object)
		if($values.Count -lt 30){ return $null }
		$p95=[int][Math]::Min($values.Count-1,[Math]::Ceiling($values.Count*.95)-1)
		$blocks.Add([ordered]@{block=$block;sampleCount=$values.Count;medianMs=$values[[int][Math]::Floor(($values.Count-1)*.5)];p95Ms=$values[$p95]})
	}
	$medians=@($blocks|ForEach-Object {[double]$_.medianMs}|Sort-Object); $p95s=@($blocks|ForEach-Object {[double]$_.p95Ms}|Sort-Object)
	[ordered]@{sampleCount=$samples.Count;blockCount=$blocks.Count;medianMs=$medians[[int][Math]::Floor(($medians.Count-1)*.5)];p95Ms=$p95s[[int][Math]::Floor(($p95s.Count-1)*.5)];blocks=@($blocks)}
}
function Hash-File([string]$path){ (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Hash-Text([string]$text){
	$sha=[Security.Cryptography.SHA256]::Create()
	try { ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)))).Replace('-','').ToLowerInvariant() }
	finally { $sha.Dispose() }
}
function Get-ChildRelativePath([string]$parent,[string]$child){
	# Windows PowerShell 5.1 predates Path.GetRelativePath. Validate that the
	# authored scene is actually inside the project before deriving its path.
	$parentFull=[IO.Path]::GetFullPath($parent).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
	$childFull=[IO.Path]::GetFullPath($child)
	Require ($childFull.StartsWith($parentFull,[StringComparison]::OrdinalIgnoreCase)) "Path is outside the project: $childFull"
	return $childFull.Substring($parentFull.Length).Replace('\','/')
}
Require (Test-Path -LiteralPath $Manifest) "Manifest is missing: $Manifest"
$workload=Get-Content -Raw -LiteralPath $Manifest | ConvertFrom-Json
Require ($workload.schemaVersion -eq 1) 'Unsupported workload schema.'
if(-not $ProfilerBenchmark) { Require ($workload.scenarios.Count -eq 6) 'Exactly six canonical scenarios are required.' }
Require ($Runs -ge 1) 'Capture requires at least one measured run.'
Require ($WarmupSeconds -ge 30) 'Warmup must be at least 30 seconds.'
Require ($DurationSeconds -ge 60) 'Measured duration must be at least 60 seconds.'
if($Mode -eq 'MatchedWindowOpen'){ Require ($OpenProfilerAtSeconds -ge 0 -and $OpenProfilerAtSeconds -lt $DurationSeconds) 'MatchedWindowOpen requires an in-range OpenProfilerAtSeconds.' }
Require (Test-Path -LiteralPath $Project) "Project is missing: $Project"
Require (Test-Path -LiteralPath $Scene) "Scene is missing: $Scene"
Require (-not [string]::IsNullOrWhiteSpace($CameraHash)) 'CameraHash is required; camera identity is never inferred.'
$existingEditors = @(Get-Process -Name $EditorProcessName -ErrorAction SilentlyContinue)
Require ($existingEditors.Count -eq 0) 'Comparable capture starts and owns exactly one installed editor.'
$adapterText = Get-Content -Raw -LiteralPath $LiveAdapter
Require ($adapterText -match 'RepresentationRoute' -and $adapterText -match 'PrefabVariants') 'Live adapter lacks required route/variant mutation operations; cannot enforce workload identity.'
if($DryRun){
	$dryRunScenarios = if($ProfilerBenchmark) { @('profiler-authored') } else { @($workload.scenarios.id) }
	Write-Output ([ordered]@{valid=$true;dryRun=$true;scenarios=$dryRunScenarios;samplesCreated=0} | ConvertTo-Json -Depth 5)
	exit 0
}
Require (-not [string]::IsNullOrWhiteSpace($InstallRoot)) 'InstallRoot is required for capture.'
Require (Test-Path -LiteralPath $InstallRoot) "Install root is missing: $InstallRoot"
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
if(-not(Test-Path $LiveAdapter)){throw "Live MCP adapter is missing: $LiveAdapter"}; if([string]::IsNullOrWhiteSpace($EditorExecutable)){throw 'Installed editor executable is required.'}; if(-not(Test-Path $EditorExecutable)){throw "Editor executable is missing: $EditorExecutable"}
$projectIdentityPath = Join-Path $Project 'game.json'
Require (Test-Path -LiteralPath $projectIdentityPath) "Project identity is missing: $projectIdentityPath"
$installedEnginePath = Join-Path (Split-Path -Parent ([IO.Path]::GetFullPath($EditorExecutable))) 'TerraEngine.dll'
Require (Test-Path -LiteralPath $installedEnginePath -PathType Leaf) 'Installed editor package is missing sibling TerraEngine.dll; refusing incomparable identity.'
$installedEngine = Get-Item -LiteralPath $installedEnginePath
$installedPackageFiles = @(
	[ordered]@{ role = 'editor'; fileName = [IO.Path]::GetFileName($EditorExecutable); sha256 = Hash-File $EditorExecutable }
	[ordered]@{ role = 'engine'; fileName = $installedEngine.Name; sha256 = Hash-File $installedEngine.FullName }
)
$installedPackageHash = Hash-Text ($installedPackageFiles | ConvertTo-Json -Compress -Depth 4)
$videoController = Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue |
	Sort-Object Name | Select-Object -First 1
$powerState = (& powercfg /getactivescheme 2>$null | Out-String).Trim()
if([string]::IsNullOrWhiteSpace($powerState)) { $powerState = 'unavailable' }
$sourceCommit = (& git rev-parse HEAD 2>$null | Select-Object -First 1).Trim()
$directPrefab = 'Assets/ValidationModels/m03/BaseCharacter_Generated/BaseCharacter.teprefab'
$densePrefab = 'Assets/ValidationModels/DenseAnimatedVG/CesiumMan_Generated/CesiumMan.teprefab'
$variantPrefabs = if($ProfilerBenchmark) { @() } else { @($directPrefab, $densePrefab) }
$contentHashes = [ordered]@{}
foreach($prefab in $variantPrefabs) {
	$prefabPath = Join-Path $Project $prefab
	Require (Test-Path -LiteralPath $prefabPath) "Required benchmark prefab is missing: $prefabPath"
	$contentHashes[$prefab] = Hash-File $prefabPath
}
# Hash the exact working tree without letting a developer's CRLF conversion
# preference emit warnings that PowerShell can promote to terminating errors.
# This is a read-only identity query; it never changes repository configuration.
$dirtyState = ((& git -c core.autocrlf=false status --porcelain=v1 -uall 2>$null | Out-String) +
	(& git -c core.autocrlf=false diff --binary --no-ext-diff 2>$null | Out-String))
$baseIdentity = [ordered]@{
	projectHash = Hash-File $projectIdentityPath
	sceneHash = Hash-File $Scene
	cameraHash = $CameraHash.ToLowerInvariant()
	settingsHash = Hash-Text ((Get-Content -Raw -LiteralPath $projectIdentityPath) +
		(Get-Content -Raw -LiteralPath $Manifest))
	powerState = $powerState
	buildHash = $sourceCommit
	dirtyDiffHash = Hash-Text $dirtyState
	installedHash = $installedPackageHash
	installedPackageHash = $installedPackageHash
	installedPackageFiles = $installedPackageFiles
	workerCount = [Environment]::ProcessorCount
	profilerHash = if($null -ne $installedEngine) { Hash-File $installedEngine.FullName } else { Hash-File $EditorExecutable }
	adapter = if($null -ne $videoController) { [string]$videoController.Name } else { 'unavailable' }
	driver = if($null -ne $videoController) { [string]$videoController.DriverVersion } else { 'unavailable' }
	operatingSystem = [Environment]::OSVersion.VersionString
	buildConfiguration = 'RelWithDebInfo'
	backend = 'Vulkan'
	timingSources = @($workload.timingSources)
	workloadManifestHash = Hash-File $Manifest
	liveAdapterHash = Hash-File $LiveAdapter
	captureSchemaVersion = 1
	profilerDetailLevel = 'sampled'
	measuredDurationSeconds = $DurationSeconds
	viewPolicy = [ordered]@{
		diagnosticsView = 'game'
		captureWidth = 480
		captureHeight = 270
		captureFreshness = 'nextFrame'
		shadows = 'runtime-settings'
		visualizer = 'off-during-timing'
	}
	contentHashes = $contentHashes
}
# Resume reuses only complete, identity-matched scenario bundles. Any missing,
# partial, stale, or malformed run causes that scenario to be captured again.
function Test-ReusableScenarioCapture($scenario,[string]$scenarioRoot,[System.Collections.IDictionary]$identity,[int]$requiredRuns) {
	$capturePath = Join-Path $scenarioRoot 'capture.json'
	if(-not (Test-Path -LiteralPath $capturePath)) { return $false }
	try { $capture = Get-Content -Raw -LiteralPath $capturePath | ConvertFrom-Json } catch { return $false }
	if($capture.schemaVersion -ne 1 -or @($capture.runs).Count -ne $requiredRuns) { return $false }
	$expected = [ordered]@{ scenario=[string]$scenario.id; scenarioVersion=1; population=[int]$scenario.population; animationPolicy=[string]$scenario.animationPolicy; routeMix=$scenario.routeMix; specialistMask=if($scenario.PSObject.Properties['specialistMask']){[string]$scenario.specialistMask}else{'none'}; seed=[int]$scenario.seed }
	foreach($entry in $identity.GetEnumerator()) { $expected[$entry.Key] = $entry.Value }
	foreach($entry in $expected.GetEnumerator()) {
		$actual = $capture.identity.($entry.Key)
		if($null -eq $actual -or ($entry.Value | ConvertTo-Json -Compress -Depth 20) -ne ($actual | ConvertTo-Json -Compress -Depth 20)) { return $false }
	}
	for($run=1; $run -le $requiredRuns; ++$run) {
		$runRoot = Join-Path $scenarioRoot ("run-{0:d2}" -f $run)
		$metricsPath = Join-Path $runRoot "run-$run-metrics.json"
		if(-not (Test-Path -LiteralPath $metricsPath)) { return $false }
		try { $metrics = Get-Content -Raw -LiteralPath $metricsPath | ConvertFrom-Json } catch { return $false }
		if($metrics.scenario -ne $scenario.id -or [int]$metrics.run -ne $run -or [UInt64]$metrics.frameTiming.count -eq 0 -or @($metrics.timingStreams).Count -eq 0 -or -not (Test-Path -LiteralPath $metrics.rawSummary)) { return $false }
	}
	return $true
}

# The installed editor accepts only the documented project launch argument. The
# existing MCP adapter performs each scenario capture and writes measured JSON.
$editor = $null
try {
	New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
	$variantClipLists = @()
	$variantSharedClipLists = @()
	foreach($prefab in $variantPrefabs) {
		$prefabPath = Join-Path $Project $prefab
		Require (Test-Path -LiteralPath $prefabPath) "Required benchmark prefab is missing: $prefabPath"
		$prefabDocument = Get-Content -Raw -LiteralPath $prefabPath | ConvertFrom-Json
		$clips = @($prefabDocument.animationBindings.clipAssetIds)
		Require ($clips.Count -gt 0) "Benchmark prefab exposes no animation clips: $prefab"
		$variantClipLists += ,($clips -join ',')
		$sharedClip = if($prefab -eq $directPrefab) {
			# The package default is a short death clip. Use the authored running
			# clip for shared/phase workloads so a 30-second warmup cannot leave the
			# visual gate sampling a terminal pose while the scheduler still advances.
			'59e16ae6dc58318220818bf31231379c'
		} else {
			[string]$clips[0]
		}
		Require ($clips -contains $sharedClip) `
			"Benchmark prefab does not expose its required shared looping clip: $prefab"
		$variantSharedClipLists += ,$sharedClip
	}
	$scenarios = if($ProfilerBenchmark) {
		@([pscustomobject]@{ id = 'profiler-authored'; population = 0; animationPolicy = 'authored'; routeMix = [pscustomobject]@{ direct = 0; denseVirtualGeometry = 0 }; seed = 0 })
	} else { @($workload.scenarios) }
	foreach($scenario in $scenarios){
		if([string]$scenario.animationPolicy -eq 'static-sparse-change' -and -not $scenario.PSObject.Properties['sparseMutationOperation']){ throw "Scenario '$($scenario.id)' lacks sparseMutationOperation; one-percent transform mutation cannot be proven." }
		$scenarioRoot = Join-Path $OutputDirectory $scenario.id; New-Item -ItemType Directory -Force $scenarioRoot | Out-Null
		if($Resume -and (Test-ReusableScenarioCapture $scenario $scenarioRoot $baseIdentity $Runs)) {
			Write-Verbose "Reusing complete identity-matched capture for $($scenario.id)."
			continue
		}
		$scenarioRuns = @()
		for($run=1; $run -le $Runs; ++$run){
			$runRoot = Join-Path $scenarioRoot ("run-{0:d2}" -f $run)
			New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
			$phaseStep = if([string]$scenario.animationPolicy -in @('phase-offset',
				'fully-heterogeneous')){ 1 } else { 0 }
			$adapterArguments = @{
				InstalledEditor = $EditorExecutable; Project = $Project
				OutputDirectory = $runRoot; PrefabInstanceCount = [int]$scenario.population
				# ValidationRoute selects only the dense-VG backend implementation.
				# Leave it empty so the benchmark measures the normal backend while
				# RepresentationRoute controls the direct/dense workload mix.
				ValidationRoute = ''; RepresentationRoute = 'automatic'
				ProfilerCaptureDurationSeconds = $DurationSeconds; ProfilerMaxFrames = 0
				WarmupSeconds = $WarmupSeconds
				# The comparison camera matches the authored Scene camera identity. The
				# adapter creates a temporary validation scene and never edits/saves the
				# authored camera itself.
				ValidationCameraPosition = @(0.0, 1.0, 9.0)
				ValidationCameraTarget = @(0.0, 1.0, 8.0)
				ValidationCameraVerticalFovDegrees = 45.0
				# Runtime profiler frames and animation authority advance only in Play.
				# Enter before warmup so all measured workloads exercise their real
				# scheduler/publication path rather than an idle editor scene.
				EnterPlayMode = $true
				McpPort = $McpPort; CreateValidationScene = (-not $ProfilerBenchmark); CleanupValidationScene = (-not $ProfilerBenchmark)
				ValidationScenePath = "Assets/Scenes/FramePublication-$($scenario.id)-$run.tescene"
			}
			if($ProfilerBenchmark) {
				$adapterArguments.ScenePath = Get-ChildRelativePath $Project $Scene
				$adapterArguments.PrefabInstanceCount = 0
			}
			# One matched quartet uses ABBA order: disabled, recording, recording,
			# disabled. This cancels monotonic thermal/clock drift without averaging
			# away raw block evidence or changing the acceptance threshold.
			$comparisonQuartetIndex = ($run - 1) % 4
			$isDisabledComparisonRun = $Mode -eq 'MatchedRecordingComparison' -and
				($comparisonQuartetIndex -eq 0 -or $comparisonQuartetIndex -eq 3)
			if($isDisabledComparisonRun) {
				$adapterArguments.SkipProfilerCapture = $true
				# The disabled leg still samples Engine::Update through the idle
				# measurement path; it never creates a capture artifact.
				$adapterArguments.ProfilerIdleMeasurementDurationSeconds = $DurationSeconds
				$adapterArguments.ProfilerIdleMeasurementWindowVisible = $false
			} elseif($Mode -eq 'MatchedRecordingComparison') {
				# Recording overhead must use the same bounded closed-window stream as
				# the disabled control; terminal Journal frame windows are retained for
				# duration/count gates but are not a comparable overhead sample.
				$adapterArguments.ProfilerMatchedOverheadMeasurement = $true
			}
			$isWindowOpenRun = $Mode -eq 'MatchedWindowOpen'
			if($Mode -eq 'MatchedWindowOpen') {
				$adapterArguments.ProfilerWindowVisibleDuringCapture = $false
				# The adapter measures five closed/open pairs inside this one recording.
				# Keeping the comparison within one run avoids a redundant second
				# five-minute capture and cancels workload drift at the measurement site.
				$adapterArguments.ProfilerWindowOpenAtSeconds = $OpenProfilerAtSeconds
			}
			$summaryPath = Join-Path $runRoot 'live-visual-summary.json'
			$reuseCompletedRunSummary = $Resume -and
				(Test-Path -LiteralPath $summaryPath -PathType Leaf)
			if(-not $reuseCompletedRunSummary)
			{
				$runningEditors = @(Get-Process -Name $EditorProcessName -ErrorAction SilentlyContinue)
				Require ($runningEditors.Count -le 1) 'Comparable capture observed more than one editor.'
				if($runningEditors.Count -eq 1)
				{
					$adapterArguments.UseExistingEditor = $true
					$editor = $runningEditors[0]
				}
				else
				{
					# The first adapter owns startup so the single editor inherits the
					# run-local MCP preference file and fixed port. It leaves that exact
					# process open for all remaining measured runs.
					$adapterArguments.KeepEditorOpen = $true
				}
			}
			if($ProfilerBenchmark) {
				# Profiler benchmarks run the authored scene exactly as supplied.  Do
				# not inject validation prefabs or mutate its animation settings. The
				# canonical authored workload contains active animation, so changing
				# post-present hashes are required instead of static byte identity.
				$adapterArguments.ExpectAnimatedChange = $true
				if($Mode -eq 'DetailedRecording' -and $DurationSeconds -ge 300) { $adapterArguments.ProfilerHealthCheckpointIntervalSeconds = 60 }
			} elseif([string]$scenario.animationPolicy -eq 'static-sparse-change') {
				$adapterArguments.MeshAssetId = '87f68c2d68360cb2318f0a77fa3eaf0b'
				$adapterArguments.MaterialAssetIds = @('fa653ebd8b3742d77f6b2386ca8a4694',
					'60c9af18ea51c6ee9ac8f3e1da50557e','81139f23f318c10dc998212f5b92814b')
				$adapterArguments.RawMeshGridSpacing = 1.4
				$adapterArguments.SparseTransformMutation = $true
				$adapterArguments.SparseTransformPercentPerFrame = [double]$scenario.sparseMutationOperation.percentPerFrame
			} else {
				$adapterArguments.ExpectAnimatedChange = $true
				$adapterArguments.PrefabVariants = $variantPrefabs
				$adapterArguments.PrefabVariantDefaultClipAssetIds = if(
					[string]$scenario.animationPolicy -in @('shared', 'phase-offset')) {
					$variantSharedClipLists
				} else {
					$variantClipLists
				}
				$adapterArguments.PrefabDefaultClipStartFrameStep = $phaseStep
				$adapterArguments.PrefabSpacing = 1.5
			}
			if(-not $reuseCompletedRunSummary)
			{
				& $LiveAdapter @adapterArguments
				if($LASTEXITCODE -ne 0){throw "Live adapter failed for $($scenario.id) run $run."}
				if($null -eq $editor)
				{
					$runningEditors = @(Get-Process -Name $EditorProcessName -ErrorAction SilentlyContinue)
					Require ($runningEditors.Count -eq 1) 'First live capture did not retain exactly one editor.'
					$editor = $runningEditors[0]
				}
			}
			Require (Test-Path -LiteralPath $summaryPath) `
				"Live adapter produced no canonical summary for $($scenario.id) run $run."
		$document = Get-Content -Raw -LiteralPath $summaryPath | ConvertFrom-Json
		$provenance = $document.diagnosticProvenance
		Require ($null -ne $provenance -and
			[string]$provenance.statsOutput -eq 'renderer-diagnostic-readback' -and
			[string]$provenance.captureOutput -eq 'dedicated-offscreen-png-readback' -and
			[string]$provenance.statsOperation -eq 'renderer.virtualGeometry.stats' -and
			[string]$provenance.captureOperation -eq 'camera.capture_custom') `
			"Live evidence for '$($scenario.id)' lacks explicit renderer/offscreen provenance."
		$routeStats = $document.postCaptureStats.data
			Require ($ProfilerBenchmark -or $null -ne $routeStats) `
				"Live evidence for '$($scenario.id)' lacks post-capture route diagnostics."
			if($ProfilerBenchmark) { $routeStats = [pscustomobject]@{ geometryRoute='authored-scene'; geometryRouteSuppressedPageRequests=0; geometryRouteSource='authored-scene' } }
			$expectedDirect = [int]$scenario.routeMix.direct
			$expectedDense = [int]$scenario.routeMix.denseVirtualGeometry
			$observedDirect = [int]$routeStats.geometryRouteSuppressedPageRequests
			$observedDense = [int]$scenario.population - $observedDirect
			$expectedRoute = if($expectedDirect -gt 0 -and $expectedDense -gt 0) {
				'mixed'
			} elseif($expectedDense -gt 0) {
				'virtual_geometry'
			} else {
				'direct_meshlet'
			}
			if(-not $ProfilerBenchmark) {
				Require ([string]$routeStats.geometryRoute -eq $expectedRoute) "Scenario '$($scenario.id)' reported route '$($routeStats.geometryRoute)' instead of '$expectedRoute'."
				Require ($observedDirect -eq $expectedDirect -and $observedDense -eq $expectedDense) "Scenario '$($scenario.id)' route counts were direct=$observedDirect dense=$observedDense; expected direct=$expectedDirect dense=$expectedDense."
			}

			if(-not $ProfilerBenchmark -and [string]$scenario.animationPolicy -ne 'static-sparse-change')
			{
				$prefabEvidence = @($document.instantiatedPrefabs)
				Require ($prefabEvidence.Count -eq 2) `
					"Scenario '$($scenario.id)' did not publish both prefab variants."
				for($variantIndex = 0; $variantIndex -lt 2; ++$variantIndex)
				{
					$expectedCount = if($variantIndex -eq 0) { $expectedDirect } else { $expectedDense }
					Require ([int]$prefabEvidence[$variantIndex].data.instanceCount -eq $expectedCount) `
						"Scenario '$($scenario.id)' variant $variantIndex has the wrong population."
					Require ([string]$prefabEvidence[$variantIndex].data.path -eq $variantPrefabs[$variantIndex]) `
						"Scenario '$($scenario.id)' variant $variantIndex used an unexpected prefab."
				}
			}

			$profilerSummary = $null
			if(-not $isDisabledComparisonRun) {
				# Read the nested envelope defensively.  StrictMode must report the
				# adapter's actual unavailable/error reason rather than throwing while
				# dereferencing a missing summary property.
				$profilerEvidence = Get-PropertyValue $document 'profilerEvidence'
				$profilerState = [string](Get-PropertyValue $profilerEvidence 'status')
				$profilerReason = [string](Get-PropertyValue $profilerEvidence 'reason')
				$summaryEnvelope = Get-PropertyValue $profilerEvidence 'summary'
				$profilerSummary = Get-PropertyValue (Get-PropertyValue $summaryEnvelope 'data') 'summary'
				Require ($null -ne $profilerSummary) "Live evidence for '$($scenario.id)' has no profiler summary (state='$profilerState', reason='$profilerReason')."
				Require ([bool](Get-PropertyValue $profilerSummary 'complete')) "Live evidence for '$($scenario.id)' has an incomplete profiler summary (state='$profilerState', reason='$profilerReason')."
			}
			$disabledEvidence = if($isDisabledComparisonRun) { Get-PropertyValue $document 'profilerIdleEvidence' } else { $null }
			$disabledTiming = if($isDisabledComparisonRun) {
				$disabledEvidence
			} else { $null }
			if($isDisabledComparisonRun) {
				Require ($null -ne $disabledTiming -and [string](Get-PropertyValue $disabledTiming 'metric') -eq 'engine-cpu-time') "Disabled evidence for '$($scenario.id)' lacks Engine CPU timing."
			}
			$overheadEvidence = if($isDisabledComparisonRun) { $disabledTiming } else { Get-PropertyValue $document 'matchedOverheadEvidence' }
			if($Mode -eq 'MatchedRecordingComparison') {
				Require ($null -ne $overheadEvidence -and [string](Get-PropertyValue $overheadEvidence 'status') -eq 'measured' -and
					[string](Get-PropertyValue $overheadEvidence 'metric') -eq 'engine-cpu-time' -and
					[string](Get-PropertyValue $overheadEvidence 'timingSource') -eq 'Engine::Update editor-excluded wall duration' -and
					[int](Get-PropertyValue $overheadEvidence 'sampleIntervalMilliseconds') -eq 100 -and
					-not [bool](Get-PropertyValue $overheadEvidence 'windowVisible')) "Matched recording overhead evidence must be a closed 100 ms Engine CPU stream."
				Require (Test-BoundedSampleCadence $overheadEvidence 100) "Matched recording overhead samples do not have the required 100 ms cadence."
			}
			$overheadTiming = if($Mode -eq 'MatchedRecordingComparison' -and $null -ne $overheadEvidence) { Get-BoundedTimingBlocks $overheadEvidence 6 } else { $null }
			if($Mode -eq 'MatchedRecordingComparison') { Require ($null -ne $overheadTiming -and $overheadTiming.blockCount -eq 6) "Live evidence for '$($scenario.id)' lacks six complete matched overhead blocks." }
			# Journal V1 intentionally persists bounded terminal windows instead of
			# the old whole-document timing arrays. Represent that authoritative CPU
			# window as one stream while leaving unavailable renderer/GPU lanes absent.
			$timing = @()
			if($null -ne $profilerSummary -and $null -ne (Get-PropertyValue $profilerSummary 'finalWindow')) {
				$finalWindow = Get-PropertyValue $profilerSummary 'finalWindow'
				$timing = @([pscustomobject]@{
					id = 'engine_cpu'
					sampleCount = [UInt64](Get-PropertyValue $finalWindow 'count')
					meanMs = [double](Get-PropertyValue $profilerSummary 'averageFrameMs')
					medianMs = [double](Get-PropertyValue $finalWindow 'medianMs')
					p95Ms = [double](Get-PropertyValue $finalWindow 'p95Ms')
				})
			} else {
				$timing = @([pscustomobject]@{
					id = 'engine_cpu'
					sampleCount = [UInt64](Get-PropertyValue $disabledTiming 'sampleCount')
					meanMs = [double](Get-PropertyValue $disabledTiming 'medianMs')
					medianMs = [double](Get-PropertyValue $disabledTiming 'medianMs')
					p95Ms = [double](Get-PropertyValue $disabledTiming 'p95Ms')
				})
			}
			Require ($timing.Count -gt 0) "Live evidence for '$($scenario.id)' lacks timing streams."
			if([string]$scenario.animationPolicy -eq 'static-sparse-change')
			{
				$mutation = $document.sparseTransformEvidence.data
				Require ($null -ne $mutation -and -not [bool]$mutation.failed) `
					"Static scenario did not publish successful sparse transform evidence."
				Require ([int]$mutation.objectCount -eq [int]$scenario.population -and
					[int]$mutation.batchSize -eq [int]([Math]::Ceiling($scenario.population * 0.01))) `
					"Static scenario did not mutate exactly one percent of its population per frame."
				Require ([UInt64]$mutation.committedFrameCount -ge [UInt64](Get-PropertyValue $profilerSummary 'completeFrameCount')) `
					"Static sparse mutation did not cover every complete profiler frame."
			}
			$stabilityWindowMilliseconds = if($Mode -eq 'MatchedRecordingComparison' -and $DurationSeconds -ge 300) { 60000.0 } else { 300000.0 }
			$firstWindow = if($Mode -eq 'MatchedRecordingComparison' -and $DurationSeconds -ge 300) { Get-EngineCpuWindow $overheadEvidence 0.0 $stabilityWindowMilliseconds } elseif($isDisabledComparisonRun) { Get-EngineCpuWindow $disabledEvidence 0.0 300000.0 } else { Get-PropertyValue $profilerSummary 'firstWindow' }
			$finalWindow = if($Mode -eq 'MatchedRecordingComparison' -and $DurationSeconds -ge 300) {
				$durationMilliseconds=[double](Get-PropertyValue $overheadEvidence 'durationSeconds')*1000.0
				Get-EngineCpuWindow $overheadEvidence ([Math]::Max(0.0,$durationMilliseconds-$stabilityWindowMilliseconds)) ([double]::PositiveInfinity)
			} elseif($isDisabledComparisonRun) {
				$durationMilliseconds=[double](Get-PropertyValue $disabledEvidence 'durationSeconds')*1000.0
				Get-EngineCpuWindow $disabledEvidence ([Math]::Max(0.0,$durationMilliseconds-300000.0)) ([double]::PositiveInfinity)
			} else { Get-PropertyValue $profilerSummary 'finalWindow' }
			if($isDisabledComparisonRun){ Require ($null -ne $firstWindow -and $null -ne $finalWindow) "Disabled evidence for '$($scenario.id)' lacks exact first/final Engine CPU windows." }
			$rendererTiming = @($timing | Where-Object { [string](Get-PropertyValue $_ 'id') -eq 'viewport_cpu' } |
				Select-Object -First 1)
			$gpuTiming = @($timing | Where-Object { [string](Get-PropertyValue $_ 'id') -eq 'viewport_gpu' } |
				Select-Object -First 1)
			$profilerStages = Get-PropertyValue $profilerSummary 'stageTimings'
			$animationStages = @()
			if($null -ne $profilerStages) {
				$animationStages = @($profilerStages | Where-Object {
					[bool](Get-PropertyValue $_ 'available') -and
					[string](Get-PropertyValue $_ 'id') -like 'animation.*' })
			}
			$animationMedian = if($animationStages.Count -gt 0) {
				[double](($animationStages | Measure-Object -Property median -Sum).Sum)
			} else { $null }
			$profilerEvidenceDocument = Get-PropertyValue $document 'profilerEvidence'
			$healthAfterEvidence = Get-PropertyValue $profilerEvidenceDocument 'healthAfter'
			$healthCheckpoints = @((Get-PropertyValue $document 'profilerHealthCheckpoints'))
			$finishInteractiveBlockMs = Get-PropertyValue $profilerEvidenceDocument 'finishInteractiveBlockMilliseconds'
			$finishMcpRoundTripMs = Get-PropertyValue $profilerEvidenceDocument 'finishMcpRoundTripMilliseconds'
			if($isDisabledComparisonRun)
			{
				# The disabled control intentionally never starts a profiler capture;
				# finish timing therefore has an explicit not-applicable value.
				$finishInteractiveBlockMs = $null
				$finishMcpRoundTripMs = $null
			}
			elseif($null -eq $finishInteractiveBlockMs -or $null -eq $finishMcpRoundTripMs)
			{
				throw 'Profiler evidence omitted authoritative finish duration or MCP round-trip duration.'
			}
			if(-not $isDisabledComparisonRun)
			{
				$finishInteractiveBlockMs = [double]$finishInteractiveBlockMs
				$finishMcpRoundTripMs = [double]$finishMcpRoundTripMs
				if([double]::IsNaN($finishInteractiveBlockMs) -or [double]::IsInfinity($finishInteractiveBlockMs) -or
					$finishInteractiveBlockMs -lt 0.0 -or [double]::IsNaN($finishMcpRoundTripMs) -or
					[double]::IsInfinity($finishMcpRoundTripMs) -or $finishMcpRoundTripMs -lt 0.0)
				{
					throw 'Profiler evidence returned invalid finish timing values.'
				}
			}
			$windowEvidence = Get-PropertyValue $profilerEvidenceDocument 'windowEvidence'
			$windowOpenEvidence = Get-PropertyValue $profilerEvidenceDocument 'window'
			$runMetrics = [ordered]@{
				schemaVersion = 1
				scenario = $scenario.id
				run = $run
				mode = if($isDisabledComparisonRun) { 'disabled' } elseif($isWindowOpenRun) { 'recording-window-open' } else { 'recording-window-closed' }
				source = 'live-mcp'
				population = [int]$scenario.population
				routeEvidence = [ordered]@{
					reported = [string]$routeStats.geometryRoute
					direct = $observedDirect
					denseVirtualGeometry = $observedDense
					source = [string]$routeStats.geometryRouteSource
					calibrationIdentity = [UInt64](Get-PropertyValue $routeStats 'geometryRouteCalibrationIdentity')
				}
				frameTiming = [ordered]@{
					count = if($null -ne $profilerSummary){[UInt64](Get-PropertyValue $profilerSummary 'committedFrameCount')}else{[UInt64](Get-PropertyValue $disabledTiming 'sampleCount')}
					meanMs = if($null -ne $profilerSummary){[double](Get-PropertyValue $profilerSummary 'averageFrameMs')}else{[double](Get-PropertyValue $disabledTiming 'medianMs')}
					medianMs = if($null -ne $profilerSummary){[double](Get-PropertyValue $profilerSummary 'frameMedianMs')}else{[double](Get-PropertyValue $disabledTiming 'medianMs')}
					p95Ms = if($null -ne $profilerSummary){[double](Get-PropertyValue $profilerSummary 'frameP95Ms')}else{[double](Get-PropertyValue $disabledTiming 'p95Ms')}
					p99Ms = if($null -ne $profilerSummary){[double](Get-PropertyValue $profilerSummary 'frameP95Ms')}else{[double](Get-PropertyValue $disabledTiming 'p95Ms')}
					worstMs = if($null -ne $profilerSummary){[double](Get-PropertyValue $profilerSummary 'frameP95Ms')}else{[double](Get-PropertyValue $disabledTiming 'maximumMs')}
				}
				timingStreams = $timing
				overheadTiming = if($null -ne $overheadTiming) { [ordered]@{
					metric = 'engine-cpu-time'; timingSource = [string](Get-PropertyValue $overheadEvidence 'timingSource')
					sampleIntervalMilliseconds = [int](Get-PropertyValue $overheadEvidence 'sampleIntervalMilliseconds')
					windowVisible = [bool](Get-PropertyValue $overheadEvidence 'windowVisible'); sampleCount = [UInt64]$overheadTiming.sampleCount; blockCount = [int]$overheadTiming.blockCount; blocks = @($overheadTiming.blocks)
					medianMs = [double]$overheadTiming.medianMs; p95Ms = [double]$overheadTiming.p95Ms
				} } else { $null }
				timings = [ordered]@{
					engineCpu = if($null -ne $profilerSummary) { [double](Get-PropertyValue $profilerSummary 'frameMedianMs') } else { [double](Get-PropertyValue $disabledTiming 'medianMs') }
					rendererCpu = if($rendererTiming.Count -eq 1) { [double]$rendererTiming[0].meanMs } else { $null }
					animationCpu = $animationMedian
					gpuTimestamp = if($gpuTiming.Count -eq 1) { [double]$gpuTiming[0].meanMs } else { $null }
				}
				firstWindow = $firstWindow
				finalWindow = $finalWindow
				window = $windowOpenEvidence
				windowMeasurement = if($null -ne $windowOpenEvidence) {
					[ordered]@{
						status = [string](Get-PropertyValue $windowOpenEvidence 'status')
						preOpen = Get-PropertyValue $windowOpenEvidence 'preOpen'
						postOpen = Get-PropertyValue $windowOpenEvidence 'postOpen'
						medianDeltaPercent = Get-PropertyValue $windowOpenEvidence 'medianDeltaPercent'
						p95DeltaPercent = Get-PropertyValue $windowOpenEvidence 'p95DeltaPercent'
						twoPercentGate = [bool](Get-PropertyValue $windowOpenEvidence 'twoPercentGate')
						maxVisibilityLatencyMilliseconds = Get-PropertyValue $windowOpenEvidence 'maxVisibilityLatencyMilliseconds'
						pairedWindows = Get-PropertyValue $windowOpenEvidence 'pairedWindows'
					}
				} else { $null }
				health = if($null -ne $healthAfterEvidence) {
					$health = $healthAfterEvidence.data
					# Summary counters may be published by the final journal summary
					# rather than the live health snapshot. Prefer health, then fall
					# back to the authoritative completed-summary values.
					$summaryCounters = Get-PropertyValue $profilerSummary 'batching'
					if($null -eq $summaryCounters) { $summaryCounters = $profilerSummary }
					$logicalInputBytes = Get-OptionalUInt64 $health 'logicalInputBytes'; if($null -eq $logicalInputBytes){$logicalInputBytes=Get-OptionalUInt64 $summaryCounters 'logicalInputBytes'}
					$encodedPayloadBytes = Get-OptionalUInt64 $health 'encodedPayloadBytes'; if($null -eq $encodedPayloadBytes){$encodedPayloadBytes=Get-OptionalUInt64 $summaryCounters 'encodedPayloadBytes'}
					$storedSegmentBytes = Get-OptionalUInt64 $health 'storedSegmentBytes'; if($null -eq $storedSegmentBytes){$storedSegmentBytes=Get-OptionalUInt64 $summaryCounters 'storedSegmentBytes'}
					$persistenceOperationCount = Get-OptionalUInt64 $health 'persistenceOperationCount'
					$committedFrameCount = Get-OptionalUInt64 $health 'committedFrames'; if($null -eq $committedFrameCount){$committedFrameCount=Get-OptionalUInt64 $summaryCounters 'committedFrameCount'}
					$framesPerPersistenceOperation = if($null -ne $committedFrameCount -and $persistenceOperationCount -gt 0){[double]$committedFrameCount/[double]$persistenceOperationCount}else{$null}
					$compressionRatio = if($null -ne $logicalInputBytes -and $logicalInputBytes -gt 0 -and $null -ne $storedSegmentBytes){[double]$storedSegmentBytes/[double]$logicalInputBytes}else{$null}
					[ordered]@{
						committedFrames = [UInt64](Get-PropertyValue $health 'committedFrames')
						residentBytes = [UInt64](Get-PropertyValue $health 'residentBytes')
						residentHighWaterBytes = [UInt64](Get-PropertyValue $health 'residentHighWaterBytes')
						pendingBytes = Get-OptionalUInt64 $health 'pendingBytes'
						persistenceLagFrames = [UInt64](Get-PropertyValue $health 'persistenceLagFrames')
						persistenceLagHighWaterFrames = [UInt64](Get-PropertyValue $health 'persistenceLagHighWaterFrames')
						droppedDetailRecords = [UInt64](Get-PropertyValue $health 'droppedDetailRecords')
						rejectedCoreFrames = [UInt64](Get-PropertyValue $health 'rejectedCoreFrames')
						collectionNanoseconds = Get-OptionalUInt64 $health 'collectionNanoseconds'
						sealingNanoseconds = Get-OptionalUInt64 $health 'sealingNanoseconds'
						queueingNanoseconds = Get-OptionalUInt64 $health 'queueingNanoseconds'
						persistenceNanoseconds = Get-OptionalUInt64 $health 'persistenceNanoseconds'
						finalizationNanoseconds = Get-OptionalUInt64 $health 'finalizationNanoseconds'
						persistedBytes = [UInt64](Get-PropertyValue $health 'persistedBytes')
						persistenceOperationCount = $persistenceOperationCount
						persistenceThroughputBytesPerSecond = [UInt64](Get-PropertyValue $health 'persistenceThroughputBytesPerSecond')
						logicalInputBytes = $logicalInputBytes
						encodedPayloadBytes = $encodedPayloadBytes
						storedSegmentBytes = $storedSegmentBytes
						committedFrameCount = $committedFrameCount
						framesPerPersistenceOperation = $framesPerPersistenceOperation
						compressionRatio = $compressionRatio
						terminalFailure = [bool](Get-PropertyValue $health 'terminalFailure')
				}
				} else { $null }
				profilerHealthCheckpoints = $healthCheckpoints
				finishInteractiveBlockMs = $finishInteractiveBlockMs
				finishMcpRoundTripMs = $finishMcpRoundTripMs
				visualReady = [bool]$document.visualContent.allCapturesPassed -and
					[bool]$document.postPresentIdentity.healthyAnimatedPresentation
				warmup = $document.warmupEvidence
				rawSummary = $summaryPath
				rawSummaryHash = Hash-File $summaryPath
			}
			$runMetrics | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath `
				(Join-Path $runRoot "run-$run-metrics.json")
			$scenarioRuns += $runMetrics
		}
		$warmupFrames = @($scenarioRuns | ForEach-Object { [UInt64]$_.warmup.completedFrames })
		$captureIdentity = [ordered]@{}
		foreach($entry in $baseIdentity.GetEnumerator()) { $captureIdentity[$entry.Key] = $entry.Value }
		$captureIdentity.scenario = [string]$scenario.id
		$captureIdentity.scenarioVersion = 1
		$captureIdentity.population = [int]$scenario.population
		$captureIdentity.animationPolicy = [string]$scenario.animationPolicy
		$captureIdentity.routeMix = $scenario.routeMix
		$captureIdentity.specialistMask = if($scenario.PSObject.Properties['specialistMask']) {
			[string]$scenario.specialistMask
		} else { 'none' }
		$captureIdentity.seed = [int]$scenario.seed
		$capture = [ordered]@{
			schemaVersion = 1
			identity = $captureIdentity
			warmup = [ordered]@{
				seconds = $WarmupSeconds
				completedFrames = if($warmupFrames.Count -gt 0) {
					[UInt64](($warmupFrames | Measure-Object -Minimum).Minimum)
				} else { [UInt64]0 }
			}
			runs = $scenarioRuns
		}
		$capture | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath `
			(Join-Path $scenarioRoot 'capture.json')
	}
}
finally {
	if($null -eq $editor)
	{
		# Startup or population can fail before the adapter returns its retained
		# process to this orchestrator. Reacquire the single process we own so a
		# failed evidence run never leaves a hidden editor behind.
		$ownedCandidates = @(Get-Process -Name $EditorProcessName -ErrorAction SilentlyContinue)
		if($ownedCandidates.Count -eq 1) { $editor = $ownedCandidates[0] }
	}
	if($null -ne $editor -and -not $editor.HasExited)
	{
		[void]$editor.CloseMainWindow()
		if(-not $editor.WaitForExit(5000))
		{
			try
			{
				$editor.Kill()
			}
			catch [System.ComponentModel.Win32Exception]
			{
				# Windows can deny Kill after shutdown has already crossed the
				# process teardown boundary. Refresh before treating that benign
				# race as a real cleanup failure.
				$editor.Refresh()
				if(-not $editor.HasExited) { throw }
			}
			if(-not $editor.HasExited) { $editor.WaitForExit() }
		}
	}
}
