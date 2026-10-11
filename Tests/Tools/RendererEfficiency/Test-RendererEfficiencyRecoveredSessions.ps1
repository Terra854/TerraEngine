<#
.SYNOPSIS
Checks explicit recovered-session admission using small synthetic CPU documents.
.DESCRIPTION
Run the installed script after configure/build/install. Supply a fresh WorkRoot:
pwsh -File Build/Tests/Tools/RendererEfficiency/Test-RendererEfficiencyRecoveredSessions.ps1 -InstallRoot Build -WorkRoot out/recovered-session-tests
The test verifies installed helper/library hashes before loading them, then copies
those scripts into isolated synthetic fixture installs. It never launches an
editor, edits the supplied install, or claims GPU/matrix acceptance.
#>
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$InstallRoot,[Parameter(Mandatory=$true)][string]$WorkRoot)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -Force
$installBase=[IO.Path]::GetFullPath($InstallRoot);$root=[IO.Path]::GetFullPath($WorkRoot)
if(Test-Path -LiteralPath $root){throw 'Recovery test WorkRoot must be fresh'}
if($root.StartsWith($installBase.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Recovery test fixtures must be outside the install'}
function InstalledTool([string]$Relative){
	$path=[IO.Path]::GetFullPath((Join-Path $installBase $Relative));if(!$path.StartsWith($installBase.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Installed test dependency escapes root'}
	$ancestor=$path;while($ancestor){if(([IO.File]::GetAttributes($ancestor)-band[IO.FileAttributes]::ReparsePoint)-ne0){throw 'Installed test dependency is redirected'};$ancestor=[IO.Path]::GetDirectoryName($ancestor)}
	$record=@($installSeal.artifacts|Where-Object path -ceq $Relative);if($record.Count-ne1 -or ([IO.FileInfo]$path).Length-ne$record[0].bytes -or (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()-cne$record[0].sha256){throw 'Installed test dependency hash/size mismatch'}
	return $path
}
$sealPath=Join-Path $installBase 'Tests/Tools/RendererEfficiency/renderer-efficiency-artifacts.json'
$ancestor=[IO.Path]::GetFullPath($sealPath);while($ancestor){if(([IO.File]::GetAttributes($ancestor)-band[IO.FileAttributes]::ReparsePoint)-ne0){throw 'Installed test seal is redirected'};$ancestor=[IO.Path]::GetDirectoryName($ancestor)}
$installSeal=[IO.File]::ReadAllText($sealPath)|ConvertFrom-Json
$helper=InstalledTool 'Tests/Tools/RendererEfficiency/RendererEfficiencyRecoveredSessions.ps1'
$lib=InstalledTool 'Tests/Tools/RendererEfficiency/RendererEfficiencyEvidenceStream.ps1'
. $helper
. $lib
InitializeRendererEfficiencyEvidenceStream
$null=Assert-RendererRecoveryPath $root
[IO.Directory]::CreateDirectory($root)|Out-Null
function WriteJson($path,$value){[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))|Out-Null;[IO.File]::WriteAllText($path,($value|ConvertTo-Json -Depth 40 -Compress),[Text.UTF8Encoding]::new($false))}
function Record([string]$path){return @{path=[IO.Path]::GetFullPath($path);sha256=Get-RendererRecoveryHash $path;bytes=([IO.FileInfo]$path).Length}}
function Fixture([switch]$WrongNativeReceiptIdentity) {
	$work=Join-Path $root ([Guid]::NewGuid().ToString('N'));$project=Join-Path $work 'project';$archive=Join-Path $work 'original/artifacts';$install=Join-Path $work 'install';[IO.Directory]::CreateDirectory($project)|Out-Null;[IO.Directory]::CreateDirectory($archive)|Out-Null
	$op='a'*32;$name='P1-shadow-invalidation-0-Candidate';$rev='b'*40
	$manifestPath=Join-Path $work 'frozen/workloads.json';$schemaPath=Join-Path $work 'frozen/Schemas/evidence.schema.json';$stepsPath=Join-Path $work 'frozen/RendererEfficiencyStageSteps.ps1'
	WriteJson $manifestPath @{schemaVersion=1;workloads=@(@{id='shadow-invalidation';stage='P1';seed=1;steps=@(@{id='s0';action='stable'})})};WriteJson $schemaPath @{schemaVersion=1};WriteJson $stepsPath @{test='stagehelper'}
	$seal=@{revision=$rev;artifacts=@(@{path='Editor/TerraEngine-Editor.exe';bytes=1;sha256='c'*64},@{path='Editor/TerraEngine.dll';bytes=2;sha256='d'*64},@{path='Engine/Shaders/Default.mesh.tspf';bytes=3;sha256='e'*64},@{path='Tests/Fixtures/RendererEfficiency/Assets/input';bytes=4;sha256='f'*64})}
	$sealPath=Join-Path $work 'frozen/renderer-efficiency-artifacts.json';WriteJson $sealPath $seal;$native=Get-RendererRecoverySealIdentity ([IO.File]::ReadAllText($sealPath)|ConvertFrom-Json)
	$fixtureHash=Get-RendererRecoveryHash $manifestPath
	$identity=[ordered]@{runId=$name;runIndex=0;fixtureId='shadow-invalidation';fixtureHash=@{status='Valid';value=$fixtureHash};seed=1;stage='P1';mode='Candidate';referenceRevision=@{status='Valid';value=$rev};candidateRevision=@{status='Valid';value=$rev}}
	foreach($field in @('installedBinaryHash','installedShaderClosureHash','installedAssetClosureHash')){$identity[$field]=@{status='Valid';value=$native[$field]}}
	$options=@{validationPreparation='Serial';shadowReuse='Enabled'}
	$sessionPath=Join-Path $work 'original/session.json';WriteJson $sessionPath @{name=$name;status='Failed';evidencePath=$null}
	$summaryPath=Join-Path $work 'original/summary.json';WriteJson $summaryPath @{status='Failed';sessions=@(@{name=$name;status='Failed'})}
	$operationPath=Join-Path $work 'original/operation.json';WriteJson $operationPath @{state='Completed';status='Valid';operationId=$op;fixtureId='shadow-invalidation';fixtureHash=$fixtureHash;completedSteps=1;exportedFrames=1;exportedArtifacts=1;projectRoot=$project}
	$lifePath=Join-Path $work 'original/runner.lifecycle.json';WriteJson $lifePath @{kind='RendererEfficiencyRunnerLifecycle';schemaVersion=1;optionsRestored=$true;normalClose=$true;watchdogFailed=$false;processExitCode=0;ownedEditorProcessId=123;restoredSettingsGeneration=1;restoredRendererSessionGeneration=2;operationId=$op;runId=$name;fixtureId='shadow-invalidation';runIndex=0;preparation='Serial';stage='P1';mode='Candidate';originalOptions=$options;restoredOptions=$options}
	$markerPath=Join-Path $project 'renderer-efficiency-project.json';WriteJson $markerPath @{projectRoot=$project;kind='RendererEfficiencyValidationProject';fixtureId='shadow-invalidation';fixtureHash=$fixtureHash;seed=1}
	$frozenPath=Join-Path $work 'frozen/receipt.json';WriteJson $frozenPath @{status='Frozen35GradingInputsOnly';files=@(@{path='workloads.json';sha256=Get-RendererRecoveryHash $manifestPath},@{path='Schemas/evidence.schema.json';sha256=Get-RendererRecoveryHash $schemaPath},@{path='RendererEfficiencyStageSteps.ps1';sha256=Get-RendererRecoveryHash $stepsPath},@{path='renderer-efficiency-artifacts.json';sha256=Get-RendererRecoveryHash $sealPath})}
	$artifactPath=Join-Path $project 'raw/test.bin';[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($artifactPath))|Out-Null;[IO.File]::WriteAllBytes($artifactPath,[byte[]](1,2,3));$artifact=@{path='raw/test.bin';sha256=Get-RendererRecoveryHash $artifactPath;kind='Counters';status='Valid';reason=''}
	$nativeReceiptMode=if($WrongNativeReceiptIdentity){'Reference'}else{'Candidate'}
	$nativeReceiptBytes=(@{schemaVersion=1;operationId=$op;fixtureId='shadow-invalidation';fixtureHash=$fixtureHash;stage='P1';mode=$nativeReceiptMode;runId=$name;runIndex=0;seed=1;receipts=@()}|ConvertTo-Json -Depth 10 -Compress)
	$tempReceipt=Join-Path $project 'receipt-temp.json';[IO.File]::WriteAllText($tempReceipt,$nativeReceiptBytes,[Text.UTF8Encoding]::new($false));$nativeReceiptHash=Get-RendererRecoveryHash $tempReceipt
	$nativeReceiptRelative='renderer-efficiency-evidence/'+$op+'/'+$nativeReceiptHash+'.receipts.json';$nativeReceiptPath=Join-Path $project $nativeReceiptRelative;[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($nativeReceiptPath))|Out-Null;[IO.File]::Move($tempReceipt,$nativeReceiptPath)
	$nativeReceiptDescriptor=@{path=$nativeReceiptRelative;sha256=$nativeReceiptHash;kind='Counters';status='Valid';reason=''}
	$document=@{schemaVersion=1;identity=$identity;requestedOptions=$options;measurementPolicy=@{warmupFrames=60;measuredFrames=300};frames=@(@{fixtureStepId='s0';stepFrame=0;effectiveOptions=$options;artifacts=@($artifact)});successCriteria=@{};artifacts=@($artifact,$nativeReceiptDescriptor)}
	$nativePath=Join-Path $project ($op+'-evidence.json');WriteJson $nativePath $document
	$archiveArtifact=Join-Path $archive 'raw/test.bin';[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($archiveArtifact))|Out-Null;[IO.File]::Copy($artifactPath,$archiveArtifact)
	$archiveNativeReceipt=Join-Path $archive $nativeReceiptRelative;[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($archiveNativeReceipt))|Out-Null;[IO.File]::Copy($nativeReceiptPath,$archiveNativeReceipt)
	$archiveLife=Join-Path $archive 'runner.lifecycle.json';[IO.File]::Copy($lifePath,$archiveLife);$lifecycle=@{path='runner.lifecycle.json';sha256=Get-RendererRecoveryHash $lifePath;kind='Counters';status='Valid';reason=''}
	$recoveredPath=Join-Path $archive ($op+'-recovered-evidence.json');[RendererEfficiencyOfflineRecoveryStream]::Write($nativePath,$recoveredPath,($lifecycle|ConvertTo-Json -Compress))
	$indexPath=Join-Path $work 'original/index.jsonl';$entries=@(@{path=$artifact.path;sha256=$artifact.sha256;bytes=3;kind='Counters';sourcePath=$artifactPath;archivePath=$archiveArtifact;rootReferenced=$true;frameReferenced=$true},@{path='runner.lifecycle.json';sha256=$lifecycle.sha256;bytes=([IO.FileInfo]$lifePath).Length;kind='Counters';sourcePath=$lifePath;archivePath=$archiveLife;rootReferenced=$true;frameReferenced=$false});[IO.File]::WriteAllLines($indexPath,@($entries|ForEach-Object {$_|ConvertTo-Json -Compress}),[Text.UTF8Encoding]::new($false))
	$streamPath=Join-Path $install 'Tests/Tools/RendererEfficiency/RendererEfficiencyEvidenceStream.ps1';[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($streamPath))|Out-Null;[IO.File]::Copy($lib,$streamPath)
	$entries+=@{path=$nativeReceiptRelative;sha256=$nativeReceiptHash;bytes=([IO.FileInfo]$nativeReceiptPath).Length;kind='Counters';sourcePath=$nativeReceiptPath;archivePath=$archiveNativeReceipt;rootReferenced=$true;frameReferenced=$false};[IO.File]::WriteAllLines($indexPath,@($entries|ForEach-Object {$_|ConvertTo-Json -Compress}),[Text.UTF8Encoding]::new($false))
	$toolPath=Join-Path $work 'original/tool.ps1';WriteJson $toolPath @{synthetic='collector'}
	$receipt=@{schemaVersion=1;kind='RendererEfficiencyOfflineRecoveryCollection';status='RecoveredCollected';nativeMatrixAccepted=$false;operationId=$op;fixtureId='shadow-invalidation';stage='P1';mode='Candidate';runId=$name;runIndex=0;preparation='Serial';identity=$identity;requestedOptions=$options;inputs=@{originalSession=Record $sessionPath;originalSummary=Record $summaryPath;operation=Record $operationPath;lifecycle=Record $lifePath;ownershipMarker=Record $markerPath;nativeEvidence=Record $nativePath;frozenInputReceipt=Record $frozenPath;installSeal=Record $sealPath;tool=Record $toolPath;toolLibrary=Record (Resolve-Path $lib)};installIdentity=$native;recoveredEvidence=Record $recoveredPath;artifactIndex=(Record $indexPath);transformation='UnchangedNativeRelativePathsAndActualLifecycleAppend';nativeFrameCount=1}
	$receipt.artifactIndex.uniqueArtifacts=3;$receipt.artifactIndex.frameArtifactReferences=1;$receipt.artifactIndex.rootArtifactReferences=3;$receipt.artifactIndex.originalRootArtifactReferences=2
	$receiptPath=Join-Path $work 'original/recovery.json';WriteJson $receiptPath $receipt
	$inventory=@{root=$install;identity=$native;receiptHash='1'*64;files=@{'Tests/Tools/RendererEfficiency/RendererEfficiencyEvidenceStream.ps1'=@{hash=Get-RendererRecoveryHash $streamPath;length=([IO.FileInfo]$streamPath).Length}}}
	return @{work=$work;receipt=$receipt;receiptPath=$receiptPath;inventory=$inventory;manifest=$manifestPath;schema=$schemaPath;steps=$stepsPath;output=Join-Path $work 'fresh';artifact=$archiveArtifact;recovered=$recoveredPath;native=$nativePath}
}
function Run($f,[switch]$ValidateOnly){WriteJson $f.receiptPath $f.receipt;return Import-RendererEfficiencyRecoveredSession -ValidateOnly:$ValidateOnly -ReceiptPath $f.receiptPath -Inventory $f.inventory -OutputRoot $f.output -Stage P1 -FixtureId shadow-invalidation -RunIndex 0 -Mode Candidate -Preparation Serial -ManifestPath $f.manifest -SchemaPath $f.schema -StageStepsPath $f.steps -InspectEvidence {param($p,$cb)[RendererEfficiencyOfflineRecoveryStream]::Inspect($p,$cb)} -VerifyTransformation {param($s,$r,$j)[RendererEfficiencyOfflineRecoveryStream]::VerifyUnchangedInsertion($s,$r,$j)}}
$passed=0
function Case($name,$body,$reject=$false){$f=Fixture;try{& $body $f|Out-Null;if($reject){throw 'Expected rejection accepted'};'PASS '+$name;$script:passed++}catch{if($reject -and $_.Exception.Message-ne'Expected rejection accepted'){'PASS rejected '+$name;$script:passed++}else{throw}}}
Case 'valid immutable archive copied and originals unchanged' {param($f)$original=Get-RendererRecoveryHash $f.receipt.inputs.originalSession.path;$r=Run $f;if($r.status-cne'RecoveredCollected' -or (Get-RendererRecoveryHash $r.evidencePath)-cne$f.receipt.recoveredEvidence.sha256 -or (Get-RendererRecoveryHash $f.receipt.inputs.originalSession.path)-cne$original){throw 'Positive invariant'}}
Case 'wrong planned preparation' {param($f)$f.receipt.preparation='Concurrent';Run $f} $true
Case 'failed actual lifecycle' {param($f)
	# Each numeric native receipt rejects zero, booleans and numeric strings.
	# Keep this inside the cohesive lifecycle case instead of inflating case count.
	foreach($field in @('ownedEditorProcessId','restoredSettingsGeneration','restoredRendererSessionGeneration')){foreach($value in @(0,$false,'1')){
		$g=Fixture;$p=$g.receipt.inputs.lifecycle.path;$j=[IO.File]::ReadAllText($p)|ConvertFrom-Json;$j.$field=$value;WriteJson $p $j;$g.receipt.inputs.lifecycle=Record $p
		$rejected=$false;try{Run $g|Out-Null}catch{$rejected=$_.Exception.Message.Contains('positive native lifecycle receipt')}
		if(!$rejected){throw 'Expected rejection accepted'}
	}}
	$p=$f.receipt.inputs.lifecycle.path;$j=[IO.File]::ReadAllText($p)|ConvertFrom-Json;$j.normalClose=$false;WriteJson $p $j;$f.receipt.inputs.lifecycle=Record $p;Run $f
} $true
Case 'native closure changed' {param($f)$f.inventory.identity.installedBinaryHash='0'*64;Run $f} $true
Case 'artifact altered' {param($f)[IO.File]::WriteAllBytes($f.artifact,[byte[]](9));Run $f} $true
Case 'unindexed native descriptor' {param($f)$j=[IO.File]::ReadAllText($f.recovered)|ConvertFrom-Json;$j.frames[0].artifacts[0].path='missing.bin';WriteJson $f.recovered $j;$f.receipt.recoveredEvidence=Record $f.recovered;Run $f} $true
Case 'frame modified despite receipt rehash' {param($f)$j=[IO.File]::ReadAllText($f.recovered)|ConvertFrom-Json;$j.frames[0].stepFrame=1;WriteJson $f.recovered $j;$f.receipt.recoveredEvidence=Record $f.recovered;Run $f} $true
Case 'fresh destination cannot overwrite' {param($f)[IO.Directory]::CreateDirectory((Join-Path $f.output 'P1-shadow-invalidation-0-Candidate'))|Out-Null;Run $f} $true
Case 'frozen schema changed' {param($f)WriteJson $f.schema @{changed=$true};Run $f} $true
Case 'changed revision rejects' {param($f)$f.inventory.identity.revision='0'*40;Run $f} $true
Case 'ValidateOnly checks closure without creating output' {param($f)$r=Run $f -ValidateOnly;if($r.status-cne'PlannedRecoveredCollected' -or (Test-Path -LiteralPath $f.output)){throw 'ValidateOnly wrote output'}}
Case 'original native count mismatch' {param($f)$p=$f.receipt.inputs.operation.path;$j=[IO.File]::ReadAllText($p)|ConvertFrom-Json;$j.exportedFrames=2;WriteJson $p $j;$f.receipt.inputs.operation=Record $p;Run $f} $true
Case 'raw whitespace mutation is rejected after honest rehash' {param($f)$raw=[IO.File]::ReadAllText($f.recovered);[IO.File]::WriteAllText($f.recovered,$raw.Replace('"schemaVersion":1','"schemaVersion" : 1'),[Text.UTF8Encoding]::new($false));$f.receipt.recoveredEvidence=Record $f.recovered;Run $f} $true
Case 'descriptor count cannot omit index entry' {param($f)$f.receipt.artifactIndex.uniqueArtifacts=1;Run $f} $true
Case 'claimed lifecycle without actual restore is rejected' {param($f)$p=$f.receipt.inputs.lifecycle.path;$j=[IO.File]::ReadAllText($p)|ConvertFrom-Json;$j.restoredOptions.shadowReuse='AlwaysRefresh';WriteJson $p $j;$f.receipt.inputs.lifecycle=Record $p;Run $f} $true
Case 'identity claims cannot replace actual streamed identity' {param($f)$f.receipt.identity.runId='fake';Run $f} $true
Case 'installed utility changed is rejected' {param($f)$p=Join-Path $f.inventory.root 'Tests/Tools/RendererEfficiency/RendererEfficiencyEvidenceStream.ps1';[IO.File]::AppendAllText($p,' changed');Run $f} $true
Case 'tool-only whole seal changes remain compatible' {param($f)$f.inventory.receiptHash='2'*64;$r=Run $f;if($r.status-cne'RecoveredCollected'){throw 'Tool seal rejected'}}
Case 'original native root count mismatch' {param($f)
	# Even fully rehashed source/archive/index bytes cannot admit another run's
	# sidecar. This subcheck keeps the count-policy test tied to actual identity.
	$wrong=Fixture -WrongNativeReceiptIdentity;$rejected=$false;try{Run $wrong|Out-Null}catch{$rejected=$_.Exception.Message.Contains('native receipt identity mismatch')}
	if(!$rejected){throw 'Expected rejection accepted'}
	$p=$f.receipt.inputs.operation.path;$j=[IO.File]::ReadAllText($p)|ConvertFrom-Json;$j.exportedArtifacts=2;WriteJson $p $j;$f.receipt.inputs.operation=Record $p;Run $f
} $true
Case 'wrong actual lifecycle mode' {param($f)$p=$f.receipt.inputs.lifecycle.path;$j=[IO.File]::ReadAllText($p)|ConvertFrom-Json;$j.mode='Reference';WriteJson $p $j;$f.receipt.inputs.lifecycle=Record $p;Run $f} $true
Case 'claimed recovered fixture mismatch' {param($f)$f.receipt.fixtureId='multiview-lights';Run $f} $true
Case 'original source artifact altered' {param($f)$p=Join-Path $f.work 'project/raw/test.bin';[IO.File]::WriteAllBytes($p,[byte[]](8));Run $f} $true
Case 'index duplicate paths rejected' {param($f)$p=$f.receipt.artifactIndex.path;$line=[IO.File]::ReadAllLines($p)[0];[IO.File]::AppendAllText($p,$line+"`n");$updated=Record $p;foreach($field in @('path','sha256','bytes')){$f.receipt.artifactIndex[$field]=$updated[$field]};Run $f} $true
Case 'archive index cannot redirect source ownership' {param($f)$p=$f.receipt.artifactIndex.path;$lines=[IO.File]::ReadAllLines($p);$row=$lines[0]|ConvertFrom-Json;$copy=Join-Path $f.work 'samebytes.bin';[IO.File]::Copy($row.sourcePath,$copy);$row.sourcePath=$copy;$lines[0]=$row|ConvertTo-Json -Compress;[IO.File]::WriteAllLines($p,$lines);$updated=Record $p;foreach($field in @('path','sha256','bytes')){$f.receipt.artifactIndex[$field]=$updated[$field]};Run $f} $true
Case 'recovery cannot write into original archive' {param($f)$f.output=[IO.Path]::GetDirectoryName($f.recovered);Run $f} $true
Case 'real junction missing destination rejected' {param($f)$target=Join-Path $f.work 'junction-target';$link=Join-Path $f.work 'junction';[IO.Directory]::CreateDirectory($target)|Out-Null;New-Item -ItemType Junction -Path $link -Target $target|Out-Null;try{$f.output=Join-Path $link 'missing';Run $f}finally{if(([IO.File]::GetAttributes($link)-band[IO.FileAttributes]::ReparsePoint)-eq0){throw 'Fixture junction changed'};[IO.Directory]::Delete($link,$false)}} $true
Write-Output "$passed recovered session admission cases passed; synthetic CPU data only"
