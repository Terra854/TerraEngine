<#
Compare two installed production frame replay receipts. Required inputs are the
frozen workload manifest and a runner-owned provenance join binding both receipt,
loaded-engine and collector pins. Session mapping is optional and explicitly
bijective; no authored identity, ordering or lifecycle generation is normalized.
Use -ProtocolOnly only for clearly synthetic parser fixtures; it never establishes
replay acceptance. Dot-source this script to use its pure comparison function.
#>
[CmdletBinding()]
param([string]$ReferencePath='', [string]$CandidatePath='', [string]$ManifestPath='',
	[string]$ProvenanceJoinPath='', [string]$OutputPath='', [string]$SessionMapPath='', [switch]$ProtocolOnly)

function FsrRequire([bool]$Condition,[string]$Code,[string]$Message) {
	if(!$Condition){throw "[FrameReplay:$Code] $Message"}
}
function FsrField([System.Text.Json.JsonElement]$Value,[string]$Name,[string]$Path='$') {
	FsrRequire ($Value.ValueKind -eq [System.Text.Json.JsonValueKind]::Object) 'shape' "Expected object at $Path"
	$found=[System.Text.Json.JsonElement]::new()
	FsrRequire ($Value.TryGetProperty($Name,[ref]$found)) 'field' "Missing $Path/$Name"
	return $found
}
function FsrString([System.Text.Json.JsonElement]$Value,[string]$Path) {
	FsrRequire ($Value.ValueKind -eq [System.Text.Json.JsonValueKind]::String) 'shape' "Expected string at $Path"
	return $Value.GetString()
}
function FsrArray([System.Text.Json.JsonElement]$Value,[string]$Path) {
	FsrRequire ($Value.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) 'shape' "Expected array at $Path"
	return ,@($Value.EnumerateArray())
}
function FsrUnsigned([System.Text.Json.JsonElement]$Value,[string]$Path,[uint64]$Minimum=0) {
	FsrRequire ($Value.ValueKind -eq [System.Text.Json.JsonValueKind]::Number -and $Value.GetRawText() -cmatch '^(0|[1-9][0-9]*)$') 'integer' "Expected exact unsigned integer at $Path"
	$number=[System.Numerics.BigInteger]::Parse($Value.GetRawText(),[Globalization.CultureInfo]::InvariantCulture)
	FsrRequire ($number -ge $Minimum -and $number -le [uint64]::MaxValue) 'integer' "Unsigned integer outside range at $Path"
	return [uint64]$number
}
function FsrBoolean([System.Text.Json.JsonElement]$Value,[bool]$Expected,[string]$Path) {
	$kind=if($Expected){[System.Text.Json.JsonValueKind]::True}else{[System.Text.Json.JsonValueKind]::False}
	FsrRequire ($Value.ValueKind -eq $kind) 'boolean' "Expected Boolean $Expected at $Path"
}
function FsrCheckUniqueProperties([System.Text.Json.JsonElement]$Value,[string]$Path='$') {
	if($Value.ValueKind -eq [System.Text.Json.JsonValueKind]::Object) {
		$keys=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
		foreach($property in $Value.EnumerateObject()) {
			FsrRequire ($keys.Add($property.Name)) 'duplicate-key' "Duplicate JSON property at $Path/$($property.Name)"
			FsrCheckUniqueProperties $property.Value ($Path+'/'+$property.Name)
		}
	} elseif($Value.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) {
		$i=0;foreach($item in $Value.EnumerateArray()){FsrCheckUniqueProperties $item ($Path+'/'+$i);$i++}
	}
}
function FsrRead([string]$Path) {
	FsrRequire ([IO.Path]::IsPathRooted($Path) -and [IO.File]::Exists($Path)) 'file' "Missing absolute input file: $Path"
	FsrNoRedirect $Path
	$options=[System.Text.Json.JsonDocumentOptions]::new();$options.MaxDepth=128
	$document=[System.Text.Json.JsonDocument]::Parse([IO.File]::ReadAllText($Path),$options)
	try{$root=$document.RootElement.Clone();FsrCheckUniqueProperties $root;return $root}finally{$document.Dispose()}
}
function FsrNoRedirect([string]$Path) {
	$cursor=[IO.Path]::GetFullPath($Path)
	while($cursor){
		if(Test-Path -LiteralPath $cursor){FsrRequire (((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'path' "Redirected replay input/output: $cursor"}
		$cursor=[IO.Path]::GetDirectoryName($cursor)
	}
}
function FsrPin([System.Text.Json.JsonElement]$Pin,[string]$Path) {
	$file=FsrString (FsrField $Pin 'path' $Path) ($Path+'/path')
	FsrRequire ([IO.Path]::IsPathRooted($file)) 'path' "Pin must be absolute: $Path"
	FsrNoRedirect $file;FsrRequire ([IO.File]::Exists($file)) 'file' "Missing pinned replay file: $file"
	$hash=FsrString (FsrField $Pin 'sha256' $Path) ($Path+'/sha256')
	$bytes=FsrUnsigned (FsrField $Pin 'bytes' $Path) ($Path+'/bytes')
	FsrRequire ($hash -cmatch '^[0-9a-f]{64}$' -and (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $hash -and (Get-Item -LiteralPath $file).Length -eq $bytes) 'pin' "Changed replay evidence: $file"
	return [pscustomobject]@{path=[IO.Path]::GetFullPath($file);sha256=$hash;bytes=$bytes}
}
function FsrSamePin($Left,$Right,[string]$Path) {
	FsrRequire ($Left.path -ieq $Right.path -and $Left.sha256 -ceq $Right.sha256 -and $Left.bytes -eq $Right.bytes) 'provenance' "Replay file identity differs from trusted join: $Path"
}

function FsrCompareValue([System.Text.Json.JsonElement]$Left,[System.Text.Json.JsonElement]$Right,[string]$Path,
	[hashtable]$SessionMaps=@{},[string]$Lane='') {
	FsrRequire ($Left.ValueKind -eq $Right.ValueKind) 'difference' "JSON type changed at $Path"
	# Normalize only exact reviewed paths. The map never applies merely because
	# an unrelated property happens to share the word sessionGeneration.
	$domain=FsrSessionDomain $Path
	if($domain -and $SessionMaps.Count -gt 0) {
		$a=[string](FsrUnsigned $Left $Path 1);$b=[string](FsrUnsigned $Right $Path 1)
		$key="$Lane|$domain|$a"
		if(@($SessionMaps.Keys|Where-Object {$_ -clike ($Lane+'|'+$domain+'|*')}).Count -gt 0){FsrRequire ($SessionMaps.ContainsKey($key)) 'session-map' 'Explicit mapped domain omitted an observed session identity.'}
		if($SessionMaps.ContainsKey($key)) {FsrRequire ($SessionMaps[$key] -ceq $b) 'session-map' "Inconsistent explicit session mapping at $Path";$null=$SessionMaps['__used'].Add($key);return}
	}
	switch($Left.ValueKind) {
		Object {
			$leftProperties=@($Left.EnumerateObject());$rightProperties=@($Right.EnumerateObject())
			FsrRequire ($leftProperties.Count -eq $rightProperties.Count) 'difference' "Object fields changed at $Path"
			foreach($property in $leftProperties) {
				$value=[System.Text.Json.JsonElement]::new()
				FsrRequire ($Right.TryGetProperty($property.Name,[ref]$value)) 'difference' "Object field disappeared at $Path/$($property.Name)"
				FsrCompareValue $property.Value $value ($Path+'/'+$property.Name.Replace('~','~0').Replace('/','~1')) $SessionMaps $Lane
			}
		}
		Array {
			FsrRequire ($Left.GetArrayLength() -eq $Right.GetArrayLength()) 'difference' "Array length changed at $Path"
			for($i=0;$i -lt $Left.GetArrayLength();$i++){FsrCompareValue $Left[$i] $Right[$i] ($Path+'/'+$i) $SessionMaps $Lane}
		}
		String {FsrRequire ($Left.GetString() -ceq $Right.GetString()) 'difference' "String/identity changed at $Path"}
		Number {FsrRequire ($Left.GetRawText() -ceq $Right.GetRawText()) 'difference' "Exact numeric value changed at $Path"}
		default {FsrRequire ($Left.GetRawText() -ceq $Right.GetRawText()) 'difference' "Value changed at $Path"}
	}
}
function FsrSessionDomain([string]$Path) {
	if($Path -cmatch '^/frames/[0-9]+/renderer/root/(rendererSessionGeneration|persistent/rendererSessionGeneration|dynamic/rendererSessionGeneration)$'){return 'Renderer'}
	if($Path -cmatch '^/frames/[0-9]+/animation/root/sessionGeneration$'){return 'Animation'}
	if($Path -cmatch '^/frames/[0-9]+/animation/root/sidecars/(callbacks|physics|attachments|procedural|rollback|completions|gpuCandidates|diagnostics)/[0-9]+/key/sessionGeneration$'){return 'Animation'}
	return ''
}
function FsrSessionMaps([string]$Path) {
	$result=@{};if(!$Path){return $result}
	$root=FsrRead $Path
	$result['__used']=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	FsrRequire ((FsrString (FsrField $root 'schemaVersion') '$/schemaVersion') -ceq 'frame-stage-session-map-v1') 'session-map' 'Wrong explicit session map schema.'
	$reverse=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	foreach($row in (FsrArray (FsrField $root 'mappings') '$/mappings')) {
		$lane=FsrString (FsrField $row 'laneId') '$/mappings/laneId';$domain=FsrString (FsrField $row 'domain') '$/mappings/domain'
		FsrRequire ($lane -cin @('FullEngine','ProducerBoundaries') -and $domain -cin @('Renderer','Animation')) 'session-map' 'Unknown session map lane/domain.'
		$a=FsrString (FsrField $row 'reference') '$/mappings/reference';$b=FsrString (FsrField $row 'candidate') '$/mappings/candidate'
		foreach($value in @($a,$b)){FsrRequire ($value -cmatch '^[1-9][0-9]*$' -and [System.Numerics.BigInteger]::Parse($value) -le [uint64]::MaxValue) 'session-map' 'Session IDs must be canonical positive UInt64 decimal strings.'}
		$key="$lane|$domain|$a"
		FsrRequire (!$result.ContainsKey($key) -and $reverse.Add("$lane|$domain|$b")) 'session-map' 'Session mapping must be bijective within each lane/domain.'
		$result[$key]=$b
	}
	return $result
}

function FsrRequiredAssertions([string]$Scenario,[string]$Lane) {
	$ids=@('actual-workers','observed-frame-count','renderer-root-observed','animation-root-observed',
		'authored-render-identity','authored-animation-identity','profile-observed','command-published',
		'mutation-not-visible-before-frame','mutation-visible-after-frame','mutation-receipt-committed')
	$ids+=if($Lane -ceq 'FullEngine'){'full-engine-execution'}else{'production-boundaries-observed'}
	$both=@{
		'multi-output'=@('output-count-at-least-two');'no-output'=@('output-count-zero')
		'held-reader'=@('held-render-root-immutable','held-animation-root-immutable')
		'scene-clear'=@('scene-clear-root-retired','scene-clear-held-roots-immutable')
		'shutdown'=@('shutdown-render-root-retired','shutdown-animation-root-retired','shutdown-held-roots-immutable')
		'restart'=@('restart-new-render-publication','restart-shutdown-roots-retired','restart-new-animation-session','restart-roots-published','restart-held-roots-immutable')
	}
	$producer=@{
		'rejection-rollback'=@('rejected-candidate-retains-previous-root','rejected-command-batch-atomic')
		'cancel-before-exchange'=@('cancellation-ingress-observed','before-exchange-retains-previous-root','before-exchange-command-rejected')
		'cancel-after-exchange'=@('cancellation-ingress-observed','after-exchange-root-committed','after-exchange-command-completed')
		'worker-failure'=@('worker-fault-injected','worker-fault-reported','failed-continuation-retired')
	}
	if($Scenario -cin @('cold-direct-complete','cold-virtual-compact')){$ids+='cold-topology-not-reused'}
	if($Scenario -cin @('warm-direct-complete','warm-virtual-compact')){$ids+='warm-topology-reused'}
	if($both.ContainsKey($Scenario)){$ids+=@($both[$Scenario])}
	if($Lane -ceq 'ProducerBoundaries' -and $producer.ContainsKey($Scenario)){$ids+=@($producer[$Scenario])}
	return $ids
}
function FsrRequireFields([System.Text.Json.JsonElement]$Value,[string[]]$Names,[string]$Path) {
	foreach($name in $Names){$null=FsrField $Value $name $Path}
}
function FsrRoot([System.Text.Json.JsonElement]$Projection,[string]$Kind,[string]$Path,[bool]$QualifiedRuntimeMultiplicity=$false) {
	$present=FsrField $Projection 'present' $Path
	FsrRequire ($present.ValueKind -in @([System.Text.Json.JsonValueKind]::True,[System.Text.Json.JsonValueKind]::False)) 'boolean' "Root presence is not Boolean at $Path"
	$root=FsrField $Projection 'root' $Path
	if(!$present.GetBoolean()){FsrRequire ($root.ValueKind -eq [System.Text.Json.JsonValueKind]::Null) 'root' "Absent root carries invented payload at $Path";return 0}
	FsrRequire ($root.ValueKind -eq [System.Text.Json.JsonValueKind]::Object) 'root' "Present root lacks its typed projection at $Path"
	$payload=0;$identities=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	if($Kind -ceq 'Renderer') {
		FsrRequireFields $root @('frameId','rendererSessionGeneration','backendResourceGeneration','sceneGeneration','animationPresentationGeneration','expectedPreviousGeneration','generation','selectedOrdinals','receipt','persistent','dynamic','views','submissionRanges','resourcePreparation') $Path
		$null=FsrUnsigned (FsrField $root 'rendererSessionGeneration') ($Path+'/rendererSessionGeneration') 1
		$null=FsrArray (FsrField $root 'selectedOrdinals') ($Path+'/selectedOrdinals')
		$null=FsrArray (FsrField $root 'views') ($Path+'/views');$null=FsrArray (FsrField $root 'submissionRanges') ($Path+'/submissionRanges')
		FsrRequireFields (FsrField $root 'receipt') @('outcome','publicationGeneration','expectedPreviousGeneration','observedPreviousGeneration','reusedPersistentPages','changedDynamicPages','viewCount','rangeCount','diagnostic') ($Path+'/receipt')
		foreach($domain in @('persistent','dynamic')) {
			$owned=FsrField $root $domain
			if($owned.ValueKind -eq [System.Text.Json.JsonValueKind]::Null){continue}
			FsrRequireFields $owned @('generation','rendererSessionGeneration','pages') ($Path+'/'+$domain)
			$null=FsrUnsigned (FsrField $owned 'rendererSessionGeneration') ($Path+'/'+$domain+'/rendererSessionGeneration') 1
			foreach($page in (FsrArray (FsrField $owned 'pages') ($Path+'/'+$domain+'/pages'))) {
				if($page.ValueKind -eq [System.Text.Json.JsonValueKind]::Null){continue}
				$rowsName=if($domain -ceq 'persistent'){'slots'}else{'lanes'}
				foreach($row in (FsrArray (FsrField $page $rowsName) ($Path+'/'+$domain+'/'+$rowsName))) {
					$key=FsrField $row 'key';$ordinal=FsrUnsigned (FsrField $key 'ordinal') ($Path+'/key/ordinal')
					$life=FsrUnsigned (FsrField $key 'lifetimeGeneration') ($Path+'/key/lifetimeGeneration')
					$occupied=FsrField $row 'occupied';FsrRequire ($occupied.ValueKind -in @([System.Text.Json.JsonValueKind]::True,[System.Text.Json.JsonValueKind]::False)) 'boolean' 'Render slot occupancy must be Boolean.'
					if($occupied.GetBoolean()) {
						FsrRequire ($identities.Add("$domain|$ordinal|$life")) 'duplicate-id' 'Duplicate occupied renderer slot identity.'
						if($domain -ceq 'persistent') {FsrRequireFields $row @('objectId','meshId','materialId','meshGeneration','materialGeneration','resourceGeneration','packetTemplateIdentity','routeTemplate') $Path;$payload++}
						else{FsrRequireFields $row @('instance','worldPose','previousModelMatrixBits','animation','transformGeneration','animationPresentationGeneration','materialOverrideGeneration','boundsGeneration','lifecycleGeneration','resourceGeneration') $Path}
					}
				}
			}
		}
		$views=FsrArray (FsrField $root 'views') ($Path+'/views');$viewIds=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
		foreach($view in $views) {
			FsrRequireFields $view @('outputKind','viewType','outputId','generation','lightIdentity','visibleOrdinals','selectedLods','routeSelectors','packetTemplateIdentities','tintSelectors','resolvedCommands') ($Path+'/views')
			$id=(FsrField $view 'outputKind').GetRawText()+'|'+(FsrField $view 'outputId').GetRawText()+'|'+(FsrField $view 'lightIdentity').GetRawText()
			$firstOutput=$viewIds.Add($id)
			$nativeRuntime=($QualifiedRuntimeMultiplicity -and (FsrUnsigned (FsrField $view 'outputKind') $Path) -eq 2 -and (FsrUnsigned (FsrField $view 'viewType') $Path) -eq 0 -and (FsrUnsigned (FsrField $view 'outputId') $Path) -eq 2)
			FsrRequire ($firstOutput -or $nativeRuntime) 'duplicate-id' 'Duplicate renderer output identity lacks qualified native camera membership.'
			foreach($command in (FsrArray (FsrField $view 'resolvedCommands') ($Path+'/views/resolvedCommands'))){FsrRequireFields $command @('objectId','meshId','materialId','worldPose','modelMatrixBits','animation') ($Path+'/views/resolvedCommands')}
		}
		$preparation=FsrField $root 'resourcePreparation';FsrRequireFields $preparation @('generation','records') ($Path+'/resourcePreparation')
		$null=FsrArray (FsrField $preparation 'records') ($Path+'/resourcePreparation/records')
	} else {
		FsrRequireFields $root @('frameId','sessionGeneration','lifecycleGeneration','authorityGeneration','receipt','pages','sidecars') $Path
		$null=FsrUnsigned (FsrField $root 'sessionGeneration') ($Path+'/sessionGeneration') 1
		FsrRequireFields (FsrField $root 'receipt') @('outcome','authorityGeneration','expectedPreviousGeneration','observedPreviousGeneration','reusedPages','changedPages','evaluatedRows','fallbackRows','eventCount','gpuCandidateCount') ($Path+'/receipt')
		foreach($page in (FsrArray (FsrField $root 'pages') ($Path+'/pages'))) {
			if($page.ValueKind -eq [System.Text.Json.JsonValueKind]::Null){continue}
			FsrRequireFields $page @('firstSlot','capacity','changedMasks','workerValidatedRowCount','rows') ($Path+'/pages')
			foreach($row in (FsrArray (FsrField $page 'rows') ($Path+'/pages/rows'))) {
				FsrRequireFields $row @('key','ownerObjectId','objectGeneration','componentGeneration','sourceGeneration','poseGeneration','authorityPublicationGeneration','tick','stateId','controller','clip','skeleton','jointStableIds','sourceObjectWorldTransform','requestedExecutionPolicy','effectiveExecutionMode','status','snapshot','active','safeFallback') ($Path+'/pages/rows')
				$key=FsrField $row 'key';$id=(FsrField $key 'slot').GetRawText()+'|'+(FsrField $key 'lifetimeGeneration').GetRawText()
				FsrRequire ($identities.Add($id)) 'duplicate-id' 'Duplicate animation authority row identity.';$payload++
				$snapshot=FsrField $row 'snapshot'
				if($snapshot.ValueKind -ne [System.Text.Json.JsonValueKind]::Null){FsrRequireFields $snapshot @('animator','publicationGeneration','tick','playback','pose') ($Path+'/snapshot')}
			}
		}
		$sidecars=FsrField $root 'sidecars'
		if($sidecars.ValueKind -ne [System.Text.Json.JsonValueKind]::Null) {
			FsrRequireFields $sidecars @('events','rootMotion','summary','workerValidatedAuthorityGeneration','callbacks','physics','attachments','procedural','rollback','completions','gpuCandidates','diagnostics') ($Path+'/sidecars')
			foreach($name in @('events','rootMotion','callbacks','physics','attachments','procedural','rollback','completions','gpuCandidates','diagnostics')){$null=FsrArray (FsrField $sidecars $name) ($Path+'/sidecars/'+$name)}
		}
	}
	return $payload
}

function FsrValidateScheduleDiagnostics([System.Text.Json.JsonElement]$Value,[string]$Path) {
	# A forged Passed flag cannot override an actual scheduler construction error.
	# Info/Warning diagnostics are retained and compared, including typed context.
	foreach($diagnostic in (FsrArray $Value $Path)) {
		$severity=FsrUnsigned (FsrField $diagnostic 'severity') ($Path+'/severity')
		$category=FsrUnsigned (FsrField $diagnostic 'category') ($Path+'/category')
		FsrRequire ($severity -le 1 -and $category -le 7) 'schedule' 'Invalid/error scheduler diagnostic contradicts replay acceptance.'
		foreach($name in @('systemName','phaseName','resourceName','message','recommendedAction')){$null=FsrString (FsrField $diagnostic $name) ($Path+'/'+$name)}
	}
}
function FsrValidateScheduleSystems([System.Text.Json.JsonElement]$Value,[string]$Path) {
	$systems=FsrArray $Value $Path;$seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	FsrRequire ($systems.Count -gt 0) 'schedule' 'Actual schedule contains no registered systems.'
	foreach($system in $systems){$name=FsrString $system $Path;FsrRequire (![string]::IsNullOrWhiteSpace($name) -and $seen.Add($name)) 'schedule' 'Duplicate/empty registered system identity.'}
}
# Publication destination IDs do not identify individual cameras. This narrow
# proof admits native repeated runtime destinations only for the closed authored
# multi-output case; the complete ordered values still compare exactly.
function FsrMultiMatrixWords([System.Text.Json.JsonElement]$Matrix,[string]$Path) {
	$columns=FsrArray $Matrix $Path;FsrRequire ($columns.Count -eq 4) 'multi-output' 'Camera matrix must contain four columns.'
	$words=[Collections.Generic.List[System.Text.Json.JsonElement]]::new()
	foreach($column in $columns){$rows=FsrArray $column $Path;FsrRequire ($rows.Count -eq 4) 'multi-output' 'Camera matrix must contain four rows.';foreach($row in $rows){$word=FsrField $row 'ieee754' $Path;FsrRequire ((FsrUnsigned $word $Path) -le [uint32]::MaxValue) 'multi-output' 'Camera matrix IEEE word exceeds float width.';$words.Add($word)}}
	return ,$words.ToArray()
}
function FsrMultiPose([System.Text.Json.JsonElement]$Pose) {
	FsrRequireFields $Pose @('cell','offsetBits','rotationBits','scaleBits','transformRevision','coordinateFrameRevision') '$/canonicalPose'
	foreach($pair in @(@('cell',3),@('offsetBits',3),@('rotationBits',4),@('scaleBits',3))) {
		$values=FsrArray (FsrField $Pose $pair[0]) '$/canonicalPose';FsrRequire ($values.Count -eq $pair[1]) 'multi-output' 'Protected camera pose has incomplete dimensions.'
		foreach($value in $values) {
			if($pair[0] -ceq 'cell'){$cell=[long]0;FsrRequire ($value.ValueKind -eq [Text.Json.JsonValueKind]::Number -and $value.TryGetInt64([ref]$cell)) 'multi-output' 'Camera cell must preserve its actual signed integer.'}
			else{$word=FsrUnsigned $value '$/canonicalPose';if($pair[0] -cne 'offsetBits'){FsrRequire ($word -le [uint32]::MaxValue) 'multi-output' 'Camera pose float word exceeds its native width.'}}
		}
	}
	foreach($name in @('transformRevision','coordinateFrameRevision')){$null=FsrUnsigned (FsrField $Pose $name) ('$/canonicalPose/'+$name)}
}
function FsrValidateMultiOutput([System.Text.Json.JsonElement]$Frame,[System.Text.Json.JsonElement]$Control) {
	$proof=FsrField $Frame 'multiOutputObservation';FsrBoolean (FsrField $proof 'passed') $true '$/multiOutputObservation/passed'
	FsrRequire ((FsrString (FsrField $proof 'association') '$/multiOutputObservation/association') -ceq 'CapturedCameraOrderToNativeRuntimeSubmissionOrder') 'multi-output' 'Camera association must describe actual ordered native submissions.'
	$expected=FsrArray (FsrField $proof 'expectedAuthoredCameraIds') '$/multiOutputObservation/expectedAuthoredCameraIds'
	$idsExpected=@('068c0000000000000000000000000001','068c0000000000000000000000000002')
	FsrRequire ($expected.Count -eq 2) 'multi-output' 'Closed multi-output fixture requires exactly two authored cameras.'
	for($i=0;$i -lt 2;$i++){FsrRequire ((FsrString $expected[$i] '$/expectedAuthoredCameraIds') -ceq $idsExpected[$i]) 'multi-output' 'Authored camera identity changed.'}
	$inputs=FsrField $Frame 'capturedInputs';$null=FsrUnsigned (FsrField $inputs 'frameId') '$/capturedInputs/frameId' 1
	$cameras=FsrArray (FsrField $inputs 'cameras') '$/capturedInputs/cameras';$observations=FsrArray (FsrField $proof 'cameraObservations') '$/multiOutputObservation/cameraObservations'
	$root=FsrField (FsrField $Frame 'renderer') 'root';$views=FsrArray (FsrField $root 'views') '$/renderer/root/views';$presentations=FsrArray (FsrField $root 'presentationInputs') '$/renderer/root/presentationInputs'
	FsrRequire ($cameras.Count -eq 2 -and $observations.Count -eq 2 -and $views.Count -eq 3 -and $presentations.Count -eq 3) 'multi-output' 'Actual camera, view and presentation membership disagree.'
	FsrRequire ((FsrUnsigned (FsrField $proof 'scenePresentationIndex') '$/scenePresentationIndex') -eq 0) 'multi-output' 'Scene presentation must retain its native first position.'
	FsrRequire ((FsrString (FsrField $Control 'authoredCameraId') '$/sceneViewControl/authoredCameraId') -ceq $idsExpected[1] -and (FsrString (FsrField $Control 'extentSource') '$/sceneViewControl/extentSource') -ceq 'WindowSystem.GetLatestWorkerSnapshot') 'multi-output' 'Scene control lacks its authored camera or actual extent source.'
	FsrBoolean (FsrField $Control 'runtimePrimaryOverride') $false '$/sceneViewControl/runtimePrimaryOverride'
	FsrMultiPose (FsrField $Control 'canonicalPose')
	$extent=FsrArray (FsrField $Control 'targetExtent') '$/sceneViewControl/targetExtent';FsrRequire ($extent.Count -eq 2) 'multi-output' 'Actual scene extent must have two dimensions.';foreach($dimension in $extent){$null=FsrUnsigned $dimension '$/targetExtent' 1}
	for($i=0;$i -lt 3;$i++) {
		$kind=if($i -eq 0){1}else{2};$type=if($i -eq 0){1}else{0};$outputId=if($i -eq 0){1}else{2}
		$output=FsrField $presentations[$i] 'output'
		foreach($pair in @(@('outputKind','kind',$kind),@('viewType','diagnosticsView',$type),@('outputId','outputId',$outputId))){FsrRequire ((FsrUnsigned (FsrField $views[$i] $pair[0]) '$/views/metadata') -eq $pair[2] -and (FsrUnsigned (FsrField $output $pair[1]) '$/presentationInputs/output') -eq $pair[2]) 'multi-output' 'Ordered selection/presentation output metadata disagree.'}
	}
	FsrCompareValue (FsrField (FsrField $presentations[0] 'output') 'targetSize') (FsrField $Control 'targetExtent') '/multiOutput/sceneExtentJoin'
	foreach($name in @('viewMatrix','projectionMatrix')){$nativeWords=FsrMultiMatrixWords (FsrField $presentations[0] $name) ('$/scene/'+$name);$controlWords=FsrArray (FsrField $Control ($name+'Bits')) '$/sceneViewControl/matrix';FsrRequire ($controlWords.Count -eq 16) 'multi-output' 'Scene control must retain sixteen exact matrix words.';for($i=0;$i -lt 16;$i++){FsrCompareValue $nativeWords[$i] $controlWords[$i] ('/multiOutput/sceneMatrixJoin/'+$name+'/'+$i)}}
	$seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	for($i=0;$i -lt 2;$i++) {
		$camera=$cameras[$i];$row=$observations[$i];$id=FsrString (FsrField $camera 'objectId') '$/capturedInputs/cameras/objectId'
		FsrRequire ($idsExpected -ccontains $id -and $seen.Add($id)) 'multi-output' 'Captured camera membership is not the closed authored pair.'
		FsrRequire ((FsrUnsigned (FsrField $row 'cameraOrdinal') '$/cameraOrdinal') -eq $i -and (FsrUnsigned (FsrField $row 'presentationIndex') '$/presentationIndex') -eq ($i+1)) 'multi-output' 'Camera ordinal association changed.'
		foreach($flag in @('activeTransformCamera','cameraEnabled')){FsrBoolean (FsrField $row $flag) $true ('$/cameraObservations/'+$flag)}
		FsrRequireFields $camera @('objectId','verticalFovBits','aspectRatioBits','nearPlaneBits','farPlaneBits','upBits') '$/capturedCamera'
		foreach($name in @('verticalFovBits','aspectRatioBits','nearPlaneBits','farPlaneBits')){FsrRequire ((FsrUnsigned (FsrField $camera $name) '$/capturedCamera') -le [uint32]::MaxValue) 'multi-output' 'Camera parameter word exceeds native float width.'}
		$up=FsrArray (FsrField $camera 'upBits') '$/capturedCamera/upBits';FsrRequire ($up.Count -eq 3) 'multi-output' 'Camera up vector must retain three words.';foreach($word in $up){FsrRequire ((FsrUnsigned $word '$/capturedCamera/upBits') -le [uint32]::MaxValue) 'multi-output' 'Camera up word exceeds native float width.'}
		FsrCompareValue (FsrField $row 'capturedCamera') $camera '/multiOutput/capturedCameraJoin';FsrCompareValue (FsrField $row 'protectedCamera') $camera '/multiOutput/protectedCameraJoin'
		FsrMultiPose (FsrField $row 'canonicalPose')
		foreach($name in @('viewMatrix','projectionMatrix')){$derived=FsrField $row ('derived'+[char]::ToUpperInvariant($name[0])+$name.Substring(1));$null=FsrMultiMatrixWords $derived ('$/cameraObservations/derived'+$name);FsrCompareValue $derived (FsrField $presentations[$i+1] $name) ('/multiOutput/runtimeMatrixJoin/'+$name)}
	}
	$ranges=FsrArray (FsrField $root 'submissionRanges') '$/submissionRanges';$indices=FsrArray (FsrField $proof 'rangeViewIndices') '$/multiOutputObservation/rangeViewIndices'
	FsrRequire ($ranges.Count -eq $indices.Count) 'multi-output' 'Range membership evidence changed size.'
	for($i=0;$i -lt $ranges.Count;$i++){$index=FsrField $ranges[$i] 'viewIndex';FsrRequire ((FsrUnsigned $index '$/submissionRanges/viewIndex') -lt 3) 'multi-output' 'Submission range refers to a nonexistent view.';FsrCompareValue $index $indices[$i] ('/multiOutput/rangeViewIndexJoin/'+$i)}
}
function FsrValidateLane([System.Text.Json.JsonElement]$Lane,[string]$LaneId,[string]$Scenario,[int]$Workers,[int]$Frames) {
	FsrRequire ((FsrString (FsrField $Lane 'laneId') '$/laneId') -ceq $LaneId) 'lane' 'Wrong, reordered or duplicate execution lane.'
	FsrRequire ((FsrString (FsrField $Lane 'status') '$/lane/status') -ceq 'Passed') 'incomplete' 'Every required execution lane must complete.'
	$authoritative=FsrField $Lane 'authoritative';$framesValue=FsrArray (FsrField $authoritative 'frames') '$/authoritative/frames'
	$construction=FsrField $authoritative 'scheduleConstruction' '$/authoritative'
	FsrRequire ((FsrField $construction 'valid').ValueKind -eq [Text.Json.JsonValueKind]::True) 'schedule' 'Actual schedule construction is invalid.'
	FsrValidateScheduleSystems (FsrField $construction 'systems') '$/authoritative/scheduleConstruction/systems'
	FsrValidateScheduleDiagnostics (FsrField $construction 'diagnostics') '$/authoritative/scheduleConstruction/diagnostics'
	FsrRequire ($framesValue.Count -eq $Frames -and $Frames -gt 0) 'empty-proof' 'Replay lane has missing/empty measured frames.'
	$ordinals=[Collections.Generic.HashSet[uint64]]::new();$payload=0;$receipts=0
	if($Scenario -cin @('cold-direct-complete','cold-virtual-compact','warm-direct-complete','warm-virtual-compact')){
		$firstRoot=FsrField (FsrField $framesValue[0] 'renderer') 'root';$firstPersistent=FsrField $firstRoot 'persistent';FsrRequire ((FsrArray (FsrField $firstPersistent 'pages') '$/first/persistent/pages').Count -gt 0) 'empty-proof' 'Cold/warm control omitted persistent topology pages.'
		if($Scenario.StartsWith('cold-')){FsrRequire ((FsrUnsigned (FsrField (FsrField $firstRoot 'receipt') 'reusedPersistentPages') '$/first/reusedPersistentPages') -eq 0) 'topology' 'Cold control reused previous persistent topology.'}
	}
	foreach($frame in $framesValue) {
		$ordinal=FsrUnsigned (FsrField $frame 'ordinal') '$/frame/ordinal'
		FsrRequire ($ordinals.Add($ordinal)) 'duplicate-id' 'Duplicate frame ordinal.'
		$renderProjection=FsrField $frame 'renderer'
		if($Scenario -cne 'no-output'){FsrBoolean (FsrField $renderProjection 'present') $true '$/frame/renderer/present'}
		else {
			FsrBoolean (FsrField $renderProjection 'present') $false '$/frame/renderer/present'
			$output=FsrField $frame 'outputObservation'
			$null=FsrUnsigned (FsrField $output 'diagnosticsFrameId') '$/outputObservation/diagnosticsFrameId' 1
			foreach($name in @('diagnosticsValid','scratchCompleted')){FsrBoolean (FsrField $output $name) $true ('$/outputObservation/'+$name)}
			foreach($name in @('gameStatsValid','sceneStatsValid')){FsrBoolean (FsrField $output $name) $false ('$/outputObservation/'+$name)}
			foreach($name in @('diagnosticsPublicationGeneration','viewSubmitWorkCount')){FsrRequire ((FsrUnsigned (FsrField $output $name) ('$/outputObservation/'+$name)) -eq 0) 'no-output' 'No-output diagnostics contradict null publication.'}
			$inputs=FsrField $frame 'capturedInputs';$null=FsrUnsigned (FsrField $inputs 'frameId') '$/capturedInputs/frameId' 1
			FsrRequire ((FsrArray (FsrField $inputs 'cameras') '$/capturedInputs/cameras').Count -eq 0 -and (FsrArray (FsrField $frame 'capturedRenderables') '$/capturedRenderables').Count -eq 0) 'no-output' 'No-output frame unexpectedly captured cameras/renderables.'
			foreach($light in (FsrArray (FsrField $inputs 'lights') '$/capturedInputs/lights')){FsrRequireFields $light @('objectId','typedValueDigest') '$/capturedInputs/lights'}
			$sceneInputs=FsrArray (FsrField $frame 'sceneMeshInputs') '$/sceneMeshInputs';FsrRequire ($sceneInputs.Count -gt 0) 'no-output' 'No-output scene omitted actual authored mesh input identities.'
			foreach($mesh in $sceneInputs){FsrRequireFields $mesh @('objectId','meshId','materialId','spatialObjectId','taskShaderCullingEnabled') '$/sceneMeshInputs';foreach($name in @('objectId','meshId','materialId')){$identity=FsrString (FsrField $mesh $name) ('$/sceneMeshInputs/'+$name);FsrRequire (![string]::IsNullOrWhiteSpace($identity) -and $identity -cnotmatch '^0+$') 'no-output' 'Empty authored input identity.'}}
		}
		FsrBoolean (FsrField (FsrField $frame 'animation') 'present') $true '$/frame/animation/present'
		if($Scenario -ceq 'multi-output'){FsrValidateMultiOutput $frame (FsrField $authoritative 'sceneViewControl')}
		$payload+=FsrRoot $renderProjection 'Renderer' '$/frame/renderer' ($Scenario -ceq 'multi-output')
		$payload+=FsrRoot (FsrField $frame 'animation') 'Animation' '$/frame/animation'
		$scheduler=FsrField $frame 'scheduler'
		FsrRequireFields $scheduler @('valid','completed','executionMode','declarations','dependencyEdges','results','phaseNames','declaredFlushNames','declaredMutationFlushCount','firstFailure') '$/frame/scheduler'
		$expectedWorkerFault=($LaneId -ceq 'ProducerBoundaries' -and $Scenario -ceq 'worker-failure' -and $ordinal -eq 0)
		if($expectedWorkerFault) {
			FsrRequire ((FsrField $scheduler 'valid').ValueKind -eq [Text.Json.JsonValueKind]::False -and (FsrField $scheduler 'completed').ValueKind -eq [Text.Json.JsonValueKind]::False) 'expected-fault' 'Injected first-frame worker fault must retain the actual invalid/noncompleted plan.'
			$failure=FsrField $scheduler 'firstFailure' '$/scheduler'
			FsrRequire ((FsrUnsigned (FsrField $failure 'code') '$/firstFailure/code') -eq 8 -and (FsrString (FsrField $failure 'workloadName') '$/firstFailure/workloadName') -ceq 'GeometryRoutePrepare' -and (FsrString (FsrField $failure 'resourceName') '$/firstFailure/resourceName') -ceq 'RenderSystem/Render/GeometryRoutePrepare#0' -and (FsrString (FsrField $failure 'message') '$/firstFailure/message') -ceq 'Job threw during worker execution: frame-replay-real-workload-fault.') 'expected-fault' 'Injected worker failure does not match the exact expected actual failure signature.'
		}else{FsrRequire ((FsrField $scheduler 'valid').ValueKind -eq [Text.Json.JsonValueKind]::True -and (FsrField $scheduler 'completed').ValueKind -eq [Text.Json.JsonValueKind]::True) 'schedule' 'Non-fault frame plan is invalid or did not complete.'}
		FsrRequire ((FsrField $scheduler 'scheduleValid').ValueKind -eq [Text.Json.JsonValueKind]::True) 'schedule' 'Actual scheduler summary is invalid despite the frame plan claim.'
		FsrValidateScheduleDiagnostics (FsrField $scheduler 'scheduleDiagnostics') '$/frame/scheduler/scheduleDiagnostics'
		FsrValidateScheduleSystems (FsrField $scheduler 'systemNames') '$/frame/scheduler/systemNames'
		FsrCompareValue (FsrField $construction 'systems') (FsrField $scheduler 'systemNames') '/scheduleConstruction/systems/frameJoin'
		$graph=FsrField $scheduler 'jobGraph' '$/scheduler'
		if($LaneId -ceq 'FullEngine'){
			FsrRequireFields $graph @('valid','completed','cancelled','fatalExecution','secondaryWorkerFailureCount','settledWorkerCount','terminalStates','workerJobCount','protectedJobCount','inlineJobCount','rejectedJobCount','fallbackJobCount','blockedDependencyCount','jobs','taskGroups','barriers') '$/scheduler/jobGraph'
			$graphEntries=0;foreach($kind in @('jobs','taskGroups','barriers')){foreach($entry in (FsrArray (FsrField $graph $kind) ('$/jobGraph/'+$kind))){FsrRequireFields $entry @('name','kind','state','executionPolicy','partitionIndex','partitionCount','itemBegin','itemEnd','usefulWorkItemCount','dependencyWave') '$/jobGraph/entry';$graphEntries++}}
			FsrRequire ($graphEntries -gt 0) 'empty-proof' 'FullEngine omitted the actual job graph result projection.'
		}else{FsrRequire ($graph.ValueKind -eq [Text.Json.JsonValueKind]::Null) 'lane' 'ProducerBoundaries cannot substitute a FullEngine private job graph claim.'}
		$declarations=FsrArray (FsrField $scheduler 'declarations') '$/scheduler/declarations';$ids=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
		FsrRequire ($declarations.Count -gt 0) 'empty-proof' 'Frame omitted actual scheduler declarations.'
		foreach($declaration in $declarations) {
			FsrRequireFields $declaration @('workloadName','ownerSystem','phase','stableDescriptorId','reads','writes','explicitDependencies','workerEligible','mainThreadRequired','phaseKind','boundaryClassification','hardBoundary') '$/scheduler/declarations'
			$id=FsrString (FsrField $declaration 'stableDescriptorId') '$/scheduler/declarations/stableDescriptorId'
			FsrRequire (![string]::IsNullOrWhiteSpace($id) -and $ids.Add($id)) 'duplicate-id' 'Duplicate/absent scheduler descriptor identity.'
			foreach($name in @('reads','writes','explicitDependencies')){$null=FsrArray (FsrField $declaration $name) ('$/scheduler/'+$name)}
		}
		# Presence is mandatory even when a real fault/omission blocks execution.
		# Completed callbacks are checked separately against chronological evidence.
		foreach($target in @('PublishAnimationAuthority','RenderPublicationCapture','GeometryRoutePrepare','VirtualGeometryPrepare','BackendCommit','RenderPublication')) {
			$matches=@($declarations|Where-Object {(FsrString (FsrField $_ 'workloadName') '$/declarations/workloadName') -ceq $target})
			FsrRequire ($matches.Count -eq 1) 'descriptor' "Missing/ambiguous required production descriptor: $target"
		}
		$results=FsrArray (FsrField $scheduler 'results') '$/scheduler/results';$resultIds=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
		FsrRequire ($results.Count -gt 0) 'empty-proof' 'Frame omitted actual scheduler execution results.'
		foreach($result in $results){$id=FsrString (FsrField $result 'stableDescriptorId') '$/scheduler/results/stableDescriptorId';FsrRequire ($ids.Contains($id) -and $resultIds.Add($id)) 'duplicate-id' 'Scheduler result has duplicate or unknown descriptor identity.'}
		if($expectedWorkerFault) {
			$geometry=@($declarations|Where-Object {(FsrString (FsrField $_ 'workloadName') '$/workloadName') -ceq 'GeometryRoutePrepare'})[0]
			$faultId=FsrString (FsrField $geometry 'stableDescriptorId') '$/GeometryRoutePrepare/stableDescriptorId'
			FsrRequire ($faultId -ceq 'RenderSystem/Render/GeometryRoutePrepare') 'expected-fault' 'Expected worker failure is not bound to its actual producer descriptor.'
			$faultResults=@($results|Where-Object {(FsrString (FsrField $_ 'stableDescriptorId') '$/stableDescriptorId') -ceq $faultId})
			FsrRequire ($faultResults.Count -eq 1) 'expected-fault' 'Expected worker failure omitted its actual execution result.'
			$faultResult=$faultResults[0]			# Descriptor identities name workloads; only the failing partition job
			# appends #0. Worker policy must agree with the actual declaration.
			FsrRequire ((FsrString (FsrField (FsrField $scheduler 'firstFailure') 'resourceName') '$/firstFailure/resourceName') -ceq ($faultId+'#0')) 'expected-fault' 'Failure job must identify partition zero of the actual descriptor.'
			FsrRequire ((FsrField $geometry 'workerEligible').ValueKind -eq [Text.Json.JsonValueKind]::True -and (FsrField $geometry 'mainThreadRequired').ValueKind -eq [Text.Json.JsonValueKind]::False) 'expected-fault' 'Injected failure must belong to a worker-eligible workload.'
			$estimated=FsrUnsigned (FsrField $geometry 'estimatedWorkItemCount') '$/faultDeclaration/estimatedWorkItemCount' 1
			$threshold=FsrUnsigned (FsrField $geometry 'inlineWorkItemThreshold') '$/faultDeclaration/inlineWorkItemThreshold'
			FsrRequire ($threshold -eq 0 -or $estimated -gt $threshold) 'expected-fault' 'Fault workload must exceed its configured inline threshold.'

			FsrRequire ((FsrField $faultResult 'inlineSelected').ValueKind -eq [Text.Json.JsonValueKind]::False -and (FsrField $faultResult 'fallbackUsed').ValueKind -eq [Text.Json.JsonValueKind]::False) 'expected-fault' 'Injected worker failure cannot be an inline or fallback execution.'
			FsrRequire ((FsrField $faultResult 'completed').ValueKind -eq [Text.Json.JsonValueKind]::False -and (FsrField $faultResult 'omitted').ValueKind -eq [Text.Json.JsonValueKind]::False -and (FsrUnsigned (FsrField $faultResult 'partitionCount') '$/faultResult/partitionCount') -eq 1 -and (FsrUnsigned (FsrField $faultResult 'executionPolicy') '$/faultResult/executionPolicy') -eq 1) 'expected-fault' 'Expected worker failure must be an actual noncompleted/non-omitted single-partition execution.'
		}
		$null=FsrArray (FsrField $scheduler 'dependencyEdges') '$/scheduler/dependencyEdges'
		FsrRequire ((FsrArray (FsrField $scheduler 'phaseNames') '$/scheduler/phaseNames').Count -gt 0) 'empty-proof' 'Frame omitted actual phases.'
		$null=FsrArray (FsrField $scheduler 'declaredFlushNames') '$/scheduler/declaredFlushNames'
		$commands=FsrArray (FsrField $frame 'commandReceipts') '$/frame/commandReceipts'
		foreach($command in $commands){FsrRequireFields $command @('target','producerId','producerSequence','correlationId','idempotencyKey','cancellationToken','status','targetTick','observableTick','publicationGeneration','duplicate') '$/frame/commandReceipts'}
		$receipts+=$commands.Count
	}
	FsrRequire ($payload -gt 0 -and $receipts -gt 0) 'empty-proof' 'Count-only/empty root projections or no command receipt cannot establish replay.'
	$boundaries=FsrArray (FsrField $authoritative 'boundaries') '$/authoritative/boundaries'
	if($LaneId -ceq 'ProducerBoundaries'){FsrRequire ($boundaries.Count -gt 0) 'empty-proof' 'Producer boundary observations are empty.'}
	FsrRequire ((FsrArray (FsrField $authoritative 'mutationObservations') '$/authoritative/mutationObservations').Count -gt 0) 'empty-proof' 'Actual mutation observations are empty.'
	$lifetimes=FsrArray (FsrField $authoritative 'lifetimeObservations') '$/authoritative/lifetimeObservations'
	if($Scenario -cin @('held-reader','scene-clear','shutdown','restart')){FsrRequire ($lifetimes.Count -gt 0) 'empty-proof' 'Lifecycle scenario contains no lifetime observation.'}
	# Renderer Clear is the required fault retirement boundary. Qualify Mixed
	# only at the actual fault checkpoint before that reset; recovery stays exact
	# but is not required to keep GPU compact admission after resource retirement.
	if($LaneId -ceq 'ProducerBoundaries' -and $Scenario -ceq 'worker-failure') {
		$profile=FsrField $authoritative 'profileObservation' '$/authoritative'
		FsrRequire ((FsrString (FsrField $profile 'requestedProfile') '$/profileObservation/requestedProfile') -ceq 'Mixed' -and (FsrString (FsrField $profile 'checkpoint') '$/profileObservation/checkpoint') -ceq 'FaultFrameBeforeRendererClear' -and (FsrUnsigned (FsrField $profile 'frameOrdinal') '$/profileObservation/frameOrdinal') -eq 0) 'profile' 'Worker-fault Mixed qualification must identify its actual first frame before renderer retirement.'
		$firstRoot=FsrField (FsrField $framesValue[0] 'renderer') 'root'
		FsrRequire ((FsrUnsigned (FsrField $profile 'nativePublicationFrameId') '$/profileObservation/nativePublicationFrameId' 1) -eq (FsrUnsigned (FsrField $firstRoot 'frameId') '$/first/renderer/frameId' 1) -and (FsrUnsigned (FsrField $profile 'nativePublicationGeneration') '$/profileObservation/nativePublicationGeneration') -eq (FsrUnsigned (FsrField $firstRoot 'generation') '$/first/renderer/generation' 1) -and (FsrUnsigned (FsrField $profile 'nativePublicationReceiptGeneration') '$/profileObservation/nativePublicationReceiptGeneration') -eq (FsrUnsigned (FsrField (FsrField $firstRoot 'receipt') 'publicationGeneration') '$/first/renderer/receipt/publicationGeneration')) 'profile' 'Fault profile observation does not join the actual held native publication.'
		foreach($frame in $framesValue){FsrRequireFields (FsrField $frame 'animationAdmission') @('planStatus','rejectionReason','rejectionItemIndex','executionPolicy','availableWorkerCount','selectedWorkerCount','itemCount','rangeCount','scratchBytes','cacheEntryCount','reusedItemCount','fallbackItemCount','deterministicPublicationValidated','cancellationRequested','counters') '$/frame/animationAdmission'}
		$counters=FsrField (FsrField $framesValue[0] 'animationAdmission') 'counters'
		foreach($name in @('completeProcessedAnimators','compactProcessedAnimators')){$observed=FsrUnsigned (FsrField $profile $name) ('$/profileObservation/'+$name);$actual=FsrUnsigned (FsrField $counters $name) ('$/first/animationAdmission/counters/'+$name);FsrRequire ($observed -gt 0 -and $observed -eq $actual) 'profile' 'Fault frame omitted or forged actual Mixed animation admission.'}
		$direct=FsrUnsigned (FsrField $profile 'directCommands') '$/profileObservation/directCommands';$virtual=FsrUnsigned (FsrField $profile 'virtualCommands') '$/profileObservation/virtualCommands';FsrRequire ($direct -gt 0 -or $virtual -gt 0) 'profile' 'Fault profile has no observed native geometry commands.'
		$post=FsrField $authoritative 'postRetirementProfileObservation' '$/authoritative'
		FsrRequire ((FsrString (FsrField $post 'checkpoint') '$/postRetirementProfileObservation/checkpoint') -ceq 'AfterMeasuredRecoveryFrames' -and (FsrUnsigned (FsrField $post 'frameOrdinal') '$/postRetirementProfileObservation/frameOrdinal') -eq ($Frames-1)) 'profile' 'Post-retirement telemetry has the wrong recovery checkpoint.'
		FsrBoolean (FsrField $post 'qualifying') $false '$/postRetirementProfileObservation/qualifying'
		FsrCompareValue (FsrField $post 'animationAdmission') (FsrField $framesValue[$Frames-1] 'animationAdmission') '/postRetirementProfileObservation/actualFrameJoin'
	}
	# These action facts are checked before equality, so matching forged receipts
	# cannot convert a rejected/publicly cancelled command into successful evidence.
	if($LaneId -ceq 'ProducerBoundaries' -and $Scenario -cin @('cancel-before-exchange','cancel-after-exchange')) {
		$observation=FsrField $authoritative 'cancellationObservation' '$/authoritative'
		$first=FsrField $observation 'firstCommandReceipt' '$/cancellationObservation'
		FsrRequireFields $first @('target','producerId','producerSequence','correlationId','idempotencyKey','cancellationToken','status','targetTick','observableTick','publicationGeneration','duplicate') '$/cancellationObservation/firstCommandReceipt'
		$firstCommands=FsrArray (FsrField $framesValue[0] 'commandReceipts') '$/frames/0/commandReceipts';FsrRequire ($firstCommands.Count -gt 0) 'cancellation' 'Cancellation omitted its actual first-frame command receipt.'
		FsrCompareValue $first $firstCommands[0] '/cancellationObservation/firstCommandReceipt/frameJoin'
		$status=FsrUnsigned (FsrField $first 'status') '$/firstCommandReceipt/status';$generation=FsrUnsigned (FsrField $first 'publicationGeneration') '$/firstCommandReceipt/publicationGeneration'
		if($Scenario -ceq 'cancel-before-exchange'){FsrRequire ($status -eq 13 -and $generation -eq 0) 'cancellation' 'Before-exchange producer epoch invalidation must reject the first command without a publication.'}
		else{$authority=FsrUnsigned (FsrField (FsrField (FsrField $framesValue[0] 'animation') 'root') 'authorityGeneration') '$/first/authorityGeneration' 1;FsrRequire ($status -eq 0 -and $generation -eq $authority) 'cancellation' 'After-exchange command must complete against its actual published authority generation.'}
		$counts=FsrField $observation 'cacheCounts' '$/cancellationObservation'
		foreach($name in @('entriesBeforeIngress','entriesAfterIngress','entriesAfterCallbacks')){$null=FsrUnsigned (FsrField $counts $name) ('$/cancellationObservation/cacheCounts/'+$name)}
		FsrRequire ((FsrString (FsrField $observation 'cacheMembershipClaim') '$/cancellationObservation/cacheMembershipClaim') -ceq 'CountOnlyNoEntryIdentityProof') 'cancellation' 'Cache counts do not prove cache entry identity or membership.'
	}
	if($Scenario -ceq 'restart') {
		# Shutdown and a new strong publication prove retirement across backend
		# instances. Their local recovery counters may legitimately repeat.
		$shutdown=FsrField $authoritative 'restartShutdownObservation' '$/authoritative'
		foreach($name in @('rendererRootRetired','animationRootRetired','contextReleased')){FsrBoolean (FsrField $shutdown $name) $true ('$/restartShutdownObservation/'+$name)}
		$identity=FsrField $authoritative 'restartPublicationIdentity' '$/authoritative'
		FsrRequire ((FsrString (FsrField $identity 'rendererGenerationDomain') '$/restartPublicationIdentity/rendererGenerationDomain') -ceq 'BackendRecoveryInstanceMayRepeatAcrossReinitialization') 'restart' 'Renderer generation domain must describe backend-local recovery counters.'
		foreach($name in @('freshContextPresent','distinctRetainedPublication')){FsrBoolean (FsrField $identity $name) $true ('$/restartPublicationIdentity/'+$name)}
		$publications=FsrField $authoritative 'restartPublications' '$/authoritative'
		foreach($kind in @('renderer','animation')) {
			$projection=FsrField $publications $kind
			$rootKind=if($kind -ceq 'renderer'){'Renderer'}else{'Animation'}
			FsrRequire ((FsrRoot $projection $rootKind ('$/restartPublications/'+$kind)) -gt 0) 'restart' 'Restart must publish nonempty typed roots.'
		}
		$oldRender=FsrField (FsrField $framesValue[-1] 'renderer') 'root'
		$freshRender=FsrField (FsrField $publications 'renderer') 'root'
		foreach($side in @('old','fresh')) {
			$native=if($side -ceq 'old'){$oldRender}else{$freshRender}
			$record=FsrField $identity $side
			foreach($name in @('rendererSessionGeneration','backendResourceGeneration','generation','frameId')) {
				$null=FsrUnsigned (FsrField $record $name) ('$/restartPublicationIdentity/'+$side+'/'+$name)
				FsrCompareValue (FsrField $record $name) (FsrField $native $name) ('/restartPublicationIdentity/'+$side+'/'+$name+'/nativeJoin')
			}
		}
		$oldAnimation=FsrField (FsrField $framesValue[-1] 'animation') 'root'
		$freshAnimation=FsrField (FsrField $publications 'animation') 'root'
		FsrRequire ((FsrUnsigned (FsrField $oldAnimation 'sessionGeneration') '$/restart/oldAnimation/sessionGeneration') -ne (FsrUnsigned (FsrField $freshAnimation 'sessionGeneration') '$/restart/freshAnimation/sessionGeneration')) 'restart' 'Animation must establish its new session after restart.'
		$retained=FsrField $authoritative 'restartRetainedRoots' '$/authoritative'
		foreach($name in @('heldRenderImmutable','heldAnimationImmutable','preShutdownRenderImmutable','preShutdownAnimationImmutable')){FsrBoolean (FsrField $retained $name) $true ('$/restartRetainedRoots/'+$name)}
	}
	$assertions=FsrArray (FsrField $authoritative 'scenarioAssertions') '$/authoritative/scenarioAssertions';$seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	foreach($assertion in $assertions) {
		$id=FsrString (FsrField $assertion 'id') '$/scenarioAssertions/id';FsrRequire ($seen.Add($id)) 'duplicate-id' 'Duplicate scenario assertion identity.'
		FsrBoolean (FsrField $assertion 'passed') $true ('$/scenarioAssertions/'+$id)
		FsrCompareValue (FsrField $assertion 'expected') (FsrField $assertion 'observed') ('/scenarioAssertions/'+$id)
		if($id -ceq 'actual-workers'){FsrRequire ((FsrUnsigned (FsrField $assertion 'expected') '$/actual-workers') -eq $Workers) 'assertion' 'Actual-worker assertion contradicts observed executor count.'}
		elseif($id -ceq 'observed-frame-count'){FsrRequire ((FsrUnsigned (FsrField $assertion 'expected') '$/observed-frame-count') -eq $Frames) 'assertion' 'Measured-frame assertion contradicts manifest.'}
		else{FsrBoolean (FsrField $assertion 'expected') $true ('$/scenarioAssertions/'+$id+'/expected')}
	}
	foreach($required in @(FsrRequiredAssertions $Scenario $LaneId)){FsrRequire ($seen.Contains($required)) 'assertion' "Missing actual scenario predicate: $LaneId/$required"}
	$diagnostic=FsrField $Lane 'diagnostic';FsrRequireFields $diagnostic @('timings','runtimeIdentifiers') '$/diagnostic'
	if($LaneId -ceq 'ProducerBoundaries'){FsrValidateCausality $Lane $Scenario}
	return $authoritative
}
function FsrValidateRecord([System.Text.Json.JsonElement]$Record,[System.Text.Json.JsonElement]$Scenario,[bool]$Synthetic) {
	FsrRequire ((FsrString (FsrField $Record 'schemaVersion') '$/schemaVersion') -ceq 'frame-stage-replay-v1') 'schema' 'Unsupported replay schema.'
	FsrBoolean (FsrField $Record 'protocolFixture') $Synthetic '$/protocolFixture'
	FsrRequire ((FsrString (FsrField $Record 'status') '$/status') -ceq 'Passed') 'incomplete' 'Incomplete/failed execution cannot establish replay.'
	FsrRequire ((FsrArray (FsrField $Record 'unsupportedObservations') '$/unsupportedObservations').Count -eq 0) 'unsupported' 'Unsupported execution observations cannot establish replay.'
	$id=FsrString (FsrField $Scenario 'id') '$/scenario/id'
	FsrRequire ((FsrString (FsrField $Record 'scenarioId') '$/scenarioId') -ceq $id) 'scenario' 'Scenario identity differs from frozen control.'
	$workers=FsrUnsigned (FsrField $Record 'requestedWorkers') '$/requestedWorkers' 1
	FsrRequire ($workers -in @(1,2,8) -and (FsrUnsigned (FsrField $Record 'actualWorkers') '$/actualWorkers' 1) -eq $workers) 'workers' 'Actual executor count does not equal the requested supported count.'
	$frames=FsrUnsigned (FsrField $Scenario 'measuredFrames') '$/measuredFrames' 1
	$required='["FullEngine","ProducerBoundaries"]';$doc=[Text.Json.JsonDocument]::Parse($required)
	try {
		$join=FsrField $Record 'coverageJoin'
		FsrCompareValue $doc.RootElement (FsrField $join 'requiredLanes') '/coverageJoin/requiredLanes'
		FsrCompareValue $doc.RootElement (FsrField $join 'observedLanes') '/coverageJoin/observedLanes'
		FsrCompareValue $doc.RootElement (FsrField $Scenario 'requiredLanes') '/scenario/requiredLanes'
	}finally{$doc.Dispose()}
	$lanes=FsrArray (FsrField $Record 'lanes') '$/lanes';FsrRequire ($lanes.Count -eq 2) 'lane' 'Both distinct mandatory lanes are required.'
	$null=FsrValidateLane $lanes[0] 'FullEngine' $id $workers $frames
	$null=FsrValidateLane $lanes[1] 'ProducerBoundaries' $id $workers $frames
	return $workers
}
function FsrControlPaths([System.Text.Json.JsonElement]$Join,[string]$Kind,[string]$Reference,[string]$Candidate) {
	# Even equal paths must be explicitly reviewed. No blanket directory prefix
	# substitution can accidentally bless a changed authored input.
	$matches=0
	foreach($pair in (FsrArray (FsrField $Join 'controlPathPairs') '$/controlPathPairs')) {
		if((FsrString (FsrField $pair 'kind') '$/controlPathPairs/kind') -ceq $Kind) {
			$a=FsrString (FsrField $pair 'reference') '$/controlPathPairs/reference';$b=FsrString (FsrField $pair 'candidate') '$/controlPathPairs/candidate'
			FsrRequire ([IO.Path]::IsPathRooted($a) -and [IO.Path]::IsPathRooted($b)) 'provenance' 'Reviewed control paths must be absolute.'
			FsrRequire ([IO.Path]::GetFullPath($a) -ieq [IO.Path]::GetFullPath($Reference) -and [IO.Path]::GetFullPath($b) -ieq [IO.Path]::GetFullPath($Candidate)) 'provenance' "Unreviewed control path for $Kind"
			$matches++
		}
	}
	FsrRequire ($matches -eq 1) 'provenance' "Exactly one reviewed control path pair is required for $Kind"
}
function InvokeFrameStageReplayComparison {
	[CmdletBinding()]
	param([Parameter(Mandatory)][string]$ReferencePath,[Parameter(Mandatory)][string]$CandidatePath,
		[Parameter(Mandatory)][string]$ManifestPath,[Parameter(Mandatory)][string]$ProvenanceJoinPath,
		[string]$SessionMapPath='', [switch]$ProtocolOnly)
	$reference=FsrRead $ReferencePath;$candidate=FsrRead $CandidatePath;$manifest=FsrRead $ManifestPath;$join=FsrRead $ProvenanceJoinPath
	FsrRequire ((FsrString (FsrField $manifest 'schemaVersion') '$/manifest/schemaVersion') -ceq 'frame-replay-workloads-v1') 'schema' 'Wrong frozen workload manifest schema.'
	FsrRequire ((FsrString (FsrField $join 'schemaVersion') '$/join/schemaVersion') -ceq 'frame-stage-replay-provenance-v1') 'provenance' 'Wrong runner provenance join schema.'
	FsrBoolean (FsrField $join 'protocolFixture') $ProtocolOnly.IsPresent '$/join/protocolFixture'
	$pairs=FsrArray (FsrField $join 'controlPathPairs') '$/controlPathPairs';FsrRequire ($pairs.Count -eq 4) 'provenance' 'Exactly four reviewed immutable-control path pairs are required.'
	$scenarios=FsrArray (FsrField $manifest 'scenarios') '$/manifest/scenarios';$ids=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);$selected=$null
	$scenarioId=FsrString (FsrField $reference 'scenarioId') '$/scenarioId'
	foreach($scenario in $scenarios){$id=FsrString (FsrField $scenario 'id') '$/manifest/scenarios/id';FsrRequire ($ids.Add($id)) 'duplicate-id' 'Duplicate workload scenario identity.';if($id -ceq $scenarioId){$selected=$scenario}}
	FsrRequire ($null -ne $selected) 'scenario' 'Unknown replay scenario.'
	$aWorkers=FsrValidateRecord $reference $selected $ProtocolOnly.IsPresent;$bWorkers=FsrValidateRecord $candidate $selected $ProtocolOnly.IsPresent
	FsrRequire ($aWorkers -eq $bWorkers) 'workers' 'Reference/candidate execute different worker counts.'
	$manifestHash=(Get-FileHash -LiteralPath $ManifestPath -Algorithm SHA256).Hash.ToLowerInvariant();$manifestBytes=(Get-Item -LiteralPath $ManifestPath).Length
	foreach($side in @('reference','candidate')) {
		$record=if($side -ceq 'reference'){$reference}else{$candidate};$path=if($side -ceq 'reference'){$ReferencePath}else{$CandidatePath}
		$trusted=FsrField $join $side '$/join';$receipt=FsrPin (FsrField $trusted 'receipt') ('$/join/'+$side+'/receipt')
		FsrRequire ($receipt.path -ieq [IO.Path]::GetFullPath($path)) 'provenance' 'Trusted receipt pin belongs to another file.'
		FsrCompareValue (FsrField $trusted 'runNonce') (FsrField $record 'runNonce') '/runNonce'
		FsrRequire (![string]::IsNullOrWhiteSpace((FsrString (FsrField $record 'runNonce') '$/runNonce'))) 'provenance' 'Run nonce is absent.'
		$controls=FsrField $record 'controls'
		foreach($kind in @('loadedEngine','hostExecutable')){FsrSamePin (FsrPin (FsrField $controls $kind) ('$/controls/'+$kind)) (FsrPin (FsrField $trusted $kind) ('$/join/'+$side+'/'+$kind)) $kind}
		FsrBoolean (FsrField $controls 'engineHashVerified') $true '$/controls/engineHashVerified'
		$expectedEnginePath=FsrString (FsrField $controls 'expectedEnginePath') '$/controls/expectedEnginePath';$observedEnginePath=FsrString (FsrField (FsrField $controls 'loadedEngine') 'path') '$/controls/loadedEngine/path'
		FsrRequire ([IO.Path]::IsPathRooted($expectedEnginePath) -and [IO.Path]::GetFullPath($expectedEnginePath) -ieq [IO.Path]::GetFullPath($observedEnginePath)) 'provenance' 'Loaded engine path differs from its preinitialization trusted ABI path.'
		FsrRequire ((FsrString (FsrField $controls 'expectedEngineSha256') '$/controls/expectedEngineSha256') -ceq (FsrString (FsrField (FsrField $controls 'loadedEngine') 'sha256') '$/controls/loadedEngine/sha256')) 'provenance' 'Engine ABI hash was not verified against the trusted loaded module before initialization.'
		$root=FsrString (FsrField $controls 'projectRoot') '$/controls/projectRoot';$trustedRoot=FsrString (FsrField $trusted 'projectRoot') '$/join/projectRoot'
		FsrRequire ([IO.Path]::IsPathRooted($root) -and [IO.Directory]::Exists($root) -and [IO.Path]::GetFullPath($root) -ieq [IO.Path]::GetFullPath($trustedRoot)) 'provenance' 'Project root differs from trusted launch.';FsrNoRedirect $root
		FsrCompareValue (FsrField $controls 'scenario') $selected '/controls/scenario'
		$manifestPin=FsrPin (FsrField $controls 'manifest') '$/controls/manifest'
		FsrRequire ($manifestPin.sha256 -ceq $manifestHash -and $manifestPin.bytes -eq $manifestBytes) 'control' 'Replay used a different workload manifest.'
	}
	$leftControls=FsrField $reference 'controls';$rightControls=FsrField $candidate 'controls'
	foreach($kind in @('manifest','assetManifest','projectClosure')) {
		$a=FsrPin (FsrField $leftControls $kind) ('$/controls/'+$kind);$b=FsrPin (FsrField $rightControls $kind) ('$/controls/'+$kind)
		FsrRequire ($a.sha256 -ceq $b.sha256 -and $a.bytes -eq $b.bytes) 'control' "Immutable authored control changed: $kind"
		FsrControlPaths $join $kind $a.path $b.path
	}
	FsrControlPaths $join 'projectRoot' (FsrString (FsrField $leftControls 'projectRoot') '$/projectRoot') (FsrString (FsrField $rightControls 'projectRoot') '$/projectRoot')
	$leftResolved=FsrArray (FsrField $leftControls 'resolvedAssetManifests') '$/controls/resolvedAssetManifests';$rightResolved=FsrArray (FsrField $rightControls 'resolvedAssetManifests') '$/controls/resolvedAssetManifests'
	FsrRequire ($leftResolved.Count -gt 0 -and $leftResolved.Count -eq $rightResolved.Count) 'control' 'Resolved asset input closure is missing or changed.'
	$seenLeft=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase);$seenRight=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
	for($i=0;$i -lt $leftResolved.Count;$i++) {
		$a=FsrPin $leftResolved[$i] '$/controls/resolvedAssetManifests/reference';$b=FsrPin $rightResolved[$i] '$/controls/resolvedAssetManifests/candidate'
		FsrRequire ($seenLeft.Add($a.path) -and $seenRight.Add($b.path)) 'control' 'Duplicate resolved asset input file.'
		FsrRequire ($a.sha256 -ceq $b.sha256 -and $a.bytes -eq $b.bytes) 'control' 'Resolved asset input bytes or order changed.'
		if($i -eq 0){FsrSamePin $a (FsrPin (FsrField $leftControls 'assetManifest') '$/controls/assetManifest') 'first resolved reference';FsrSamePin $b (FsrPin (FsrField $rightControls 'assetManifest') '$/controls/assetManifest') 'first resolved candidate'}
		else {
			$relativeA=[IO.Path]::GetRelativePath((FsrString (FsrField $leftControls 'projectRoot') '$/projectRoot'),$a.path);$relativeB=[IO.Path]::GetRelativePath((FsrString (FsrField $rightControls 'projectRoot') '$/projectRoot'),$b.path)
			FsrRequire (![IO.Path]::IsPathRooted($relativeA) -and !$relativeA.StartsWith('..') -and $relativeA -ieq $relativeB -and $relativeA.Replace('\','/') -cmatch '^Build/Assets/\.terra-animation-manifests/[^/]+\.json$') 'control' 'Resolved asset input escaped the explicitly reviewed project-root mapping.'
		}
	}
	$maps=FsrSessionMaps $SessionMapPath
	$leftLanes=FsrArray (FsrField $reference 'lanes') '$/lanes';$rightLanes=FsrArray (FsrField $candidate 'lanes') '$/lanes'
	for($i=0;$i -lt 2;$i++){$lane=FsrString (FsrField $leftLanes[$i] 'laneId') '$/laneId';FsrCompareValue (FsrField $leftLanes[$i] 'authoritative') (FsrField $rightLanes[$i] 'authoritative') '' $maps $lane}
	if($maps.ContainsKey('__used')){FsrRequire ($maps['__used'].Count -eq $maps.Count-1) 'session-map' 'Unused explicit session mapping is not an observation.'}
	return [pscustomobject]@{schemaVersion='frame-stage-replay-comparison-v1';status='Passed';scenarioId=$scenarioId;workers=$aWorkers;lanes=@('FullEngine','ProducerBoundaries');protocolOnly=$ProtocolOnly.IsPresent;replayAcceptance=(!$ProtocolOnly.IsPresent);gpuAcceptance=$false;realProcessesLaunched=$false}
}
function FsrIntegral($Value,[string]$Label) {
	FsrRequire ($null -ne $Value -and $Value -isnot [bool] -and $Value -isnot [string] -and [string]$Value -cmatch '^(0|[1-9][0-9]*)$') 'causality' "Invalid actual integer: $Label"
	return [uint64]$Value
}
function FsrValidateCausality([System.Text.Json.JsonElement]$Lane,[string]$Scenario) {
	# Receipt claims never substitute for the chronological callback trace. This
	# independent check joins every invocation to the returned scheduler plan.
	$value=$Lane.GetRawText()|ConvertFrom-Json -AsHashtable -Depth 100
	$auth=$value.authoritative;$offCoordinatorFault=$false;$rawByFrame=@{};$checksByFrame=@{};$normalized=[Collections.Generic.List[object]]::new()
	foreach($item in $value.diagnostic.runtimeIdentifiers) {
		if($item -is [Collections.IDictionary] -and $item.kind -ceq 'ActualBoundaryObservationOrder') {
			$key=[string](FsrIntegral $item.frameOrdinal 'raw frameOrdinal')
			FsrRequire (!$rawByFrame.ContainsKey($key)) 'causality' 'Duplicate chronological callback frame.';$rawByFrame[$key]=$item
		}
	}
	FsrRequire ($auth.ContainsKey('causality') -and $auth.causality -is [array]) 'causality' 'Missing independently checkable causality summaries.'
	foreach($check in $auth.causality){$key=[string](FsrIntegral $check.frameOrdinal 'causality frameOrdinal');FsrRequire (!$checksByFrame.ContainsKey($key)) 'causality' 'Duplicate causality frame.';$checksByFrame[$key]=$check}
	FsrRequire ($rawByFrame.Count -eq $auth.frames.Count -and $checksByFrame.Count -eq $auth.frames.Count) 'causality' 'Missing/extra raw callback evidence or summaries.'
	foreach($frame in $auth.frames) {
		$key=[string]$frame.ordinal;FsrRequire ($rawByFrame.ContainsKey($key) -and $checksByFrame.ContainsKey($key)) 'causality' 'Callback evidence does not join measured frame.'
		$declarations=[Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal);$results=[Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal);$invocations=[Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal);$byDescriptor=[Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal);$sequence=0;$expected=0;$dependencies=0;$omittedDependencies=0
		foreach($decl in $frame.scheduler.declarations){$declarations[$decl.stableDescriptorId]=$decl}
		foreach($result in $frame.scheduler.results){$results[$result.stableDescriptorId]=$result;FsrRequire ($result.completed -is [bool] -and $result.omitted -is [bool]) 'causality' 'Missing actual completion/omission flags.';$null=FsrIntegral $result.partitionCount 'result partitionCount';$null=FsrIntegral $result.executionPolicy 'result executionPolicy'}
		$events=$rawByFrame[$key].events;FsrRequire ($events -is [array] -and $events.Count -gt 0) 'causality' 'Empty chronological callback evidence.'
		foreach($event in $events) {
			$sequence++;$id=$event.stableDescriptorId
			FsrRequire ($id -is [string] -and $declarations.ContainsKey($id) -and $results.ContainsKey($id)) 'causality' 'Observed callback has no actual declaration/result.'
			FsrRequire ((FsrIntegral $event.frameOrdinal 'event frame') -eq $frame.ordinal -and (FsrIntegral $event.observedSequence 'observedSequence') -eq $sequence) 'causality' 'Chronological sequence is missing, forged, or reordered.'
			FsrRequire ((FsrIntegral $event.executionOrder 'executionOrder') -eq (FsrIntegral $declarations[$id].executionOrder 'declared executionOrder')) 'causality' 'Callback execution order differs from actual declaration.'
			$count=FsrIntegral $event.partitionCount 'partitionCount';$partition=FsrIntegral $event.partitionIndex 'partitionIndex'
			FsrRequire ($count -eq $results[$id].partitionCount -and $partition -lt $count -and (FsrIntegral $event.itemBegin 'itemBegin') -le (FsrIntegral $event.itemEnd 'itemEnd')) 'causality' 'Callback partition metadata contradicts returned plan.'
			FsrRequire ($event.checkpoint -cin @('Enter','Exit') -and $event.failed -is [bool] -and $event.onCoordinator -is [bool]) 'causality' 'Invalid callback checkpoint/failure fact.'
			if($frame.ordinal -eq 0 -and $event.checkpoint -ceq 'Exit' -and $event.failed -and !$event.onCoordinator -and $id -ceq 'RenderSystem/Render/GeometryRoutePrepare' -and $declarations[$id].workloadName -ceq 'GeometryRoutePrepare'){$offCoordinatorFault=$true}
			$invocationKey=$id+'|'+$partition
			if(!$invocations.ContainsKey($invocationKey)){$invocations[$invocationKey]=[Collections.Generic.List[object]]::new()};$invocations[$invocationKey].Add($event)
			if(!$byDescriptor.ContainsKey($id)){$byDescriptor[$id]=[Collections.Generic.List[object]]::new()};$byDescriptor[$id].Add($event)
		}
		foreach($pair in $invocations.Values) {
			FsrRequire ($pair.Count -eq 2 -and $pair[0].checkpoint -ceq 'Enter' -and $pair[1].checkpoint -ceq 'Exit' -and $pair[0].observedSequence -lt $pair[1].observedSequence) 'causality' 'Callback Enter/Exit is missing, duplicated, or reversed.'
			foreach($field in @('partitionCount','itemBegin','itemEnd','onCoordinator')){FsrRequire ($pair[0][$field] -eq $pair[1][$field]) 'causality' 'Callback partition metadata changed between Enter and Exit.'}
		}
		foreach($result in $frame.scheduler.results) {
			# A dormant scheduler-owned node has no callback to settle. Only the
			# complete actual omission tuple authorizes this narrowly scoped case.
			if($result.omitted -or $result.executionPolicy -eq 3){FsrRequire ($result.omitted -and $result.executionPolicy -eq 3 -and $result.completed -and $result.partitionCount -eq 0 -and !$byDescriptor.ContainsKey($result.stableDescriptorId)) 'causality' 'Contradictory native omission flags, policy, completion, partition count, or observed callback.'}
			if($result.completed -and !$result.omitted){$expected+=$result.partitionCount;for($part=0;$part -lt $result.partitionCount;$part++){FsrRequire ($invocations.ContainsKey($result.stableDescriptorId+'|'+$part)) 'causality' 'Completed partition has no actual callback.'}}
		}
		foreach($edge in $frame.scheduler.dependencyEdges) {
			FsrRequire ($edge -is [array] -and $edge.Count -eq 2) 'causality' 'Invalid dependency edge shape.'
			$beforeIndex=FsrIntegral $edge[0] 'prerequisiteIndex';$afterIndex=FsrIntegral $edge[1] 'dependentIndex'
			FsrRequire ($beforeIndex -lt $frame.scheduler.declarations.Count -and $afterIndex -lt $frame.scheduler.declarations.Count) 'causality' 'Dependency index outside actual plan.'
			$before=$frame.scheduler.declarations[$beforeIndex].stableDescriptorId;$after=$frame.scheduler.declarations[$afterIndex].stableDescriptorId
			if(!$byDescriptor.ContainsKey($after)){continue}
			if($results.ContainsKey($before)){$prerequisite=$results[$before];if($prerequisite.omitted -and $prerequisite.executionPolicy -eq 3 -and $prerequisite.completed -and $prerequisite.partitionCount -eq 0 -and !$byDescriptor.ContainsKey($before)){$omittedDependencies++;continue}}
			$dependencies++
			FsrRequire ($byDescriptor.ContainsKey($before)) 'causality' 'Entered dependent has no observed prerequisite.'
			$lastExit=($byDescriptor[$before]|Where-Object checkpoint -CEQ 'Exit'|Measure-Object observedSequence -Maximum).Maximum
			$firstEnter=($byDescriptor[$after]|Where-Object checkpoint -CEQ 'Enter'|Measure-Object observedSequence -Minimum).Minimum
			FsrRequire ($null -ne $lastExit -and $null -ne $firstEnter -and $lastExit -lt $firstEnter) 'causality' 'Dependent entered before every prerequisite partition settled.'
		}
		$check=$checksByFrame[$key]
		FsrRequire ($check.passed -is [bool] -and $check.passed -and $check.issues -is [array] -and $check.issues.Count -eq 0) 'causality' 'Collector reported failed or incomplete causality.'
		FsrRequire ((FsrIntegral $check.observedInvocationCount 'observedInvocationCount') -eq $invocations.Count -and (FsrIntegral $check.expectedCompletedInvocationCount 'expectedCompletedInvocationCount') -eq $expected -and (FsrIntegral $check.checkedDependencyCount 'checkedDependencyCount') -eq $dependencies -and (FsrIntegral $check.omittedDependencyCount 'omittedDependencyCount') -eq $omittedDependencies) 'causality' 'Causality summary does not match raw evidence.'
		$ordered=[Collections.Generic.List[object]]::new();foreach($event in $events){$ordered.Add($event)}
		$ordered.Sort([Comparison[object]]{
			param($left,$right)
			$comparison=([uint64]$left.executionOrder).CompareTo([uint64]$right.executionOrder);if($comparison){return $comparison}
			$comparison=[StringComparer]::Ordinal.Compare([string]$left.stableDescriptorId,[string]$right.stableDescriptorId);if($comparison){return $comparison}
			$comparison=([uint64]$left.partitionIndex).CompareTo([uint64]$right.partitionIndex);if($comparison){return $comparison}
			return ([int]($left.checkpoint -ceq 'Exit')).CompareTo([int]($right.checkpoint -ceq 'Exit'))
		})
		foreach($event in $ordered){$copy=@{};foreach($field in $event.Keys){if($field -cne 'observedSequence'){$copy[$field]=$event[$field]}};$normalized.Add($copy)}
	}
	if($Scenario -ceq 'worker-failure'){FsrRequire $offCoordinatorFault 'causality' 'Worker failure has no actual failed off-coordinator callback.'}
	$doc=[Text.Json.JsonDocument]::Parse((ConvertTo-Json -InputObject @($normalized.ToArray()) -Depth 100))
	try{FsrCompareValue $doc.RootElement (FsrField (FsrField $Lane 'authoritative') 'boundaries') '/boundaries/raw-join'}finally{$doc.Dispose()}
}
if($MyInvocation.InvocationName -ne '.') {
	$ErrorActionPreference='Stop';$exitCode=1
	try {
		FsrRequire (![string]::IsNullOrWhiteSpace($OutputPath) -and [IO.Path]::IsPathRooted($OutputPath) -and ![IO.File]::Exists($OutputPath)) 'output' 'Output must be an absolute fresh file.'
		FsrNoRedirect $OutputPath
		$result=InvokeFrameStageReplayComparison -ReferencePath $ReferencePath -CandidatePath $CandidatePath -ManifestPath $ManifestPath -ProvenanceJoinPath $ProvenanceJoinPath -SessionMapPath $SessionMapPath -ProtocolOnly:$ProtocolOnly
		$exitCode=0
	} catch {$result=[pscustomobject]@{schemaVersion='frame-stage-replay-comparison-v1';status='Failed';protocolOnly=$ProtocolOnly.IsPresent;replayAcceptance=$false;gpuAcceptance=$false;realProcessesLaunched=$false;error=$_.Exception.Message}}
	if($OutputPath -and [IO.Path]::IsPathRooted($OutputPath) -and ![IO.File]::Exists($OutputPath)) {
		FsrNoRedirect $OutputPath;$stream=[IO.File]::Open($OutputPath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
		try{$bytes=[Text.UTF8Encoding]::new($false).GetBytes(($result|ConvertTo-Json -Depth 30));$stream.Write($bytes,0,$bytes.Length)}finally{$stream.Dispose()}
	}
	$result|ConvertTo-Json -Depth 30;exit $exitCode
}
