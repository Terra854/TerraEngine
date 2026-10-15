<# Installed parity evidence join. Does not launch hosts, compare new images, or certify timing.
Uses actual three-scale matching-ABI comparator receipts and retains their limits.
Usage: & ./Join-RuntimeMemorySafetyParity.ps1 -ParityRunPath <absolute parity-run.json>
 -InstallRoot <current Build> -FrozenToolRoot <immutable v2 Build>
 -ComparatorPath <pinned Compare-RuntimeMemorySafetyParity.py> -OutputPath <fresh JSON>
#>
[CmdletBinding()]
param([string]$ParityRunPath,[string]$InstallRoot,[string]$FrozenToolRoot,[string]$ComparatorPath,[string]$OutputPath)
$script:RmsParityJoinerPath=$PSCommandPath
function RmsParityRequire([bool]$Condition,[string]$Message){if(!$Condition){throw "Parity join rejected: $Message"}}
function RmsParityField($Value,[string]$Name){RmsParityRequire ($null -ne $Value -and $null -ne $Value.PSObject.Properties[$Name]) "Missing $Name";return $Value.$Name}
function RmsParityBool($Value,[string]$Name,[bool]$Expected=$true){$v=RmsParityField $Value $Name;RmsParityRequire ($v -is [bool] -and $v -eq $Expected) "Unobserved or contradictory $Name"}
function RmsParityUint($Value,[string]$Name,[long]$Minimum=0){$v=RmsParityField $Value $Name;RmsParityRequire ($v -is [ValueType] -and $v -isnot [bool] -and [double]$v -ge $Minimum -and [double]$v -le 9007199254740991 -and [double]$v -eq [Math]::Truncate([double]$v)) "Invalid integer $Name";return [long]$v}
function RmsParityInstant($Value){
	# PowerShell JSON readers may return either date objects or strings. Avoid
	# casting an actual date to locale-dependent display text before parsing it.
	if($Value -is [DateTimeOffset]){return $Value.ToUniversalTime()}
	if($Value -is [DateTime]){RmsParityRequire ($Value.Kind -ne [DateTimeKind]::Unspecified) 'Timestamp has no timezone';return [DateTimeOffset]::new($Value).ToUniversalTime()}
	RmsParityRequire ($Value -is [string] -and $Value -cmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,7})?(?:Z|[+-]\d{2}:\d{2})$') 'Malformed or ambiguous timestamp'
	$instant=[DateTimeOffset]::MinValue
	RmsParityRequire ([DateTimeOffset]::TryParse($Value,[Globalization.CultureInfo]::InvariantCulture,[Globalization.DateTimeStyles]::None,[ref]$instant)) 'Invalid ISO timestamp'
	return $instant.ToUniversalTime()
}
function RmsParityCommandChronology($Command){
	$start=RmsParityInstant (RmsParityField $Command startedUtc);$end=RmsParityInstant (RmsParityField $Command completedUtc)
	RmsParityRequire ($end -ge $start) 'Command completion precedes its start'
}
function RmsParityPath([string]$Path){
	RmsParityRequire ([IO.Path]::IsPathRooted($Path)) 'Absolute paths required';$full=[IO.Path]::GetFullPath($Path);$cursor=$full
	while($cursor){if(Test-Path -LiteralPath $cursor){RmsParityRequire (((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) "Redirected path $cursor"};$cursor=[IO.Path]::GetDirectoryName($cursor)};return $full
}
function RmsParityPin([string]$Path){$full=RmsParityPath $Path;$before=Get-Item -LiteralPath $full;RmsParityRequire (!$before.PSIsContainer) 'Pin requires file';$length=$before.Length;$ticks=$before.LastWriteTimeUtc.Ticks;$hash=(Get-FileHash -LiteralPath $full).Hash.ToLowerInvariant();$after=Get-Item -LiteralPath $full;RmsParityRequire ($length -eq $after.Length -and $ticks -eq $after.LastWriteTimeUtc.Ticks) "File changed during hashing $full";return [pscustomobject]@{path=$full;bytes=$length;sha256=$hash}}
function RmsParityKeep($Pin){
	$path=RmsParityField $Pin path;$hash=RmsParityField $Pin sha256;$bytes=RmsParityUint $Pin bytes
	RmsParityRequire ($hash -cmatch '^[a-f0-9]{64}$') 'Invalid SHA-256';$key=[IO.Path]::GetFullPath($path)
	if($script:parityPins.ContainsKey($key)){$prior=$script:parityPins[$key];RmsParityRequire ($prior.sha256 -ceq $hash -and $prior.bytes -eq $bytes) "Conflicting pins $path";return}
	$actual=RmsParityPin $path;RmsParityRequire ($actual.sha256 -ceq $hash -and $actual.bytes -eq $bytes) "Changed pinned evidence $path";$script:parityPins[$key]=$actual
}
function RmsParityWalk($Value){
	if($null -eq $Value -or $Value -is [string] -or $Value -is [ValueType]){return}
	if($Value -is [Collections.IEnumerable] -and $Value -isnot [pscustomobject]){foreach($item in $Value){RmsParityWalk $item};return}
	if($null -ne $Value.PSObject.Properties['path'] -and $null -ne $Value.PSObject.Properties['sha256'] -and $null -ne $Value.PSObject.Properties['bytes']){RmsParityKeep $Value;return}
	foreach($property in $Value.PSObject.Properties){RmsParityWalk $property.Value}
}
function RmsParityJsonElement($Element){
	if($Element.ValueKind -eq [Text.Json.JsonValueKind]::Object){$keys=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);foreach($property in $Element.EnumerateObject()){RmsParityRequire ($keys.Add($property.Name)) "Duplicate JSON key $($property.Name)";RmsParityJsonElement $property.Value}}
	elseif($Element.ValueKind -eq [Text.Json.JsonValueKind]::Array){foreach($item in $Element.EnumerateArray()){RmsParityJsonElement $item}}
}
function RmsParityRead([string]$Path){$pin=RmsParityPin $Path;RmsParityKeep $pin;$text=[IO.File]::ReadAllText($pin.path);$doc=[Text.Json.JsonDocument]::Parse($text);try{RmsParityJsonElement $doc.RootElement}finally{$doc.Dispose()};return ConvertFrom-Json -InputObject $text -Depth 100}
function RmsParityEqual($Left,$Right,[string]$Message){RmsParityRequire (($Left|ConvertTo-Json -Depth 100 -Compress) -ceq ($Right|ConvertTo-Json -Depth 100 -Compress)) $Message}
function RmsParityImage($Row){
	$image=RmsParityField $Row image;$extent=@(RmsParityField $Row nativeExtent);RmsParityRequire ($extent.Count -eq 2 -and $extent[0] -gt 0 -and $extent[1] -gt 0) 'Missing complete image extent'
	$pixels=RmsParityUint $image comparedPixels 1;$channels=RmsParityUint $image comparedChannels 1
	RmsParityRequire ($pixels -eq [long]$extent[0]*[long]$extent[1] -and $channels -eq $pixels*4 -and (RmsParityUint $image maximumChannelError) -le 2 -and (RmsParityUint $image channelsExceedingTolerance) -eq 0 -and (RmsParityField $image firstFailure) -eq $null) 'Whole-image comparison failed/incomplete'
	RmsParityRequire ((RmsParityField $image maximumNormalizedChannelError) -eq $image.maximumChannelError/255.0) 'Normalized image error differs from actual channel delta'
	foreach($kind in @('baselineCommand','candidateCommand')){foreach($name in @('frameSerial','commandSerial','submitSerial','completionSerial')){$null=RmsParityUint (RmsParityField $Row $kind) $name 1}}
	foreach($kind in @('baselineRawColor','candidateRawColor','baselineRgba8','candidateRgba8')){RmsParityKeep (RmsParityField $Row $kind)}
	foreach($kind in @('baselineRgba8','candidateRgba8')){RmsParityRequire ($Row.$kind.bytes -eq $channels) 'RGBA comparison byte count differs'}
}
function RmsParityNormalCoverage($Comparison,$Hosts,$Definition){
	RmsParityBool $Comparison normalCommandCohortEqual
	$coverage=RmsParityField $Comparison normalOutputCoverage
	foreach($name in @('authoredCameraCount','normalQueuedCameraCommands','backendSnapshotCapacity','expectedAdmittedCameraCommands','actualRecordedCameraCommands','actualNormalPublishedViewCount','actualCommandCount','lightingArtifactCommandCount','lightingPurposeArtifactCount')){$null=RmsParityUint $coverage $name 1}
	$admitted=[long]$Definition.cameraCount;$artifactCommands=RmsParityUint $coverage lightingArtifactCommandCount 1
	RmsParityRequire ($coverage.authoredCameraCount -eq $Definition.cameraCount -and $coverage.normalQueuedCameraCommands -eq $Definition.cameraCount -and $coverage.backendSnapshotCapacity -eq 8 -and $coverage.expectedAdmittedCameraCommands -eq $admitted -and $coverage.actualRecordedCameraCommands -eq $admitted -and $coverage.actualNormalPublishedViewCount -eq 1 -and $coverage.actualCommandCount -eq $admitted -and $artifactCommands -le $admitted -and $coverage.lightingPurposeArtifactCount -eq 2*$artifactCommands -and $coverage.queuePolicy -ceq 'AllAuthoredCamerasInCapturedOrderSharedGameOutput2' -and $coverage.readbackLimit -ceq 'Lighting payload readback deduplicates frame/view/purpose; final Game View covers the completed normal shared target' -and $coverage.capacityLimit -ceq 'Editor required-output overflow submits and completes a prefix, then reuses the same eight physical snapshot slices' -and $coverage.capacityPolicy -ceq 'EditorRequiredOutputOverflowSerializesAndReusesEightPhysicalSlices') 'Actual normal output coverage/counts/limits missing'
	RmsParityBool $coverage perCameraImageCoverage $false;RmsParityBool $coverage privateReferenceArraysObserved $false
	$semantic=@{}
	foreach($variant in @('Baseline','Candidate')){
		$nativeHost=$Hosts[$variant];RmsParityEqual $coverage $nativeHost.normalOutputCoverage 'Host/comparator coverage differs'
		$rows=@(RmsParityField $nativeHost normalCommandMetadata);RmsParityRequire ($rows.Count -eq 2*$admitted) 'Missing actual admitted normal camera command metadata'
		$lastCommand=0;$validCommands=0;$semantic[$variant]=@()
		for($ordinal=0;$ordinal -lt $admitted;$ordinal++){
			$pair=@($rows[$ordinal*2],$rows[$ordinal*2+1]);$serial=RmsParityUint $pair[0] commandSerial 1;RmsParityRequire ($serial -gt $lastCommand) 'Normal command serial order changed';$lastCommand=$serial
			RmsParityRequire ($pair[0].status -in @(1,2) -and $pair[0].status -eq $pair[1].status -and ($ordinal -ne 0 -or $pair[0].status -eq 1)) 'Native command lacks both complete purpose observations';if($pair[0].status -eq 1){$validCommands++}
			for($purpose=0;$purpose -lt 2;$purpose++){
				$row=$pair[$purpose];RmsParityBool $row analysisPurpose ([bool]$purpose);RmsParityBool $row preparationSettled
				RmsParityRequire ($row.commandOrdinal -eq $ordinal -and $row.commandSerial -eq $serial -and $row.frameSerial -eq $nativeHost.selectedFrameSerial -and $row.viewType -eq 0 -and $row.viewId -eq 2) 'Native command frame/order/view identity differs'
				foreach($name in @('fixtureControlGeneration','settingsGeneration','optionsSessionGeneration')){$null=RmsParityUint $row $name 1}
				RmsParityRequire ($row.fixtureControlGeneration -eq $nativeHost.fixtureControlAdmission.controlGeneration -and $row.settingsGeneration -eq $nativeHost.fixtureControlAdmission.settingsGeneration) 'Actual command/admitted control settings differ'
				if($row.status -eq 1){RmsParityRequire ($row.reason -ceq '') 'Valid lighting command has unavailable reason';RmsParityBool $row hasArtifacts;foreach($name in @('submitSerial','completionSerial')){$null=RmsParityUint $row $name 1}}
				else{RmsParityRequire ($row.status -eq 2 -and $row.reason -ceq 'Duplicate native lighting purpose observation.') 'Later duplicate metadata must state actual unavailable reason';RmsParityBool $row hasArtifacts $false;foreach($name in @('submitSerial','completionSerial')){RmsParityRequire ((RmsParityUint $row $name) -eq 0) 'Unavailable metadata cannot claim a submission/completion receipt'}}
				$semantic[$variant]+=,[ordered]@{commandOrdinal=$ordinal;analysisPurpose=[bool]$purpose;status=$row.status;reason=$row.reason;viewType=$row.viewType;viewId=$row.viewId;hasArtifacts=$row.hasArtifacts}
			}
		}
		RmsParityRequire ($validCommands -eq $artifactCommands) 'Observed artifact command count differs from actual metadata'
	}
	RmsParityEqual $semantic.Baseline $semantic.Candidate 'Actual ordered command metadata semantics differ'
}
function RmsParityFinalOutput($Comparison,$Hosts){
	foreach($variant in @('Baseline','Candidate')){
		$nativeHost=$Hosts[$variant];$output=RmsParityField $Comparison ($variant.ToLowerInvariant()+'FinalNormalOutput');RmsParityEqual $output $nativeHost.finalNormalOutput 'Actual final output/comparator proof differs'
		RmsParityRequire ($output.policy -ceq 'EditorNormalGameViewAfterAllAuthoredCameraSubmissions' -and $output.frameId -eq $nativeHost.selectedFrameSerial -and $output.freshness -ceq 'latestCompleted' -and $output.mimeType -ceq 'image/png') 'Final normal completed output identity/policy missing'
		foreach($name in @('ok','backendReadback')){RmsParityBool $output $name}
		foreach($name in @('deterministicFallback','pendingAsync','dedicatedRenderTarget','allowBackendFallback','overlaysIncluded')){RmsParityBool $output $name $false}
		RmsParityBool $output.presentation submitted;$null=RmsParityUint $output.presentation submissionSerial 1
		RmsParityRequire ((RmsParityUint $output.presentation nativeResult) -eq 0) 'Normal editor presentation failed';RmsParityKeep $output.png
		$size=@($output.size);RmsParityRequire ($size.Count -eq 2 -and $size[0] -eq 1280 -and $size[1] -eq 720) 'Final normal output extent differs from native comparison policy'
		$diagnostics=RmsParityField $output snapshotDiagnostics;RmsParityBool $diagnostics valid;RmsParityRequire ((RmsParityUint $diagnostics maxSnapshotsPerFrame 1) -eq 8) 'Actual physical snapshot capacity differs'
		foreach($name in @('peakSnapshotCount','activeSnapshotCount','completedSnapshotCount')){$null=RmsParityUint $diagnostics $name}
		# These diagnostics count admitted owners cumulatively; their peak can
		# exceed the eight physical slices after a successful editor prefix flush.
		$events=@(RmsParityField $diagnostics fallbackEvents)
		foreach($event in $events){RmsParityRequire ($event.kind -eq 2 -and $event.reason -eq 1 -and $event.result -eq 1 -and $event.outputId -eq 2 -and $event.required -is [bool] -and $event.required) 'Unexpected native normal-output fallback'}
		RmsParityRequire ($events.Count -eq [Math]::Floor(($Comparison.normalOutputCoverage.authoredCameraCount-1)/8)) 'Missing actual inherited editor prefix serialization event'
	}
	RmsParityEqual $Comparison.baselineFinalNormalOutput.snapshotDiagnostics $Comparison.candidateFinalNormalOutput.snapshotDiagnostics 'Actual snapshot counts/fallback events differ'
	RmsParityEqual $Comparison.baselineFinalNormalOutput.size $Comparison.candidateFinalNormalOutput.size 'Final normal output dimensions differ'
	$image=RmsParityField $Comparison finalNormalOutputImage;$size=$Comparison.baselineFinalNormalOutput.size;$pixels=[long]$size[0]*[long]$size[1]
	RmsParityRequire ((RmsParityUint $image comparedPixels 1) -eq $pixels -and (RmsParityUint $image comparedChannels 1) -eq $pixels*4 -and (RmsParityUint $image maximumChannelError) -le 2 -and (RmsParityUint $image channelsExceedingTolerance) -eq 0 -and $image.maximumNormalizedChannelError -eq $image.maximumChannelError/255.0 -and $null -eq $image.firstFailure) 'Final normal output whole-image/alpha comparison failed'
}
function RmsParitySourcePair($Pair,[string]$RelativePath){
	RmsParityRequire ($Pair.referenceGitBlob -cmatch '^[a-f0-9]{40}$') 'Missing native source Git blob'
	$bytes=[IO.File]::ReadAllBytes($Pair.referenceSource.path);$header=[Text.Encoding]::UTF8.GetBytes('blob '+$bytes.Length+[char]0);$blob=[Security.Cryptography.SHA1]::HashData([byte[]]($header+$bytes));$actual=[Convert]::ToHexString($blob).ToLowerInvariant();RmsParityRequire ($actual -ceq $Pair.referenceGitBlob) 'Archived native source does not match actual Git blob'
	RmsParityRequire ($Pair.candidateSource.path.Replace('\','/').EndsWith('/'+$RelativePath,[StringComparison]::OrdinalIgnoreCase)) 'Current native source proof points elsewhere'
}
function RmsParitySamePin($Left,$Right,[string]$Message){
	RmsParityKeep $Left;RmsParityKeep $Right
	RmsParityRequire ([IO.Path]::GetFullPath($Left.path) -ieq [IO.Path]::GetFullPath($Right.path) -and $Left.sha256 -ceq $Right.sha256 -and $Left.bytes -eq $Right.bytes) $Message
}
function RmsParitySourceProof($Comparison,$Hosts,[string]$ReferenceRevision,$ReferenceReceipt=$null){
	$pin=RmsParityField $Comparison rendererSourceProof;RmsParityKeep $pin;$proof=RmsParityRead $pin.path;RmsParityWalk $proof
	RmsParityRequire ($proof.sourceRevision -ceq $ReferenceRevision -and $proof.referenceGitBlob -cmatch '^[a-f0-9]{40}$' -and $null -ne $proof.findings -and ![string]::IsNullOrWhiteSpace($proof.interpretation)) 'Old/current native queue-flow provenance missing'
	# The portable support manifest describes headers, not the archive receipt.
	# Bind the generated source proof explicitly to the actual native/run archive
	# file and retain its generator/Git provenance instead of inferring this join.
	$provenance=RmsParityField $proof provenance;$archivePin=RmsParityField $provenance referenceReceipt
	RmsParityKeep (RmsParityField $provenance generator);RmsParityKeep (RmsParityField $provenance gitExecutable)
	foreach($variant in @('Baseline','Candidate')){RmsParitySamePin $archivePin (RmsParityField $Hosts[$variant] archiveReceipt) 'Source proof and native host archive receipt differ'}
	if($null -ne $ReferenceReceipt){RmsParitySamePin $archivePin $ReferenceReceipt 'Source proof and actual parity-run archive receipt differ'}
	$archive=RmsParityRead $archivePin.path;RmsParityRequire ((RmsParityField $archive sourceRevision) -ceq $ReferenceRevision) 'Pinned archive source revision differs from native source proof'
	foreach($variant in @('Baseline','Candidate')){RmsParityRequire ($Hosts[$variant].rendererSourceProof.sha256 -ceq $pin.sha256) 'Host/comparator native queue-flow proof differs'}
	RmsParitySourcePair $proof 'Engine/Renderer/RenderSystem.cpp'
	RmsParityRequire ((RmsParityUint $proof backendSnapshotCapacity 1) -eq 8) 'Native capacity source proof differs'
	$additional=@(RmsParityField $proof additionalSources)
	foreach($name in @('VulkanShadowValidationReadback.cpp','VulkanReadback.cpp','VulkanEditorRendering.cpp','VulkanContextImpl.h','VulkanFrameRendering.cpp')){
		$relative='Engine/Renderer/RHI/Vulkan/'+$name;$pair=@($additional|Where-Object {$_.sourcePath -ceq $relative});RmsParityRequire ($pair.Count -eq 1) "Missing native admission/readback/presentation source pair $name";RmsParitySourcePair $pair[0] $relative
		foreach($side in @('referenceSource','candidateSource')){
			$text=[IO.File]::ReadAllText($pair[0].$side.path)
			if($name -ceq 'VulkanContextImpl.h'){RmsParityRequire ([regex]::IsMatch($text,'static\s+constexpr\s+uint32_t\s+MAX_RENDER_SNAPSHOTS_PER_FRAME\s*=\s*8u\s*;')) 'Old/current backend capacity definition differs'}
			if($name -ceq 'VulkanFrameRendering.cpp'){
				foreach($expression in @('if\s*\(\s*!impl->editorMode\s*\)','(?:vkQueueSubmit|SubmitGraphicsQueue)\s*\(','(?:vkQueueWaitIdle|WaitGraphicsQueueIdle)\s*\(','renderSnapshotSlotsUsed\s*\[\s*currentFrame\s*\]\.fill\(false\)','RenderSnapshotFallbackResult::SerializedSafely')){RmsParityRequire ([regex]::IsMatch($text,$expression)) 'Old/current editor prefix submit/wait/reuse source proof missing'}
			}
		}
	}
	$compat=RmsParityField $proof referenceCompatibilityCapacityHeader;RmsParityKeep $compat
	RmsParityRequire ([regex]::IsMatch([IO.File]::ReadAllText($compat.path),'static\s+constexpr\s+uint32_t\s+MAX_RENDER_SNAPSHOTS_PER_FRAME\s*=\s*8u\s*;')) 'Actual archived compatibility header capacity differs'
}
function RmsParityScratch($Comparison,$Candidate,$Definition){
	RmsParityBool $Comparison candidateCapturedArraysObservedAndEqual;RmsParityBool $Comparison authoredCameraLightValuesIdentityAndOrderEqual;RmsParityBool $Comparison actualOrderedGpuLightPayloadAndIdsEqual
	$scratch=RmsParityField $Comparison candidateScratch;$native=RmsParityField $Candidate candidateScratch;RmsParityBool $scratch capturedArraysEqual;RmsParityBool $native typedIdentityOrderComponentBitsEqual
	foreach($key in @('expectedInputs','capturedInputs','capturedEpisode','terminalEpisode','selectedFrameReclamation','retirementReclamation')){RmsParityEqual $scratch.$key $native.$key "Comparator/native scratch proof differs: $key"}
	$authored=RmsParityRead $Candidate.authoredInputs.path;$expected=RmsParityRead $scratch.expectedInputs.path;$captured=RmsParityRead $scratch.capturedInputs.path
	RmsParityRequire ($expected.observationKind -ceq 'independentProtectedQuery' -and $captured.observationKind -ceq 'actualNativeCapturedArrays') 'Typed array observation provenance missing'
	RmsParityRequire ($authored.observationKind -ceq 'protectedAuthoredInputProjection' -and @($expected.cameras).Count -eq $Definition.cameraCount -and @($expected.lights).Count -eq $Definition.lightCount) 'Typed array cohort differs from frozen scale'
	foreach($kind in @('cameras','lights')){
		$projection=@($authored.$kind|ForEach-Object {$copy=[ordered]@{};foreach($property in $_.PSObject.Properties){if($property.Name -cne 'pose'){$copy[$property.Name]=$property.Value}};[pscustomobject]$copy})
		RmsParityEqual @($expected.$kind) $projection 'Independent protected component oracle differs';RmsParityEqual @($captured.$kind) @($expected.$kind) 'Actual typed capture IDs/order/component values differ'
	}
	$capture=$scratch.capturedEpisode;$terminal=$scratch.terminalEpisode
	foreach($name in @('lifetimeId','episodeId','frameId','capacityBytes','resetCount')){RmsParityRequire ((RmsParityUint $capture $name) -eq (RmsParityUint $terminal $name)) 'Captured/terminal identity differs'}
	RmsParityRequire ($capture.lifetimeId -gt 0 -and $capture.episodeId -gt 0 -and $capture.frameId -eq $Candidate.selectedFrameSerial -and $capture.capacityBytes -eq 16777216 -and $capture.usedBytes -gt 0 -and $capture.outcomeCode -eq 1 -and $terminal.outcomeCode -eq 2 -and $terminal.usedBytes -ge $capture.usedBytes) 'Selected native episode did not complete'
	$count=RmsParityUint $terminal fallbackAllocationCount;RmsParityRequire ($count -eq (RmsParityUint $Comparison actualCandidateFallbackAllocationCount) -and $count -eq (RmsParityUint $scratch actualFallbackAllocationCount)) 'Fallback count is not actual selected terminal pressure'
	$retired=$scratch.retirementReclamation;RmsParityRequire ($retired.lifetimeId -eq $terminal.lifetimeId -and (RmsParityUint $retired totalFallbackAllocationCount) -eq (RmsParityUint $retired totalFallbackDeallocationCount) -and (RmsParityUint $retired outstandingFallbackAllocationCount) -eq 0 -and (RmsParityUint $retired outstandingFallbackBytes) -eq 0) 'Actual retirement not balanced';return $count
}
function RmsParityLightingCohort($Comparison,$Hosts,$Definition){
		$rows=@($comparison.comparisons);$artifactCount=$comparison.normalOutputCoverage.lightingArtifactCommandCount;RmsParityRequire ($rows.Count -eq 2*$artifactCount) 'Incomplete actual available lighting-purpose artifact cohort'
		$artifactOrdinals=@{}
		for($i=0;$i -lt $rows.Count;$i++){
			$row=$rows[$i];RmsParityRequire ($row.observedRowOrdinal -eq $i -and $row.cohortOrdinal -ge 0 -and $row.cohortOrdinal -lt $Definition.cameraCount -and $row.viewType -eq 0 -and $row.viewId -eq 2 -and $row.analysisPurpose -is [bool]) 'Actual available lighting artifact order missing';RmsParityImage $row
			foreach($variant in @('Baseline','Candidate')){$metadata=@($hosts[$variant].normalCommandMetadata|Where-Object {$_.commandOrdinal -eq $row.cohortOrdinal -and $_.analysisPurpose -eq $row.analysisPurpose});RmsParityRequire ($metadata.Count -eq 1 -and $metadata[0].status -eq 1 -and $metadata[0].hasArtifacts) 'Available artifact metadata purpose missing';foreach($name in @('frameSerial','commandSerial','submitSerial','completionSerial')){RmsParityRequire ($row.($variant.ToLowerInvariant()+'Command').$name -eq $metadata[0].$name) 'Available image/payload command differs from actual native metadata'}}
			$key=[string]$row.cohortOrdinal+':'+[string]$row.analysisPurpose;RmsParityRequire (!$artifactOrdinals.ContainsKey($key)) 'Repeated available artifact command/purpose';$artifactOrdinals[$key]=$true
		}
		foreach($metadata in @($hosts.Candidate.normalCommandMetadata|Where-Object {$_.status -eq 1})){RmsParityRequire ($artifactOrdinals.ContainsKey([string]$metadata.commandOrdinal+':'+[string]$metadata.analysisPurpose)) 'A real available native artifact was omitted from comparison'}
}
function RmsParityQualification($Qualification,$Materialization,$Comparison){
	$q=$Qualification;$m=$Materialization
	RmsParityRequire ($q.schemaVersion -ceq 'runtime-memory-safety-qualified-runtime-v1' -and $q.status -ceq 'Qualified' -and $q.policy -ceq 'FullActualPostImportReferenceRuntimeClosure' -and $q.scale -ceq $Comparison.scale) 'Wrong actual qualification schema/policy/scale'
	$project=RmsParityPath $q.projectRoot;RmsParityRequire ($project -ieq (RmsParityPath $m.projectRoot)) 'Qualification/materialization project differs'
	foreach($name in @('originalMaterialization','referenceClosureReceipt','referenceCapture','referenceLifecycle','originalPackageManifest')){RmsParityKeep (RmsParityField $q $name)}
	RmsParityEqual (RmsParityRead $q.originalMaterialization.path) $m 'Original immutable materialization changed'
	RmsParitySamePin $q.referenceClosureReceipt $Comparison.qualificationSource 'Comparison used a different reference closure'
	$closure=RmsParityRead $q.referenceClosureReceipt.path;RmsParityRequire ($closure.schemaVersion -ceq 'runtime-memory-safety-post-import-closure-v1' -and $closure.status -ceq 'ReferenceObserved') 'Wrong qualified reference closure protocol'
	$reference=@($closure.references|Where-Object {$_.lane -ceq ($q.scale+'-A1')});RmsParityRequire ($reference.Count -eq 1) 'Reference qualification source lane missing/duplicated'
	RmsParitySamePin $q.referenceCapture $reference[0].capture 'Qualification reference capture differs';RmsParitySamePin $q.referenceLifecycle $reference[0].lifecycle 'Qualification reference normal closure differs'
	$manifestRelative='Build/Assets/.terra-animation-manifests/da35202f311bd5ea56e23beb0aafbd9a.json'
	$required=@($manifestRelative,'Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroIdle_493d9bc3.teanim','Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroIdle_fad19fd5.teanim','Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroSkeleton_9beae902.teskel','Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroSkeleton_b7d2176c.teskel','Build/Assets/Models/ImportedHero_Generated/ImportedHeroBody.temf','Build/Assets/Models/ImportedHero_Generated/ImportedHeroMaterial.tmat','ProjectSettings/RenderSettings.json','Assets/Scenes/Scene.tescene')
	RmsParityRequire (@($q.files).Count -eq 9 -and @($Comparison.qualifiedActualPostImportRuntimeClosure.PSObject.Properties).Count -eq 9) 'Incomplete qualified nine-path closure'
	$backupPath=Join-Path ([IO.Path]::GetDirectoryName($project)) 'qualification-original-package-manifest.json';RmsParityRequire ([IO.Path]::GetFullPath($q.originalPackageManifest.path) -ieq $backupPath) 'Original manifest backup escaped its owned fixture'
	$seen=@{};$transitions=@()
	foreach($row in $q.files){
		$relative=[string](RmsParityField $row relative);RmsParityRequire ($required -ccontains $relative -and !$seen.ContainsKey($relative)) 'Unapproved or duplicate qualified path';$seen[$relative]=$true
		$target=RmsParityPath (Join-Path $project $relative);RmsParityRequire ($target.StartsWith($project.TrimEnd('/','\')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -and [IO.Path]::GetFullPath($row.qualified.path) -ieq $target) 'Qualified live path escaped or changed'
		RmsParityKeep $row.source;RmsParityKeep $row.qualified;RmsParitySamePin $row.source $reference[0].runtime.$relative 'Qualified input is not its actual reference source'
		RmsParityRequire ($row.source.sha256 -ceq $row.qualified.sha256 -and $row.source.bytes -eq $row.qualified.bytes) 'Qualified runtime differs from actual reference bytes'
		$compared=$Comparison.qualifiedActualPostImportRuntimeClosure.PSObject.Properties[$relative];RmsParityRequire ($null -ne $compared -and $compared.Value.sha256 -ceq $row.qualified.sha256 -and $compared.Value.bytes -eq $row.qualified.bytes) 'Compared/runtime qualified closure differs'
		$inputs=@($m.inputClosure|Where-Object {$_.projectPath -ceq $relative});$generated=$m.generatedHashes.PSObject.Properties[$relative];RmsParityRequire ($inputs.Count -le 1) 'Initial materialization path duplicated'
		$before=RmsParityField $row original;$history=$null;$origin='AbsentAtMaterialization'
		if($null -eq $before){RmsParityRequire ($inputs.Count -eq 0 -and $null -eq $generated) 'Qualification claims absent bytes that existed in materialization'}
		else{
			# This pin describes the instant before Copy-Item replaced the target.
			# Never rehash its old digest against the intentionally replaced path.
			RmsParityRequire ((RmsParityPath $before.path) -ieq $target -and $before.sha256 -cmatch '^[a-f0-9]{64}$') 'Historical before-path/hash changed';$null=RmsParityUint $before bytes
			if($inputs.Count -eq 1){$origin='MaterializationInputClosure';RmsParityRequire ($before.sha256 -ceq $inputs[0].sha256 -and $before.bytes -eq $inputs[0].sizeBytes) 'Historical before-bytes differ from initial input closure'}
			elseif($null -ne $generated){$origin='MaterializationGeneratedHash';RmsParityRequire ($before.sha256 -ceq $generated.Value) 'Historical before-hash differs from generated materialization'}
			else{RmsParityRequire $false 'Historical before-state has no immutable materialization provenance'}
			$history=[ordered]@{provenance='QualificationBeforeCopySnapshot';projectRelativePath=$relative;observedBeforeSha256=$before.sha256;observedBeforeBytes=$before.bytes;initialState=$origin;isLivePathPin=$false}
		}
		if($relative -ceq $manifestRelative){RmsParityRequire ($null -ne $before -and $q.originalPackageManifest.sha256 -ceq $before.sha256 -and $q.originalPackageManifest.bytes -eq $before.bytes) 'Preserved original manifest differs from before-bytes'}
		if($relative -ceq 'Assets/Scenes/Scene.tescene'){RmsParityRequire ($row.qualified.sha256 -ceq $m.generatedHashes.$relative) 'Qualification changed frozen authored scene'}
		$transitions+=,[ordered]@{relative=$relative;beforeState=$origin;historicalBefore=$history;referenceSource=$row.source;qualifiedRuntime=$row.qualified}
	}
	return [ordered]@{scale=$q.scale;projectRoot=$project;policy='HistoricalBeforeCopyAndStrictLiveQualifiedClosure';materialization=$q.originalMaterialization;preservedOriginalManifest=$q.originalPackageManifest;transitions=$transitions}
}
function Join-RuntimeMemorySafetyParity([string]$RunPath,[string]$CandidateRoot,[string]$ToolRoot,[string]$Comparator,[string]$Destination){
	$script:parityPins=@{};$output=RmsParityPath $Destination;RmsParityRequire (!(Test-Path $output)) 'Output must be fresh'
	$current=RmsParityPath $CandidateRoot;$frozen=RmsParityPath $ToolRoot;$comparatorPin=RmsParityPin $Comparator;RmsParityKeep $comparatorPin
	foreach($root in @($current,$frozen)){RmsParityRequire (!$output.StartsWith($root.TrimEnd('/','\')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) 'Output overlaps installed inputs'}
	$manifestPath=Join-Path $current 'Tests/Tools/RuntimeMemorySafety/runtime-memory-safety-artifacts.json';$manifest=RmsParityRead $manifestPath;$manifestPin=RmsParityPin $manifestPath
	RmsParityRequire ($manifest.schemaVersion -ceq 'runtime-memory-safety-v1') 'Wrong candidate install manifest'
	foreach($file in $manifest.files){$path=[IO.Path]::GetFullPath((Join-Path $current $file.path));RmsParityRequire ($path.StartsWith($current.TrimEnd('/','\')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) 'Manifest escapes candidate';RmsParityKeep ([pscustomobject]@{path=$path;sha256=$file.sha256.ToLowerInvariant();bytes=$file.length})}
	$fixturePath=Join-Path $frozen 'Tests/Tools/RuntimeMemorySafety/Fixtures/manifest.json';$fixture=RmsParityRead $fixturePath;$fixturePin=RmsParityPin $fixturePath
	RmsParityRequire ($fixture.imageNormalizedChannelError -eq 2.0/255 -and $fixture.allowExcludedImageRegions -eq $false -and @($fixture.scales).Count -eq 3) 'Frozen fixture image policy differs'
	$candidateFixture=RmsParityPin (Join-Path $current 'Tests/Tools/RuntimeMemorySafety/Fixtures/manifest.json');RmsParityRequire ($candidateFixture.sha256 -ceq $fixturePin.sha256) 'Candidate/frozen fixture changed'
	$frozenAdapter=RmsParityPin (Join-Path $frozen 'Tests/Tools/RuntimeMemorySafety/MaterializeRuntimeMemorySafetyFixture.ps1');RmsParityKeep $frozenAdapter
	$run=RmsParityRead $RunPath;RmsParityRequire ($run.schemaVersion -ceq 'runtime-memory-safety-parity-run-v1' -and $run.status -ceq 'CompletedComparisons') 'Parity run incomplete/unverified';RmsParityBool $run allRequestedScalesComparedEqual
	RmsParityBool $run nativeAcceptance $false;RmsParityBool $run performanceAcceptance $false;RmsParityBool $run privateReferenceCapturedCameraArraysObserved $false
	RmsParityEqual @($run.requestedScales|Sort-Object) @('normal','small','stress') 'All three actual scales required'
	RmsParityRequire ([IO.Path]::GetFullPath($run.frozenToolRoot) -ieq $frozen -and @($run.hosts).Count -eq 6 -and @($run.comparisons).Count -eq 3 -and @($run.commands).Count -ge 20) 'Run host/tool/command matrix incomplete'
	RmsParityWalk $run;$sourcePins=RmsParityRead $run.sourceInputs.path;foreach($pin in $sourcePins){RmsParityKeep $pin}
	$environment=RmsParityRead $run.environment.path;RmsParityRequire (![string]::IsNullOrWhiteSpace($environment.os) -and $environment.processArchitecture -ceq 'X64' -and $null -ne $environment.values) 'Actual host environment metadata missing'
	RmsParityRequire (@($sourcePins|Where-Object {$_.path -ieq $comparatorPin.path -and $_.sha256 -ceq $comparatorPin.sha256}).Count -eq 1) 'Comparator was not an actual pinned run input'
	foreach($command in $run.commands){RmsParityRequire ($command.exitCode -eq 0 -and $null -ne $command.log -and $command.arguments.Count -gt 0) 'Command failed/missing actual execution metadata';RmsParityCommandChronology $command}
	$requiredCommands=@('configure','build');foreach($scale in @('small','normal','stress')){foreach($variant in @('Baseline','Candidate')){foreach($stage in @('materialize','qualify','watchdog')){$requiredCommands+="$scale-$variant-$stage"}};$requiredCommands+="$scale-compare"}
	foreach($name in $requiredCommands){RmsParityRequire (@($run.commands|Where-Object {$_.name -ceq $name}).Count -eq 1) "Missing/repeated actual command $name"}
	$scales=@();$qualificationTransitions=@()
	foreach($scale in @('small','normal','stress')){
		$definition=@($fixture.scales|Where-Object {$_.id -ceq $scale});$lane=@($run.comparisons|Where-Object {$_.scale -ceq $scale});RmsParityRequire ($definition.Count -eq 1 -and $lane.Count -eq 1 -and $lane[0].status -ceq 'ComparedEqual') 'Missing/duplicate successful scale'
		$comparison=RmsParityRead $lane[0].receipt.path;RmsParityWalk $comparison
		RmsParityRequire ($comparison.schemaVersion -ceq 'runtime-memory-safety-parity-comparison-v1' -and $comparison.status -ceq 'ComparedEqual' -and $comparison.scale -ceq $scale -and $comparison.comparator.sha256 -ceq $comparatorPin.sha256 -and $comparison.fixtureManifest.sha256 -ceq $fixturePin.sha256) 'Scale/comparator/fixture comparison identity differs'
		RmsParityBool $comparison privateReferenceCapturedCameraArraysObserved $false;RmsParityBool $comparison nativeAcceptance $false;RmsParityBool $comparison performanceAcceptance $false
		RmsParityRequire ($comparison.pixelTolerance.integerChannelError -eq 2 -and $comparison.pixelTolerance.denominator -eq 255 -and @($comparison.pixelTolerance.excludedRegions).Count -eq 0 -and $comparison.cameraOutputJoin -ceq 'All authored camera command metadata in native serial order plus final completed shared Game View; common reference API exposes no private captured arrays or per-command camera UUID') 'Comparator limits were changed or omitted'
		$compareCommand=@($run.commands|Where-Object {$_.name -ceq "$scale-compare"})[0]
		foreach($actualArgument in @($comparatorPin.path,$comparison.baseline.path,$comparison.candidate.path,$fixturePin.path)){RmsParityRequire (@($compareCommand.arguments|Where-Object {$_ -ieq $actualArgument}).Count -eq 1) 'Comparator command did not consume the pinned actual inputs'}
		$hosts=@{}
		foreach($variant in @('Baseline','Candidate')){
			$hostLane=@($run.hosts|Where-Object {$_.scale -ceq $scale -and $_.variant -ceq $variant});RmsParityRequire ($hostLane.Count -eq 1 -and $hostLane[0].status -ceq 'Captured') 'Missing/duplicate captured ABI host'
			$nativeHost=RmsParityRead $hostLane[0].hostReceipt.path;RmsParityWalk $nativeHost;$hosts[$variant]=$nativeHost
			$comparisonPin=$comparison.($variant.ToLowerInvariant());RmsParityRequire ($comparisonPin.sha256 -ceq $hostLane[0].hostReceipt.sha256) 'Comparator did not consume actual host receipt'
			RmsParityRequire ($nativeHost.schemaVersion -ceq 'runtime-memory-safety-parity-prototype-v3' -and $nativeHost.status -ceq 'Captured' -and $nativeHost.variant -ceq $variant -and $nativeHost.fixtureScale -ceq $scale) 'Wrong host ABI/scale/status'
			RmsParityBool $nativeHost normalShutdownCompleted;RmsParityBool $nativeHost pausedPlayWithoutExplicitStep
			RmsParityRequire ($nativeHost.archiveReceipt.sha256 -ceq $run.referenceReceipt.sha256 -and $nativeHost.materialization.sha256 -ceq $hostLane[0].materialization.sha256) 'Host/archive/materialization join differs'
			$materialization=RmsParityRead $nativeHost.materialization.path;RmsParityRequire ($materialization.generatorSha256 -ceq $frozenAdapter.sha256 -and $materialization.fixtureManifestSha256 -ceq $fixturePin.sha256) 'Host used a different frozen generator or fixture'
			$watch=RmsParityRead $hostLane[0].watchdogReceipt.path;RmsParityRequire ($watch.exitCode -eq 0 -and $watch.timedOut -is [bool] -and !$watch.timedOut) 'Host watchdog failed'
			$qualification=RmsParityRead $nativeHost.qualification.path
			RmsParityRequire ($qualification.status -ceq 'Qualified' -and $qualification.policy -ceq 'FullActualPostImportReferenceRuntimeClosure' -and @($qualification.files).Count -eq 9 -and $qualification.referenceClosureReceipt.sha256 -ceq $comparison.qualificationSource.sha256 -and $nativeHost.qualification.sha256 -ceq $hostLane[0].qualification.sha256) 'Actual runtime qualification missing/different'
			RmsParityRequire ($qualification.originalMaterialization.sha256 -ceq $nativeHost.materialization.sha256) 'Qualification rewrote original materialization identity'
			$transition=RmsParityQualification $qualification $materialization $comparison;$transition['variant']=$variant;$qualificationTransitions+=,$transition
			if($variant -ceq 'Candidate'){RmsParityRequire ([IO.Path]::GetFullPath($nativeHost.expectedEngine.path) -ieq (Join-Path $current 'Editor/TerraEngine.dll') -and $nativeHost.loadedEngine.sha256 -ceq $nativeHost.expectedEngine.sha256) 'Parity used a different candidate runtime'}
		}
		$count=RmsParityScratch $comparison $hosts.Candidate $definition[0];RmsParityRequire ($count -eq $lane[0].actualCandidateFallbackAllocationCount) 'Aggregate fallback count changed'
		RmsParityNormalCoverage $comparison $hosts $definition[0];RmsParityFinalOutput $comparison $hosts
		$archive=RmsParityRead $run.referenceReceipt.path;RmsParitySourceProof $comparison $hosts $archive.sourceRevision $run.referenceReceipt
		RmsParityLightingCohort $comparison $hosts $definition[0]
		$scales+=,[ordered]@{scale=$scale;cameraLightIdentityExact=$true;cameraLightOrderExact=$true;publishedOutputsEquivalent=$true;fallbackCount=$count;rawEvidence=@($lane[0].receipt,$comparison.baseline,$comparison.candidate,$run.sourceInputs,$run.environment,$comparison.comparator,$comparison.qualificationSource);limits=$comparison.pixelTolerance;cameraOutputJoin=$comparison.cameraOutputJoin;normalOutputCoverage=$comparison.normalOutputCoverage;finalNormalOutputImage=$comparison.finalNormalOutputImage;rendererSourceProof=$comparison.rendererSourceProof;privateReferenceCapturedCameraArraysObserved=$false}
	}
	# Recheck every consumed pin at the end so a mutable closure cannot qualify
	# merely because it happened to match at the beginning of this join.
	foreach($pin in @($script:parityPins.Values)){$actual=RmsParityPin $pin.path;RmsParityRequire ($actual.sha256 -ceq $pin.sha256 -and $actual.bytes -eq $pin.bytes) 'Evidence changed during join'}
	# The installed runner verifies each scale's raw pins directly, so include
	# transitive outputs/commands/qualification/source inputs, not just JSON parents.
	$allPins=@($script:parityPins.Values|Sort-Object path);foreach($scaleRow in $scales){$scaleRow.rawEvidence=$allPins}
	$result=[ordered]@{schemaVersion='runtime-memory-safety-parity-v1';status='Passed';runtimeManifestSha256=$manifestPin.sha256;fixtureSha256=$fixturePin.sha256;createdUtc=[DateTime]::UtcNow.ToString('o');scales=$scales;qualificationTransitions=$qualificationTransitions;rawEvidence=@($script:parityPins.Values|Sort-Object path);sourceRun=(RmsParityPin $RunPath);joiner=(RmsParityPin $script:RmsParityJoinerPath);nativeAcceptance=$false;performanceAcceptance=$false;scope='Three-scale parity only; matching-ABI common reference output plus actual candidate typed arrays; no private baseline camera-array claim'}
	$null=[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($output));$stream=[IO.FileStream]::new($output,[IO.FileMode]::CreateNew);try{$bytes=[Text.UTF8Encoding]::new($false).GetBytes(($result|ConvertTo-Json -Depth 100));$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)}finally{$stream.Dispose()};return $result
}
if($MyInvocation.InvocationName -ne '.'){
	Set-StrictMode -Version Latest;$ErrorActionPreference='Stop'
	try{$null=Join-RuntimeMemorySafetyParity $ParityRunPath $InstallRoot $FrozenToolRoot $ComparatorPath $OutputPath;Write-Output 'Actual three-scale parity evidence joined; no timing acceptance';exit 0}catch{Write-Error $_ -ErrorAction Continue;exit 2}
}
