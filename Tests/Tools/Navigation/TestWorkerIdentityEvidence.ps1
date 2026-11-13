# Five bounded schema cases plus one actual scheduled-navigation worker identity qualification.
# Schema fixture numbers and native identity observations are NOT performance measurements.
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
$script:NativeWorkerIdentityQualified=$false
$script:NativeWorkerCommands=[Collections.Generic.List[object]]::new()
$script:NativeWorkerReceipts=[Collections.Generic.List[object]]::new()
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
	[IO.File]::WriteAllText((Join-Path $WorkRoot 'result.json'), ([ordered]@{fixtureProtocolOnly=$false;evidenceKind='mixed-schema-and-native-worker-identity';schemaProtocolCases=5;nativeQualificationCases=1;nativeWorkerIdentityQualified=$script:NativeWorkerIdentityQualified;nativePerformanceMeasured=$false;nativeCommands=@($script:NativeWorkerCommands.ToArray());nativeReceipts=@($script:NativeWorkerReceipts.ToArray());selected=$script:Cases.Count;failures=$failures;cases=@($script:Cases.ToArray());functionProvenance=@($script:FunctionProvenance.ToArray())}|ConvertTo-Json -Depth 32),$script:Utf8NoBom)
	Write-Output "$($script:Cases.Count) selected cases, $failures failures; five schema cases and one native worker-identity qualification; native performance NOT measured."
	if($failures) { exit 1 }
}

