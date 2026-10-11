[CmdletBinding()]
param([string]$WorkRoot='', [string]$Grader='', [string]$Manifest='', [string[]]$CaseFilter=@())

# CPU-only supporting regressions. The test inspector reads deliberately
# synthetic journal-shaped JSON, not native JournalV1. Its pinned bytes exercise
# the real grader's parser trust/membership boundary; these tests never establish
# genuine capture provenance, GPU stage proof, or full native acceptance.
# Run with either bundled pwsh or Windows PowerShell 5.1 and a fresh WorkRoot.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$utf8=[Text.UTF8Encoding]::new($false)
if(!$Grader){$Grader=Join-Path $PSScriptRoot 'Compare-RendererEfficiencyBaselineTiming.ps1'}
if(!$Manifest){$Manifest=Join-Path $PSScriptRoot '../../Fixtures/RendererEfficiency/workloads.json'}
if(!$WorkRoot){$WorkRoot=Join-Path ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))) ('out/evidence/baseline-timing-tests/'+[Guid]::NewGuid().ToString('N'))}
$WorkRoot=[IO.Path]::GetFullPath($WorkRoot)
if(Test-Path -LiteralPath $WorkRoot){throw 'WorkRoot must be fresh'}
[IO.Directory]::CreateDirectory((Join-Path $WorkRoot 'journals'))|Out-Null
function WriteJson([string]$Path,$Value){[IO.File]::WriteAllText($Path,($Value|ConvertTo-Json -Depth 30 -Compress),$utf8)}
function Clone($Value){return ($Value|ConvertTo-Json -Depth 30 -Compress|ConvertFrom-Json)}
$inspector=Join-Path $WorkRoot 'synthetic-inspector.ps1'
$inspectorText=@'
# Test-only substitute. Never ship or use this parser for native acceptance.
$ErrorActionPreference='Stop'
$v=[IO.File]::ReadAllText($args[1])|ConvertFrom-Json
$offset=[int]$args[3];$count=[int]$args[5]
if($offset -lt 0 -or $offset+$count -gt @($v.frames).Count){exit 1}
@{formatIdentifier='profilerJournalV1';formatVersion=1;complete=$true;frames=@($v.frames[$offset..($offset+$count-1)])}|ConvertTo-Json -Depth 8 -Compress
exit 0
'@
[IO.File]::WriteAllText($inspector,$inspectorText,$utf8)
$inspectorSha=(Get-FileHash $inspector -Algorithm SHA256).Hash.ToLowerInvariant()
$archive='a'*64;$candidateClosure='b'*64
$workloads=@((Get-Content $Manifest -Raw|ConvertFrom-Json).workloads|Where-Object representativeForSC008)
function MakeDocument([string]$Role){
	$runs=@()
	foreach($fixture in $workloads){foreach($preparation in @('Serial','Concurrent')){for($run=0;$run -lt 3;++$run){
		$frames=@(for($i=0;$i -lt 360;++$i){[pscustomobject]@{frameIndex=$i;durationMs=10.0;complete=$true}})
		$name=$Role+'-'+$fixture.id+'-'+$preparation+'-'+$run
		$relative='journals/'+$name+'.synthetic.json';$path=Join-Path $WorkRoot $relative
		WriteJson $path @{testOnlyNonce=$name;frames=$frames}
		$runs+=[pscustomobject]@{fixtureId=$fixture.id;seed=$fixture.seed;preparation=$preparation;runIndex=$run;viewIds=@($fixture.cameraIds);
			conditions=[pscustomobject]@{adapter='SyntheticAdapter';driver='SyntheticDriver';resolution=[pscustomobject]@{width=1920;height=1080};cameraHash=('c'*64);sceneHash=('d'*64);inputHash=('e'*64);settings=[pscustomobject]@{vsync=$false;scene=$true;game=$true};simulationPolicy=[pscustomobject]@{deterministic=$true;fixedDeltaTimeSeconds=(1.0/60);temporalJitterEnabled=$false;animationPolicy='Frozen authored state'}};
			journal=[pscustomobject]@{path=$relative;sha256=(Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant();bytes=([IO.FileInfo]$path).Length;startOrdinal=0};frames=$frames}
	}}}
	return [pscustomobject]@{schemaVersion=1;role=$Role;metricDomain='engine_cpu';clock='ProfilerClock';boundary='Engine::Update.BeginFrame-EndFrame.ExcludingEditorWork';warmupFrames=60;measuredFrames=300;runsPerFixture=3;installClosureSha256=$(if($Role -eq 'Reference'){$archive}else{$candidateClosure});archiveClosureSha256=$archive;runs=$runs}
}
$reference=MakeDocument Reference;$candidate=MakeDocument Candidate
$referencePath=Join-Path $WorkRoot 'reference.json';WriteJson $referencePath $reference
$hostExecutable=(Get-Process -Id $PID).Path
$results=[Collections.Generic.List[object]]::new()
function Case([string]$Name,[int]$Expected,[scriptblock]$Change,[scriptblock]$ReferenceChange=$null){
	if($CaseFilter.Count -gt 0 -and $Name -cnotin $CaseFilter){return}
	$document=Clone $candidate
	& $Change $document
	$path=Join-Path $WorkRoot ($Name+'.json');$output=Join-Path $WorkRoot ($Name+'-comparison.json')
	WriteJson $path $document
	# Build each pair separately so policy tests cannot mutate another case.
	$caseReference=Clone $reference
	if($ReferenceChange){& $ReferenceChange $caseReference}
	$caseReferencePath=Join-Path $WorkRoot ($Name+'-reference.json')
	WriteJson $caseReferencePath $caseReference
	& $hostExecutable -NoProfile -ExecutionPolicy Bypass -File $Grader -Reference $caseReferencePath -Candidate $path -ArchiveClosureSha256 $archive -JournalInspector $inspector -JournalInspectorSha256 $inspectorSha -Manifest $Manifest -Output $output *> (Join-Path $WorkRoot ($Name+'.log'))
	$actual=$LASTEXITCODE;$comparison=Get-Content $output -Raw|ConvertFrom-Json
	if($actual -ne $Expected){throw "Case $Name expected $Expected, got $actual`: $($comparison.reason)"}
	if($comparison.stageProof -or $comparison.fullAcceptance){throw 'CPU timing gate invented native acceptance'}
	$results.Add([ordered]@{name=$Name;exitCode=$actual;status='Passed';gradingStatus=$comparison.status})
	Write-Output "[PASS] $Name"
}
function AlterMeasuredJournal($Document,[string]$Name,[scriptblock]$Edit){
	$row=$Document.runs[0]
	& $Edit $row.frames
	$relative='journals/'+$Name+'.synthetic.json';$path=Join-Path $WorkRoot $relative
	WriteJson $path @{testOnlyNonce=$Name;frames=$row.frames}
	$row.journal.path=$relative;$row.journal.sha256=(Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant();$row.journal.bytes=([IO.FileInfo]$path).Length
}
Case exact-matched 0 {param($v)}
Case boundary-five-percent 0 {param($v) AlterMeasuredJournal $v boundary {param($f)for($i=60;$i -lt 360;++$i){$f[$i].durationMs=10.5}}}
Case median-regression 1 {param($v) AlterMeasuredJournal $v median {param($f)for($i=60;$i -lt 360;++$i){$f[$i].durationMs=10.5001}}}
Case p95-regression 1 {param($v) AlterMeasuredJournal $v p95 {param($f)for($i=344;$i -lt 360;++$i){$f[$i].durationMs=11.0}}}
Case summary-only 3 {param($v)$v.runs[0].frames=@()}
Case missing-run 3 {param($v)$v.runs=@($v.runs|Select-Object -Skip 1)}
Case duplicate-run 3 {param($v)$v.runs[1]=$v.runs[0]}
Case wrong-view 3 {param($v)$v.runs[0].viewIds=@('unknown-view')}
Case missing-config 3 {param($v)$v.runs[0].conditions.settings=[pscustomobject]@{}}
Case wrong-domain 3 {param($v)$v.metricDomain=@('engine_cpu','gpu')}
Case threshold-spoof 3 {param($v)$v|Add-Member maximumRatio 2.0}
Case nonfinite-duration 3 {param($v)$v.runs[0].frames[60].durationMs='Infinity'}
Case zero-duration 3 {param($v)$v.runs[0].frames[60].durationMs=0.0}
Case incomplete-frame 3 {param($v)$v.runs[0].frames[60].complete=$false}
Case duplicate-index 3 {param($v)$v.runs[0].frames[60].frameIndex=59}
Case gap-index 3 {param($v)$v.runs[0].frames[60].frameIndex=61}
Case fabricated-raw-timing 3 {param($v)$v.runs[0].frames[60].durationMs=9.0}
Case wrong-journal-sha 3 {param($v)$v.runs[0].journal.sha256='f'*64}
Case wrong-journal-bytes 3 {param($v)$v.runs[0].journal.bytes+=1}
Case escaping-journal 3 {param($v)$v.runs[0].journal.path='../escaped.journal'}
Case reused-journal 3 {param($v)$v.runs[1].journal=$v.runs[0].journal}
Case wrong-archive 3 {param($v)$v.archiveClosureSha256='f'*64}
Case same-runtime 3 {param($v)$v.installClosureSha256=$archive}
Case mismatched-camera 3 {param($v)$v.runs[0].conditions.cameraHash='f'*64}
Case wrong-seed 3 {param($v)$v.runs[0].seed=1}
Case wrong-simulation 3 {param($v)$v.runs[0].conditions.simulationPolicy.deterministic=$false}
# The explicit paused mode declares no advancement without inventing delta time.
function FreezePose($Document){
	foreach($run in $Document.runs){$run.conditions.simulationPolicy=[pscustomobject]@{mode='PausedFrozenPose';advancesSimulation=$false;deterministic=$true;temporalJitterEnabled=$false;animationPolicy='FrozenPose.NoStepOrSeekDuringCapture'}}
}
Case frozen-matched 0 {param($v) FreezePose $v} {param($v) FreezePose $v}
Case frozen-advances-simulation 3 {param($v) FreezePose $v; $v.runs[0].conditions.simulationPolicy.advancesSimulation=$true} {param($v) FreezePose $v}
Case frozen-step-declaration 3 {param($v) FreezePose $v; $v.runs[0].conditions.simulationPolicy.animationPolicy='StepDuringCapture'} {param($v) FreezePose $v}
Case frozen-mismatched-policy 3 {param($v) FreezePose $v}
Case zero-without-frozen 3 {param($v) $v.runs[0].conditions.simulationPolicy.fixedDeltaTimeSeconds=0.0}
if($results.Count -eq 0){throw 'No matching contract case was executed'}
WriteJson (Join-Path $WorkRoot 'result.json') @{status='Passed';scope='Synthetic CPU grader regressions only';caseCount=$results.Count;graderSha256=(Get-FileHash $Grader -Algorithm SHA256).Hash.ToLowerInvariant();cases=$results.ToArray()}
Write-Output "$($results.Count) baseline timing contracts passed."
