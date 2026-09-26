# RunInstalledLocomotionAcceptance.ps1 - source-free real-application evidence.
# Requires PowerShell 7 and an installed RelWithDebInfo Build tree. The editor,
# exporter and exported launcher are actual application processes; CTest or a
# test executable is never substituted for Play/export qualification.
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$InstallRoot,
	[Parameter(Mandatory)][string]$OutputDirectory,
	[string]$FixtureAssetRoot,
	[ValidateSet('smoke','qualification')][string]$Scenario = 'smoke',
	[ValidateRange(1,8)][int]$WorkerCount = 8,
	[ValidateRange(1024,65535)][int]$McpPort = 43167
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$install = [IO.Path]::GetFullPath($InstallRoot)
$output = [IO.Path]::GetFullPath($OutputDirectory)
$installPrefix = $install.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
if($output.Equals($install,[StringComparison]::OrdinalIgnoreCase) -or
	$output.StartsWith($installPrefix,[StringComparison]::OrdinalIgnoreCase)) {
	throw 'Evidence and disposable projects must be outside the install tree.'
}
$manifestPath = Join-Path $install 'Tests/Fixtures/Locomotion/workloads.json'
$generator = Join-Path $install 'Tests/Tools/Locomotion/NewLocomotionWorkloadProject.ps1'
$authoringDriver = Join-Path $install 'Tests/Tools/Locomotion/InvokeLocomotionAuthoringAcceptance.ps1'
$editor = Join-Path $install 'Editor/TerraEngine-Editor.exe'
$exporter = Join-Path $install 'Editor/TerraEngine-Export.exe'
foreach($path in @($manifestPath,$generator,$editor,$exporter)) {
	if(!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing installed input: $path" }
}
if([string]::IsNullOrWhiteSpace($FixtureAssetRoot))
{
	$FixtureAssetRoot = Join-Path $install 'Tests/Fixtures/Locomotion/Generated'
}
$assets = [IO.Path]::GetFullPath($FixtureAssetRoot)
if(!(Test-Path -LiteralPath $assets -PathType Container))
{
	throw "Missing installed cooked locomotion fixture assets: $assets"
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json -AsHashtable
if($manifest.fixtureId -cne 'locomotion-060-v1' -or [int]$manifest.schemaVersion -ne 1)
{
	throw 'Installed locomotion workload manifest has the wrong frozen identity or schema.'
}
$expectedCases = @($manifest.lanes.semantic.caseIds)
if($expectedCases.Count -ne 65 -or @($expectedCases | Select-Object -Unique).Count -ne 65)
{
	throw 'Frozen semantic workload must contain exactly 65 unique named cases.'
}
if($Scenario -eq 'qualification' -and $WorkerCount -ne 8)
{
	throw 'Full qualification includes 1/4/8-worker semantic runs and uses eight workers for sustained/paired timing.'
}

New-Item -ItemType Directory -Path $output -Force | Out-Null
$runId = 'locomotion-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
$work = Join-Path $output $runId
New-Item -ItemType Directory -Path $work | Out-Null
$summaryPath = Join-Path $work 'acceptance.json'
$summary = [ordered]@{ schemaVersion=1; runId=$runId; fixtureId=$manifest.fixtureId
	status='running'; qualificationRequested=($Scenario -eq 'qualification')
	manifestSha256=(Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
	installRoot=$install; artifactSha256=[ordered]@{
		engine=(Get-FileHash -LiteralPath (Join-Path $install 'Editor/TerraEngine.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
		editor=(Get-FileHash -LiteralPath $editor -Algorithm SHA256).Hash.ToLowerInvariant()
		exporter=(Get-FileHash -LiteralPath $exporter -Algorithm SHA256).Hash.ToLowerInvariant() }
	lanes=@(); pairs=@(); failures=@() }

function Invoke-InstalledProcess([string]$Executable,[string[]]$Arguments,[string]$WorkingDirectory,
	[string]$LogPrefix,[int]$TimeoutSeconds,[int]$Workers)
{
	$start = [Diagnostics.ProcessStartInfo]::new()
	$start.FileName = $Executable
	$start.WorkingDirectory = $WorkingDirectory
	$start.UseShellExecute = $false
	$start.CreateNoWindow = $true
	$start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
	$start.RedirectStandardOutput = $true
	$start.RedirectStandardError = $true
	$start.Environment['TERRAENGINE_THREADPOOL_WORKERS'] = [string]$Workers
	foreach($argument in $Arguments) { $start.ArgumentList.Add($argument) }
	$process = [Diagnostics.Process]::new()
	$process.StartInfo = $start
	$started = [DateTime]::UtcNow
	if(!$process.Start()) { throw "Could not launch installed process: $Executable" }
	$stdoutFile = [IO.FileStream]::new("$LogPrefix.stdout.log",[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::Read)
	$stderrFile = [IO.FileStream]::new("$LogPrefix.stderr.log",[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::Read)
	$stdout = $process.StandardOutput.BaseStream.CopyToAsync($stdoutFile)
	$stderr = $process.StandardError.BaseStream.CopyToAsync($stderrFile)
	$peakBytes = [int64]0
	try {
		while(!$process.WaitForExit(1000)) {
			try { $process.Refresh(); $peakBytes = [Math]::Max($peakBytes,$process.PeakWorkingSet64) }
			catch [InvalidOperationException] { }
			if(([DateTime]::UtcNow-$started).TotalSeconds -gt $TimeoutSeconds) {
				# This exact child, not an executable name or broad process group, is
				# terminated on timeout. Retained logs still show the incomplete run.
				$process.Kill($true)
				$process.WaitForExit()
				throw "Installed process exceeded $TimeoutSeconds seconds: $Executable"
			}
		}
		return [ordered]@{ exitCode=$process.ExitCode
			wallSeconds=([DateTime]::UtcNow-$started).TotalSeconds
			peakWorkingSetBytes=$peakBytes
			stdout="$LogPrefix.stdout.log"; stderr="$LogPrefix.stderr.log" }
	} finally {
		[void]$stdout.GetAwaiter().GetResult()
		[void]$stderr.GetAwaiter().GetResult()
		$stdoutFile.Dispose(); $stderrFile.Dispose(); $process.Dispose()
	}
}

function Assert-Report([string]$Path,[string]$ExpectedMode,[string]$ExpectedRunId,
	[int]$ExpectedWorkers,[string]$ExpectedScenario,[string]$ExpectedCaseId)
{
	if(!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Installed application omitted report: $Path" }
	$report = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable
	if($report.schemaVersion -ne 1 -or $report.runId -cne $ExpectedRunId -or
		$report.executionMode -cne $ExpectedMode -or $report.scenario -cne $ExpectedScenario -or
		$report.actualWorkerCount -ne $ExpectedWorkers -or
		$report.pending -or !$report.succeeded -or !$report.orderlyShutdown) {
		throw "Installed application returned failed, missing or mismatched identity evidence: $Path"
	}
	if($ExpectedScenario -eq 'smoke') {
		if($report.executedCaseCount -ne 1 -or @($report.cases).Count -ne 1 -or
			$report.cases[0].id -cne 'intent.manual-world' -or !$report.cases[0].passed -or
			$report.cases[0].lastPhysicalTick -lt ($report.cases[0].startPhysicalTick+120) -or
			$report.cases[0].finalX -le ($report.cases[0].initialX+0.5) -or
			@($report.rawTrace).Count -lt 120 -or $report.qualificationComplete) {
			throw "Smoke report lacks real physical movement, tick or receipt trace: $Path"
		}
		foreach($sequence in @(1,2,3)) {
			if(@($report.rawTrace | Where-Object { $_.event -ceq 'admitted' -and $_.sequence -eq $sequence }).Count -ne 1 -or
				@($report.rawTrace | Where-Object { $_.event -ceq 'applied' -and $_.sequence -eq $sequence }).Count -ne 1) {
				throw "Smoke report lacks exact queued/applied receipt sequence $sequence : $Path"
			}
		}
	}
	if($ExpectedScenario -eq 'semantic') {
		if($report.executedCaseCount -ne 1 -or @($report.cases).Count -ne 1 -or
			$report.cases[0].id -cne $ExpectedCaseId -or !$report.cases[0].passed -or
			$report.cases[0].lastPhysicalTick -le $report.cases[0].startPhysicalTick -or
			@($report.rawTrace).Count -eq 0 -or $report.qualificationComplete) {
			throw "Semantic report did not prove exactly the requested live case $ExpectedCaseId : $Path"
		}
	}
	return $report
}

function New-InstalledProject([string]$Name,[int]$Workers,[string]$StartupScene)
{
	$project = Join-Path $work $Name
	& $generator -OutputDirectory $project -ManifestPath $manifestPath -FixtureAssetRoot $assets `
		-WorkerCount $Workers -Population 1000 -InstallRoot $install -StartupScene $StartupScene
	# A PowerShell generator throws on failure. LASTEXITCODE belongs to native
	# children and may contain a stale value from an earlier installed process.
	$metadataPath = Join-Path $project 'Assets/Locomotion/workload-metadata.json'
	if(!(Test-Path -LiteralPath $metadataPath -PathType Leaf)) { throw "Generator omitted $metadataPath" }
	$metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json -AsHashtable
	if($metadata.sourceManifestSha256 -cne $summary.manifestSha256 -or
		$metadata.startupScene -cne "Scenes/$StartupScene.tescene" -or @($metadata.caseIds).Count -ne 65 -or
		@($metadata.actors).Count -ne 1000 -or @($metadata.terrain.sourceObjectIds).Count -ne 63) {
		throw "Generated project metadata does not match frozen workload identity: $metadataPath"
	}
	return [ordered]@{ path=$project; metadata=$metadata; metadataPath=$metadataPath }
}

function New-InstalledExport([string]$Name,[string]$ProjectPath)
{
	$package = Join-Path $work "Package-$Name"
	$profile = Join-Path $ProjectPath 'BuildProfiles/locomotion.json'
	if(!(Test-Path -LiteralPath $profile -PathType Leaf)) { throw "Generator omitted export profile: $profile" }
	$export = Invoke-InstalledProcess $exporter @('--project',$ProjectPath,'--profile',$profile,
		'--output',$package,'--evidence',(Join-Path $work "export-$Name"),'--run-id',"$runId-$Name") `
		(Split-Path -Parent $exporter) (Join-Path $work "$Name-exporter") 900 $WorkerCount
	if($export.exitCode -ne 0) { throw "Installed exporter failed for $Name; see $($export.stderr)" }
	$launcher = Join-Path $package ((Split-Path $ProjectPath -Leaf) + '.exe')
	if(!(Test-Path -LiteralPath $launcher -PathType Leaf) -or
		!(Test-Path -LiteralPath ([IO.Path]::ChangeExtension($launcher,'.tepak')) -PathType Leaf)) {
		throw "Export omitted launcher/archive pair for $Name"
	}
	return [ordered]@{ package=$package; launcher=$launcher; export=$export }
}

function Invoke-LocomotionLane([string]$Lane,[string]$ScenarioId,[int]$Workers,
	[string]$ProjectPath,[string]$Launcher,[string]$ActorObjectId,[string]$CaseId = '')
{
	$caseRunId = "$runId-$ScenarioId-$CaseId-$Lane-$Workers"
	$optionsPath = Join-Path $work "$caseRunId-options.json"
	$reportPath = Join-Path $work "$caseRunId-report.json"
	$options = [ordered]@{ runId=$caseRunId; scenario=$ScenarioId
		executionMode=$(if($Lane -eq 'editor'){ 'editor-play' }else{ 'export' })
		requiredWorkerCount=$Workers; fixtureManifestPath=$manifestPath
		scenePath=$(if($ScenarioId -eq 'smoke'){ 'Scenes/Smoke.tescene' }else{ 'Scenes/Semantic.tescene' })
		smokeObjectId=$ActorObjectId; qualificationRequested=($Scenario -eq 'qualification' -and $ScenarioId -ne 'smoke') }
	if($CaseId) { $options.caseId = $CaseId }
	$options | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $optionsPath -Encoding utf8
	$executable = if($Lane -eq 'editor') { $editor } else { $Launcher }
	$arguments = if($Lane -eq 'editor') {
		@('--project',$ProjectPath,'--validate-editor-locomotion',$optionsPath,$reportPath)
	} else { @('--validate-export-locomotion',$optionsPath,$reportPath) }
	if($Lane -eq 'export-headless') { $arguments += '--headless-cpu-fallback' }
	$timeout = if($ScenarioId -eq 'sustained') { 1500 } elseif($ScenarioId -eq 'paired') { 420 } else { 300 }
	$process = Invoke-InstalledProcess $executable $arguments (Split-Path -Parent $executable) `
		(Join-Path $work $caseRunId) $timeout $Workers
	if($process.exitCode -ne 0) { throw "Installed $Lane process failed ($($process.exitCode)); see $($process.stderr)" }
	$expectedMode = if($Lane -eq 'editor') { 'editor-play' } else { 'export' }
	$report = Assert-Report $reportPath $expectedMode $caseRunId $Workers $ScenarioId $CaseId
	return [ordered]@{ lane=$Lane; scenario=$ScenarioId; workers=$Workers
		caseId=$CaseId; options=$optionsPath; report=$reportPath; process=$process; evidence=$report }
}

try {
	$smoke = New-InstalledProject 'LocomotionSmoke' $WorkerCount 'Smoke'
	$smokeId = [string]$smoke.metadata.roles.validationSmoke.objectId
	if($smokeId -cne '60060000000000000000000000000001')
	{
		throw 'The generated smoke actor UUID does not match the frozen validation identity.'
	}
	$smokeExport = New-InstalledExport 'smoke' $smoke.path
	foreach($lane in @('editor','export-rendered','export-headless')) {
		$summary.lanes += Invoke-LocomotionLane $lane 'smoke' $WorkerCount $smoke.path $smokeExport.launcher $smokeId
	}
	if($Scenario -eq 'qualification') {
		# Every following lane must be supplied by the real app harness. A smoke
		# report is never promoted to semantic, sustained or paired acceptance.
		if(!(Test-Path -LiteralPath $authoringDriver -PathType Leaf))
		{
			throw "Installed MCP authoring driver is missing: $authoringDriver"
		}
		$authoringRoot = Join-Path $work 'authoring-mcp'
		$authoring = Invoke-InstalledProcess (Join-Path $PSHOME 'pwsh.exe') `
			@('-NoProfile','-File',$authoringDriver,'-InstallRoot',$install,
				'-ProjectPath',$smoke.path,'-OutputDirectory',$authoringRoot,'-Port',"$McpPort") `
			$work (Join-Path $work 'authoring-mcp') 1500 $WorkerCount
		$authoringReports = @(Get-ChildItem -LiteralPath $authoringRoot -Recurse -Filter 'authoring-acceptance.json' -File)
		if($authoringReports.Count -ne 1) { throw 'The installed editor MCP authoring run omitted its unique report.' }
		$summary['authoring'] = Get-Content -LiteralPath $authoringReports[0].FullName -Raw | ConvertFrom-Json -AsHashtable
		$summary['authoringProcess'] = $authoring
		if($authoring.exitCode -ne 0 -or $summary.authoring.status -cne 'passed')
		{
			$summary.failures += 'Installed editor MCP authoring gate is failed or incomplete.'
		}
		foreach($workers in @(1,4,8)) {
			$project = New-InstalledProject "LocomotionSemantic-$workers" $workers 'Semantic'
			$export = New-InstalledExport "semantic-$workers" $project.path
			$semanticId = [string]$project.metadata.roles.semantic.objectId
			foreach($caseId in $expectedCases) {
				foreach($lane in @('editor','export-rendered','export-headless')) {
					$summary.lanes += Invoke-LocomotionLane $lane 'semantic' $workers $project.path $export.launcher $semanticId $caseId
				}
			}
		}
		# SC-007/008 are deliberately not inferred from semantic reports. Until
		# the app emits their exact durations, counters and matched replay traces,
		# qualification remains incomplete and the runner exits nonzero.
		throw 'SC-007 sustained and SC-008 paired real-application reports are not yet implemented.'
	}
	$summary.status = 'smoke_passed_not_qualification'
} catch {
	$summary.status = 'failed'
	$summary.failures += $_.Exception.Message
	throw
} finally {
	$summary | ConvertTo-Json -Depth 48 | Set-Content -LiteralPath $summaryPath -Encoding utf8
}

Write-Output "Installed locomotion smoke evidence: $summaryPath"
