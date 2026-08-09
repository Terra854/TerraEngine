[CmdletBinding()]
param([string]$Project,[string]$Scene,[string]$Manifest,[string]$OutputDirectory,[string]$InstallRoot,[string]$EditorExecutable,[string]$CameraHash,[string]$EditorProcessName='TerraEngine-Editor',[string]$LiveAdapter=(Join-Path $PSScriptRoot '../VirtualGeometry/CaptureVirtualGeometryLiveVisual.ps1'),[int]$WarmupSeconds=30,[int]$DurationSeconds=60,[int]$Runs=5,[switch]$DryRun,[switch]$Resume)
$ErrorActionPreference='Stop'
function Require([bool]$ok,[string]$message){ if(-not $ok){ throw $message } }
function Hash-File([string]$path){ (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Hash-Text([string]$text){
	$sha=[Security.Cryptography.SHA256]::Create()
	try { ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)))).Replace('-','').ToLowerInvariant() }
	finally { $sha.Dispose() }
}
Require (Test-Path -LiteralPath $Manifest) "Manifest is missing: $Manifest"
$workload=Get-Content -Raw -LiteralPath $Manifest | ConvertFrom-Json
Require ($workload.schemaVersion -eq 1) 'Unsupported workload schema.'
Require ($workload.scenarios.Count -eq 6) 'Exactly six canonical scenarios are required.'
Require ($Runs -eq 5) 'Comparable captures require five measured runs.'
Require ($WarmupSeconds -ge 30) 'Warmup must be at least 30 seconds.'
Require ($DurationSeconds -ge 60) 'Measured duration must be at least 60 seconds.'
Require (Test-Path -LiteralPath $Project) "Project is missing: $Project"
Require (Test-Path -LiteralPath $Scene) "Scene is missing: $Scene"
Require (-not [string]::IsNullOrWhiteSpace($CameraHash)) 'CameraHash is required; camera identity is never inferred.'
$existingEditors = @(Get-Process -Name $EditorProcessName -ErrorAction SilentlyContinue)
Require ($existingEditors.Count -eq 0) 'Comparable capture starts and owns exactly one installed editor.'
$adapterText = Get-Content -Raw -LiteralPath $LiveAdapter
Require ($adapterText -match 'RepresentationRoute' -and $adapterText -match 'PrefabVariants') 'Live adapter lacks required route/variant mutation operations; cannot enforce workload identity.'
if($DryRun){ Write-Output ([ordered]@{valid=$true;dryRun=$true;scenarios=@($workload.scenarios.id);samplesCreated=0} | ConvertTo-Json -Depth 5); exit 0 }
Require (-not [string]::IsNullOrWhiteSpace($InstallRoot)) 'InstallRoot is required for capture.'
Require (Test-Path -LiteralPath $InstallRoot) "Install root is missing: $InstallRoot"
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
if(-not(Test-Path $LiveAdapter)){throw "Live MCP adapter is missing: $LiveAdapter"}; if([string]::IsNullOrWhiteSpace($EditorExecutable)){throw 'Installed editor executable is required.'}; if(-not(Test-Path $EditorExecutable)){throw "Editor executable is missing: $EditorExecutable"}
$projectIdentityPath = Join-Path $Project 'game.json'
Require (Test-Path -LiteralPath $projectIdentityPath) "Project identity is missing: $projectIdentityPath"
$installedEngine = Get-ChildItem -LiteralPath $InstallRoot -Recurse -File -Filter 'TerraEngine-Engine.dll' |
	Select-Object -First 1
$videoController = Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue |
	Sort-Object Name | Select-Object -First 1
