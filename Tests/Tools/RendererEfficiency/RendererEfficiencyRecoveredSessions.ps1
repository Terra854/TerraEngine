<#
Explicit recovery admission for one exact planned session in a fresh suite.
Dot-source this file. The caller supplies the reviewed bounded JSON inspector;
this helper validates its actual metadata/descriptors, never a Passed flag.
It copies archive bytes without changing native identities or failed records.
#>
function Assert-RendererRecovery([bool]$Condition,[string]$Reason) {
	if(!$Condition){throw ('Recovered session rejected: '+$Reason)}
}
function Get-RendererRecoveryHash([string]$Path) {
	return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Assert-RendererRecoveryPath([string]$Path) {
	$full=[IO.Path]::GetFullPath($Path);$ancestor=$full
	# Missing destinations are expected. Permission and other I/O errors reject;
	# checking every existing ancestor also finds junctions below missing paths.
	while($ancestor){
		try{$a=[IO.File]::GetAttributes($ancestor)}catch{
			$failure=$_.Exception;while($failure.InnerException){$failure=$failure.InnerException}
			if($failure-is[IO.FileNotFoundException] -or $failure-is[IO.DirectoryNotFoundException]){$a=$null}else{throw}
		}
		if($null-ne$a){Assert-RendererRecovery (($a-band[IO.FileAttributes]::ReparsePoint)-eq0) 'redirected path'}
		$ancestor=[IO.Path]::GetDirectoryName($ancestor)
	}
	return $full
}
function Get-RendererRecoveryContained([string]$Root,[string]$Relative) {
	Assert-RendererRecovery ($Relative-is[string] -and $Relative.Length-gt0 -and ![IO.Path]::IsPathRooted($Relative)) 'expected relative archive path'
	$base=[IO.Path]::GetFullPath($Root).TrimEnd('\','/');$full=[IO.Path]::GetFullPath((Join-Path $base $Relative))
	Assert-RendererRecovery ($full.StartsWith($base+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) 'archive path escapes root'
	return Assert-RendererRecoveryPath $full
}
function Test-RendererRecoveryRecord($Record) {
	Assert-RendererRecovery ($Record.path-is[string] -and [IO.Path]::IsPathRooted($Record.path) -and $Record.sha256-is[string] -and $Record.sha256-cmatch'^[0-9a-f]{64}$' -and $Record.bytes-is[ValueType] -and $Record.bytes-isnot[bool] -and $Record.bytes-ge0) 'malformed file record'
	$path=Assert-RendererRecoveryPath $Record.path
	Assert-RendererRecovery ([IO.File]::Exists($path) -and ([IO.FileInfo]$path).Length-eq$Record.bytes -and (Get-RendererRecoveryHash $path)-ceq$Record.sha256) 'file size/hash mismatch'
	return $path
}
function Read-RendererRecoverySmall($Record) {
	$path=Test-RendererRecoveryRecord $Record
	Assert-RendererRecovery (([IO.FileInfo]$path).Length-le4194304) 'metadata exceeds four MiB'
	return [IO.File]::ReadAllText($path)|ConvertFrom-Json
}
function Get-RendererRecoverySealIdentity($Seal) {
	$ordered=[Collections.Generic.SortedDictionary[string,object]]::new([StringComparer]::Ordinal)
	foreach($entry in $Seal.artifacts){
		Assert-RendererRecovery ($entry.path-is[string] -and $entry.sha256-cmatch'^[0-9a-f]{64}$') 'malformed original seal entry'
		$sort=[BitConverter]::ToString([Text.Encoding]::UTF8.GetBytes($entry.path)).Replace('-','')
		Assert-RendererRecovery (!$ordered.ContainsKey($sort)) 'duplicate original seal entry';$ordered.Add($sort,$entry)
	}
	$parts=@{installedBinaryHash=[Text.StringBuilder]::new();installedShaderClosureHash=[Text.StringBuilder]::new();installedAssetClosureHash=[Text.StringBuilder]::new()}
	foreach($entry in $ordered.Values){
		$name=$entry.path;$line=$name+"`t"+([uint64]$entry.bytes).ToString([Globalization.CultureInfo]::InvariantCulture)+"`t"+$entry.sha256+"`n"
		if($name-ceq'Editor/TerraEngine-Editor.exe' -or $name-ceq'Editor/TerraEngine.dll'){$null=$parts.installedBinaryHash.Append($line)}
		if(('/'+$name)-match'/(Shaders|InstalledShaders)/' -and [IO.Path]::GetExtension($name)-in@('.spv','.tspf','.hlsl','.hlsli','.vert','.frag','.comp','.task','.mesh','.geom','.tesc','.tese','.glsl')){$null=$parts.installedShaderClosureHash.Append($line)}
		if($name-like'Tests/Fixtures/RendererEfficiency/*' -or ('/'+$name)-match'/Assets/'){$null=$parts.installedAssetClosureHash.Append($line)}
	}
	$result=@{revision=$Seal.revision}
	foreach($name in $parts.Keys){Assert-RendererRecovery ($parts[$name].Length-gt0) 'empty original native closure';$sha=[Security.Cryptography.SHA256]::Create();try{$result[$name]=[BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($parts[$name].ToString()))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}}
	return $result
}
function Import-RendererEfficiencyRecoveredSession {
	[CmdletBinding()]
	param([Parameter(Mandatory=$true)][string]$ReceiptPath,[Parameter(Mandatory=$true)]$Inventory,
	[Parameter(Mandatory=$true)][string]$OutputRoot,[Parameter(Mandatory=$true)][string]$Stage,
	[Parameter(Mandatory=$true)][string]$FixtureId,[Parameter(Mandatory=$true)][int]$RunIndex,
	[Parameter(Mandatory=$true)][string]$Mode,[Parameter(Mandatory=$true)][string]$Preparation,
	[Parameter(Mandatory=$true)][string]$ManifestPath,[Parameter(Mandatory=$true)][string]$SchemaPath,
	[Parameter(Mandatory=$true)][string]$StageStepsPath,[Parameter(Mandatory=$true)][scriptblock]$InspectEvidence,
	[Parameter(Mandatory=$true)][scriptblock]$VerifyTransformation,[switch]$ValidateOnly)
	# The first recovery contract covers this completed P1 fixture only. Shared
	# All-stage workloads need explicit stage-selected completion proofs later.
	Assert-RendererRecovery ($Stage-ceq'P1' -and $FixtureId-ceq'shadow-invalidation' -and $RunIndex-ge0 -and $RunIndex-lt3 -and $Mode-cin@('Reference','Candidate') -and $Preparation-cin@('Serial','Concurrent')) 'unsupported recovered session selection'
	# Only the runner binds this internal callback, from its verified installed
	# utility. Record that installed utility independently of the older collector.
	$streamRelative='Tests/Tools/RendererEfficiency/RendererEfficiencyEvidenceStream.ps1'
	Assert-RendererRecovery ($Inventory.files.ContainsKey($streamRelative)) 'verified installed stream utility missing'
	$streamEntry=$Inventory.files[$streamRelative];$streamPath=Get-RendererRecoveryContained $Inventory.root $streamRelative
	$null=Test-RendererRecoveryRecord @{path=$streamPath;sha256=$streamEntry.hash;bytes=$streamEntry.length}
	$receiptFile=Assert-RendererRecoveryPath $ReceiptPath;$receiptHash=Get-RendererRecoveryHash $receiptFile
	Assert-RendererRecovery (([IO.FileInfo]$receiptFile).Length-le4194304) 'oversized recovery receipt'
	$r=[IO.File]::ReadAllText($receiptFile)|ConvertFrom-Json
	Assert-RendererRecovery ($r.schemaVersion-eq1 -and $r.kind-ceq'RendererEfficiencyOfflineRecoveryCollection' -and $r.status-ceq'RecoveredCollected' -and $r.nativeMatrixAccepted-is[bool] -and !$r.nativeMatrixAccepted -and $r.transformation-ceq'UnchangedNativeRelativePathsAndActualLifecycleAppend') 'unsupported recovery contract'
	foreach($pair in @(@('stage',$Stage),@('fixtureId',$FixtureId),@('runIndex',$RunIndex),@('mode',$Mode),@('preparation',$Preparation))){Assert-RendererRecovery ($r.($pair[0])-ceq$pair[1]) 'recovery does not match exact planned session'}
	$name="$Stage-$FixtureId-$RunIndex-$Mode";Assert-RendererRecovery ($r.runId-ceq$name -and $r.operationId-cmatch'^[0-9a-f]{32}$') 'invalid original run/operation'
	$metadata=@{};foreach($key in @('originalSession','originalSummary','operation','lifecycle','ownershipMarker','frozenInputReceipt','installSeal')){$metadata[$key]=Read-RendererRecoverySmall $r.inputs.$key}
	foreach($key in @('nativeEvidence','tool','toolLibrary')){$null=Test-RendererRecoveryRecord $r.inputs.$key}
	$s=$metadata.originalSession;$summary=$metadata.originalSummary;$op=$metadata.operation;$life=$metadata.lifecycle;$marker=$metadata.ownershipMarker
	Assert-RendererRecovery ($s.status-ceq'Failed' -and $null-eq$s.evidencePath -and $s.name-ceq$name -and $summary.status-ceq'Failed' -and @($summary.sessions|Where-Object {$_.name-ceq$name -and $_.status-ceq'Failed'}).Count-eq1) 'original failure record mismatch'
	$manifest=[IO.File]::ReadAllText((Assert-RendererRecoveryPath $ManifestPath))|ConvertFrom-Json
	$fixture=@($manifest.workloads|Where-Object id -ceq $FixtureId);Assert-RendererRecovery ($fixture.Count-eq1 -and $fixture[0].stage-ceq$Stage) 'unknown concrete recovered fixture'
	Assert-RendererRecovery ($op.state-ceq'Completed' -and $op.status-ceq'Valid' -and $op.operationId-ceq$r.operationId -and $op.fixtureId-ceq$FixtureId -and $op.completedSteps-eq$fixture[0].steps.Count) 'native completion missing'
	foreach($field in @('optionsRestored','normalClose')){Assert-RendererRecovery ($life.$field-is[bool] -and $life.$field) 'actual lifecycle failed'}
	Assert-RendererRecovery ($life.watchdogFailed-is[bool] -and !$life.watchdogFailed -and $life.processExitCode-eq0 -and $life.operationId-ceq$r.operationId -and $life.runId-ceq$name -and $life.fixtureId-ceq$FixtureId -and $life.runIndex-eq$RunIndex -and $life.preparation-ceq$Preparation -and $life.stage-ceq$Stage -and $life.mode-ceq$Mode) 'actual lifecycle identity mismatch'
	foreach($field in @('ownedEditorProcessId','restoredSettingsGeneration','restoredRendererSessionGeneration')){
		Assert-RendererRecovery ($life.$field-is[ValueType] -and $life.$field-isnot[bool] -and $life.$field-gt0) 'positive native lifecycle receipt required'
	}
	Assert-RendererRecovery ($life.kind-ceq'RendererEfficiencyRunnerLifecycle' -and $life.schemaVersion-eq1 -and $null-ne$life.originalOptions -and $null-ne$life.restoredOptions -and ($life.originalOptions|ConvertTo-Json -Depth 10 -Compress)-ceq($life.restoredOptions|ConvertTo-Json -Depth 10 -Compress)) 'actual protected options were not restored exactly'
	$project=Assert-RendererRecoveryPath $marker.projectRoot
	Assert-RendererRecovery ([string]::Equals([IO.Path]::GetFullPath($op.projectRoot),$project,[StringComparison]::OrdinalIgnoreCase) -and $marker.kind-ceq'RendererEfficiencyValidationProject' -and $marker.fixtureId-ceq$FixtureId -and $marker.seed-eq$fixture[0].seed) 'original project ownership mismatch'
	Assert-RendererRecovery ([IO.Path]::GetFullPath($r.inputs.nativeEvidence.path)-ceq(Get-RendererRecoveryContained $project ($r.operationId+'-evidence.json'))) 'original native document escapes owned operation'
	Assert-RendererRecovery ($metadata.frozenInputReceipt.status-ceq'Frozen35GradingInputsOnly') 'wrong frozen input receipt'
	$oldSeal=@($metadata.frozenInputReceipt.files|Where-Object path -ceq 'renderer-efficiency-artifacts.json')
	Assert-RendererRecovery ($oldSeal.Count-eq1 -and $oldSeal[0].sha256-ceq$r.inputs.installSeal.sha256) 'original seal differs from frozen inputs'
	foreach($pair in @(@('workloads.json',$ManifestPath),@('Schemas/evidence.schema.json',$SchemaPath),@('RendererEfficiencyStageSteps.ps1',$StageStepsPath))){$item=@($metadata.frozenInputReceipt.files|Where-Object path -ceq $pair[0]);Assert-RendererRecovery ($item.Count-eq1 -and (Get-RendererRecoveryHash (Assert-RendererRecoveryPath $pair[1]))-ceq$item[0].sha256) 'frozen grading input changed'}
	$sealIdentity=Get-RendererRecoverySealIdentity $metadata.installSeal
	$recovered=Test-RendererRecoveryRecord $r.recoveredEvidence;$archive=[IO.Path]::GetDirectoryName($recovered)
	$index=Test-RendererRecoveryRecord $r.artifactIndex;$entries=@{};$reader=[IO.File]::OpenText($index)
	# The index is untrusted until every source/archive byte and every descriptor
	# emitted by the actual recovered document agrees. Keep only compact records.
	try{while($null-ne($line=$reader.ReadLine())){
		Assert-RendererRecovery ($line.Length-le4194304) 'oversized index row';$a=$line|ConvertFrom-Json
		$path=Get-RendererRecoveryContained $archive $a.path;Assert-RendererRecovery (!$entries.ContainsKey($a.path) -and $path-ceq[IO.Path]::GetFullPath($a.archivePath)) 'duplicate/misbound archive row'
		$expectedSource=if($a.path-ceq'runner.lifecycle.json'){$r.inputs.lifecycle.path}else{Get-RendererRecoveryContained $project $a.path}
		Assert-RendererRecovery ([string]::Equals([IO.Path]::GetFullPath($a.sourcePath),[IO.Path]::GetFullPath($expectedSource),[StringComparison]::OrdinalIgnoreCase)) 'index source escapes original ownership'
		foreach($inputPath in @($a.sourcePath,$path)){$null=Test-RendererRecoveryRecord @{path=$inputPath;sha256=$a.sha256;bytes=$a.bytes}}
		$entries[$a.path]=@{record=$a;root=$false;frame=$false}
	}}finally{$reader.Dispose()}
	# Verify the transformation itself, rather than trusting its receipt label.
	# Retain only fixed-size frame hashes and small root values while streaming.
	$sourceState=@{metadata=@{};frames=[Collections.Generic.List[string]]::new();artifacts=[Collections.Generic.List[string]]::new();observedOptions=@{};nativeReceipts=[Collections.Generic.List[object]]::new()}
	$hashText={param($text) $sha=[Security.Cryptography.SHA256]::Create();try{return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text))).Replace('-','')}finally{$sha.Dispose()}}
	$sourceCallback=[Action[string,string]]{param($field,$json)
		if($field-ceq'frames'){
			$sourceState.frames.Add((& $hashText $json));$frame=$json|ConvertFrom-Json
			Assert-RendererRecovery ($null-ne$frame.effectiveOptions) 'native frame has no observed options'
			$sourceState.observedOptions[($frame.effectiveOptions|ConvertTo-Json -Depth 10 -Compress)]=$true
		}
		elseif($field-ceq'artifacts'){
			$sourceState.artifacts.Add((& $hashText $json));$descriptor=$json|ConvertFrom-Json
			if($descriptor.path -is [string] -and $descriptor.path.EndsWith('.receipts.json',[StringComparison]::Ordinal)){$sourceState.nativeReceipts.Add($descriptor)}
		}
		else{Assert-RendererRecovery (!$sourceState.metadata.ContainsKey($field)) 'duplicate original metadata';$sourceState.metadata[$field]=$json}
	}
	& $InspectEvidence $r.inputs.nativeEvidence.path $sourceCallback
	# The native writer appends one action receipt after its raster artifact count.
	# Accept that exact sidecar only after its content-addressed bytes and identity
	# prove that it belongs to this operation; arbitrary extra artifacts still fail.
	Assert-RendererRecovery ($sourceState.nativeReceipts.Count-eq1) 'one native receipt sidecar required'
	$nativeReceipt=$sourceState.nativeReceipts[0]
	Assert-RendererRecovery ($nativeReceipt.path-ceq('renderer-efficiency-evidence/'+$r.operationId+'/'+$nativeReceipt.sha256+'.receipts.json') -and $nativeReceipt.kind-ceq'Counters' -and $nativeReceipt.status-ceq'Valid' -and $entries.ContainsKey($nativeReceipt.path)) 'native receipt descriptor invalid'
	$nativeReceiptEntry=$entries[$nativeReceipt.path].record
	Assert-RendererRecovery ($nativeReceiptEntry.sha256-ceq$nativeReceipt.sha256 -and $nativeReceiptEntry.kind-ceq'Counters' -and $nativeReceiptEntry.rootReferenced-eq$true -and $nativeReceiptEntry.frameReferenced-eq$false) 'native receipt index mismatch'
	$nativeReceiptDocument=Read-RendererRecoverySmall @{path=$nativeReceiptEntry.sourcePath;sha256=$nativeReceiptEntry.sha256;bytes=$nativeReceiptEntry.bytes}
	foreach($field in @('operationId','fixtureId','fixtureHash','stage','mode','runId')){Assert-RendererRecovery ($nativeReceiptDocument.$field-is[string]) 'native receipt identity type invalid'}
	foreach($field in @('schemaVersion','runIndex','seed')){Assert-RendererRecovery ($nativeReceiptDocument.$field-is[ValueType] -and $nativeReceiptDocument.$field-isnot[bool]) 'native receipt identity type invalid'}
	Assert-RendererRecovery ($nativeReceiptDocument.receipts-is[Array]) 'native receipt actions must be an array'
	Assert-RendererRecovery ($nativeReceiptDocument.schemaVersion-eq1 -and $nativeReceiptDocument.operationId-ceq$r.operationId -and $nativeReceiptDocument.fixtureId-ceq$FixtureId -and $nativeReceiptDocument.fixtureHash-ceq(Get-RendererRecoveryHash $ManifestPath) -and $nativeReceiptDocument.stage-ceq$Stage -and $nativeReceiptDocument.mode-ceq$Mode -and $nativeReceiptDocument.runId-ceq$name -and $nativeReceiptDocument.runIndex-eq$RunIndex -and $nativeReceiptDocument.seed-eq$fixture[0].seed -and $null-ne$nativeReceiptDocument.receipts) 'native receipt identity mismatch'
	$state=@{metadata=@{};frameCount=0;frameReferences=0;rootReferences=0;nativeRootReferences=0}
	$consumeArtifact={param($a,$root)
		Assert-RendererRecovery ($entries.ContainsKey($a.path)) 'native artifact missing from index';$entry=$entries[$a.path]
		Assert-RendererRecovery ($a.status-ceq'Valid' -and $a.sha256-ceq$entry.record.sha256 -and $a.kind-ceq$entry.record.kind) 'native descriptor/index mismatch'
		if($root){$entry.root=$true;$state.rootReferences++}else{$entry.frame=$true;$state.frameReferences++}
	}
	$callback=[Action[string,string]]{param($field,$json)
		$value=$json|ConvertFrom-Json
		if($field-ceq'artifacts'){
			if($value.path-cne'runner.lifecycle.json'){
				Assert-RendererRecovery ($state.nativeRootReferences-lt$sourceState.artifacts.Count -and $sourceState.artifacts[$state.nativeRootReferences]-ceq(& $hashText $json)) 'original root artifact changed or reordered';$state.nativeRootReferences++
			}else{Assert-RendererRecovery ($state.nativeRootReferences-eq$sourceState.artifacts.Count -and $state.rootReferences-eq$sourceState.artifacts.Count) 'lifecycle must be the sole appended root descriptor';$state.lifecycleJson=$json}
			& $consumeArtifact $value $true
		}
		elseif($field-ceq'frames'){
			Assert-RendererRecovery ($state.frameCount-lt$sourceState.frames.Count -and $sourceState.frames[$state.frameCount]-ceq(& $hashText $json)) 'original native frame changed or reordered'
			$state.frameCount++;foreach($a in $value.artifacts){& $consumeArtifact $a $false}
		}
		else{Assert-RendererRecovery (!$state.metadata.ContainsKey($field) -and $sourceState.metadata.ContainsKey($field) -and $sourceState.metadata[$field]-ceq$json) 'original native metadata changed';$state.metadata[$field]=$value}
	}
	& $InspectEvidence $recovered $callback
	# The same verified installed utility checks raw source prefix/suffix bytes,
	# not merely the normalized values used for identity and descriptor checks.
	& $VerifyTransformation $r.inputs.nativeEvidence.path $recovered $state.lifecycleJson
	Assert-RendererRecovery ($state.metadata.schemaVersion-eq1 -and $state.frameCount-eq$r.nativeFrameCount -and $state.frameCount-eq$sourceState.frames.Count -and $state.frameCount-eq$op.exportedFrames -and $sourceState.artifacts.Count-eq($op.exportedArtifacts+1) -and $sourceState.artifacts.Count-eq$r.artifactIndex.originalRootArtifactReferences -and $state.nativeRootReferences-eq$sourceState.artifacts.Count -and $state.rootReferences-eq($sourceState.artifacts.Count+1) -and $entries.Count-eq$r.artifactIndex.uniqueArtifacts -and $state.frameReferences-eq$r.artifactIndex.frameArtifactReferences -and $state.rootReferences-eq$r.artifactIndex.rootArtifactReferences) 'actual archive inventory counts differ'
	foreach($entry in $entries.Values){Assert-RendererRecovery (($entry.root -or $entry.frame) -and $entry.root-eq$entry.record.rootReferenced -and $entry.frame-eq$entry.record.frameReferenced) 'unreferenced/misclassified index entry'}
	$id=$state.metadata.identity;$options=$state.metadata.requestedOptions
	Assert-RendererRecovery ($sourceState.observedOptions.Count-eq1 -and $sourceState.observedOptions.ContainsKey(($options|ConvertTo-Json -Depth 10 -Compress))) 'native frames did not observe requested options'
	Assert-RendererRecovery (($id|ConvertTo-Json -Depth 30 -Compress)-ceq($r.identity|ConvertTo-Json -Depth 30 -Compress) -and ($options|ConvertTo-Json -Depth 10 -Compress)-ceq($r.requestedOptions|ConvertTo-Json -Depth 10 -Compress)) 'actual recovered identity differs from receipt'
	foreach($field in @('installedBinaryHash','installedShaderClosureHash','installedAssetClosureHash')){Assert-RendererRecovery ($id.$field.status-ceq'Valid' -and $id.$field.value-ceq$Inventory.identity[$field] -and $id.$field.value-ceq$sealIdentity[$field] -and $r.installIdentity.$field-ceq$sealIdentity[$field]) 'native closure changed'}
	foreach($field in @('referenceRevision','candidateRevision')){Assert-RendererRecovery ($id.$field.status-ceq'Valid' -and $id.$field.value-ceq$Inventory.identity.revision -and $id.$field.value-ceq$sealIdentity.revision) 'native revision changed'}
	Assert-RendererRecovery ($id.runId-ceq$name -and $id.runIndex-eq$RunIndex -and $id.fixtureId-ceq$FixtureId -and $id.stage-ceq$Stage -and $id.mode-ceq$Mode -and $id.seed-eq$fixture[0].seed -and $options.validationPreparation-ceq$Preparation -and $id.fixtureHash.status-ceq'Valid' -and $id.fixtureHash.value-ceq(Get-RendererRecoveryHash $ManifestPath) -and $marker.fixtureHash-ceq$id.fixtureHash.value -and $op.fixtureHash-ceq$id.fixtureHash.value) 'actual fixture identity mismatch'
	Assert-RendererRecovery ($entries.ContainsKey('runner.lifecycle.json') -and $entries['runner.lifecycle.json'].record.sha256-ceq$r.inputs.lifecycle.sha256) 'actual lifecycle is absent from native closure'
	$destination=Get-RendererRecoveryContained $OutputRoot $name;Assert-RendererRecovery (!(Test-Path -LiteralPath $destination)) 'recovery destination must be fresh'
	foreach($originalRoot in @($project,$archive,[IO.Path]::GetDirectoryName($r.inputs.originalSession.path))){Assert-RendererRecovery (!$destination.StartsWith($originalRoot.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) 'recovery output would modify original session tree'}
	Assert-RendererRecovery ((Get-RendererRecoveryHash $receiptFile)-ceq$receiptHash -and (Get-RendererRecoveryHash $recovered)-ceq$r.recoveredEvidence.sha256 -and (Get-RendererRecoveryHash $index)-ceq$r.artifactIndex.sha256) 'recovery inputs changed during validation'
	foreach($key in @('originalSession','originalSummary','operation','lifecycle','ownershipMarker','nativeEvidence','frozenInputReceipt','installSeal','tool','toolLibrary')){$null=Test-RendererRecoveryRecord $r.inputs.$key}
	if($ValidateOnly){return [pscustomobject]@{name=$name;fixtureId=$FixtureId;stage=$Stage;mode=$Mode;runIndex=$RunIndex;preparation=$Preparation;status='PlannedRecoveredCollected';nativeMatrixAccepted=$false;evidencePath=$recovered;sourceReceiptPath=$receiptFile;sourceReceiptSha256=$receiptHash}}
	$destArchive=Get-RendererRecoveryContained $destination 'artifacts';[IO.Directory]::CreateDirectory($destArchive)|Out-Null
	foreach($entry in $entries.Values){$a=$entry.record;$path=Get-RendererRecoveryContained $destArchive $a.path;[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))|Out-Null;[IO.File]::Copy($a.archivePath,$path,$false);$null=Test-RendererRecoveryRecord @{path=$path;sha256=$a.sha256;bytes=$a.bytes}}
	$evidencePath=Get-RendererRecoveryContained $destArchive ([IO.Path]::GetFileName($recovered));[IO.File]::Copy($recovered,$evidencePath,$false);$null=Test-RendererRecoveryRecord @{path=$evidencePath;sha256=$r.recoveredEvidence.sha256;bytes=$r.recoveredEvidence.bytes}
	$overlay=[ordered]@{schemaVersion=1;kind='RendererEfficiencyRecoveredSessionOverlay';status='RecoveredCollected';nativeMatrixAccepted=$false;sourceReceipt=@{path=$receiptFile;sha256=$receiptHash};streamUtility=@{path=$streamPath;sha256=$streamEntry.hash};currentInstallReceiptHash=$Inventory.receiptHash;originalInstallReceiptHash=$r.inputs.installSeal.sha256;evidencePath=$evidencePath;evidenceSha256=$r.recoveredEvidence.sha256;operationId=$r.operationId;runId=$name;preparation=$Preparation;artifactCount=$entries.Count}
	$overlayPath=Get-RendererRecoveryContained $destination 'recovery-overlay.json';[IO.File]::WriteAllText($overlayPath,($overlay|ConvertTo-Json -Depth 10),[Text.UTF8Encoding]::new($false))
	return [pscustomobject]@{name=$name;fixtureId=$FixtureId;stage=$Stage;mode=$Mode;runIndex=$RunIndex;status='RecoveredCollected';reason='Closed native session collected by explicit verified recovery';ownershipVerified=$true;optionsObserved=$true;lifecycleVerified=$true;evidencePath=$evidencePath;ownedEditorProcessId=$life.ownedEditorProcessId;ownedEditorExitCode=$life.processExitCode;recoveryReceiptPath=$overlayPath;recoveryReceiptSha256=Get-RendererRecoveryHash $overlayPath}
}
