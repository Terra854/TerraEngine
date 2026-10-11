<#
.SYNOPSIS
Runs renderer efficiency validation from a sealed installed bundle.
.DESCRIPTION
Prerequisites: configure/build/install with BUILD_TESTING, an available Vulkan
adapter and a fresh disposable WorkRoot and OutputDirectory. Example:
pwsh -File ./Build/Tests/Tools/RendererEfficiency/Invoke-RendererEfficiencyValidation.ps1 -InstallRoot ./Build -WorkRoot ./out/renderer-work -OutputDirectory ./out/renderer-evidence -Stage P1 -Mode Compare
DryRun checks installed inputs and reports planned commands without launching any
process or writing a project. FoundationSmoke tests ownership/options/lifecycle
only; absent GPU evidence remains Unverified (exit 2), never acceptance.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$InstallRoot,
	[Parameter(Mandatory=$true)][string]$WorkRoot,
	[Parameter(Mandatory=$true)][string]$OutputDirectory,
	[ValidateSet('P1','P2','P3','P4','P5','All')][string]$Stage='All',
	[ValidateSet('Reference','Candidate','Compare')][string]$Mode='Compare',
	[ValidateSet('Serial','Concurrent')][string]$ValidationPreparation='Concurrent',
	[ValidateRange(3,100)][int]$Runs=3,
	[ValidateRange(60,100000)][int]$WarmupFrames=60,
	[ValidateRange(300,100000)][int]$MeasuredFrames=300,
	[ValidateRange(1024,65535)][int]$McpPort=43161,
	[ValidateRange(10,600)][int]$StartupTimeoutSeconds=180,
	# Full native color/depth export covers every frozen step and sample. Keep a
	# bounded six-hour operation deadline so collection need not truncate windows.
	[ValidateRange(30,21600)][int]$OperationTimeoutSeconds=21600,
	[string]$BaselineInstallRoot='',
	# Explicit closed collection receipts may replace exact planned launches only
	# after their native provenance and complete artifact closure are verified.
	[string[]]$RecoveredSessions=@(),
	[switch]$FoundationSmoke,
	[switch]$DryRun
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
# A caller may inherit another PowerShell edition's module search path. Resolve
# hashing/JSON helpers from this executable's own built-in modules instead.
try {
	Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -Force -ErrorAction Stop
} catch {
	Write-Output '{"schemaVersion":1,"status":"Invalid","reason":"Built-in PowerShell Utility module could not load","exitCode":3}'
	exit 3
}
$utf8=[Text.UTF8Encoding]::new($false)
$summary=[ordered]@{schemaVersion=1;status='Unverified';stage=$Stage;mode=$Mode;dryRun=[bool]$DryRun;foundationSmoke=[bool]$FoundationSmoke;installRoot='';installReceiptHash='';manifestHash='';sessions=@();plannedCommands=@();reason='';exitCode=2}
$script:rpcId=0;$script:editor=$null;$script:stdoutTask=$null;$script:stderrTask=$null;$script:priorToken=$null;$script:operationId='';$script:sessionOutput='';$script:watchdogFailed=$false;$script:ownsOutput=$false;$script:ownedProjectRoot=''
function Has($Value,[string]$Name) { return $null -ne $Value -and $null -ne $Value.PSObject.Properties[$Name] }
function JsonWrite([string]$Path,$Value) { [IO.File]::WriteAllText($Path,($Value|ConvertTo-Json -Depth 100),$utf8) }
function JsonFragment($Value) {
	# Windows PowerShell emits no characters for a null InputObject; JSON needs null.
	if($null -eq $Value){return 'null'}
	return ConvertTo-Json -InputObject $Value -Compress -Depth 100
}
function WriteEvidenceJson([string]$Path,$Value) {
	# Retain every root property, including fields not yet recognized by the schema.
	# Native frames/artifacts are the large arrays. Convert only one array item or
	# other root value at a time; InputObject preserves nested/singleton arrays.
	# Peak scratch space is bounded by that item/value, not the complete document.
	if($Value -isnot [pscustomobject]){throw 'Evidence root must be an object'}
	$full=[IO.Path]::GetFullPath($Path);NoRedirect $full
	$parent=[IO.Path]::GetDirectoryName($full)
	$temporary=[IO.Path]::Combine($parent,('.evidence-write-'+[Guid]::NewGuid().ToString('N')+'.tmp'))
	$writer=$null;$stream=$null;$temporaryOwned=$false;$serializationComplete=$false
	try {
		$stream=[IO.FileStream]::new($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
		$temporaryOwned=$true
		$writer=[IO.StreamWriter]::new($stream,$utf8,65536)
		$writer.Write('{');$first=$true
		foreach($property in $Value.PSObject.Properties) {
			if(!$first){$writer.Write(',')};$first=$false
			$writer.Write((ConvertTo-Json -InputObject ([string]$property.Name) -Compress -Depth 100));$writer.Write(':')
			if($property.Value -is [array]) {
				$writer.Write('[');$firstItem=$true
				foreach($item in $property.Value){if(!$firstItem){$writer.Write(',')};$firstItem=$false;$writer.Write((JsonFragment $item))}
				$writer.Write(']')
			} else {$writer.Write((JsonFragment $property.Value))}
		}
		$writer.Write('}');$writer.Flush();$writer.Dispose();$writer=$null;$stream=$null
		$serializationComplete=$true
		# Same-directory staging publishes atomically and preserves the old archive
		# on failure. A reader without delete sharing can temporarily prevent the
		# replacement. Retry only that publication of this call's complete bytes;
		# never serialize again, reuse another temporary or relax path checks.
		$publicationClock=[Diagnostics.Stopwatch]::StartNew()
		while($true) {
			NoRedirect $full;NoRedirect $temporary
			try {
				# NullString passes a real null backup path in both PS editions.
				if([IO.File]::Exists($full)){[IO.File]::Replace($temporary,$full,[System.Management.Automation.Language.NullString]::Value)}else{[IO.File]::Move($temporary,$full)}
				break
			} catch {
				$failure=$_.Exception;while($failure.InnerException){$failure=$failure.InnerException}
				# Windows sharing/lock violations and ReplaceFile's inability to
				# remove the replaced file are bounded retries. Permission, path,
				# disk and all other failures remain immediate errors.
				$transient=$failure -is [IO.IOException] -and ($failure.HResult -band 65535) -in @(32,33,1175)
				if(!$transient -or $publicationClock.ElapsedMilliseconds -ge 750){throw}
				$remaining=750-$publicationClock.ElapsedMilliseconds
				if($remaining -le 0){throw}
				Start-Sleep -Milliseconds ([int][Math]::Min(25,$remaining))
			}
		}
	} finally {
		if($null -ne $writer){$writer.Dispose()}elseif($null -ne $stream){$stream.Dispose()}
		# Incomplete serialization is this call's disposable scratch. A completed
		# publication that failed keeps its unique temporary and prior destination
		# for forensic verification; it must never be silently discarded or reused.
		if($temporaryOwned -and !$serializationComplete -and [IO.File]::Exists($temporary)){[IO.File]::Delete($temporary)}
	}
}

function Below([string]$Root,[string]$Path) {
	$prefix=[IO.Path]::GetFullPath($Root).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
	if(![IO.Path]::GetFullPath($Path).StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) { throw "Path escapes required root: $Path" }
}
function NoRedirect([string]$Path) {
	# Inspect every ancestor directly: even a missing leaf beneath a junction escapes.
	# Avoid provider enumeration; each call reads fresh attributes with no cache.
	$current=[IO.Path]::GetFullPath($Path)
	while($current) {
		$attributes=$null
		try { $attributes=[IO.File]::GetAttributes($current) }
		catch {
			# PowerShell wraps .NET exceptions. Only absent files/directories are
			# permitted here; access, device, and other IO failures stay fail-closed.
			$exception=$_.Exception
			while($exception.InnerException) { $exception=$exception.InnerException }
			if($exception -isnot [IO.FileNotFoundException] -and $exception -isnot [IO.DirectoryNotFoundException]) { throw }
		}
		if($null -ne $attributes -and ($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
			throw "Redirected path is not allowed: $current"
		}
		# Trim separators only for parent lookup, after checking the drive/root itself.
		$next=[IO.Path]::GetDirectoryName($current.TrimEnd([IO.Path]::DirectorySeparatorChar,[IO.Path]::AltDirectorySeparatorChar))
		if($next -eq $current) { break }
		$current=$next
	}
}
function Fresh([string]$Path) { NoRedirect $Path;if(Test-Path -LiteralPath $Path) { throw "Directory must be fresh: $Path" } }
function Inventory([string]$Root,[bool]$AllowLegacy=$false) {
	NoRedirect $Root
	$manifestPath=Join-Path $Root 'Tests/Tools/RendererEfficiency/renderer-efficiency-artifacts.json'
	if(!(Test-Path -LiteralPath $manifestPath) -and $AllowLegacy){$manifestPath=Join-Path $Root 'correctness-artifacts.json'}
	$receipt=Get-Content -LiteralPath $manifestPath -Raw|ConvertFrom-Json
	if($receipt.schemaVersion -ne 1) { throw 'Invalid installed artifact receipt' }
	$featureSeal=Has $receipt 'artifacts'
	if($featureSeal -and (!(Has $receipt 'revision') -or !(Has $receipt 'buildConfiguration') -or !(Has $receipt 'sourceDirty'))){throw 'Incomplete renderer efficiency install seal'}
	$entries=if($featureSeal){$receipt.artifacts}else{$receipt.files}
	if(!$entries){throw 'Empty installed inventory'}
	$map=@{}
	foreach($file in $entries) {
		$relative=if($featureSeal){$file.path}else{$file.relativePath};$length=if($featureSeal){$file.bytes}else{$file.length}
		if([IO.Path]::IsPathRooted($relative) -or $map.ContainsKey($relative) -or $file.sha256 -notmatch '^[a-fA-F0-9]{64}$') { throw 'Invalid installed inventory path/hash' }
		$path=[IO.Path]::GetFullPath((Join-Path $Root $relative));Below $Root $path
		$map[$relative]=@{path=$path;hash=$file.sha256;length=$length}
		# A seal is a comparison identity only after every declared file verifies.
		NoRedirect $path
		if(!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -ne $length -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $file.sha256){throw "Stale installed seal entry: $relative"}
	}
	$identity=@{revision=$receipt.revision;installedClosureHash=(Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()}
	# Order by unsigned UTF8 bytes, exactly as the native running-module verifier.
	$ordered=[Collections.Generic.SortedDictionary[string,string]]::new([StringComparer]::Ordinal)
	foreach($name in $map.Keys){$ordered.Add(([BitConverter]::ToString($utf8.GetBytes($name)).Replace('-','')),$name)}
	$binary=[Text.StringBuilder]::new();$shader=[Text.StringBuilder]::new();$asset=[Text.StringBuilder]::new()
	foreach($name in $ordered.Values){
		$item=$map[$name];$line=$name+"`t"+([uint64]$item.length).ToString([Globalization.CultureInfo]::InvariantCulture)+"`t"+$item.hash.ToLowerInvariant()+"`n"
		if($name -ceq 'Editor/TerraEngine-Editor.exe' -or $name -ceq 'Editor/TerraEngine.dll'){$null=$binary.Append($line)}
		if(('/'+$name) -match '/(Shaders|InstalledShaders)/' -and [IO.Path]::GetExtension($name) -in @('.spv','.tspf','.hlsl','.hlsli','.vert','.frag','.comp','.task','.mesh','.geom','.tesc','.tese','.glsl')){$null=$shader.Append($line)}
		if($name -like 'Tests/Fixtures/RendererEfficiency/*' -or ('/'+$name) -match '/Assets/'){$null=$asset.Append($line)}
	}
	foreach($pair in @(@('installedBinaryHash',$binary),@('installedShaderClosureHash',$shader),@('installedAssetClosureHash',$asset))){
		if(!$pair[1].Length){throw 'Installed comparison closure is empty'}
		$sha=[Security.Cryptography.SHA256]::Create();try{$identity[$pair[0]]=[BitConverter]::ToString($sha.ComputeHash($utf8.GetBytes($pair[1].ToString()))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
	}
	return @{root=$Root;files=$map;receiptHash=$identity.installedClosureHash;identity=$identity}
}
function Installed($Inventory,[string]$Relative) {
	if(!$Inventory.files.ContainsKey($Relative)) { throw "Required input absent from installed receipt: $Relative" }
	$item=$Inventory.files[$Relative];NoRedirect $item.path
	if(!(Test-Path -LiteralPath $item.path -PathType Leaf) -or (Get-Item -LiteralPath $item.path).Length -ne $item.length -or (Get-FileHash -LiteralPath $item.path -Algorithm SHA256).Hash -ine $item.hash) { throw "Missing or stale installed input: $Relative" }
	return $item.path
}
# Inspect only the process launched by this runner. Exit data is unavailable
# while it is running; neither RPC errors nor log text imply an exit code.
function OwnedProcessObservation {
	$observation=[ordered]@{processId=$null;hasExited=$null;exitCode=$null;reason='No owned editor process is available'}
	if($null -eq $script:editor){return $observation}
	try {
		$script:editor.Refresh();$observation.processId=$script:editor.Id;$observation.hasExited=$script:editor.HasExited
		if($observation.hasExited){$observation.exitCode=$script:editor.ExitCode;$observation.reason=''}else{$observation.reason='Owned editor is still running'}
	}catch{$observation.reason='Owned process exit observation unavailable: '+$_.Exception.Message}
	return $observation
}
# Request/response content is frozen before opening its append handle. A short
# monitor lock may delay this write, but never reissues the completed RPC. Retry
# only handle acquisition: retrying an interrupted write could duplicate a row.
function WriteRequestLog($Entry) {
	$path=Join-Path $script:sessionOutput 'requests.jsonl'
	$bytes=$utf8.GetBytes(($Entry|ConvertTo-Json -Depth 100 -Compress)+[Environment]::NewLine)
	$clock=[Diagnostics.Stopwatch]::StartNew();$stream=$null
	while($null -eq $stream) {
		try{$stream=[IO.FileStream]::new($path,[IO.FileMode]::Append,[IO.FileAccess]::Write,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))}
		catch {
			$errorCause=$_.Exception;while($errorCause.InnerException){$errorCause=$errorCause.InnerException}
			$nativeCode=$errorCause.HResult -band 0xffff
			if($errorCause -isnot [IO.IOException] -or $nativeCode -notin @(32,33) -or $clock.ElapsedMilliseconds -ge 750){throw}
			Start-Sleep -Milliseconds ([Math]::Max(1,[Math]::Min(25,750-$clock.ElapsedMilliseconds)))
		}
	}
	try{$stream.Write($bytes,0,$bytes.Length);$stream.Flush()}finally{$stream.Dispose()}
}
function Rpc([string]$Method,$Params=@{},[int]$TimeoutSeconds=30) {
	$script:rpcId++;$request=@{jsonrpc='2.0';id=$script:rpcId;method=$Method;params=$Params}
	try {
		$response=(Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$McpPort/mcp" -Method Post -ContentType 'application/json' -Body ($request|ConvertTo-Json -Depth 100 -Compress) -TimeoutSec $TimeoutSeconds).Content|ConvertFrom-Json
	} catch {
		$failure=$_.Exception.Message;$owned=OwnedProcessObservation
		if($owned.hasExited -eq $true -and $null -ne $owned.exitCode){$failure+='; owned editor exited with code '+$owned.exitCode}
		WriteRequestLog @{utc=[DateTime]::UtcNow.ToString('o');request=$request;error=$failure;ownedProcess=$owned}
		throw [InvalidOperationException]::new($failure)
	}
	# Log I/O failures must propagate directly, rather than turning a completed
	# RPC into another attempted error row or retrying its already obtained result.
	WriteRequestLog @{utc=[DateTime]::UtcNow.ToString('o');request=$request;response=$response}
	if(Has $response 'error') { throw "MCP protocol error for $Method" };return $response
}
function Tool([string]$Name,$Arguments=@{},[int]$TimeoutSeconds=30) {
	$response=Rpc 'tools/call' @{name=$Name;arguments=$Arguments} $TimeoutSeconds
	$text=@($response.result.content|Where-Object {$_.type -eq 'text'})
	if($text.Count -ne 1) { throw "Missing unique MCP envelope for $Name" }
	$envelope=$text[0].text|ConvertFrom-Json
	if(!$envelope.ok) { throw "MCP $Name failed: $($envelope|ConvertTo-Json -Depth 20 -Compress)" }
	return $envelope.data
}
function WaitGeneration([uint64]$Generation) {
	$clock=[Diagnostics.Stopwatch]::StartNew()
	do { $state=Tool 'renderer.efficiency.stats';if((Has $state 'settingsGeneration') -and [uint64]$state.settingsGeneration -ge $Generation) { return $state };Start-Sleep -Milliseconds 100 } while($clock.Elapsed.TotalSeconds -lt $StartupTimeoutSeconds)
	throw 'Requested options did not reach the protected renderer capture'
}
function Options([string]$SelectedStage,[string]$SelectedMode) {
	# Start from the same complete value on each side; references differ only in
	# their stage switch. P4/P5 force equal refresh work instead of measuring reuse.
	# A stage uses only optimizations already delivered at its prerequisite gates.
	# Later controls must not alter an earlier correctness or performance baseline.
	$value=[ordered]@{shadowReuse='Enabled';lightingUploads='Full';lightingDepth='CompleteScan';shadowSubmission='Singleton';staticShadowCacheBudgetBytes=0;validationPreparation=$ValidationPreparation}
	if($SelectedStage -in @('P2','P3','P4','P5')){$value.lightingUploads='Dirty'}
	if($SelectedStage -in @('P3','P4','P5')){$value.lightingDepth='Cooperative'}
	if($SelectedStage -in @('P4','P5')){$value.shadowSubmission='Grouped'}
	if($SelectedStage -eq 'P5'){$value.staticShadowCacheBudgetBytes=67108864}
	if($SelectedStage -in @('P4','P5')) {$value.shadowReuse='AlwaysRefresh'}
	if($SelectedMode -eq 'Reference') {
		switch($SelectedStage) { P1 {$value.shadowReuse='AlwaysRefresh'} P2 {$value.lightingUploads='Full'} P3 {$value.lightingDepth='CompleteScan'} P4 {$value.shadowSubmission='Singleton'} P5 {$value.staticShadowCacheBudgetBytes=0} }
	}
	return $value
}
function SaveEvidence($Value,[string]$Project,[string]$Directory) {
	# Bulk payloads originate in the generated project. Copy their exact verified
	# bytes into the run archive before the comparer resolves relative references.
	# Keep native descriptor identity unchanged by placing the document beside its
	# copied project-relative closure, rather than rewriting descriptor paths.
	$archiveBase=Join-Path $Directory 'artifacts';[IO.Directory]::CreateDirectory($archiveBase)|Out-Null
	$copied=@{}
	foreach($artifact in @($Value.artifacts)+@($Value.frames|ForEach-Object {$_.artifacts})) {
		if($artifact.status -ne 'Valid'){continue}
		if([IO.Path]::IsPathRooted($artifact.path)){throw 'Evidence artifact must be project-relative'}
		$source=[IO.Path]::GetFullPath((Join-Path $Project $artifact.path));Below $Project $source;NoRedirect $source
		if($copied.ContainsKey($artifact.path)){
			if($copied[$artifact.path].hash -ine $artifact.sha256){throw 'Conflicting content-addressed artifact references'}
			continue
		}
		if(!(Test-Path -LiteralPath $source -PathType Leaf) -or (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ine $artifact.sha256){throw 'Evidence artifact hash mismatch'}
		$destination=[IO.Path]::GetFullPath((Join-Path $archiveBase $artifact.path));Below $archiveBase $destination;NoRedirect $destination
		[IO.Directory]::CreateDirectory((Split-Path $destination))|Out-Null;[IO.File]::Copy($source,$destination,$false)
		$copied[$artifact.path]=@{hash=$artifact.sha256}
	}
	$path=Join-Path $archiveBase 'evidence.json';WriteEvidenceJson $path $Value;return $path
}
function LoadOwnedEvidence($State,[string]$Project,$Inventory,[string]$RunName) {
	# Large documents are atomically published by the host's worker. The status
	# contains only its operation-owned relative path, never arbitrary file access.
	if(!(Has $State 'evidenceArtifactPath') -or $State.evidenceArtifactPath -cne ($script:operationId+'-evidence.json')){throw 'Missing operation-owned evidence document'}
	$path=[IO.Path]::GetFullPath((Join-Path $Project $State.evidenceArtifactPath));Below $Project $path;NoRedirect $path
	$markerPath=Join-Path $Project 'renderer-efficiency-project.json';NoRedirect $markerPath
	$before=(Get-FileHash -LiteralPath $markerPath -Algorithm SHA256).Hash
	$document=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
	$marker=Get-Content -LiteralPath $markerPath -Raw|ConvertFrom-Json
	if($document.schemaVersion -ne 1 -or $document.identity.runId -cne $RunName -or $document.identity.fixtureHash.value -ine $marker.fixtureHash -or $State.fixtureHash -ine $marker.fixtureHash){throw 'Evidence document identity changed'}
	foreach($field in @('installedBinaryHash','installedShaderClosureHash','installedAssetClosureHash')){
		if($document.identity.$field.status -ne 'Valid' -or $document.identity.$field.value -cne $Inventory.identity[$field]){throw "Running editor closure differs from verified install: $field"}
	}
	foreach($field in @('referenceRevision','candidateRevision')){if($document.identity.$field.status -ne 'Valid' -or $document.identity.$field.value -cne $Inventory.identity.revision){throw 'Evidence revision differs from installed seal'}}
	if((Get-FileHash -LiteralPath $markerPath -Algorithm SHA256).Hash -cne $before){throw 'Evidence project ownership changed during read'}
	return $document
}
# Read the same native import predicate used by ordinary window-close approval.
# It proves that modal condition is clear, not that every canceled job drained.
function PrepareOwnedNormalClose {
	if(!$script:ownedProjectRoot){throw 'Owned close readiness has no launched project identity'}
	$root=[IO.Path]::GetFullPath($script:ownedProjectRoot);NoRedirect $root
	$clock=[Diagnostics.Stopwatch]::StartNew();$firstClear=$null;$limit=[Math]::Min(60,$StartupTimeoutSeconds)
	while($clock.Elapsed.TotalSeconds -lt $limit) {
		# Bound blocking HTTP calls by the remaining preparation deadline, too.
		$remaining=[int][Math]::Min(30,[Math]::Floor($limit-$clock.Elapsed.TotalSeconds))
		if($remaining -lt 1){break}
		$state=Tool 'project.state' @{} $remaining
		if(!(Has $state hasProject) -or $state.hasProject -ne $true -or !(Has $state projectRoot) -or [IO.Path]::GetFullPath($state.projectRoot) -ine $root){throw 'Owned close project identity changed'}
		if(!(Has $state sceneDirty) -or $state.sceneDirty -isnot [bool] -or !(Has $state assetImportsActive) -or $state.assetImportsActive -isnot [bool]){throw 'Owned close readiness capability unavailable'}
		if($state.sceneDirty) {
			if(!(Has $state activeScenePath) -or $state.activeScenePath -isnot [string] -or !$state.activeScenePath){throw 'Owned dirty scene save target unavailable'}
			$scene=$state.activeScenePath
			# The native loaded scene source is absolute and is exactly SaveScene's
			# default target. Do not infer a save destination from a name or layout.
			if(![IO.Path]::IsPathRooted($scene)){throw 'Native owned scene save target is not absolute'}
			$target=[IO.Path]::GetFullPath($scene)
			Below $root $target;NoRedirect $target
			if([IO.Path]::GetExtension($target) -ine '.tescene' -or !(Test-Path -LiteralPath $target -PathType Leaf)){throw 'Owned scene save target is not an existing contained scene'}
			$remaining=[int][Math]::Min(30,[Math]::Floor($limit-$clock.Elapsed.TotalSeconds));if($remaining -lt 1){break}
			$saved=Tool 'scene.save' @{} $remaining
			if(!(Has $saved dirty) -or $saved.dirty -isnot [bool]){throw 'Native scene save completion proof unavailable'}
			$script:lifecycleData.savedScene=$true;$script:lifecycleData.savedScenePath=$target;$firstClear=$null
		}elseif(!$state.assetImportsActive) {
			if($null -eq $firstClear){$firstClear=$clock.ElapsedMilliseconds}
			elseif($clock.ElapsedMilliseconds-$firstClear -ge 100) {
				$script:lifecycleData.assetImportCloseReady=$true
				$script:lifecycleData.closeReadinessProof=[ordered]@{projectRoot=$state.projectRoot;assetImportsActive=$state.assetImportsActive;sceneDirty=$state.sceneDirty;observations=2;minimumSeparationMilliseconds=100;actualSeparationMilliseconds=($clock.ElapsedMilliseconds-$firstClear)}
				return
			}
		}else{$firstClear=$null}
		Start-Sleep -Milliseconds 100
	}
	throw 'Owned project did not reach bounded clean-scene/import-close readiness'
}
function StopOwned {
	if($null -eq $script:editor) { return }
	$script:lifecycleData.ownedEditorProcessId=$script:editor.Id
	$script:editor.Refresh()
	if(!$script:editor.HasExited) {
		$closePrepared=$false
		try {
			if($script:operationId) { $null=Tool 'renderer.efficiency.validation.stop' @{operationId=$script:operationId};$script:operationId='' }
			if($script:priorToken) {
				# A real device recreation retires the earlier options token. Queue
				# its exact saved values as a new request in this owned editor, then
				# prove those values reached the protected capture before closing it.
				$original=$script:priorToken.options
				$reset=Tool 'renderer.efficiency.validation.set' $original
				$restored=WaitGeneration ([uint64]$reset.requestedGeneration)
				foreach($field in $original.PSObject.Properties.Name){if($restored.effectiveOptions.$field -ne $original.$field){throw 'Prior renderer options did not restore'}}
				$script:lifecycleData.originalOptions=$original;$script:lifecycleData.restoredOptions=$restored.effectiveOptions
				$script:lifecycleData.restoredSettingsGeneration=$restored.settingsGeneration
				$script:lifecycleData.restoredRendererSessionGeneration=$restored.rendererSessionGeneration
				$script:lifecycleData.optionsRestored=$true;$script:priorToken=$null
			}
			PrepareOwnedNormalClose;$closePrepared=$true
		} catch { $script:watchdogFailed=$true;$script:lifecycleData.reason=$_.Exception.Message }
		# Ordinary window close preserves renderer retirement and project teardown.
		if($closePrepared) {
			$script:lifecycleData.closeAccepted=$script:editor.CloseMainWindow()
			if($script:lifecycleData.closeAccepted){$script:lifecycleData.exitWaitSucceeded=$script:editor.WaitForExit(10000)}
			$script:lifecycleData.normalClose=$script:lifecycleData.closeAccepted -and $script:lifecycleData.exitWaitSucceeded
		}
		if(!$script:lifecycleData.normalClose) { $script:watchdogFailed=$true;$script:editor.Refresh();if(!$script:editor.HasExited){$script:editor.Kill();$script:editor.WaitForExit(10000)|Out-Null} }
	}
	if($script:editor.HasExited){$script:lifecycleData.processExitCode=$script:editor.ExitCode;if($script:editor.ExitCode -ne 0){$script:watchdogFailed=$true}}
	$script:lifecycleData.watchdogFailed=$script:watchdogFailed
	if($script:stdoutTask) {[IO.File]::WriteAllText((Join-Path $script:sessionOutput 'editor.stdout.log'),$script:stdoutTask.GetAwaiter().GetResult(),$utf8)}
	if($script:stderrTask) {[IO.File]::WriteAllText((Join-Path $script:sessionOutput 'editor.stderr.log'),$script:stderrTask.GetAwaiter().GetResult(),$utf8)}
	$script:editor.Dispose();$script:editor=$null;$script:stdoutTask=$null;$script:stderrTask=$null
}
function Launch([string]$Editor,[string]$Project,[string]$SessionOutput,[bool]$RequireEfficiency=$true) {
	$script:sessionOutput=$SessionOutput;$script:priorToken=$null;$script:operationId='';$script:ownedProjectRoot=[IO.Path]::GetFullPath($Project)
	$script:lifecycleData=[ordered]@{schemaVersion=1;kind='RendererEfficiencyRunnerLifecycle';ownedEditorProcessId=0;optionsRestored=$false;normalClose=$false;closeAccepted=$false;exitWaitSucceeded=$false;savedScene=$false;savedScenePath=$null;assetImportCloseReady=$false;closeReadinessProof=$null;processExitCode=$null;watchdogFailed=$false;reason=''}
	$preferences=Join-Path $SessionOutput 'editor-preferences.json'
	JsonWrite $preferences @{recentProjects=@();autoBuildScripts=$false;editorMcpEnabled=$true;editorMcpWriteModeDefault='writes_enabled';editorMcpListenPort=$McpPort;vulkanValidationLayers=$false;renderDebugDiagnostics=$false;rendererDiagnosticsPreferencesInitialized=$true}
	$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,$McpPort);try{$listener.Start()}finally{$listener.Stop()}
	$start=[Diagnostics.ProcessStartInfo]::new();$start.FileName=$Editor;$start.Arguments="--project `"$Project`"";$start.WorkingDirectory=Split-Path $Editor;$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
	$start.EnvironmentVariables['TERRAENGINE_EDITOR_PREFS_SESSION_PATH']=$preferences;$start.EnvironmentVariables['TERRAENGINE_SUPPRESS_PROJECT_RECENTS']='1'
	$script:editor=[Diagnostics.Process]::Start($start);if(!$script:editor){throw 'Owned editor launch failed'}
	$script:stdoutTask=$script:editor.StandardOutput.ReadToEndAsync();$script:stderrTask=$script:editor.StandardError.ReadToEndAsync()
	$clock=[Diagnostics.Stopwatch]::StartNew();$available=@();$ready=$false;$missingRegistered=@()
	$efficiencyTools=@('renderer.efficiency.stats','renderer.efficiency.validation.set','renderer.efficiency.validation.reset','renderer.efficiency.validation.start','renderer.efficiency.validation.status','renderer.efficiency.validation.stop')
	while($clock.Elapsed.TotalSeconds -lt $StartupTimeoutSeconds) {
		if($script:editor.HasExited){throw ('Editor exited before MCP readiness with code '+$script:editor.ExitCode)}
		try {
			$hello=Rpc 'initialize' @{protocolVersion='2025-03-26';clientInfo=@{name='renderer-efficiency';version='1'}}
			if($hello.result.serverInfo.name -eq 'TerraEngine Editor') {
				$descriptors=@((Rpc 'tools/list').result.tools);$available=@($descriptors|Where-Object {$_.availability.available}|ForEach-Object {$_.name})
				$registered=@($descriptors|ForEach-Object {$_.name})
				$missingRegistered=@($efficiencyTools|Where-Object {$_ -notin $registered})
				# Registered tools temporarily unavailable during asynchronous project
				# opening must be polled again; they are not an unsupported interface.
				if($RequireEfficiency -and $missingRegistered.Count){break}
				$pendingEfficiency=@($efficiencyTools|Where-Object {$_ -notin $available})
				if('project.state' -in $available -and 'camera.capture_custom' -in $available -and (!$RequireEfficiency -or !$pendingEfficiency.Count)) {
					$projectState=Tool 'project.state'
					if($projectState.hasProject -and (Has $projectState 'activeScene') -and $projectState.activeScene -and [IO.Path]::GetFullPath($projectState.projectRoot) -ieq [IO.Path]::GetFullPath($Project)) {$ready=$true;break}
				}
			}
		} catch { };Start-Sleep -Milliseconds 200
	}
	if($RequireEfficiency -and $missingRegistered.Count){throw [InvalidOperationException]::new('UNVERIFIED: Installed editor does not register efficiency tools: '+($missingRegistered -join ', '))}
	if(!$ready){throw 'Installed editor/MCP did not become available'}
	# Port availability before launch is insufficient: identify the actual owner.
	$listeners=@(Get-NetTCPConnection -LocalPort $McpPort -State Listen -ErrorAction Stop)
	if(!$listeners.Count -or @($listeners|Where-Object {$_.OwningProcess -ne $script:editor.Id}).Count){throw 'MCP endpoint is not owned by the launched editor'}
	$actual=Tool 'project.state';if([IO.Path]::GetFullPath($actual.projectRoot) -ine [IO.Path]::GetFullPath($Project)){throw 'Editor opened a different project'}
	return ,$available
}
function CanonicalWorldPosition($Vector) {
	# The strict custom-camera API accepts signed cell strings plus bounded metre
	# offsets. Normalize fixture vectors using the engine's 65,536-metre cells.
	if(@($Vector).Count -ne 3){throw 'Fixture camera position requires three coordinates'}
	$cells=@();$offsets=@()
	foreach($coordinate in $Vector){$value=[double]$coordinate;if([double]::IsNaN($value) -or [double]::IsInfinity($value)){throw 'Fixture camera coordinate must be finite'};$cell=[Math]::Floor(($value+32768.0)/65536.0);$cells+=$cell.ToString('0',[Globalization.CultureInfo]::InvariantCulture);$offsets+=($value-$cell*65536.0)}
	return @{cell=@{x=$cells[0];y=$cells[1];z=$cells[2]};offsetMetres=$offsets}
}
function CompatibilityCapture([string[]]$Tools,$Camera,[string]$Directory) {
	# Archived post-P1 bundles lack future stage APIs. Preserve ordinary camera and
	# profiler evidence via their original interfaces without inventing new counters.
	$result=[ordered]@{status='Unverified';reason='Stage-specific completion-qualified evidence is unavailable';camera=$null;profiler=$null;transactionCapability=('transaction.execute' -in $Tools)}
	if('camera.capture_custom' -in $Tools) {
		$reply=Rpc 'tools/call' @{name='camera.capture_custom';arguments=@{camera=@{viewType=$Camera.viewType;position=(CanonicalWorldPosition $Camera.position);lookAt=(CanonicalWorldPosition $Camera.target);up=$Camera.up;projection='perspective';verticalFovDegrees=$Camera.verticalFovDegrees;nearPlane=$Camera.nearPlane;farPlane=$Camera.farPlane};width=1920;height=1080;format='image/png';freshness='nextFrame';allowBackendFallback=$false;timeoutMs=30000}}
		$text=@($reply.result.content|Where-Object {$_.type -eq 'text'});if($text.Count -ne 1){throw 'Compatibility camera capture omitted its envelope'};$capture=$text[0].text|ConvertFrom-Json
		if(!$capture.ok -or !$capture.data.backendReadback -or $capture.data.deterministicFallback){throw 'Compatibility camera capture did not produce a genuine GPU readback'}
		$images=@($reply.result.content|Where-Object {$_.type -eq 'image' -and $_.mimeType -eq 'image/png'});if($images.Count -ne 1){throw 'Compatibility capture did not return one PNG'}
		$path=Join-Path $Directory 'compatibility.png';[IO.File]::WriteAllBytes($path,[Convert]::FromBase64String($images[0].data));$result.camera=@{path='compatibility.png';sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()}
	}
	if('profiler.capture.start' -in $Tools -and 'profiler.capture.stop' -in $Tools) {
		$start=Tool 'profiler.capture.start' @{captureLevel='sampled';maxFrames=4096;sampleIntervalFrames=1};$deadline=[Diagnostics.Stopwatch]::StartNew()
		# This fallback receipt is raw profiler evidence, not a claim of 300 matched
		# deterministic measured frames. Later stage collection must prove that count.
		while($deadline.Elapsed.TotalSeconds -lt 2){Start-Sleep -Milliseconds 100}
		$stop=Tool 'profiler.capture.stop';$result.profiler=@{start=$start;stop=$stop}
		if((Has $stop 'jobId') -and 'jobs.get' -in $Tools) {
			$clock=[Diagnostics.Stopwatch]::StartNew();do{$job=Tool 'jobs.get' @{jobId=$stop.jobId};if($job.job.state -in @('succeeded','failed','canceled')){break};Start-Sleep -Milliseconds 100}while($clock.Elapsed.TotalSeconds -lt 60)
			$result.profiler.job=$job;if($job.job.state -ne 'succeeded'){throw 'Archived profiler persistence did not succeed'}
		}
	}
	JsonWrite (Join-Path $Directory 'compatibility.json') $result;return $result
}
try {
	$install=[IO.Path]::GetFullPath($InstallRoot);$work=[IO.Path]::GetFullPath($WorkRoot);$output=[IO.Path]::GetFullPath($OutputDirectory)
	$summary.installRoot=$install;Fresh $work;Fresh $output
	$expectedRunnerPath=[IO.Path]::GetFullPath((Join-Path $install 'Tests/Tools/RendererEfficiency/Invoke-RendererEfficiencyValidation.ps1'))
	if([IO.Path]::GetFullPath($PSCommandPath) -ine $expectedRunnerPath){throw 'Invoke the installed runner from InstallRoot'}
	if($work -ieq $output -or $work.StartsWith($output.TrimEnd('\','/')+'\',[StringComparison]::OrdinalIgnoreCase) -or $output.StartsWith($work.TrimEnd('\','/')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'WorkRoot and OutputDirectory must be disjoint'}
	foreach($path in @($work,$output)) { if($path.StartsWith($install.TrimEnd('\','/')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Disposable paths must be outside installed bundle'} }
	$inventory=Inventory $install;$summary.installReceiptHash=$inventory.receiptHash
	$installedEditorPath=Installed $inventory 'Editor/TerraEngine-Editor.exe';$materializer=Installed $inventory 'Tests/Tools/RendererEfficiency/TerraEngine-RendererEfficiencyFixtureMaterializer.exe'
	$manifest=Installed $inventory 'Tests/Fixtures/RendererEfficiency/workloads.json';$schema=Installed $inventory 'Tests/Tools/RendererEfficiency/Schemas/evidence.schema.json';$comparer=Installed $inventory 'Tests/Tools/RendererEfficiency/Compare-RendererEfficiencyEvidence.ps1'
	$installedRunner=Installed $inventory 'Tests/Tools/RendererEfficiency/Invoke-RendererEfficiencyValidation.ps1';if([IO.Path]::GetFullPath($PSCommandPath) -ine $installedRunner){throw 'Invoke the installed runner from InstallRoot'}
	# Use the same sealed stage selection as the comparer before launching a project.
	$stageStepsPath=Installed $inventory 'Tests/Tools/RendererEfficiency/RendererEfficiencyStageSteps.ps1'
	. $stageStepsPath
	foreach($relative in @('Editor/TerraEngine.dll','Editor/glfw3.dll','Editor/dxcompiler.dll','Editor/Project/Templates/UserSettings/EditorLayout.ini','Editor/Project/Templates/UserSettings/EditorLayout.terra')){$null=Installed $inventory $relative}
	$shaderPaths=@($inventory.files.Keys|Where-Object {$_ -like 'Intermediate/InstalledShaders/*/Lighting/*.spv'})
	if(!$shaderPaths.Count){throw 'Installed Lighting shader closure is absent'};foreach($relative in $shaderPaths){$null=Installed $inventory $relative}
	foreach($shader in @('ClusterDepthMask.comp.spv','ClusterGrid.comp.spv','ClusterLightCount.comp.spv','ClusterLightFill.comp.spv','ClusterLightPrefix.comp.spv','LightCull.comp.spv')){if(@($shaderPaths|Where-Object {[IO.Path]::GetFileName($_) -eq $shader}).Count -ne 1){throw "Missing or ambiguous installed Lighting shader: $shader"}}
	foreach($shader in @('Default.task.tspf','Default.mesh.tspf','Default.frag.tspf')){$paths=@($inventory.files.Keys|Where-Object {$_ -like ('Intermediate/InstalledShaders/*/'+$shader)});if($paths.Count -ne 1){throw "Missing or ambiguous installed Default shader: $shader"};$null=Installed $inventory $paths[0]}
	$inputs=@($inventory.files.Keys|Where-Object {$_ -like 'Tests/Fixtures/RendererEfficiency/Inputs/AnimationImportValidation/*'})
	if(!$inputs.Count){throw 'Installed animation fixture closure is absent'};foreach($relative in $inputs){$null=Installed $inventory $relative}
	$inputPrefix='Tests/Fixtures/RendererEfficiency/Inputs/AnimationImportValidation/'
	foreach($relative in @('Assets/Models/ImportedHero.gltf','Assets/Models/ImportedHero.gltf.meta','Assets/Models/ImportedHero.gltf.import.json','Assets/Models/ImportedHero_Generated/ImportedHero.teprefab','Assets/Models/ImportedHero_Generated/ImportedHero.teprefab.meta','Assets/Models/ImportedHero_Generated/ImportedHeroMaterial.tmat','Assets/Models/ImportedHero_Generated/ImportedHeroMaterial.tmat.meta','Build/Assets/.terra-animation-manifests/da35202f311bd5ea56e23beb0aafbd9a.json','Build/Assets/Models/ImportedHero_Generated/ImportedHeroBody.temf','Build/Assets/Models/ImportedHero_Generated/ImportedHeroMaterial.tmat','Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroIdle_fad19fd5.teanim','Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroSkeleton_b7d2176c.teskel')){$null=Installed $inventory ($inputPrefix+$relative)}
	$manifestValue=Get-Content -LiteralPath $manifest -Raw|ConvertFrom-Json;if($manifestValue.schemaVersion -ne 1 -or @($manifestValue.workloads).Count -ne 9){throw 'Unsupported workload manifest'}
	# BCrypt fixture receipts and the host use canonical lowercase hex identities.
	$summary.manifestHash=(Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash.ToLowerInvariant()
	$selectedStages=if($Stage -eq 'All'){@('P1','P2','P3','P4','P5')}else{@($Stage)}
	foreach($selectedStage in $selectedStages) {
		foreach($fixture in @($manifestValue.workloads|Where-Object {$_.stage -eq $selectedStage -or $_.stage -eq 'All'})) {
			$summary.plannedCommands+=@{executable=$materializer;arguments=@('--install-root',$install,'--work-root',(Join-Path $work ($selectedStage+'-'+$fixture.id)),'--fixture',$fixture.id,'--manifest',$manifest);editor=$installedEditorPath;editorArguments=@('--project','<materialized work>/project');stage=$selectedStage;fixture=$fixture.id}
		}
	}
	if($BaselineInstallRoot) {
		$baseline=Inventory ([IO.Path]::GetFullPath($BaselineInstallRoot)) $true;$null=Installed $baseline 'Editor/TerraEngine-Editor.exe'
		$summary.plannedCommands+=@{baselineInstallRoot=$baseline.root;baselineReceiptHash=$baseline.receiptHash;collection='existing camera.capture_custom/profiler.capture/transaction.execute interfaces; missing stage proof remains Unverified'}
	}
	$recoveredByName=@{}
	if($RecoveredSessions.Count) {
		# Bind internal callbacks exclusively to the sealed installed utility. All
		# recovered sessions validate before the first launch; DryRun never copies.
		. (Installed $inventory 'Tests/Tools/RendererEfficiency/RendererEfficiencyEvidenceStream.ps1')
		. (Installed $inventory 'Tests/Tools/RendererEfficiency/RendererEfficiencyRecoveredSessions.ps1')
		InitializeRendererEfficiencyEvidenceStream
		$inspectRecovered={param([string]$Path,[Action[string,string]]$Consumer) [RendererEfficiencyOfflineRecoveryStream]::Inspect($Path,$Consumer)}
		$verifyRecovered={param([string]$Source,[string]$Recovered,[string]$LifecycleJson) [RendererEfficiencyOfflineRecoveryStream]::VerifyUnchangedInsertion($Source,$Recovered,$LifecycleJson)}
		if(!$DryRun) {
			if([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT){throw [InvalidOperationException]::new('UNVERIFIED: Live runner requires Windows')}
			[IO.Directory]::CreateDirectory($output)|Out-Null;$script:ownsOutput=$true
		}
		foreach($recoveryPath in $RecoveredSessions) {
			NoRedirect $recoveryPath
			if(!(Test-Path -LiteralPath $recoveryPath -PathType Leaf) -or ([IO.FileInfo]$recoveryPath).Length -gt 4194304){throw 'Missing or oversized explicit recovery receipt'}
			$recovery=Get-Content -LiteralPath $recoveryPath -Raw|ConvertFrom-Json
			$fixture=@($manifestValue.workloads|Where-Object {$_.id -ceq $recovery.fixtureId -and ($_.stage -ceq $recovery.stage -or $_.stage -ceq 'All')})
			$allowedModes=if($Mode -eq 'Compare'){@('Reference','Candidate')}else{@($Mode)}
			if($FoundationSmoke -or $recovery.stage -cnotin $selectedStages -or $fixture.Count -ne 1 -or $recovery.mode -cnotin $allowedModes -or $recovery.runIndex -lt 0 -or $recovery.runIndex -ge $Runs -or $recovery.preparation -cne $ValidationPreparation){throw 'Recovery is outside the complete planned matrix'}
			$name="$($recovery.stage)-$($recovery.fixtureId)-$($recovery.runIndex)-$($recovery.mode)"
			if($recoveredByName.ContainsKey($name)){throw 'Duplicate recovered planned session'}
			$arguments=@{ReceiptPath=$recoveryPath;Inventory=$inventory;OutputRoot=$output;Stage=$recovery.stage;FixtureId=$recovery.fixtureId;RunIndex=$recovery.runIndex;Mode=$recovery.mode;Preparation=$ValidationPreparation;ManifestPath=$manifest;SchemaPath=$schema;StageStepsPath=$stageStepsPath;InspectEvidence=$inspectRecovered;VerifyTransformation=$verifyRecovered;ValidateOnly=[bool]$DryRun}
			$recoveredByName[$name]=Import-RendererEfficiencyRecoveredSession @arguments
		}
	}
	if($DryRun) {$summary.status='PreflightPassed';$summary.reason='No process launched or project created; GPU acceptance was not requested';$summary.exitCode=0;Write-Output ($summary|ConvertTo-Json -Depth 100);exit 0}
	if([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT){throw [InvalidOperationException]::new('UNVERIFIED: Live runner requires Windows')}
	[IO.Directory]::CreateDirectory($work)|Out-Null;[IO.Directory]::CreateDirectory($output)|Out-Null;$script:ownsOutput=$true
	JsonWrite (Join-Path $work 'runner-ownership.json') @{schemaVersion=1;ownerId=[Guid]::NewGuid().ToString('N');createdUtc=[DateTime]::UtcNow.ToString('o');installRoot=$install;outputDirectory=$output}
	$referenceEvidence=@();$candidateEvidence=@();$unavailable=$false
	foreach($selectedStage in $selectedStages) {
		$fixtures=@($manifestValue.workloads|Where-Object {$_.stage -eq $selectedStage -or $_.stage -eq 'All'});if($FoundationSmoke){$fixtures=@($fixtures|Select-Object -First 1)}
		foreach($fixture in $fixtures) {
			# Check P1 reuse first so stable-content failures surface before the full
			# refresh collection. Both modes still use fresh processes and the same
			# frozen inputs, warmup windows, sample counts and paired comparisons.
			$modes=if($Mode -eq 'Compare'){if($selectedStage -eq 'P1'){@('Candidate','Reference')}else{@('Reference','Candidate')}}else{@($Mode)};if($FoundationSmoke){$modes=@('Candidate')}
			$runCount=if($FoundationSmoke){1}else{$Runs}
			for($run=0;$run -lt $runCount;$run++) { foreach($selectedMode in $modes) {
				$name="$selectedStage-$($fixture.id)-$run-$selectedMode"
				if($recoveredByName.ContainsKey($name)) {
					$session=$recoveredByName[$name];$summary.sessions+=$session
					JsonWrite (Join-Path (Join-Path $output $name) 'session.json') $session
					if($selectedMode -eq 'Reference'){$referenceEvidence+=$session.evidencePath}else{$candidateEvidence+=$session.evidencePath}
					$recoveredByName.Remove($name);continue
				}
				$sessionWork=Join-Path $work $name;$sessionOutput=Join-Path $output $name;[IO.Directory]::CreateDirectory($sessionOutput)|Out-Null
				# Shared definitions author the exact concrete stage subset while their
				# frozen manifest identity and existing P1 default remain unchanged.
				$materializerOutput=& $materializer --install-root $install --work-root $sessionWork --fixture $fixture.id --manifest $manifest --stage $selectedStage 2>&1
				[IO.File]::WriteAllText((Join-Path $sessionOutput 'materializer.log'),($materializerOutput|Out-String),$utf8);if($LASTEXITCODE -ne 0){throw 'Installed fixture materialization failed'}
				$project=Join-Path $sessionWork 'project';$marker=Get-Content -LiteralPath (Join-Path $project 'renderer-efficiency-project.json') -Raw|ConvertFrom-Json
				if($marker.kind -ne 'RendererEfficiencyValidationProject' -or $marker.fixtureId -ne $fixture.id -or $marker.fixtureHash -ine $summary.manifestHash -or [IO.Path]::GetFullPath($marker.projectRoot) -ine $project){throw 'Materialized project ownership does not match'}
				if(!(Has $marker 'stepsPath') -or !(Has $marker 'stepsHash')){throw 'Materializer omitted the owned stage recipe'}
				$stepsPath=[IO.Path]::GetFullPath((Join-Path $project $marker.stepsPath));Below $project $stepsPath;NoRedirect $stepsPath
				if((Get-FileHash -LiteralPath $stepsPath -Algorithm SHA256).Hash -ine $marker.stepsHash){throw 'Materialized stage recipe hash does not match'}
				$recipe=Get-Content -LiteralPath $stepsPath -Raw|ConvertFrom-Json
				$expectedStepIds=@((GetRendererEfficiencyStageSteps $fixture $selectedStage)|ForEach-Object {$_.id})
				$actualStepIds=@($recipe.steps|ForEach-Object {$_.id})
				if(!$expectedStepIds.Count -or [string]::Join('|',$expectedStepIds) -cne [string]::Join('|',$actualStepIds)){throw 'Materialized steps do not match the frozen stage order'}
				$session=[ordered]@{name=$name;fixtureId=$fixture.id;stage=$selectedStage;mode=$selectedMode;runIndex=$run;status='Unverified';reason='';ownershipVerified=$false;optionsObserved=$false;lifecycleVerified=$false;evidencePath=$null;ownedProcess=$null;ownedEditorProcessId=$null;ownedEditorExitCode=$null}
				# Each launch owns its own status snapshot; an earlier run must never
				# supply the operation identity when this launch fails before status.
				$state=$null;$document=$null
				try {
					$tools=Launch $installedEditorPath $project $sessionOutput;$session.ownershipVerified=$true
					$required=@('renderer.efficiency.stats','renderer.efficiency.validation.set','renderer.efficiency.validation.reset','renderer.efficiency.validation.start','renderer.efficiency.validation.status','renderer.efficiency.validation.stop')
					if(@($required|Where-Object {$_ -notin $tools}).Count){$camera=$manifestValue.cameras[0];$session.compatibility=CompatibilityCapture $tools $camera $sessionOutput;$session.reason='Stage automation is unavailable';$unavailable=$true}
					else {
						$options=Options $selectedStage $selectedMode;$set=Tool 'renderer.efficiency.validation.set' $options;$script:priorToken=$set.priorToken;$observed=WaitGeneration ([uint64]$set.requestedGeneration)
						foreach($field in $options.Keys){if($observed.effectiveOptions.$field -ne $options[$field]){throw "Requested option was not observed: $field" }};$session.optionsObserved=$true
						$started=Tool 'renderer.efficiency.validation.start' @{fixtureId=$fixture.id;fixtureHash=$marker.fixtureHash;fixtureManifestSha256=$summary.manifestHash;ownershipMarkerPath=(Join-Path $project 'renderer-efficiency-project.json');stage=$selectedStage;seed=$fixture.seed;runs=$Runs;warmupFrames=$WarmupFrames;measuredFrames=$MeasuredFrames;runId=$name;runIndex=$run;mode=$selectedMode};$script:operationId=$started.operationId
						$clock=[Diagnostics.Stopwatch]::StartNew()
						do {
							$state=Tool 'renderer.efficiency.validation.status' @{operationId=$script:operationId}
							if($state.operationId -ne $script:operationId -or [IO.Path]::GetFullPath($state.projectRoot) -ine [IO.Path]::GetFullPath($project) -or $state.fixtureHash -ine $marker.fixtureHash){throw 'Validation operation ownership changed'}
							if($state.status -eq 'Unavailable' -or $FoundationSmoke){$session.reason=$state.unavailableReason;$unavailable=$true;break}
							if($state.state -in @('Completed','Failed','Stopped')){break};Start-Sleep -Milliseconds 100
						}while($clock.Elapsed.TotalSeconds -lt $OperationTimeoutSeconds)
						JsonWrite (Join-Path $sessionOutput 'operation.json') $state
						if($state.state -eq 'Failed'){throw 'Validation operation failed'}
						if($state.status -ne 'Unavailable' -and !$FoundationSmoke -and $state.state -ne 'Completed'){throw 'Validation operation watchdog expired'}
						if($state.state -eq 'Completed' -and (Has $state 'evidenceArtifactPath')) {
							$document=LoadOwnedEvidence $state $project $inventory $name
							$evidencePath=SaveEvidence $document $project $sessionOutput;$session.evidencePath=$evidencePath
							if($selectedMode -eq 'Reference'){$referenceEvidence+=$evidencePath}else{$candidateEvidence+=$evidencePath}
						}elseif($state.state -eq 'Completed') {throw 'Completed operation omitted schema-v1 evidence'}
						$stopped=Tool 'renderer.efficiency.validation.stop' @{operationId=$script:operationId};$script:operationId='';if($stopped.state -ne 'Stopped'){throw 'Session stop did not settle'}
						if((Has $stopped 'restorationStatus') -and $stopped.restorationStatus -ne 'Queued') { $session.reason='Prior options restoration unavailable: '+$stopped.restorationError;throw $session.reason }
						if($session.reason){$session.status='Unverified'}else{$session.status='Collected'}
					}
				} catch {
					# Archive the actual operation/transport failure before cleanup can
					# replace its context. Cleanup never implies restored options.
					$session.status='Failed';$session.reason=$_.Exception.Message;$session.ownedProcess=OwnedProcessObservation
					throw
				} finally {
					# Stop clears the live token, so retain only this launch's observed
					# operation identity before cleanup, including first-status failures.
					$ownedOperationId=if($null -ne $state -and (Has $state 'operationId')){$state.operationId}elseif($script:operationId){$script:operationId}else{$null}
					$cleanupFailure=$null
					try{StopOwned}catch{$cleanupFailure=$_;$session.status='Failed';$session.reason+='; owned editor cleanup failed: '+$_.Exception.Message}
					$session.ownedEditorProcessId=$script:lifecycleData.ownedEditorProcessId;$session.ownedEditorExitCode=$script:lifecycleData.processExitCode
					if($script:watchdogFailed) {
						$session.status='Failed'
						if(!$session.reason){$session.reason='Owned editor failed restoration or normal close'}
						if($script:lifecycleData.reason){$session.reason+='; '+$script:lifecycleData.reason}
					}
					if($session.status -eq 'Failed' -and $null -ne $session.ownedEditorExitCode){$session.reason+='; owned editor process exit code: '+$session.ownedEditorExitCode}
					# Cleanup evidence is useful even when no measurement document was
					# produced. Only actual restoration and normal process exit verify it.
					$session.lifecycleVerified=($script:lifecycleData.optionsRestored -and $script:lifecycleData.normalClose -and $script:lifecycleData.processExitCode -eq 0 -and !$script:lifecycleData.watchdogFailed)
					$script:lifecycleData.fixtureId=$fixture.id;$script:lifecycleData.fixtureHash=$marker.fixtureHash
					$script:lifecycleData.stage=$selectedStage;$script:lifecycleData.mode=$selectedMode
					$script:lifecycleData.runId=$name;$script:lifecycleData.runIndex=$run;$script:lifecycleData.preparation=$ValidationPreparation
					$script:lifecycleData.operationId=$ownedOperationId
					$lifecycleBase=if($session.evidencePath){[IO.Path]::GetDirectoryName($session.evidencePath)}else{$sessionOutput}
					$lifecyclePath=Join-Path $lifecycleBase 'runner.lifecycle.json';JsonWrite $lifecyclePath $script:lifecycleData
					if($session.evidencePath) {
						# Retain this launch's parsed graph; reparsing the archive doubles peak memory.
						if($null -eq $document){throw 'Evidence document owner was not retained'}
						$document.artifacts+=@([pscustomobject]@{kind='Counters';status='Valid';path='runner.lifecycle.json';sha256=(Get-FileHash -LiteralPath $lifecyclePath -Algorithm SHA256).Hash.ToLowerInvariant();reason=''})
						WriteEvidenceJson $session.evidencePath $document
					}
					JsonWrite (Join-Path $sessionOutput 'session.json') $session;$summary.sessions+=$session
					if($cleanupFailure){throw $cleanupFailure}
				}
				if($script:watchdogFailed){throw 'Owned editor failed restoration or normal close; forced cleanup is failure'}
			} }
		}
		if($FoundationSmoke){break}
	}
	if($recoveredByName.Count){throw 'Verified recovery was not consumed by its exact planned launch'}
	# Baseline compatibility uses an independent archived binary and the same
	# materialized project assets; never rebuild or substitute the candidate.
	if($BaselineInstallRoot -and !$FoundationSmoke) {
		$baselineEditor=Installed $baseline 'Editor/TerraEngine-Editor.exe';$baselineOutput=Join-Path $output 'archived-post-P1';[IO.Directory]::CreateDirectory($baselineOutput)|Out-Null
		$baselineProject=$summary.sessions[0].name;$baselineProject=Join-Path (Join-Path $work $baselineProject) 'project'
		try{$tools=Launch $baselineEditor $baselineProject $baselineOutput $false;$null=CompatibilityCapture $tools $manifestValue.cameras[0] $baselineOutput}finally{StopOwned}
		$unavailable=$true;$summary.reason='Archived compatibility artifacts collected; deterministic total-frame baseline proof still required'
	}
	if($unavailable -or $FoundationSmoke){$summary.status='Unverified';$summary.exitCode=2;if(!$summary.reason){$summary.reason='Stage GPU providers or required samples are unavailable'}}
	elseif($Mode -eq 'Compare') {
		$codes=@()
		foreach($selectedStage in $selectedStages) {
			$stageReferences=@($referenceEvidence|Where-Object {(Get-Content -LiteralPath $_ -Raw|ConvertFrom-Json).identity.stage -eq $selectedStage})
			$stageCandidates=@($candidateEvidence|Where-Object {(Get-Content -LiteralPath $_ -Raw|ConvertFrom-Json).identity.stage -eq $selectedStage})
			if(!$stageReferences.Count -or !$stageCandidates.Count){$codes+=2;continue}
			$compareOutput=Join-Path $output ($selectedStage+'-comparison.json');& $comparer -Reference $stageReferences -Candidate $stageCandidates -Stage $selectedStage -Manifest $manifest -Schema $schema -Output $compareOutput|Out-Null;$codes+=$LASTEXITCODE
		}
		$summary.exitCode=if(1 -in $codes){1}elseif(3 -in $codes){3}elseif(2 -in $codes){2}else{0}
		if($Stage -eq 'All' -and !$BaselineInstallRoot -and $summary.exitCode -eq 0){$summary.exitCode=2;$summary.reason='Final total-frame regression comparison requires independent archived post-P1 baseline evidence'}
		$summary.status=if($summary.exitCode -eq 0){'Passed'}elseif($summary.exitCode -eq 1){'Failed'}elseif($summary.exitCode -eq 3){'Invalid'}else{'Unverified'}
	}else{$summary.status='Unverified';$summary.exitCode=2;$summary.reason='Single-side evidence collected; matched reference/candidate comparison is required'}
} catch {
	$summary.reason=$_.Exception.Message
	if($summary.sessions.Count -and $summary.sessions[-1].status -eq 'Failed' -and $summary.sessions[-1].reason){$summary.reason=$summary.sessions[-1].reason}
	if($summary.reason.StartsWith('UNVERIFIED:')){$summary.status='Unverified';$summary.exitCode=2}
	elseif($script:editor -or $script:watchdogFailed -or $summary.sessions.Count){$summary.status='Failed';$summary.exitCode=1}
	else{$summary.status='Invalid';$summary.exitCode=3}
} finally {
	try{StopOwned}catch{$summary.status='Failed';$summary.exitCode=1;$summary.reason='Owned editor cleanup failed: '+$_.Exception.Message}
	if(!$DryRun -and $script:ownsOutput){JsonWrite (Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) 'summary.json') $summary}
}
Write-Output ($summary|ConvertTo-Json -Depth 100)
exit $summary.exitCode