$powerState = (& powercfg /getactivescheme 2>$null | Out-String).Trim()
if([string]::IsNullOrWhiteSpace($powerState)) { $powerState = 'unavailable' }
$sourceCommit = (& git rev-parse HEAD 2>$null | Select-Object -First 1).Trim()
$directPrefab = 'Assets/ValidationModels/m03/BaseCharacter_Generated/BaseCharacter.teprefab'
$densePrefab = 'Assets/ValidationModels/DenseAnimatedVG/CesiumMan_Generated/CesiumMan.teprefab'
$variantPrefabs = @($directPrefab, $densePrefab)
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
	installedHash = Hash-File $EditorExecutable
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
	foreach($scenario in $workload.scenarios){
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
				McpPort = 43155; CreateValidationScene = $true; CleanupValidationScene = $true
				ValidationScenePath = "Assets/Scenes/FramePublication-$($scenario.id)-$run.tescene"
			}
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
			if([string]$scenario.animationPolicy -eq 'static-sparse-change') {
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
			& $LiveAdapter @adapterArguments
			if($LASTEXITCODE -ne 0){throw "Live adapter failed for $($scenario.id) run $run."}
			if($null -eq $editor)
			{
				$runningEditors = @(Get-Process -Name $EditorProcessName -ErrorAction SilentlyContinue)
				Require ($runningEditors.Count -eq 1) 'First live capture did not retain exactly one editor.'
				$editor = $runningEditors[0]
			}
			$summaryPath = Join-Path $runRoot 'live-visual-summary.json'
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
			Require ($null -ne $routeStats) `
				"Live evidence for '$($scenario.id)' lacks post-capture route diagnostics."
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
			Require ([string]$routeStats.geometryRoute -eq $expectedRoute) `
				"Scenario '$($scenario.id)' reported route '$($routeStats.geometryRoute)' instead of '$expectedRoute'."
			Require ($observedDirect -eq $expectedDirect -and $observedDense -eq $expectedDense) `
				"Scenario '$($scenario.id)' route counts were direct=$observedDirect dense=$observedDense; expected direct=$expectedDirect dense=$expectedDense."

			if([string]$scenario.animationPolicy -ne 'static-sparse-change')
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

			$profilerSummary = $document.profilerEvidence.summary.data.summary
			Require ($null -ne $profilerSummary -and [bool]$profilerSummary.complete) `
				"Live evidence for '$($scenario.id)' lacks a complete profiler summary."
			$timing = @($profilerSummary.timingStreams)
			Require ($timing.Count -gt 0) `
				"Live evidence for '$($scenario.id)' lacks profiler timing streams."
			if([string]$scenario.animationPolicy -eq 'static-sparse-change')
			{
				$mutation = $document.sparseTransformEvidence.data
				Require ($null -ne $mutation -and -not [bool]$mutation.failed) `
					"Static scenario did not publish successful sparse transform evidence."
				Require ([int]$mutation.objectCount -eq [int]$scenario.population -and
					[int]$mutation.batchSize -eq [int]([Math]::Ceiling($scenario.population * 0.01))) `
					"Static scenario did not mutate exactly one percent of its population per frame."
				Require ([UInt64]$mutation.committedFrameCount -ge [UInt64]$profilerSummary.completeFrameCount) `
					"Static sparse mutation did not cover every complete profiler frame."
			}
			$rendererTiming = @($timing | Where-Object { [string]$_.id -eq 'viewport_cpu' } |
				Select-Object -First 1)
			$gpuTiming = @($timing | Where-Object { [string]$_.id -eq 'viewport_gpu' } |
				Select-Object -First 1)
			$animationStages = @($profilerSummary.stageTimings | Where-Object {
				[bool]$_.available -and [string]$_.id -like 'animation.*' })
			$animationMedian = if($animationStages.Count -gt 0) {
				[double](($animationStages | Measure-Object -Property median -Sum).Sum)
			} else { $null }
			$runMetrics = [ordered]@{
				schemaVersion = 1
				scenario = $scenario.id
				run = $run
				source = 'live-mcp'
				population = [int]$scenario.population
				routeEvidence = [ordered]@{
					reported = [string]$routeStats.geometryRoute
					direct = $observedDirect
					denseVirtualGeometry = $observedDense
					source = [string]$routeStats.geometryRouteSource
					calibrationIdentity = [UInt64]$routeStats.geometryRouteCalibrationIdentity
				}
				frameTiming = [ordered]@{
					count = [UInt64]$profilerSummary.completeFrameCount
					meanMs = [double]$profilerSummary.averageFrameMs
					medianMs = [double]$profilerSummary.frameMedianMs
					p95Ms = [double]$profilerSummary.frameP95Ms
					p99Ms = [double]$profilerSummary.frameP99Ms
					worstMs = [double]$profilerSummary.frameWorstMs
				}
				timingStreams = $timing
				timings = [ordered]@{
					engineCpu = [double]$profilerSummary.frameMedianMs
					rendererCpu = if($rendererTiming.Count -eq 1) { [double]$rendererTiming[0].meanMs } else { $null }
					animationCpu = $animationMedian
					gpuTimestamp = if($gpuTiming.Count -eq 1) { [double]$gpuTiming[0].meanMs } else { $null }
				}
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
		if(-not $editor.WaitForExit(5000)) { $editor.Kill(); $editor.WaitForExit() }
	}
}