if([string]::IsNullOrWhiteSpace($StressProducer)) { $StressProducer=Join-Path $script:RepoRoot 'Tests/Tools/Physics/RunInstalledEditorPhysicsStress.ps1' }
. (Get-ActualProducerFunctions $StressProducer @('Get-PropertyValue','Get-WorkerResolutionCheck','Get-PhysicsRuntimeWorkerIdentity'))
foreach($case in @(
	[pscustomobject]@{name='MatchedCountControl';requested='2';observed=2;exact=$true;hardware=8},
	[pscustomobject]@{name='RequestedFourObservedTwoMustReject';requested='4';observed=2;exact=$false;hardware=8},
	[pscustomobject]@{name='MissingRuntimeObservationCannotBecomeConfiguredSuccess';requested='4';observed=$null;exact=$false;hardware=8},
	[pscustomobject]@{name='MaxUsesChildHardwareObservation';requested='max';observed=3;exact=$true;hardware=3},
	[pscustomobject]@{name='ZeroAliasUsesChildHardwareObservation';requested='0';observed=3;exact=$true;hardware=3}
)) {
	Invoke-ProtocolCase "Physics.WorkerIdentity.$($case.name)" {
		# Explicit copied-child-schema fixture, NOT evidence of native worker execution.
		$runtime=if($null -eq $case.observed){$null}else{[pscustomobject]@{observedPoolCapacity=$case.observed;executingWorkerIndices=@(0);executionEvidenceSource='fixture child telemetry';executionAvailability='observed';hardwareConcurrency=$case.hardware}}
		$identity=Get-PhysicsRuntimeWorkerIdentity -Requested $case.requested -RuntimeEvidence $runtime
		$record=([ordered]@{requestedWorkerCount=$case.requested;summary=[ordered]@{actualWorkerCount=$identity.actualWorkerCount;workerIdentity=$identity.workerIdentity;hardwareEvidence=[ordered]@{logicalProcessors=$case.hardware}}}|ConvertTo-Json -Depth 16|ConvertFrom-Json)
		$check=Get-WorkerResolutionCheck $record
		if($null -eq $case.observed) {
			Assert-Protocol (-not $check.available -and -not $check.exact) 'Missing runtime capacity must remain unavailable, never copied from request intent.'
		} else {
			Assert-Protocol ($identity.actualWorkerCount -eq $case.observed -and [bool]$check.exact -eq $case.exact) 'Actual producer + comparability gate must reject requested/reported capacity mismatch.'
		}
	}
}
Invoke-ProtocolCase 'Navigation.EqualAgentsUseActualPoolObservationAndRejectDifferentIdentity' {
	if([string]::IsNullOrWhiteSpace($NavigationExecutable)) { throw 'Native qualification deferred: supply the matching actual navigation producer executable; no native worker identity is synthesized.' }
	if([string]::IsNullOrWhiteSpace($NavigationManifest)) { $NavigationManifest=Join-Path $script:RepoRoot 'Tests/Fixtures/Navigation/manifest.json' }
	$records=@()
	foreach($requested in @(2,4)) {
		$output=Join-Path $WorkRoot "navigation-pool-$requested.json"
		$start=[Diagnostics.ProcessStartInfo]::new();$start.FileName=[IO.Path]::GetFullPath($NavigationExecutable);$start.UseShellExecute=$false
		$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
		$start.EnvironmentVariables['TERRAENGINE_THREADPOOL_WORKERS']=[string]$requested
		$start.CreateNoWindow=$true;$start.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
		$start.WorkingDirectory=Split-Path $start.FileName
		$childArguments=@('--qualification-worker-identity','--lane','headless','--manifest',[IO.Path]::GetFullPath($NavigationManifest),'--output',$output,'--matrices','semantic','--agents','256','--duration-seconds','1','--cycles','1','--idle-pairs','1')
		foreach($argument in $childArguments){$start.ArgumentList.Add($argument)}
		$script:NativeWorkerCommands.Add([ordered]@{executable=$start.FileName;executableSha256=(Get-FileHash -LiteralPath $start.FileName -Algorithm SHA256).Hash;arguments=$childArguments;requestedWorkerIntent=$requested;output=$output;scope='bounded actual scheduled navigation identity, not performance'})
		$child=[Diagnostics.Process]::Start($start)
		try {
			$stdout=$child.StandardOutput.ReadToEndAsync();$stderr=$child.StandardError.ReadToEndAsync()
			if(-not $child.WaitForExit(45000)) {$child.Kill();$child.WaitForExit();throw 'Owned bounded native worker-identity child timed out.'}
			[IO.File]::WriteAllText((Join-Path $WorkRoot "navigation-pool-$requested.stdout.log"),$stdout.GetAwaiter().GetResult())
			[IO.File]::WriteAllText((Join-Path $WorkRoot "navigation-pool-$requested.stderr.log"),$stderr.GetAwaiter().GetResult())
			# Retain failed/unverified native receipts too; a failed child can never
			# become qualified, but its real observations must remain reviewable.
			if(Test-Path -LiteralPath $output -PathType Leaf){$script:NativeWorkerReceipts.Add([ordered]@{path=$output;sha256=(Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash;requestedWorkerIntent=$requested;exitCode=$child.ExitCode})}
			Assert-Protocol ($child.ExitCode -eq 0) 'Actual headless semantic producer must complete before its identity can qualify.'
		} finally {$child.Dispose()}
		$record=Get-Content -LiteralPath $output -Raw|ConvertFrom-Json
		Assert-Protocol ($record.schemaVersion -ceq 'navigation-worker-identity-v1' -and $record.status -ceq 'Passed' -and $record.success -eq $true -and $record.nativePerformanceMeasured -eq $false) 'Actual opt-in scheduled qualification must complete without claiming performance measurement.'
		Assert-Protocol ($record.content.agents -eq 256 -and $record.content.submittedRouteCount -eq 256 -and $record.content.completedRouteCount -eq 256 -and $record.content.poolCapacityAfterCleanup -eq 0) 'Both children must complete256 real owned routes and settle their owned pool.'
		Assert-Protocol ($record.content.executionMode -ceq 'scheduled-worker-identity' -and $record.content.droppedNavigationWorkerRecords -eq 0) 'Qualification must retain the complete bounded real navigation-worker record set.'
		$ranges=@($record.content.navigationWorkerExecutionRecords)
		Assert-Protocol ($ranges.Count -gt 0) 'Idle pool initialization cannot satisfy qualification without useful completed navigation-worker ranges.'
		foreach($range in $ranges){Assert-Protocol ($range.descriptorId -ceq 'NavigationSystem/Navigation/Prepare' -and $range.stage -ceq 'SubmittedObjectRoutes' -and $range.workerIdentityAvailable -eq $true -and $range.workerIdentitySource -ceq 'ThreadPool::CurrentExecutingWorkerIndex inside actual production range callback' -and $range.completed -eq $true -and $range.itemEnd -gt $range.itemBegin -and $range.usefulWorkItemCount -gt 0 -and $range.completionSequence -gt 0) 'Every retained identity must come from a completed useful production navigation range.'}
		$observation=$record.content.runtimeWorkerEvidence
		Assert-Protocol ($null -ne $observation -and $null -ne $observation.observedPoolCapacity) 'Producer must retain actual runtime pool capacity separately from request/agents.'
		Assert-Protocol ($record.identity.workerCount -eq $observation.observedPoolCapacity) 'Navigation identity must match observed pool capacity, including honest zero/main-thread limitation.'
		Assert-Protocol ($observation.observedPoolCapacity -eq $requested) 'Opt-in child must observe the requested actual pool capacity, never relabel a mismatch.'
		$rangeWorkers=@($ranges|ForEach-Object {$_.workerId}|Sort-Object -Unique)
		Assert-Protocol ((ConvertTo-Json -InputObject @($record.content.navigationExecutingWorkerIndices|Sort-Object) -Compress) -ceq (ConvertTo-Json -InputObject $rangeWorkers -Compress)) 'Scoped executing indices must equal actual useful navigation range identities.'
		foreach($index in $rangeWorkers){Assert-Protocol ($index -ge 0 -and $index -lt $observation.observedPoolCapacity) 'Actual executing worker index must fit the independently observed pool.'}
		Assert-Protocol (@($observation.executingWorkerIndices).Count -le $observation.observedPoolCapacity) 'Pool capacity must not be labeled as executing worker evidence.'
		if($observation.observedPoolCapacity -eq 0) {Assert-Protocol (@($observation.executingWorkerIndices).Count -eq 0) 'A zero-worker pool cannot claim any executed worker identity.'}
		if(@($observation.executingWorkerIndices).Count -eq 0) {
			Assert-Protocol ($observation.executionAvailability -like 'unavailable*') 'No dispatch observations must remain explicitly unavailable, even when pool capacity is positive.'
		} else {
			Assert-Protocol ($observation.executionAvailability -eq 'observed' -and @($observation.executingWorkerIndices|Sort-Object -Unique).Count -eq @($observation.executingWorkerIndices).Count) 'Executing identities must come from unique real dispatch observations, not configured capacity.'
		}
		$records+=,$record
	}
	Assert-Protocol ($records[0].content.runtimeWorkerEvidence.observedPoolCapacity -ne $records[1].content.runtimeWorkerEvidence.observedPoolCapacity) 'Qualification requires two genuinely different observed pools; two zero/main-thread runs are an explicit unavailable condition.'
	foreach($name in @('buildSha256','sourceSha256','settingsSha256','workloadSha256','renderer','powerMode')){Assert-Protocol ($records[0].identity.$name -ceq $records[1].identity.$name) "Only actual worker pool may differ in this bounded identity comparison: $name"}
	Assert-Protocol ($records[0].lane -ceq $records[1].lane -and $records[0].workloadId -ceq $records[1].workloadId) 'Both children must execute the same actual qualification workload.'

	. (Get-ActualProducerFunctions (Join-Path $script:RepoRoot 'Tests/Tools/Navigation/CompareNavigationEvidence.ps1') @('Get-PropertyValue','Get-IdentityKey'))
	Assert-Protocol ((Get-IdentityKey $records[0] $false) -cne (Get-IdentityKey $records[1] $false)) 'Actual navigation comparability key must reject equal-agent evidence from different runtime worker pools.'
	$script:NativeWorkerIdentityQualified=$true
}
Complete-ProtocolTests
