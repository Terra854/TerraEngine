# TestValidationModels.ps1 - Fail-closed installed-tree validation of acquired models.
#
# Live execution creates a disposable project and isolated preferences, launches
# only installed binaries, and records actual MCP responses plus optional
# operator-supplied mouse/keyboard proof. It never creates passing manual or
# visual evidence on the operator's behalf. -AutomationOnly continues through
# all machine-verifiable work while recording the manual matrix as incomplete;
# -DryRun validates immutable inputs without launching any process.

[CmdletBinding()]
param(
	[ValidateNotNullOrEmpty()][string]$Manifest = 'Tools/Animation/ValidationModels.json',
	[ValidateNotNullOrEmpty()][string]$CacheRoot = 'out/validation/animation-models',
	[ValidateNotNullOrEmpty()][string]$InstallRoot = 'out/build/RelWithDebInfo/Build',
	[ValidateNotNullOrEmpty()][string]$ResultsRoot = 'out/validation/animation-results',
	[ValidateNotNullOrEmpty()][string]$ExportRuntimeResultsRoot = 'out/validation/animation/final/export-runtime',
	[ValidateRange(1024, 65535)][int]$McpPort = 43143,
	[ValidateRange(30, 600)][int]$EditorStartupTimeoutSeconds = 180,
	[ValidateRange(60, 3600)][int]$ManualEvidenceTimeoutSeconds = 900,
	[string]$ResumeRunRoot = '',
	[switch]$AutomationOnly,
	[switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:Utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$script:McpSequence = 0
$script:McpEndpoint = ''
$script:McpTranscript = ''
$script:AnimationMutationSequence = 0
# Phase F deliberately validates this fixed, licensed acquisition corpus.  A
# caller may choose where the manifest/cache live, but may not silently turn a
# four-model real-content gate into a smaller or substitute fixture run.
$script:RequiredValidationModelArchives = [ordered]@{
	'khronos-animated-morph-cube' = '214ee56160a50dbf22543a1d66dbf860986e87f0efac3d89feac1359d0e6aeab'
	'khronos-rigged-figure' = 'd6be85417d3e256861ee733eea6916093a7af7c79c16366181fd8abcaeb38cf5'
	'khronos-fox-multiple-clips' = 'd97044e701822bac5a62696459b27d7b375aada5de8574ed4362edbba94771f7'
	'quaternius-ultimate-animated-character-pack' = '959db10766e2d2e218a9e4c8216c16ec300e8bf88bbef34f69bacad1e5f4b702'
}

function Get-AbsolutePath([string]$Path, [string]$Base) { if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }; return [System.IO.Path]::GetFullPath((Join-Path $Base $Path)) }
function Assert-AtOrBelow([string]$Parent, [string]$Child, [string]$Description) { $p = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/'); $c = [System.IO.Path]::GetFullPath($Child).TrimEnd('\', '/'); if (-not $c.Equals($p, [System.StringComparison]::OrdinalIgnoreCase) -and -not $c.StartsWith($p + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) { throw "$Description must remain below '$p'." } }
function Write-Json([string]$Path, $Value) { [System.IO.Directory]::CreateDirectory((Split-Path -Parent $Path)) | Out-Null; [System.IO.File]::WriteAllText($Path, (($Value | ConvertTo-Json -Depth 64) + "`n"), $script:Utf8NoBom) }
function Add-JsonLine([string]$Path, $Value) { [System.IO.File]::AppendAllText($Path, (($Value | ConvertTo-Json -Depth 64 -Compress) + "`n"), $script:Utf8NoBom) }
function Get-Sha256([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Get-TextSha256([string]$Text) {
	$hash = [System.Security.Cryptography.SHA256]::Create()
	try { return ([System.BitConverter]::ToString($hash.ComputeHash($script:Utf8NoBom.GetBytes($Text))).Replace('-', '')).ToLowerInvariant() }
	finally { $hash.Dispose() }
}
function Get-Property($Object, [string]$Name) {
	if ($null -eq $Object) { return $null }
	# Validation helpers construct ordered dictionaries for evidence while JSON
	# parsing produces PSCustomObject instances. Handle both explicitly: an
	# OrderedDictionary exposes keys through IDictionary rather than PS properties.
	if ($Object -is [System.Collections.IDictionary]) {
		if ($Object.Contains($Name)) { return $Object[$Name] }
		return $null
	}
	$property = $Object.PSObject.Properties[$Name]
	if ($null -eq $property) { return $null }
	return $property.Value
}
function Get-NestedValue($Object, [string]$Path) { $value = $Object; foreach ($segment in $Path.Split('.')) { $value = Get-Property $value $segment; if ($null -eq $value) { return $null } }; return $value }
function Get-RequiredJsonProperty($Object, [string]$Name, [string]$Description) { if ($null -eq $Object -or $null -eq $Object.PSObject.Properties[$Name]) { throw "$Description is missing required property '$Name'." }; return $Object.PSObject.Properties[$Name].Value }
function ConvertFrom-JsonPreservingArrays([string]$Json, [string]$Description) {
	# Windows PowerShell 5.1 flattens one-element JSON arrays. JavaScriptSerializer
	# is used only for type/shape checks so evidence cannot turn an object or scalar
	# into a required array through PowerShell's normal pipeline coercion.
	Add-Type -AssemblyName System.Web.Extensions
	try {
		$serializer = [System.Web.Script.Serialization.JavaScriptSerializer]::new()
		$serializer.MaxJsonLength = [int]::MaxValue
		return $serializer.DeserializeObject($Json)
	}
	catch {
		throw "$Description is not valid JSON: $($_.Exception.Message)"
	}
}
function Get-RequiredPreservedJsonMember($Object, [string]$Name, [string]$Description) {
	if ($Object -isnot [System.Collections.IDictionary] -or @($Object.Keys) -cnotcontains $Name) { throw "$Description is missing required property '$Name'." }
	Write-Output -NoEnumerate $Object[$Name]
}
function Get-PositiveUInt64($Object, [string]$Name, [string]$Description) {
	# Stable animation IDs are full unsigned 64-bit values. Parse their decimal
	# spelling explicitly so IDs above JavaScript's 53-bit integer range remain
	# exact when supplied by a real editor authoring session.
	$value = Get-RequiredJsonProperty $Object $Name $Description
	$parsed = [UInt64]0
	if ($value -isnot [string] -or -not [UInt64]::TryParse(
		[string]$value,
		[Globalization.NumberStyles]::None,
		[Globalization.CultureInfo]::InvariantCulture,
		[ref]$parsed) -or $parsed -eq 0) {
		throw "$Description property '$Name' must be a nonzero unsigned 64-bit decimal string."
	}
	return $parsed
}
function Find-AnimatorComponentInObjectGraph([object[]]$Objects) {
	foreach ($object in @($Objects)) {
		# PowerShell represents @($null) as a one-element array. Skip that sentinel
		# explicitly so a leaf object without `children` cannot recurse forever.
		if ($null -eq $object) { continue }
		$components = Get-Property $object 'components'
		$animator = Get-Property $components 'AnimatorComponent'
		if ($null -ne $animator) { return $animator }
		$children = Get-Property $object 'children'
		if ($null -ne $children) {
			$nested = Find-AnimatorComponentInObjectGraph @($children)
			if ($null -ne $nested) { return $nested }
		}
	}
	return $null
}

function Find-PlayableValidationPrefab([string]$ProjectRoot, [string[]]$PreferredSkeletonAssetIds = @()) {
	$assetRoot = [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot 'Assets')).TrimEnd('\', '/')
	$preferredSkeletons = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
	foreach ($skeletonId in @($PreferredSkeletonAssetIds)) { if (-not [string]::IsNullOrWhiteSpace($skeletonId)) { [void]$preferredSkeletons.Add($skeletonId) } }
	$candidates = [System.Collections.Generic.List[object]]::new()
	foreach ($prefabPath in @(Get-ChildItem -LiteralPath (Join-Path $assetRoot 'Models') -Filter '*.teprefab' -File -Recurse)) {
		try {
			$document = Get-Content -Raw -LiteralPath $prefabPath.FullName | ConvertFrom-Json
			$animator = Find-AnimatorComponentInObjectGraph @((Get-Property $document 'objects'))
			if ($null -eq $animator) { continue }
			$defaultClip = [string](Get-Property $animator 'defaultClipAssetId')
			$skeleton = [string](Get-Property $animator 'skeletonOverrideAssetId')
			if ($defaultClip -notmatch '^[0-9a-f]{32}$' -or $defaultClip -eq ('0' * 32) -or
				$skeleton -notmatch '^[0-9a-f]{32}$' -or $skeleton -eq ('0' * 32)) {
				continue
			}
			$generatedRoot = $prefabPath.Directory.FullName
			$relativeGeneratedRoot = $generatedRoot.Substring($assetRoot.Length + 1)
			$cookedGeneratedRoot = Join-Path (Join-Path $ProjectRoot 'Build/Assets') $relativeGeneratedRoot
			# Package importers may publish cooked animation files beside the source
			# prefab or beneath Build/Assets. Search both legitimate locations so a
			# glTF package is not discarded merely because its cooker keeps generated
			# binary files out of the authoring Assets tree.
			$artifactRoots = @($generatedRoot)
			if (Test-Path -LiteralPath $cookedGeneratedRoot -PathType Container) {
				$artifactRoots += $cookedGeneratedRoot
			}
			$clipCount = @($artifactRoots | ForEach-Object { Get-ChildItem -LiteralPath $_ -Filter '*.teanim' -File -Recurse }).Count
			$skeletonCount = @($artifactRoots | ForEach-Object { Get-ChildItem -LiteralPath $_ -Filter '*.teskel' -File -Recurse }).Count
			if ($clipCount -lt 1 -or $skeletonCount -lt 1) { continue }
			$relative = $prefabPath.FullName.Substring($assetRoot.Length + 1).Replace('\', '/')
			$candidates.Add([ordered]@{
				absolutePath = $prefabPath.FullName
				assetPath = "Assets/$relative"
				defaultClipAssetId = $defaultClip
				skeletonAssetId = $skeleton
				preferred = $preferredSkeletons.Count -eq 0 -or $preferredSkeletons.Contains($skeleton)
				clipCount = $clipCount
				skeletonCount = $skeletonCount
				sha256 = Get-Sha256 $prefabPath.FullName
			})
		}
		catch {
			# A malformed generated prefab is not silently promoted to the runtime
			# fixture. Other valid candidates remain eligible and the final empty
			# candidate check reports a single actionable failure.
		}
	}
	$selected = @($candidates | Sort-Object -Property @{ Expression = { [bool]$_.preferred }; Descending = $true }, @{ Expression = { [int]$_.clipCount }; Descending = $true }, @{ Expression = { [string]$_.assetPath }; Descending = $false } | Select-Object -First 1)
	if ($selected.Count -ne 1) {
		throw 'No generated prefab contains a resolvable AnimatorComponent plus real cooked clip and skeleton files.'
	}
	if ($preferredSkeletons.Count -gt 0 -and -not [bool]$selected[0].preferred) {
		throw 'No generated prefab binds the preferred real-model skeleton selected for deterministic recording.'
	}
	return $selected[0]
}

function Assert-Manifest($Document) {
	if ([int](Get-Property $Document 'schemaVersion') -ne 1 -or [string](Get-Property $Document 'feature') -ne 'complete-animation-system' -or [string](Get-Property $Document 'manifestStatus') -ne 'verified') { throw 'Manifest has an unsupported or unverified identity.' }
	$models = @((Get-Property $Document 'models'))
	if ($models.Count -ne $script:RequiredValidationModelArchives.Count) { throw "The real-model gate requires exactly $($script:RequiredValidationModelArchives.Count) manifest-pinned model payloads." }
	$ids = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
	foreach ($model in $models) {
		$id = [string](Get-Property $model 'id')
		if (-not $ids.Add($id) -or -not $script:RequiredValidationModelArchives.Contains($id)) { throw "Model '$id' is not one of the four required acquired-model payloads." }
		if ([string](Get-Property $model 'verificationStatus') -ne 'verified' -or [string](Get-Property $model 'archiveSha256') -cne [string]$script:RequiredValidationModelArchives[$id] -or @((Get-Property $model 'files')).Count -lt 1) { throw "Model '$id' is not acquisition-ready or no longer matches its manifest-pinned archive hash; real-model validation is blocked." }
		if ([string](Get-Property $model 'licenseSha256') -notmatch '^[0-9a-f]{64}$' -or [string](Get-Property $model 'licenseFileName') -match '[\\/]') { throw "Model '$id' lacks a pinned, model-specific license record." }
		if (@((Get-Property $model 'validationScenarios')).Count -lt 1 -or @((Get-Property $model 'expectedFeatures')).Count -lt 1) { throw "Model '$id' has no testable scenarios/features." }
	}
	foreach ($id in $script:RequiredValidationModelArchives.Keys) { if (-not $ids.Contains($id)) { throw "Required acquired model '$id' is absent from the manifest." } }
	return $models
}

function Resolve-InstalledBinary([string]$Root, [string[]]$RelativeCandidates, [string]$Description) {
	foreach ($relative in $RelativeCandidates) { $candidate = Join-Path $Root $relative; if (Test-Path -LiteralPath $candidate -PathType Leaf) { $path = (Resolve-Path -LiteralPath $candidate).Path; Assert-AtOrBelow $Root $path $Description; return $path } }
	throw "$Description was not found in the installed tree '$Root'."
}
function Test-LoopbackPortAvailable([int]$Port) { $listener = $null; try { $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port); $listener.Start(); return $true } catch { return $false } finally { if ($null -ne $listener) { $listener.Stop() } } }
function Invoke-McpRpc([string]$Method, $Params = @{}) {
	$script:McpSequence++; $request = [ordered]@{ jsonrpc = '2.0'; id = $script:McpSequence; method = $Method; params = $Params }; $started = Get-Date
	try { $reply = (Invoke-WebRequest -UseBasicParsing -Uri $script:McpEndpoint -Method Post -ContentType 'application/json' -Body ($request | ConvertTo-Json -Depth 64 -Compress) -TimeoutSec 30).Content | ConvertFrom-Json; Add-JsonLine $script:McpTranscript ([ordered]@{ sequence = $script:McpSequence; timestamp = (Get-Date).ToString('o'); request = $request; response = $reply }); if ($null -ne (Get-Property $reply 'error')) { throw "MCP returned error $($reply.error.message)." }; return $reply }
	catch { Add-JsonLine $script:McpTranscript ([ordered]@{ sequence = $script:McpSequence; timestamp = (Get-Date).ToString('o'); request = $request; error = $_.Exception.Message }); throw }
}
function Get-McpEnvelope($Reply, [string]$Tool) { $items = @((Get-Property (Get-Property $Reply 'result') 'content') | Where-Object { [string](Get-Property $_ 'type') -eq 'text' }); if ($items.Count -ne 1) { throw "MCP tool '$Tool' did not return exactly one typed text result." }; return ([string]$items[0].text | ConvertFrom-Json) }
function Invoke-McpToolEnvelope([string]$Name, $Arguments = @{}) { $result = Invoke-McpRpc 'tools/call' ([ordered]@{ name = $Name; arguments = $Arguments }); return Get-McpEnvelope $result $Name }
function Invoke-McpTool([string]$Name, $Arguments = @{}) { $envelope = Invoke-McpToolEnvelope $Name $Arguments; if (-not [bool](Get-Property $envelope 'ok')) { throw "MCP tool '$Name' reported ok=false with status '$([string](Get-Property $envelope 'status'))'." }; return $envelope }
function Invoke-McpPngCapture([string]$Name, $Arguments, [string]$OutputPath) {
	# Capture tools return a typed JSON envelope plus one binary image block. Keep
	# both: the transcript proves the transport contract, while the PNG remains
	# available for an operator to inspect without replaying the editor session.
	$reply = Invoke-McpRpc 'tools/call' ([ordered]@{ name = $Name; arguments = $Arguments })
	$envelope = Get-McpEnvelope $reply $Name
	if (-not [bool](Get-Property $envelope 'ok')) { throw "MCP tool '$Name' reported ok=false with status '$([string](Get-Property $envelope 'status'))'." }
	$images = @((Get-Property (Get-Property $reply 'result') 'content') | Where-Object { [string](Get-Property $_ 'type') -eq 'image' -and [string](Get-Property $_ 'mimeType') -eq 'image/png' })
	if ($images.Count -ne 1) { throw "MCP capture '$Name' did not return exactly one PNG image block." }
	$bytes = [Convert]::FromBase64String([string](Get-Property $images[0] 'data'))
	if ($bytes.Length -lt 8 -or $bytes[0] -ne 0x89 -or $bytes[1] -ne 0x50 -or $bytes[2] -ne 0x4e -or $bytes[3] -ne 0x47) { throw "MCP capture '$Name' returned an invalid PNG payload." }
	[System.IO.Directory]::CreateDirectory((Split-Path -Parent $OutputPath)) | Out-Null
	[System.IO.File]::WriteAllBytes($OutputPath, $bytes)
	return $envelope
}
function Assert-Tool([string[]]$Advertised, [string]$Required) { if ($Advertised -notcontains $Required) { throw "Installed editor MCP does not advertise required tool '$Required'." } }

function Invoke-AnimationMutation([string]$Name, $Arguments, [ref]$Generation, $NoOpFallbackArguments = $null) {
	# Every typed animation write uses the service-wide optimistic generation.
	# Advancing exactly once per successful non-dry request proves the harness did
	# not silently replay a receipt or skip a protected mutation boundary.
	$expected = [UInt64]$Generation.Value
	if ($expected -lt 1) { throw "Animation mutation '$Name' has no valid expected generation." }
	$script:AnimationMutationSequence++
	$key = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp{0:d4}-{1}' -f $script:AnimationMutationSequence, $Name.Replace('.', '-')
	$payload = [ordered]@{}
	foreach ($argumentName in @($Arguments.Keys)) { $payload[$argumentName] = $Arguments[$argumentName] }
	$payload['idempotencyKey'] = $key
	$payload['correlationId'] = $key
	$payload['expectedGeneration'] = $expected
	$envelope = Invoke-McpToolEnvelope $Name $payload
	if (-not [bool](Get-Property $envelope 'ok')) {
		$category = [string](Get-NestedValue $envelope 'error.category')
		$details = [string](Get-NestedValue $envelope 'error.details')
		$isPersistedNoOp = $category -ceq 'conflict' -and $details -like '*no-op against the current animation document generation*'
		if ($null -eq $NoOpFallbackArguments -or -not $isPersistedNoOp) {
			throw "MCP tool '$Name' reported ok=false with status '$([string](Get-Property $envelope 'status'))'."
		}

		# A resumed installed-editor run can reopen an asset after a previous
		# attempt already persisted the requested value. The service correctly
		# rejects that no-op without advancing its generation. Apply the caller's
		# deliberately different checked value so this run still proves one real,
		# atomic authored mutation instead of accepting or manufacturing success.
		$script:AnimationMutationSequence++
		$fallbackKey = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp{0:d4}-{1}-resume-fallback' -f $script:AnimationMutationSequence, $Name.Replace('.', '-')
		$fallbackPayload = [ordered]@{}
		foreach ($argumentName in @($NoOpFallbackArguments.Keys)) {
			$fallbackPayload[$argumentName] = $NoOpFallbackArguments[$argumentName]
		}
		$fallbackPayload['idempotencyKey'] = $fallbackKey
		$fallbackPayload['correlationId'] = $fallbackKey
		$fallbackPayload['expectedGeneration'] = $expected
		$envelope = Invoke-McpToolEnvelope $Name $fallbackPayload
		if (-not [bool](Get-Property $envelope 'ok')) {
			throw "MCP no-op fallback for '$Name' reported ok=false with status '$([string](Get-Property $envelope 'status'))'."
		}
		$data = Get-Property $envelope 'data'
		$data | Add-Member -NotePropertyName 'resumedAfterPersistedNoOp' -NotePropertyValue $true -Force
	}
	$returned = [UInt64](Get-NestedValue $envelope 'data.generation')
	if ($returned -ne $expected + 1) {
		throw "Animation mutation '$Name' returned generation '$returned'; expected exactly '$($expected + 1)'."
	}
	$Generation.Value = $returned
	return $envelope
}

function Get-AnimationSubassets($ModelAssets, [string]$Kind) {
	return @(@((Get-Property $ModelAssets 'subassets')) | Where-Object { [string](Get-Property $_ 'kind') -ceq $Kind })
}

function Invoke-AnimationPreviewMatrix([object[]]$AssetModels, [UInt64]$Generation) {
	$models = [System.Collections.Generic.List[object]]::new()
	foreach ($model in @($AssetModels)) {
		$clips = @(Get-AnimationSubassets $model 'animation_clip')
		if ($clips.Count -eq 0) {
			# RiggedFigure intentionally supplies a bind pose without a clip. Its
			# Skeleton Editor proof remains manual; manufacturing a fake clip would
			# invalidate the real-model matrix.
			$models.Add([ordered]@{ id = [string]$model.id; status = 'not-applicable'; reason = 'The verified source package publishes no animation clip.' })
			continue
		}
		$clipCount = if ([string]$model.id -ceq 'khronos-fox-multiple-clips') { [Math]::Min(2, $clips.Count) } else { 1 }
		if ([string]$model.id -ceq 'khronos-fox-multiple-clips' -and $clipCount -ne 2) {
			throw 'The verified Fox package did not publish the two distinct clips required for transition preview.'
		}
		$clipEvidence = [System.Collections.Generic.List[object]]::new()
		for ($clipIndex = 0; $clipIndex -lt $clipCount; ++$clipIndex) {
			$clip = $clips[$clipIndex]
			$clipId = [string](Get-Property $clip 'assetId')
			if ($clipId -notmatch '^[0-9a-f]{32}$') { throw "Preview clip for '$($model.id)' has no stable asset identity." }
			$actions = [System.Collections.Generic.List[object]]::new()
			foreach ($request in @(
				[ordered]@{ action = 'open'; looping = $true; reverse = $false; speed = 1.0 },
				[ordered]@{ action = 'set_looping'; flag = $true },
				[ordered]@{ action = 'play' },
				[ordered]@{ action = 'tick'; deltaSeconds = (1.0 / 30.0) },
				[ordered]@{ action = 'pause' },
				[ordered]@{ action = 'seek'; normalizedTime = 0.5 },
				[ordered]@{ action = 'resume' },
				[ordered]@{ action = 'set_speed'; speed = 0.75 },
				[ordered]@{ action = 'set_reverse'; flag = $true },
				[ordered]@{ action = 'tick'; deltaSeconds = (1.0 / 30.0) },
				[ordered]@{ action = 'set_reverse'; flag = $false },
				[ordered]@{ action = 'stop' },
				[ordered]@{ action = 'close' }
			)) {
				$arguments = [ordered]@{ assetId = $clipId }
				foreach ($name in @($request.Keys)) { $arguments[$name] = $request[$name] }
				$envelope = Invoke-AnimationMutation 'animation.preview.play' $arguments ([ref]$Generation)
				$actions.Add([ordered]@{
					action = [string]$request.action
					generation = [UInt64](Get-NestedValue $envelope 'data.generation')
					data = Get-Property $envelope 'data'
				})
			}
			$tick = @($actions | Where-Object { [string]$_.action -ceq 'tick' } | Select-Object -First 1)
			if ($tick.Count -ne 1) { throw "Preview sequence for '$($model.id)' omitted its evaluated tick." }
			if ([string]$model.id -ceq 'khronos-animated-morph-cube' -and [int](Get-NestedValue $tick[0] 'data.output.morphCount') -lt 1) {
				throw 'AnimatedMorphCube preview published no evaluated morph channel.'
			}
			if ([string]$model.id -ne 'khronos-animated-morph-cube' -and [int](Get-NestedValue $tick[0] 'data.output.jointCount') -lt 1) {
				throw "Skinned preview for '$($model.id)' published no evaluated joints."
			}
			$clipEvidence.Add([ordered]@{
				assetId = $clipId
				displayName = [string](Get-Property $clip 'displayName')
				publishedGeneration = [UInt64](Get-Property $clip 'publishedGeneration')
				actions = @($actions)
			})
		}
		$models.Add([ordered]@{ id = [string]$model.id; status = 'passed'; clips = @($clipEvidence) })
	}
	return [ordered]@{ generation = $Generation; models = @($models) }
}

function Read-McpJsonResource([string]$Uri) {
	$reply = Invoke-McpRpc 'resources/read' ([ordered]@{ uri = $Uri })
	$contents = @((Get-NestedValue $reply 'result.contents'))
	if ($contents.Count -ne 1 -or [string](Get-Property $contents[0] 'mimeType') -ne 'application/json') { throw "MCP resource '$Uri' did not return exactly one JSON document." }
	return ([string](Get-Property $contents[0] 'text') | ConvertFrom-Json)
}

function Assert-TypedMutationDescriptor(
	$Descriptor,
	[string]$Capability,
	[string]$ExpectedPollOperation = 'animation.job.get',
	[string]$ExpectedVisibilityBoundary = 'animation_authoring_atomic_publish') {
	# Discovery is part of the safety boundary. A live run must not treat a tool
	# with a similarly named but weaker mutation contract as approval-protected.
	$valid = $null -ne $Descriptor
	$valid = $valid -and [string](Get-Property $Descriptor 'approvalPolicy') -eq 'high_impact'
	$valid = $valid -and [bool](Get-Property $Descriptor 'requiresApproval') -eq $true
	$valid = $valid -and [bool](Get-NestedValue $Descriptor 'jobBehavior.returnsDurableJob') -eq $true
	$valid = $valid -and [string](Get-NestedValue $Descriptor 'jobBehavior.pollOperation') -eq $ExpectedPollOperation
	$valid = $valid -and [bool](Get-NestedValue $Descriptor 'execution.protectedCommit') -eq $true
	$valid = $valid -and [string](Get-NestedValue $Descriptor 'execution.visibilityBoundary') -eq $ExpectedVisibilityBoundary
	if (-not $valid) {
		throw "Installed editor '$Capability' does not advertise its exact high-impact, protected-commit, durable-job contract."
	}
}

function Assert-ApprovedHighImpactBinding($Approval, [string]$Capability) {
	# The original waiting response only proves that a prompt was created. Read
	# the retained approval record after a human approves it and bind the later
	# job to that exact request/fingerprint before accepting any output.
	$recent = Read-McpJsonResource 'terraengine://approvals/recent'
	$approvalId = [string](Get-Property $Approval 'approvalId')
	$requestId = [string](Get-Property $Approval 'requestId')
	$impactFingerprint = [string](Get-Property $Approval 'impactFingerprint')
	if ([string]::IsNullOrWhiteSpace($approvalId) -or [string]::IsNullOrWhiteSpace($requestId) -or [string]::IsNullOrWhiteSpace($impactFingerprint)) {
		throw "High-impact '$Capability' approval omitted its immutable request binding."
	}
	$byApprovalId = @((Get-Property $recent 'approvals') | Where-Object { [string](Get-Property $_ 'approvalId') -eq $approvalId })
	# The job can be visible one scheduler turn before the bridge retains the
	# completed approval result. That brief state is not acceptance: callers keep
	# waiting until the exact retained binding appears, while a conflicting record
	# fails immediately instead of being mistaken for the requested mutation.
	if ($byApprovalId.Count -eq 0) { return $null }
	if ($byApprovalId.Count -ne 1) { throw "High-impact '$Capability' approval ID '$approvalId' is ambiguously retained." }
	$record = $byApprovalId[0]
	$bindingMatches =
		[string](Get-Property $record 'requestId') -eq $requestId -and
		[string](Get-Property $record 'capability') -eq $Capability -and
		[string](Get-Property $record 'impactFingerprint') -eq $impactFingerprint
	if (-not $bindingMatches) {
		throw "High-impact '$Capability' approval was retained with a conflicting immutable request binding."
	}

	$state = [string](Get-Property $record 'state')
	$approved = [bool](Get-Property $record 'approved')
	$dispatchCompleted = [bool](Get-Property $record 'dispatchCompleted')
	# Approval dispatch publishes the durable animation job before the bridge
	# atomically replaces its retained approval snapshot. Treat that matching
	# pending/dispatching window as incomplete evidence and poll again; never
	# weaken the immutable request/fingerprint comparison above.
	if (($state -eq 'pending' -and -not $approved -and -not $dispatchCompleted) -or
		($state -eq 'dispatching' -and $approved -and -not $dispatchCompleted)) {
		return $null
	}
	if ($state -ne 'approved' -or -not $approved -or -not $dispatchCompleted) {
		throw "High-impact '$Capability' approval reached unexpected retained state '$state'."
	}
	return $record
}

function Assert-RecordingBindCurrentDescriptor($Descriptor) {
	# An approval may remain pending for minutes while AnimationSystem continues to
	# publish. A client-supplied pose generation/tick can therefore never be a
	# safe recording precondition. Require the explicit protected-commit mode that
	# binds and retains the current immutable snapshot only after approval.
	$bindSchema = Get-NestedValue $Descriptor 'inputSchema.properties.bindCurrentPublication'
	$generationSchema = Get-NestedValue $Descriptor 'inputSchema.properties.publicationGeneration'
	if ($null -eq $bindSchema -or [string](Get-Property $bindSchema 'type') -cne 'boolean' -or
		$null -eq $generationSchema -or [Int64](Get-Property $generationSchema 'minimum') -ne 0) {
		throw 'Installed editor animation.recording.start lacks approval-safe bindCurrentPublication support.'
	}
}

function Get-GenericImportEvidence($Envelope, $Model, [bool]$Force) {
	$ModelId = [string](Get-Property $Model 'id')
	$importResult = Get-NestedValue $Envelope 'data.importResult'
	$status = [string](Get-Property $importResult 'status')
	if ($null -eq $importResult -or $status -notin @('Succeeded', 'SucceededWithWarnings')) { throw "Generic import for '$ModelId' did not publish a usable package; status was '$status'." }
	$manifest = Get-Property $importResult 'animationManifest'
	$packageGuid = [string](Get-Property $importResult 'packageGuid')
	$manifestPackageGuid = [string](Get-Property $manifest 'packageGuid')
	$generation = [uint64](Get-Property $manifest 'publishedGeneration')
	if ($packageGuid -notmatch '^[0-9a-f]{32}$' -or $manifestPackageGuid -cne $packageGuid -or $generation -lt 1) {
		throw "Generic import for '$ModelId' omitted a matching stable package identity or published generation."
	}
	if ([bool](Get-Property $manifest 'cancelled') -or -not [bool](Get-Property $manifest 'publishedAtomically') -or [bool](Get-Property $manifest 'preservedLastKnownGood')) {
		throw "Generic import for '$ModelId' did not prove a new, non-cancelled atomic publication."
	}

	# Stable child identity is the entire semantic mapping, not merely the GUID
	# set. Comparing these normalized records catches a reimport that accidentally
	# swaps two otherwise valid GUIDs between different package children.
	$subAssets = @((Get-Property $manifest 'subAssets'))
	$seenGuids = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
	$identityRecords = @()
	foreach ($subAsset in $subAssets) {
		$guid = [string](Get-Property $subAsset 'guid')
		$kind = [string](Get-Property $subAsset 'kind')
		$semanticKey = [string](Get-Property $subAsset 'semanticKey')
		$sourcePath = [string](Get-Property $subAsset 'sourcePath')
		$compiledPath = [string](Get-Property $subAsset 'compiledPath')
		if ($guid -notmatch '^[0-9a-f]{32}$' -or -not $seenGuids.Add($guid) -or [string]::IsNullOrWhiteSpace($kind) -or [string]::IsNullOrWhiteSpace($semanticKey)) {
			throw "Generic import for '$ModelId' published an invalid or duplicate animation-child identity."
		}
		$identityRecords += [ordered]@{ guid = $guid; kind = $kind; semanticKey = $semanticKey; sourcePath = $sourcePath; compiledPath = $compiledPath }
	}
	if ($identityRecords.Count -lt 1) { throw "Generic import for '$ModelId' omitted animation-child identity evidence." }
	$subassetIdentities = @($identityRecords | ForEach-Object { ConvertTo-Json -InputObject $_ -Compress } | Sort-Object)

	# The acquisition manifest mixes package-import facts with later preview and
	# runtime scenarios. Prove every feature that has an unambiguous child-kind
	# representation here, and leave semantic/runtime properties to their named
	# validation scenarios instead of treating copied manifest text as evidence.
	$requiredKinds = @{}
	$verifiedImportFeatures = @()
	$pendingScenarioFeatures = @()
	foreach ($featureValue in @((Get-Property $Model 'expectedFeatures'))) {
		$feature = ([string]$featureValue).ToLowerInvariant()
		switch ($feature) {
			'animation' { $requiredKinds['AnimationClip'] = [Math]::Max([int]$requiredKinds['AnimationClip'], 1); $verifiedImportFeatures += $feature; break }
			'multiple-clips' { $requiredKinds['AnimationClip'] = [Math]::Max([int]$requiredKinds['AnimationClip'], 2); $verifiedImportFeatures += $feature; break }
			'morph' { $requiredKinds['MorphTarget'] = [Math]::Max([int]$requiredKinds['MorphTarget'], 1); $verifiedImportFeatures += $feature; break }
			'materials' { $requiredKinds['Material'] = [Math]::Max([int]$requiredKinds['Material'], 1); $verifiedImportFeatures += $feature; break }
			'skeleton' { $requiredKinds['Skeleton'] = [Math]::Max([int]$requiredKinds['Skeleton'], 1); $verifiedImportFeatures += $feature; break }
			'skin' {
				$requiredKinds['Skeleton'] = [Math]::Max([int]$requiredKinds['Skeleton'], 1)
				$requiredKinds['DeformableMesh'] = [Math]::Max([int]$requiredKinds['DeformableMesh'], 1)
				$verifiedImportFeatures += $feature
				break
			}
			'fbx' {
				if ([string](Get-Property $manifest 'importerName') -notmatch '(?i)fbx') { throw "Generic import for '$ModelId' did not use the expected FBX importer." }
				$verifiedImportFeatures += $feature
				break
			}
			default { $pendingScenarioFeatures += $feature }
		}
	}
	$kindCounts = [ordered]@{}
	foreach ($kind in @($requiredKinds.Keys | Sort-Object)) {
		$actual = @($subAssets | Where-Object { [string](Get-Property $_ 'kind') -ceq [string]$kind }).Count
		$required = [int]$requiredKinds[$kind]
		if ($actual -lt $required) { throw "Generic import for '$ModelId' published $actual '$kind' children; at least $required were required." }
		$kindCounts[$kind] = $actual
	}
	return [ordered]@{
		surface = 'asset.import'
		force = $Force
		status = $status
		packageGuid = $packageGuid
		publishedGeneration = $generation
		publishedAtomically = $true
		subassetGuids = @($seenGuids | Sort-Object)
		subassetIdentities = $subassetIdentities
		requiredKindCounts = $kindCounts
		verifiedImportFeatures = @($verifiedImportFeatures | Sort-Object -Unique)
		pendingScenarioFeatures = @($pendingScenarioFeatures | Sort-Object -Unique)
	}
}

function Assert-PendingTypedApproval($Envelope, [string]$Capability) {
	$status = [string](Get-Property $Envelope 'status')
	$requestId = [string](Get-Property $Envelope 'requestId')
	$approvalId = [string](Get-NestedValue $Envelope 'data.approvalId')
	$impactFingerprint = [string](Get-NestedValue $Envelope 'data.impactFingerprint')
	if ([bool](Get-Property $Envelope 'ok') -ne $false -or $status -ne 'waiting_approval' -or [string]::IsNullOrWhiteSpace($requestId) -or [string]::IsNullOrWhiteSpace($approvalId) -or [string]::IsNullOrWhiteSpace($impactFingerprint)) { throw "Typed operation '$Capability' did not enter its required approval boundary with an immutable request binding." }
	$pending = Read-McpJsonResource 'terraengine://approvals/pending'
	$match = @((Get-Property $pending 'approvals') | Where-Object { [string]$_.approvalId -eq $approvalId -and [string]$_.requestId -eq $requestId -and [string]$_.capability -eq $Capability -and [string]$_.state -eq 'pending' })
	if ($match.Count -ne 1) { throw "Approval '$approvalId' for '$Capability' was not discoverable through the pending-approval resource." }
	return [ordered]@{ capability = $Capability; approvalPolicy = 'high_impact'; requestId = $requestId; approvalId = $approvalId; impactFingerprint = $impactFingerprint; status = $status; executed = $false; jobId = $null; reason = 'Automation-only validation cannot approve a high-impact editor mutation.' }
}

function Wait-ForApprovedAnimationJob(
	$Envelope,
	[string]$Capability,
	[string]$CorrelationId,
	[string]$RunRoot,
	[string]$JobCapability = '') {
	$approval = Assert-PendingTypedApproval $Envelope $Capability
	if ([string]::IsNullOrWhiteSpace($JobCapability)) { $JobCapability = $Capability }
	$approval.reason = 'Waiting for a real editor UI approval.'
	Add-JsonLine (Join-Path $RunRoot 'editor/approval-requests.jsonl') $approval
	Write-Host "Approve $Capability request $($approval.approvalId) in Preferences -> Approvals within five minutes."
	$deadline = (Get-Date).AddSeconds([Math]::Min($ManualEvidenceTimeoutSeconds, 300))
	while ((Get-Date) -lt $deadline) {
		$resource = Read-McpJsonResource 'terraengine://animation/jobs'
		$jobs = @((Get-Property $resource 'jobs') | Where-Object { [string]$_.capability -eq $JobCapability -and [string]$_.correlationId -eq $CorrelationId })
		# An approved handler can reject before durable-job admission (for example,
		# if its immutable target binding became stale). Surface that exact result
		# immediately instead of hiding it behind a five-minute job polling timeout.
		$approvedRecord = Assert-ApprovedHighImpactBinding $approval $Capability
		if ($null -ne $approvedRecord -and [bool](Get-NestedValue $approvedRecord 'dispatchResult.ok') -ne $true) {
			$dispatchResult = Get-Property $approvedRecord 'dispatchResult'
			throw "Approved '$Capability' dispatch failed before durable job admission: $($dispatchResult | ConvertTo-Json -Depth 16 -Compress)"
		}
		if ($jobs.Count -gt 0) {
			if ($jobs.Count -ne 1) { throw "Approved '$Capability' request has $($jobs.Count) durable jobs for correlation '$CorrelationId'; expected exactly one." }
			if ($null -eq $approvedRecord) { Start-Sleep -Milliseconds 250; continue }
			$job = $jobs[0]
			$jobId = [string](Get-Property $job 'jobId')
			if ([string]::IsNullOrWhiteSpace($jobId)) { throw "Approved '$Capability' job omitted its retained ID." }
			$completed = Wait-McpJob $jobId 'animation.job.get'
			return [ordered]@{
				capability = $Capability
				jobCapability = $JobCapability
				approvalPolicy = 'high_impact'
				requestId = $approval.requestId
				approvalId = $approval.approvalId
				impactFingerprint = $approval.impactFingerprint
				status = 'approved'
				executed = $true
				jobId = $jobId
				approvedBinding = [ordered]@{ requestId = [string](Get-Property $approvedRecord 'requestId'); impactFingerprint = [string](Get-Property $approvedRecord 'impactFingerprint') }
				jobState = [string](Get-Property $completed 'state')
				# Preserve the durable terminal payload. Recording validation needs
				# the exact publication generation/tick bound at approved execution.
				jobResult = Get-Property $completed 'result'
				reason = 'Recovered through terraengine://animation/jobs after real editor approval.'
			}
		}
		Start-Sleep -Milliseconds 500
	}
	throw "Timed out waiting for editor approval and retained job discovery for '$Capability'."
}

function Wait-ForApprovedExportJob($Envelope, [string]$CorrelationId, [string]$RunRoot) {
	# Approval execution happens inside the editor and does not replay the original
	# MCP response. Recover the durable job from the shared session resource using
	# the caller-owned correlation ID, then poll its normal jobs.get contract.
	$approval = Assert-PendingTypedApproval $Envelope 'export.start'
	$approval.reason = 'Waiting for a real editor UI approval.'
	Add-JsonLine (Join-Path $RunRoot 'editor/approval-requests.jsonl') $approval
	Write-Host "Approve export.start request $($approval.approvalId) in Preferences -> Approvals within five minutes."
	$deadline = (Get-Date).AddSeconds([Math]::Min($ManualEvidenceTimeoutSeconds, 300))
	while ((Get-Date) -lt $deadline) {
		$resource = Read-McpJsonResource 'terraengine://jobs/recent'
		$jobs = @((Get-Property $resource 'jobs') | Where-Object { [string]$_.capability -eq 'export.start' -and [string]$_.correlationId -eq $CorrelationId })
		if ($jobs.Count -gt 0) {
			if ($jobs.Count -ne 1) { throw "Approved export.start request has $($jobs.Count) durable jobs for correlation '$CorrelationId'; expected exactly one." }
			$approvedRecord = Assert-ApprovedHighImpactBinding $approval 'export.start'
			if ($null -eq $approvedRecord) { Start-Sleep -Milliseconds 250; continue }
			$jobId = [string](Get-Property $jobs[0] 'jobId')
			if ([string]::IsNullOrWhiteSpace($jobId)) { throw 'Approved export.start job omitted its retained ID.' }
			$completed = Wait-McpJob $jobId 'jobs.get' 1200
			return [ordered]@{ approval = [ordered]@{ capability = 'export.start'; approvalPolicy = 'high_impact'; requestId = $approval.requestId; approvalId = $approval.approvalId; impactFingerprint = $approval.impactFingerprint; status = 'approved'; executed = $true; jobId = $jobId; approvedBinding = [ordered]@{ requestId = [string](Get-Property $approvedRecord 'requestId'); impactFingerprint = [string](Get-Property $approvedRecord 'impactFingerprint') }; reason = 'Recovered through terraengine://jobs/recent after real editor approval.' }; job = $completed }
		}
		Start-Sleep -Milliseconds 500
	}
	throw 'Timed out waiting for editor approval and retained job discovery for export.start.'
}

function Wait-McpJob([string]$JobId, [string]$PollTool, [int]$TimeoutSeconds = 600) {
	# Animation jobs publish their snapshot directly in data, while shared export
	# jobs publish data.job. Normalize both real contracts for the validation driver.
	$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
	while ((Get-Date) -lt $deadline) {
		$reply = Invoke-McpTool $PollTool ([ordered]@{ jobId = $JobId })
		$job = if ($PollTool -eq 'animation.job.get') { Get-Property $reply 'data' } else { Get-NestedValue $reply 'data.job' }
		if ($null -eq $job) { throw "MCP job '$JobId' omitted its retained snapshot." }
		$state = [string](Get-Property $job 'state')
		if ($state -eq 'succeeded') { return $job }
		if ($state -eq 'failed' -or $state -eq 'canceled') {
			$details = Get-Property $job 'result'
			throw "MCP job '$JobId' reached terminal state '$state': $($details | ConvertTo-Json -Depth 16 -Compress)"
		}
		Start-Sleep -Milliseconds 250
	}
	throw "Timed out after $TimeoutSeconds seconds waiting for MCP job '$JobId'."
}

function Invoke-InstalledProcess([string]$Executable, [string[]]$Arguments, [string]$WorkingDirectory, [string]$LogStem, [int]$TimeoutSeconds = 120) {
	# Redirect both streams before waiting so launcher failures remain durable and
	# a quiet GUI-subsystem executable cannot hide its exit code from automation.
	$start = [System.Diagnostics.ProcessStartInfo]::new()
	$start.FileName = $Executable
	$start.Arguments = (($Arguments | ForEach-Object { '"' + ([string]$_).Replace('"', '\"') + '"' }) -join ' ')
	$start.WorkingDirectory = $WorkingDirectory
	$start.UseShellExecute = $false
	$start.CreateNoWindow = $true
	$start.RedirectStandardOutput = $true
	$start.RedirectStandardError = $true
	$process = [System.Diagnostics.Process]::new()
	$process.StartInfo = $start
	if (-not $process.Start()) { throw "Failed to start installed process '$Executable'." }
	$stdoutTask = $process.StandardOutput.ReadToEndAsync()
	$stderrTask = $process.StandardError.ReadToEndAsync()
	if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
		$process.Kill()
		[void]$process.WaitForExit(10000)
		throw "Installed process '$Executable' exceeded its $TimeoutSeconds-second timeout."
	}
	$stdout = $stdoutTask.Result
	$stderr = $stderrTask.Result
	[System.IO.File]::WriteAllText("$LogStem.stdout.log", $stdout, $script:Utf8NoBom)
	[System.IO.File]::WriteAllText("$LogStem.stderr.log", $stderr, $script:Utf8NoBom)
	return [ordered]@{ executable = $Executable; arguments = @($Arguments); exitCode = $process.ExitCode; stdout = $stdout; stderr = $stderr }
}

function Get-PngVisualStatistics([byte[]]$Bytes) {
	# A valid PNG header is not sufficient visual evidence. Decode every pixel
	# into one canonical layout so a black, transparent, or uniform backend
	# fallback cannot be mistaken for rendered animation output.
	$signature = [byte[]](0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a)
	if ($Bytes.Length -lt 24) { throw 'The exported runtime PNG is truncated.' }
	for ($index = 0; $index -lt $signature.Length; ++$index) {
		if ($Bytes[$index] -ne $signature[$index]) { throw 'The exported runtime capture is not a PNG.' }
	}
	Add-Type -AssemblyName System.Drawing
	$stream = $null
	$source = $null
	$bitmap = $null
	$graphics = $null
	$bitmapData = $null
	try {
		$stream = [System.IO.MemoryStream]::new($Bytes, $false)
		$source = [System.Drawing.Image]::FromStream($stream, $true, $true)
		$bitmap = [System.Drawing.Bitmap]::new(
			$source.Width,
			$source.Height,
			[System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
		$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
		$graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
		$graphics.DrawImage($source, 0, 0, $source.Width, $source.Height)
		$graphics.Dispose()
		$graphics = $null

		$rectangle = [System.Drawing.Rectangle]::new(0, 0, $bitmap.Width, $bitmap.Height)
		$bitmapData = $bitmap.LockBits(
			$rectangle,
			[System.Drawing.Imaging.ImageLockMode]::ReadOnly,
			[System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
		$rowLength = [int]$bitmap.Width * 4
		$canonicalPixels = [byte[]]::new($rowLength * [int]$bitmap.Height)
		$row = [byte[]]::new($rowLength)
		$nonBlackVisiblePixels = [UInt64]0
		$firstRgba = [UInt32]0
		$haveFirstPixel = $false
		$uniform = $true
		for ($y = 0; $y -lt $bitmap.Height; ++$y) {
			$rowPointer = [System.IntPtr]::Add($bitmapData.Scan0, $y * $bitmapData.Stride)
			[System.Runtime.InteropServices.Marshal]::Copy($rowPointer, $row, 0, $rowLength)
			[System.Buffer]::BlockCopy($row, 0, $canonicalPixels, $y * $rowLength, $rowLength)
			for ($offset = 0; $offset -lt $rowLength; $offset += 4) {
				$blue = [UInt32]$row[$offset]
				$green = [UInt32]$row[$offset + 1]
				$red = [UInt32]$row[$offset + 2]
				$alpha = [UInt32]$row[$offset + 3]
				# Use an unsigned 64-bit intermediate so colors with red >= 128 do
				# not become negative Int32 values during PowerShell bit shifting.
				$rgba = [UInt32](
					([UInt64]$red -shl 24) -bor
					([UInt64]$green -shl 16) -bor
					([UInt64]$blue -shl 8) -bor
					[UInt64]$alpha)
				if (-not $haveFirstPixel) {
					$firstRgba = $rgba
					$haveFirstPixel = $true
				}
				elseif ($rgba -ne $firstRgba) {
					$uniform = $false
				}
				if (($red -ne 0 -or $green -ne 0 -or $blue -ne 0) -and $alpha -gt 0) {
					$nonBlackVisiblePixels++
				}
			}
		}
		$hash = [System.Security.Cryptography.SHA256]::Create()
		try {
			$pixelSha256 = ([System.BitConverter]::ToString(
				$hash.ComputeHash($canonicalPixels))).Replace('-', '').ToLowerInvariant()
		}
		finally {
			$hash.Dispose()
		}
		return [ordered]@{
			width = $bitmap.Width
			height = $bitmap.Height
			pixelCount = [UInt64]$bitmap.Width * [UInt64]$bitmap.Height
			visibleNonBlackPixelCount = $nonBlackVisiblePixels
			isRgbaUniform = $uniform
			pixelSha256 = $pixelSha256
		}
	}
	finally {
		if ($null -ne $bitmapData -and $null -ne $bitmap) { $bitmap.UnlockBits($bitmapData) }
		if ($null -ne $graphics) { $graphics.Dispose() }
		if ($null -ne $bitmap) { $bitmap.Dispose() }
		if ($null -ne $source) { $source.Dispose() }
		if ($null -ne $stream) { $stream.Dispose() }
	}
}

function Read-AnimationParityReport(
	$ProcessResult,
	[string]$ExpectedMode,
	[bool]$RequireVisualCapture) {
	# Engine logging may surround the compact report. Select exactly one line by
	# its complete schema identity instead of accepting the last parseable JSON.
	$candidates = [System.Collections.Generic.List[object]]::new()
	foreach ($line in @(([string]$ProcessResult.stdout -split "`r?`n"))) {
		$trimmed = $line.Trim()
		if (-not $trimmed.StartsWith('{') -or -not $trimmed.EndsWith('}')) { continue }
		try {
			$candidate = $trimmed | ConvertFrom-Json
			if ([int](Get-Property $candidate 'schemaVersion') -eq 1 -and
				$null -ne (Get-Property $candidate 'ticks') -and
				$null -ne (Get-Property $candidate 'visualCapture')) {
				$candidates.Add($candidate)
			}
		}
		catch {
			# Non-report diagnostic JSON is ignored only when it lacks the complete
			# parity identity above. A malformed candidate cannot be selected.
		}
	}
	if ($candidates.Count -ne 1) {
		throw "The $ExpectedMode launcher emitted $($candidates.Count) animation parity reports; expected exactly one."
	}
	$report = $candidates[0]
	if ($ProcessResult.exitCode -ne 0 -or
		(Get-Property $report 'succeeded') -isnot [bool] -or
		[bool](Get-Property $report 'succeeded') -ne $true -or
		(Get-Property $report 'orderlyShutdown') -isnot [bool] -or
		[bool](Get-Property $report 'orderlyShutdown') -ne $true) {
		throw "The $ExpectedMode launcher did not report successful evaluation and orderly shutdown."
	}
	if ([string](Get-Property $report 'mode') -cne $ExpectedMode -or
		[int](Get-Property $report 'requestedTicks') -ne 120 -or
		[int](Get-Property $report 'ticksEvaluated') -ne 120 -or
		[Math]::Abs([double](Get-Property $report 'fixedDeltaSeconds') - (1.0 / 60.0)) -gt 0.0000001) {
		throw "The $ExpectedMode launcher did not execute the exact bounded 120 x 1/60 parity fixture."
	}
	$ticks = @((Get-Property $report 'ticks'))
	if ($ticks.Count -ne 120 -or [int](Get-Property $report 'animatorCount') -lt 1) {
		throw "The $ExpectedMode report omitted ticks or published animators."
	}
	$stableOwners = ''
	for ($tickIndex = 0; $tickIndex -lt $ticks.Count; ++$tickIndex) {
		$tick = $ticks[$tickIndex]
		if ([int](Get-Property $tick 'tickIndex') -ne $tickIndex) {
			throw "The $ExpectedMode report has a non-contiguous tick sequence at $tickIndex."
		}
		$animators = @((Get-Property $tick 'animators'))
		if ($animators.Count -ne [int](Get-Property $report 'animatorCount')) {
			throw "The $ExpectedMode animator population changed at tick $tickIndex."
		}
		$owners = @()
		foreach ($animator in $animators) {
			$owner = [string](Get-Property $animator 'ownerObjectId')
			$mode = Get-Property $animator 'effectiveExecutionMode'
			if ($owner -notmatch '^[0-9a-f]{32}$' -or
				[string](Get-Property $animator 'status') -cne 'Ok' -or
				[string](Get-Property $animator 'stateHash') -notmatch '^[0-9a-f]{16}$' -or
				[string](Get-Property $animator 'poseHash') -notmatch '^[0-9a-f]{16}$' -or
				$mode -is [string] -or
				(Get-Property $animator 'rootMotion') -isnot [bool]) {
				throw "The $ExpectedMode report contains an invalid animator record at tick $tickIndex."
			}
			$owners += $owner
		}
		$owners = @($owners | Sort-Object -Unique)
		if ($owners.Count -ne $animators.Count) {
			throw "The $ExpectedMode report duplicates an animator owner at tick $tickIndex."
		}
		$ownerKey = $owners -join ','
		if ($tickIndex -eq 0) { $stableOwners = $ownerKey }
		elseif ($ownerKey -cne $stableOwners) {
			throw "The $ExpectedMode report changed animator identity at tick $tickIndex."
		}
	}

	$visual = Get-Property $report 'visualCapture'
	if ((Get-Property $visual 'required') -isnot [bool] -or
		[bool](Get-Property $visual 'required') -ne $RequireVisualCapture -or
		(Get-Property $visual 'available') -isnot [bool]) {
		throw "The $ExpectedMode report has an invalid visual-capture contract."
	}
	if ($RequireVisualCapture) {
		if ([bool](Get-Property $visual 'available') -ne $true -or
			(Get-Property $visual 'backendReadback') -isnot [bool] -or
			[bool](Get-Property $visual 'backendReadback') -ne $true -or
			(Get-Property $visual 'deterministicFallback') -isnot [bool] -or
			[bool](Get-Property $visual 'deterministicFallback') -ne $false -or
			[string](Get-Property $visual 'mimeType') -cne 'image/png' -or
			[int](Get-Property $visual 'width') -lt 64 -or
			[int](Get-Property $visual 'height') -lt 64 -or
			[UInt64](Get-Property $visual 'frameId') -eq 0 -or
			[string]::IsNullOrWhiteSpace([string](Get-Property $visual 'pngBase64'))) {
			throw 'The rendered-runtime report did not contain a real backend PNG readback.'
		}
	}
	elseif ([bool](Get-Property $visual 'available')) {
		throw 'The headless parity lane unexpectedly claimed visual output.'
	}
	return $report
}

function Compare-AnimationParityReports($Rendered, $Headless) {
	# Effective modes are expected to differ: the rendered lane must exercise a
	# GPU-capable path while headless must use CPU fallback. Every authoritative
	# identity, state, pose, event, and root-motion field must still match exactly.
	$renderedTicks = @((Get-Property $Rendered 'ticks'))
	$headlessTicks = @((Get-Property $Headless 'ticks'))
	if ($renderedTicks.Count -ne $headlessTicks.Count -or
		[int](Get-Property $Rendered 'animatorCount') -ne [int](Get-Property $Headless 'animatorCount')) {
		throw 'Rendered/headless reports differ in their declared tick or animator population.'
	}
	$renderedModes = [System.Collections.Generic.HashSet[int]]::new()
	$headlessModes = [System.Collections.Generic.HashSet[int]]::new()
	for ($tickIndex = 0; $tickIndex -lt $renderedTicks.Count; ++$tickIndex) {
		$renderedByOwner = @{}
		foreach ($animator in @((Get-Property $renderedTicks[$tickIndex] 'animators'))) {
			$renderedByOwner[[string](Get-Property $animator 'ownerObjectId')] = $animator
			[void]$renderedModes.Add([int](Get-Property $animator 'effectiveExecutionMode'))
		}
		$headlessByOwner = @{}
		foreach ($animator in @((Get-Property $headlessTicks[$tickIndex] 'animators'))) {
			$headlessByOwner[[string](Get-Property $animator 'ownerObjectId')] = $animator
			[void]$headlessModes.Add([int](Get-Property $animator 'effectiveExecutionMode'))
		}
		if ($renderedByOwner.Count -ne $headlessByOwner.Count) {
			throw "Rendered/headless animator counts differ at tick $tickIndex."
		}
		foreach ($owner in $renderedByOwner.Keys) {
			if (-not $headlessByOwner.ContainsKey($owner)) {
				throw "Headless parity omitted animator '$owner' at tick $tickIndex."
			}
			$left = $renderedByOwner[$owner]
			$right = $headlessByOwner[$owner]
			foreach ($field in @('status', 'stateHash', 'poseHash', 'eventCount', 'rootMotion')) {
				if ([string](Get-Property $left $field) -cne [string](Get-Property $right $field)) {
					throw "Rendered/headless authoritative field '$field' differs for '$owner' at tick $tickIndex."
				}
			}
		}
	}
	if ($headlessModes.Count -ne 1 -or -not $headlessModes.Contains(2)) {
		throw 'The headless lane did not remain in CPU deterministic/fallback mode.'
	}
	if (-not ($renderedModes.Contains(0) -or $renderedModes.Contains(1))) {
		throw 'The rendered lane did not exercise GPU Visual or Hybrid Authoritative mode.'
	}
	return [ordered]@{
		passed = $true
		comparedTicks = $renderedTicks.Count
		comparedAnimatorsPerTick = [int](Get-Property $Rendered 'animatorCount')
		comparedFields = @('status', 'stateHash', 'poseHash', 'eventCount', 'rootMotion')
		renderedEffectiveModes = @($renderedModes | Sort-Object)
		headlessEffectiveModes = @($headlessModes | Sort-Object)
	}
}

function Get-AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpAuthoringAssetId($Object, [string]$Name, [string]$Description) {
	$value = [string](Get-RequiredJsonProperty $Object $Name $Description)
	if ($value -notmatch '^[0-9a-f]{32}$' -or $value -ceq ('0' * 32)) {
		throw "$Description property '$Name' must be one nonzero lowercase UUID."
	}
	return $value
}

function Wait-ForAnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpAuthoringReady(
	[string]$RunRoot,
	[object[]]$AssetModels,
	$RuntimePrefab,
	$NativeWindow,
	[System.Diagnostics.Process]$Editor,
	[UInt64]$Generation) {
	$fox = @($AssetModels | Where-Object { [string]$_.id -ceq 'khronos-fox-multiple-clips' })
	$quaternius = @($AssetModels | Where-Object { [string]$_.id -ceq 'quaternius-ultimate-animated-character-pack' })
	if ($fox.Count -ne 1 -or $quaternius.Count -ne 1) { throw 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp authoring requires exactly one Fox and one Quaternius package record.' }
	$foxClips = @(Get-AnimationSubassets $fox[0] 'animation_clip')
	$foxSkeletons = @(Get-AnimationSubassets $fox[0] 'skeleton')
	$quaterniusSkeletons = @(Get-AnimationSubassets $quaternius[0] 'skeleton')
	$runtimeSkeleton = @($foxSkeletons | Where-Object { [string]$_.assetId -ceq [string]$RuntimePrefab.skeletonAssetId })
	if ($foxClips.Count -lt 2 -or $runtimeSkeleton.Count -ne 1 -or $quaterniusSkeletons.Count -lt 1) {
		throw 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp authoring prerequisites do not expose two Fox clips plus exact Fox and Quaternius skeleton publications.'
	}

	$requestPath = Join-Path $RunRoot 'editor/authoring-request.json'
	$readyPath = Join-Path $RunRoot 'editor/authoring-ready.json'
	$request = [ordered]@{
		schemaVersion = 1
		task = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp'
		instruction = 'Use only the launched installed editor and real UI controls. Create or duplicate the requested production animation assets beneath Assets/Validation/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp, bind them to the exact real-model assets below, save/import them, then write authoring-ready.json. Decimal stable IDs must be copied exactly as strings. Do not use repository semantic JSON fixtures or rename JSON as a cooked asset.'
		nativeWindow = $NativeWindow
		realModelBindings = [ordered]@{
			fox = [ordered]@{
				packageAssetId = [string]$fox[0].assetId
				skeletonAssetId = [string]$RuntimePrefab.skeletonAssetId
				clipAssetIds = @($foxClips | ForEach-Object { [string]$_.assetId })
			}
			quaternius = [ordered]@{
				packageAssetId = [string]$quaternius[0].assetId
				skeletonAssetIds = @($quaterniusSkeletons | ForEach-Object { [string]$_.assetId })
			}
		}
		requiredAssets = @(
			[ordered]@{ kind = 'controller'; path = 'Assets/Validation/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpFoxController.teanimctrl'; requirement = 'Bind the Fox skeleton/fingerprint, author two distinct Fox clip states, a Boolean transition parameter, and a transition between those states.' },
			[ordered]@{ kind = 'skeleton'; path = 'Assets/Validation/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpFoxSkeleton.teskel'; requirement = 'Duplicate the generated Fox skeleton into an editable standalone skeleton, preserve every joint in index order, and report the root joint stable ID.' },
			[ordered]@{ kind = 'retarget'; path = 'Assets/Validation/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpQuaterniusToFox.teretarget'; requirement = 'Bind Quaternius as source and Fox as target, author at least one required mapping and one reachable IK/contact rule.' },
			[ordered]@{ kind = 'motionDatabase'; path = 'Assets/Validation/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpFoxMotion.temotiondb'; requirement = 'Build a real database from Fox clips with at least one retained sample.' },
			[ordered]@{ kind = 'ragdoll'; path = 'Assets/Validation/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpFoxRagdoll.teragdoll'; requirement = 'Bind the Fox skeleton, generate at least one body, and author at least one joint blend record.' },
			[ordered]@{ kind = 'recording'; path = 'Assets/Validation/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpFoxRecording.teanim'; requirement = 'Create a registered Animation Clip starter at the exact recording destination so its durable .meta GUID already matches the recorder assetId.' }
		)
		readyFile = [ordered]@{
			path = 'editor/authoring-ready.json'
			requiredShape = [ordered]@{
				schemaVersion = 1
				completed = $true
				controller = [ordered]@{ assetId = '<uuid>'; parameterStableId = '<decimal>'; sourceStateId = '<decimal>'; destinationStateId = '<decimal>'; transitionStableId = '<decimal>' }
				skeleton = [ordered]@{ assetId = '<uuid>'; rootStableJointId = '<decimal>' }
				retarget = [ordered]@{ assetId = '<uuid>'; mappingStableId = '<decimal>'; sourceJointId = '<decimal>'; targetJointId = '<decimal>'; contactStableId = '<decimal>'; targetChainId = '<decimal>' }
				motionDatabase = [ordered]@{ assetId = '<uuid>'; sampleStableId = '<decimal>' }
				ragdoll = [ordered]@{ assetId = '<uuid>'; bodyStableId = '<decimal>'; blendStableId = '<decimal>' }
				recording = [ordered]@{ assetId = '<uuid>'; compatibilityFingerprint = '<decimal>'; jointStableIds = @('<all Fox joint IDs in skeleton index order>') }
			}
		}
	}
	Write-Json $requestPath $request
	Write-Host "Complete the installed-editor AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp authoring request at: $requestPath"
	Write-Host "Then write the checked asset identities to: $readyPath"

	$deadline = (Get-Date).AddSeconds($ManualEvidenceTimeoutSeconds)
	while ((Get-Date) -lt $deadline) {
		$Editor.Refresh()
		if ($Editor.HasExited) { throw "Installed editor exited before AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp authored assets were ready." }
		if (Test-Path -LiteralPath $readyPath -PathType Leaf) {
			try {
				$candidate = Get-Content -Raw -LiteralPath $readyPath | ConvertFrom-Json
				$completed = Get-Property $candidate 'completed'
				if ($completed -is [bool] -and $completed) { break }
			}
			catch { }
		}
		Start-Sleep -Milliseconds 500
	}
	if (-not (Test-Path -LiteralPath $readyPath -PathType Leaf)) { throw 'Timed out waiting for editor/authoring-ready.json.' }

	$text = Get-Content -Raw -LiteralPath $readyPath
	$strict = ConvertFrom-JsonPreservingArrays $text 'authoring-ready.json'
	try { $ready = $text | ConvertFrom-Json } catch { throw "authoring-ready.json is invalid JSON: $($_.Exception.Message)" }
	if ($strict -isnot [System.Collections.IDictionary] -or $ready -is [System.Array]) { throw 'authoring-ready.json must contain one JSON object.' }
	$completedStrict = Get-RequiredPreservedJsonMember $strict 'completed' 'authoring-ready.json'
	if ($completedStrict -isnot [bool] -or -not $completedStrict -or [int](Get-RequiredJsonProperty $ready 'schemaVersion' 'authoring-ready.json') -ne 1) {
		throw 'authoring-ready.json must declare schemaVersion 1 and JSON Boolean completed:true.'
	}

	$controller = Get-RequiredJsonProperty $ready 'controller' 'authoring-ready.json'
	$skeleton = Get-RequiredJsonProperty $ready 'skeleton' 'authoring-ready.json'
	$retarget = Get-RequiredJsonProperty $ready 'retarget' 'authoring-ready.json'
	$motion = Get-RequiredJsonProperty $ready 'motionDatabase' 'authoring-ready.json'
	$ragdoll = Get-RequiredJsonProperty $ready 'ragdoll' 'authoring-ready.json'
	$recording = Get-RequiredJsonProperty $ready 'recording' 'authoring-ready.json'
	$normalized = [ordered]@{
		controller = [ordered]@{
			assetId = Get-AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpAuthoringAssetId $controller 'assetId' 'authoring-ready controller'
			parameterStableId = Get-PositiveUInt64 $controller 'parameterStableId' 'authoring-ready controller'
			sourceStateId = Get-PositiveUInt64 $controller 'sourceStateId' 'authoring-ready controller'
			destinationStateId = Get-PositiveUInt64 $controller 'destinationStateId' 'authoring-ready controller'
			transitionStableId = Get-PositiveUInt64 $controller 'transitionStableId' 'authoring-ready controller'
		}
		skeleton = [ordered]@{
			assetId = Get-AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpAuthoringAssetId $skeleton 'assetId' 'authoring-ready skeleton'
			rootStableJointId = Get-PositiveUInt64 $skeleton 'rootStableJointId' 'authoring-ready skeleton'
		}
		retarget = [ordered]@{
			assetId = Get-AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpAuthoringAssetId $retarget 'assetId' 'authoring-ready retarget'
			mappingStableId = Get-PositiveUInt64 $retarget 'mappingStableId' 'authoring-ready retarget'
			sourceJointId = Get-PositiveUInt64 $retarget 'sourceJointId' 'authoring-ready retarget'
			targetJointId = Get-PositiveUInt64 $retarget 'targetJointId' 'authoring-ready retarget'
			contactStableId = Get-PositiveUInt64 $retarget 'contactStableId' 'authoring-ready retarget'
			targetChainId = Get-PositiveUInt64 $retarget 'targetChainId' 'authoring-ready retarget'
		}
		motionDatabase = [ordered]@{
			assetId = Get-AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpAuthoringAssetId $motion 'assetId' 'authoring-ready motion database'
			sampleStableId = Get-PositiveUInt64 $motion 'sampleStableId' 'authoring-ready motion database'
		}
		ragdoll = [ordered]@{
			assetId = Get-AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpAuthoringAssetId $ragdoll 'assetId' 'authoring-ready ragdoll'
			bodyStableId = Get-PositiveUInt64 $ragdoll 'bodyStableId' 'authoring-ready ragdoll'
			blendStableId = Get-PositiveUInt64 $ragdoll 'blendStableId' 'authoring-ready ragdoll'
		}
		recording = [ordered]@{
			assetId = Get-AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpAuthoringAssetId $recording 'assetId' 'authoring-ready recording'
			compatibilityFingerprint = Get-PositiveUInt64 $recording 'compatibilityFingerprint' 'authoring-ready recording'
			jointStableIds = @()
		}
	}
	$recordingStrict = Get-RequiredPreservedJsonMember $strict 'recording' 'authoring-ready.json'
	if ($recordingStrict -isnot [System.Collections.IDictionary]) { throw 'authoring-ready recording must be a JSON object.' }
	$jointsStrict = Get-RequiredPreservedJsonMember $recordingStrict 'jointStableIds' 'authoring-ready recording'
	if ($jointsStrict -isnot [System.Array] -or $jointsStrict.Count -lt 1 -or $jointsStrict.Count -gt 64) {
		throw 'authoring-ready recording.jointStableIds must be a non-empty JSON array within the MCP 64-joint bound.'
	}
	$jointIds = [System.Collections.Generic.List[UInt64]]::new()
	$uniqueJointIds = [System.Collections.Generic.HashSet[UInt64]]::new()
	foreach ($jointValue in @($jointsStrict)) {
		$jointId = [UInt64]0
		if ($jointValue -isnot [string] -or
			-not [UInt64]::TryParse([string]$jointValue, [Globalization.NumberStyles]::None, [Globalization.CultureInfo]::InvariantCulture, [ref]$jointId) -or
			$jointId -eq 0 -or -not $uniqueJointIds.Add($jointId)) {
			throw 'authoring-ready recording.jointStableIds contains a zero, duplicate, or non-decimal-string ID.'
		}
		$jointIds.Add($jointId)
	}
	$normalized.recording.jointStableIds = @($jointIds)
	if ([UInt64]$normalized.skeleton.rootStableJointId -ne [UInt64]$jointIds[0]) {
		throw 'The duplicated Fox root stable ID is not first in the declared skeleton-index-order recording binding.'
	}
	$expectedFingerprint = [UInt64](Get-Property $runtimeSkeleton[0] 'compatibilityFingerprint')
	if ($normalized.recording.compatibilityFingerprint -ne $expectedFingerprint) {
		throw 'authoring-ready recording fingerprint does not match the selected generated Fox skeleton publication.'
	}

	$importers = [ordered]@{
		controller = 'AnimationControllerAssetImporter'
		skeleton = 'SkeletonAssetImporter'
		retarget = 'RetargetProfileAssetImporter'
		motionDatabase = 'MotionDatabaseAssetImporter'
		ragdoll = 'RagdollProfileAssetImporter'
		recording = 'AnimationClipAssetImporter'
	}
	$expectedSources = [ordered]@{
		controller = 'Validation/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpFoxController.teanimctrl'
		skeleton = 'Validation/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpFoxSkeleton.teskel'
		retarget = 'Validation/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpQuaterniusToFox.teretarget'
		motionDatabase = 'Validation/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpFoxMotion.temotiondb'
		ragdoll = 'Validation/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpFoxRagdoll.teragdoll'
		recording = 'Validation/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpFoxRecording.teanim'
	}
	$inspections = [ordered]@{}
	foreach ($kind in @($importers.Keys)) {
		$assetId = [string]$normalized[$kind].assetId
		$inspection = Invoke-McpTool 'animation.asset.inspect' ([ordered]@{
			assetId = $assetId
			maximumItems = 64
			correlationId = "AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpAuthoringInspect$kind"
		})
		if ([string](Get-NestedValue $inspection 'data.importer') -cne [string]$importers[$kind]) {
			throw "Authored '$kind' asset '$assetId' is not registered through '$($importers[$kind])'."
		}
		$sourcePath = ([string](Get-NestedValue $inspection 'data.sourcePath')).Replace('\', '/')
		if ($sourcePath -cne [string]$expectedSources[$kind]) {
			throw "Authored '$kind' asset source '$sourcePath' does not match the requested disposable-project path '$($expectedSources[$kind])'."
		}
		if ([UInt64](Get-NestedValue $inspection 'data.generation') -ne $Generation) {
			throw "Read-only authored '$kind' inspection unexpectedly changed the animation generation."
		}
		$inspections[$kind] = Get-Property $inspection 'data'
	}
	$normalized['runtime'] = [ordered]@{
		prefab = [string]$RuntimePrefab.assetPath
		skeletonAssetId = [string]$RuntimePrefab.skeletonAssetId
		defaultClipAssetId = [string]$RuntimePrefab.defaultClipAssetId
		skeletonPublishedGeneration = [UInt64](Get-Property $runtimeSkeleton[0] 'publishedGeneration')
	}
	$normalized['inspections'] = $inspections
	$normalized['readyPath'] = $readyPath
	$normalized['readySha256'] = Get-Sha256 $readyPath
	return $normalized
}

function Invoke-AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpAuthoredMutationMatrix($Ready, [UInt64]$Generation, [string]$RunRoot) {
	$operations = [System.Collections.Generic.List[object]]::new()
	$requests = @(
		# The installed-editor authoring flow deliberately created a Trigger. Its
		# value is monostate, so true/false payload members are correctly ignored.
		# Rename the stable parameter to exercise a real persisted edit without
		# changing its type or breaking the transition that references its ID.
		[ordered]@{ name = 'animation.controller.edit'; assetId = $Ready.controller.assetId; payload = [ordered]@{ action = 'set_parameter_default'; stableId = $Ready.controller.parameterStableId; name = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpShouldWalk' }; fallbackPayload = [ordered]@{ action = 'set_parameter_default'; stableId = $Ready.controller.parameterStableId; name = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpShouldWalkResumed' } },
		[ordered]@{ name = 'animation.controller.edit'; assetId = $Ready.controller.assetId; payload = [ordered]@{ action = 'set_transition'; stableId = $Ready.controller.transitionStableId; sourceStateId = $Ready.controller.sourceStateId; destinationStateId = $Ready.controller.destinationStateId; conditionParameterId = $Ready.controller.parameterStableId; durationTicks = 15; exitTimeTicks = 1; destinationOffsetNormalized = 0.0; priority = 43 }; fallbackPayload = [ordered]@{ action = 'set_transition'; stableId = $Ready.controller.transitionStableId; sourceStateId = $Ready.controller.sourceStateId; destinationStateId = $Ready.controller.destinationStateId; conditionParameterId = $Ready.controller.parameterStableId; durationTicks = 16; exitTimeTicks = 1; destinationOffsetNormalized = 0.0; priority = 44 } },
		[ordered]@{ name = 'animation.advanced.edit'; assetId = $Ready.skeleton.assetId; payload = [ordered]@{ action = 'skeleton.set_joint'; stableId = $Ready.skeleton.rootStableJointId; semantic = 'root' }; fallbackPayload = [ordered]@{ action = 'skeleton.set_joint'; stableId = $Ready.skeleton.rootStableJointId; semantic = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp_verified_root' } },
		[ordered]@{ name = 'animation.advanced.edit'; assetId = $Ready.retarget.assetId; payload = [ordered]@{ action = 'retarget.set_mapping'; stableId = $Ready.retarget.mappingStableId; sourceJointId = $Ready.retarget.sourceJointId; targetJointId = $Ready.retarget.targetJointId; required = $true }; fallbackPayload = [ordered]@{ action = 'retarget.set_mapping'; stableId = $Ready.retarget.mappingStableId; sourceJointId = $Ready.retarget.sourceJointId; targetJointId = $Ready.retarget.targetJointId; required = $false } },
		[ordered]@{ name = 'animation.advanced.edit'; assetId = $Ready.retarget.assetId; payload = [ordered]@{ action = 'retarget.set_contact'; stableId = $Ready.retarget.contactStableId; sourceJointId = $Ready.retarget.sourceJointId; targetJointId = $Ready.retarget.targetJointId; targetChainId = $Ready.retarget.targetChainId; toleranceMeters = 0.02; preservePosition = $true; preserveOrientation = $true; required = $true }; fallbackPayload = [ordered]@{ action = 'retarget.set_contact'; stableId = $Ready.retarget.contactStableId; sourceJointId = $Ready.retarget.sourceJointId; targetJointId = $Ready.retarget.targetJointId; targetChainId = $Ready.retarget.targetChainId; toleranceMeters = 0.02; preservePosition = $true; preserveOrientation = $true; required = $false } },
		# The operator authors one checked mapping/contact pair, not necessarily a
		# complete rig map. Do not manufacture a full-coverage metric here.
		[ordered]@{ name = 'animation.advanced.edit'; assetId = $Ready.retarget.assetId; payload = [ordered]@{ action = 'retarget.set_quality'; reachableToleranceMeters = 0.005; reviewReason = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp installed-editor Quaternius-to-Fox mapping and stricter reachable IK tolerance reviewed.' }; fallbackPayload = [ordered]@{ action = 'retarget.set_quality'; reachableToleranceMeters = 0.006; reviewReason = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp resumed installed-editor Quaternius-to-Fox mapping review.' } },
		# Continuity groups are topology: changing only one linked sample would
		# correctly invalidate its predecessor/successor references. Exercise the
		# independently editable query metadata while preserving that checked graph.
		[ordered]@{ name = 'animation.advanced.edit'; assetId = $Ready.motionDatabase.assetId; payload = [ordered]@{ action = 'motion.set_sample_metadata'; stableId = $Ready.motionDatabase.sampleStableId; tagMask = [UInt64]0x43; contactMask = [UInt32]3; phaseQuantized = [UInt16]10923; transitionFlags = [UInt16]1 }; fallbackPayload = [ordered]@{ action = 'motion.set_sample_metadata'; stableId = $Ready.motionDatabase.sampleStableId; tagMask = [UInt64]0x44; contactMask = [UInt32]4; phaseQuantized = [UInt16]10924; transitionFlags = [UInt16]2 } },
		[ordered]@{ name = 'animation.advanced.edit'; assetId = $Ready.ragdoll.assetId; payload = [ordered]@{ action = 'ragdoll.set_body'; stableId = $Ready.ragdoll.bodyStableId; massKilograms = 2.0; linearDamping = 0.15; angularDamping = 0.2; gravityFactor = 1.0; allowSleep = $true; startAwake = $true }; fallbackPayload = [ordered]@{ action = 'ragdoll.set_body'; stableId = $Ready.ragdoll.bodyStableId; massKilograms = 2.1; linearDamping = 0.16; angularDamping = 0.21; gravityFactor = 1.0; allowSleep = $true; startAwake = $true } },
		[ordered]@{ name = 'animation.advanced.edit'; assetId = $Ready.ragdoll.assetId; payload = [ordered]@{ action = 'ragdoll.set_blend'; stableId = $Ready.ragdoll.blendStableId; defaultWeight = 0.5; activationBlendSeconds = 0.15; recoveryBlendSeconds = 0.25; motorStrengthScale = 1.0; includeDescendants = $true }; fallbackPayload = [ordered]@{ action = 'ragdoll.set_blend'; stableId = $Ready.ragdoll.blendStableId; defaultWeight = 0.4; activationBlendSeconds = 0.16; recoveryBlendSeconds = 0.26; motorStrengthScale = 0.9; includeDescendants = $false } }
	)
	foreach ($request in $requests) {
		$arguments = [ordered]@{
			assetId = [string]$request.assetId
			payload = $request.payload
		}
		$fallbackArguments = [ordered]@{
			assetId = [string]$request.assetId
			payload = $request.fallbackPayload
		}
		$envelope = Invoke-AnimationMutation ([string]$request.name) $arguments ([ref]$Generation) $fallbackArguments
		if ([string](Get-NestedValue $envelope 'data.action') -cne [string]$request.payload.action) {
			throw "Typed authored mutation '$($request.payload.action)' did not return its exact action identity."
		}
		$resumedAfterPersistedNoOp = [bool](Get-NestedValue $envelope 'data.resumedAfterPersistedNoOp')
		$operations.Add([ordered]@{
			name = [string]$request.name
			action = [string]$request.payload.action
			assetId = [string]$request.assetId
			generation = [UInt64](Get-NestedValue $envelope 'data.generation')
			resumedAfterPersistedNoOp = $resumedAfterPersistedNoOp
			effectivePayload = if ($resumedAfterPersistedNoOp) { $request.fallbackPayload } else { $request.payload }
			data = Get-Property $envelope 'data'
		})
	}
	$validation = Invoke-McpTool 'animation.controller.validate' ([ordered]@{
		assetId = [string]$Ready.controller.assetId
		maximumDiagnostics = 64
		correlationId = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpControllerValidation'
	})
	if ([bool](Get-NestedValue $validation 'data.valid') -ne $true -or [string](Get-NestedValue $validation 'data.status') -cne 'ok') {
		throw 'The fully edited AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp controller failed checked controller validation.'
	}
	if ([UInt64](Get-NestedValue $validation 'data.generation') -ne $Generation) {
		throw 'Read-only controller validation unexpectedly changed the animation generation.'
	}
	$evidence = [ordered]@{
		schemaVersion = 1
		task = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp'
		generation = $Generation
		operations = @($operations)
		controllerValidation = Get-Property $validation 'data'
	}
	$path = Join-Path $RunRoot 'editor/typed-authoring-evidence.json'
	Write-Json $path $evidence
	return [ordered]@{ generation = $Generation; evidencePath = $path; evidenceSha256 = Get-Sha256 $path; operations = @($operations); controllerValidation = Get-Property $validation 'data' }
}

function Invoke-AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpRuntimeRecording(
	[string]$RunRoot,
	[string]$RuntimeObjectId,
	$AuthoringReady,
	[UInt64]$Generation) {
	# First prove the selected real Fox animator has a complete CPU-authoritative
	# pose and that a typed runtime seek reaches its protected publication. Manual
	# policy stops automatic time, but it intentionally does not stop publication
	# generations from advancing on editor frames.
	$jointIds = @($AuthoringReady.recording.jointStableIds)
	if ($jointIds.Count -lt 1) { throw 'The recording source has no checked stable-joint binding.' }
	$deadline = (Get-Date).AddSeconds(30)
	$readyDiagnostics = $null
	while ((Get-Date) -lt $deadline) {
		$diagnostics = Invoke-McpTool 'animation.runtime.diagnostics' ([ordered]@{
			assetId = [string]$AuthoringReady.runtime.defaultClipAssetId
			correlationId = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpRecordingInitialPublication'
			maximumItems = 64
			includeDetails = $false
		})
		$active = [int](Get-NestedValue $diagnostics 'data.summary.counters.activeAnimators')
		$evaluated = [int](Get-NestedValue $diagnostics 'data.summary.counters.evaluatedAnimators')
		$suspended = [int](Get-NestedValue $diagnostics 'data.summary.counters.suspendedAnimators')
		$degraded = [int](Get-NestedValue $diagnostics 'data.summary.counters.degradedAnimators')
		$cpuFallback = [int](Get-NestedValue $diagnostics 'data.summary.counters.cpuFallbackAnimators')
		$cpuJoints = [int](Get-NestedValue $diagnostics 'data.summary.counters.cpuSampledJoints')
		if ($active -eq 1 -and $evaluated -eq 1 -and $suspended -eq 1 -and
			$degraded -eq 0 -and $cpuFallback -eq 1 -and $cpuJoints -eq $jointIds.Count) {
			$readyDiagnostics = $diagnostics
			break
		}
		Start-Sleep -Milliseconds 250
	}
	if ($null -eq $readyDiagnostics) { throw 'The Manual-policy Fox animator did not publish its initial immutable pose.' }
	$initialFixedTick = [Int64](Get-NestedValue $readyDiagnostics 'data.summary.fixedTick')
	$targetTick = $initialFixedTick + 8
	$seek = Invoke-AnimationMutation 'animation.runtime.parameters' ([ordered]@{
		target = [ordered]@{ objectId = $RuntimeObjectId }
		commands = @([ordered]@{ kind = 'seek'; seekTick = 1 })
		targetTick = $targetTick
	}) ([ref]$Generation)
	if ([string](Get-NestedValue $seek 'data.status') -cnotin @('queued', 'ok')) {
		throw 'The deterministic Manual-policy recording seek was not queued.'
	}

	$deadline = (Get-Date).AddSeconds(30)
	$settledDiagnostics = $null
	while ((Get-Date) -lt $deadline) {
		$diagnostics = Invoke-McpTool 'animation.runtime.diagnostics' ([ordered]@{
			assetId = [string]$AuthoringReady.runtime.defaultClipAssetId
			correlationId = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpRecordingSeekSettle'
			maximumItems = 64
			includeDetails = $false
		})
		$fixedTick = [Int64](Get-NestedValue $diagnostics 'data.summary.fixedTick')
		$queueDepth = [UInt64](Get-NestedValue $diagnostics 'data.summary.counters.commandQueueDepth')
		$active = [int](Get-NestedValue $diagnostics 'data.summary.counters.activeAnimators')
		$evaluated = [int](Get-NestedValue $diagnostics 'data.summary.counters.evaluatedAnimators')
		$suspended = [int](Get-NestedValue $diagnostics 'data.summary.counters.suspendedAnimators')
		$degraded = [int](Get-NestedValue $diagnostics 'data.summary.counters.degradedAnimators')
		$cpuFallback = [int](Get-NestedValue $diagnostics 'data.summary.counters.cpuFallbackAnimators')
		$cpuJoints = [int](Get-NestedValue $diagnostics 'data.summary.counters.cpuSampledJoints')
		if ($fixedTick -ge $targetTick -and $queueDepth -eq 0 -and
			$active -eq 1 -and $evaluated -eq 1 -and $suspended -eq 1 -and
			$degraded -eq 0 -and $cpuFallback -eq 1 -and $cpuJoints -eq $jointIds.Count) {
			$settledDiagnostics = $diagnostics
			break
		}
		Start-Sleep -Milliseconds 250
	}
	if ($null -eq $settledDiagnostics) { throw 'The deterministic recording seek did not reach its protected animation publication boundary.' }

	$recordingAssetId = [string]$AuthoringReady.recording.assetId
	$correlation = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpRealModelRecording'
	$recordingEnvelope = Invoke-McpToolEnvelope 'animation.recording.start' ([ordered]@{
		target = [ordered]@{ objectId = $RuntimeObjectId }
		assetId = $recordingAssetId
		skeletonAssetId = [string]$AuthoringReady.runtime.skeletonAssetId
		sourceAnimatorId = $RuntimeObjectId
		compatibilityFingerprint = [UInt64]$AuthoringReady.recording.compatibilityFingerprint
		# The high-impact action may wait in the approval UI while pose generations
		# keep advancing. Zero is accepted only with this explicit mode: approved
		# execution must atomically retain the latest immutable snapshot, derive its
		# generation/tick, and return those exact values as durable job evidence.
		bindCurrentPublication = $true
		publicationGeneration = [UInt32]0
		jointStableIds = $jointIds
		destinationPath = 'Validation/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpFoxRecording.teanim'
		# Bind-current replaces this schema-compatible placeholder range with the
		# retained snapshot's [max(0,tick-1), tick] one-shot range.
		firstTick = 0
		lastTick = 1
		sampleRate = 60
		clock = 'manual'
		channels = @([ordered]@{
			stableChannelId = [UInt64]430281
			stableTargetId = [UInt64]$jointIds[0]
			name = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp Root Translation'
			kind = 'bone_translation'
			interpolation = 'linear'
		})
		sourceSceneIdentity = 'Assets/Scenes/ValidationModels.tescene'
		sourceSessionIdentity = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpInstalledEditorRealModel'
		samplesPerChunk = 1
		maximumCapturedSamples = 2
		maximumStagedBytes = 16777216
		segmentDurationTicks = 1
		idempotencyKey = $correlation
		correlationId = $correlation
		expectedGeneration = $Generation
	})
	$approval = Wait-ForApprovedAnimationJob $recordingEnvelope 'animation.recording.start' $correlation $RunRoot 'animation.recording.finalize'
	$Generation++
	$recordingResult = Get-Property $approval 'jobResult'
	$boundGeneration = [UInt64](Get-Property $recordingResult 'boundPublicationGeneration')
	$boundTick = [Int64](Get-Property $recordingResult 'boundTick')
	$boundJointIds = @((Get-Property $recordingResult 'boundJointStableIds'))
	if ($boundGeneration -lt 1 -or $boundTick -lt $targetTick -or
		$boundJointIds.Count -ne $jointIds.Count -or
		(($boundJointIds | ForEach-Object { [string][UInt64]$_ }) -join ',') -cne
			(($jointIds | ForEach-Object { [string][UInt64]$_ }) -join ',') -or
		[bool](Get-Property $recordingResult 'published') -ne $true -or
		[bool](Get-Property $recordingResult 'candidateValidated') -ne $true -or
		[UInt64](Get-Property $recordingResult 'capturedSamples') -lt 1) {
		throw 'The approved recording job omitted or contradicted its exact bind-current publication, tick, joint, or publication evidence.'
	}
	$destination = Join-Path $RunRoot 'project/Assets/Validation/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpFoxRecording.teanim'
	if (-not (Test-Path -LiteralPath $destination -PathType Leaf) -or (Get-Item -LiteralPath $destination).Length -eq 0) {
		throw 'The approved recording job did not atomically publish Validation/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpFoxRecording.teanim.'
	}
	$imported = Invoke-McpTool 'asset.import' ([ordered]@{ path = 'Validation/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpFoxRecording.teanim'; force = $true })
	$importStatus = [string](Get-NestedValue $imported 'data.status')
	if ($importStatus -cnotin @('queued', 'imported')) {
		throw "The published recording did not enter the generic asset import pipeline; status was '$importStatus'."
	}
	# Non-package assets correctly return an asynchronous "queued" acknowledgement
	# rather than the model package's synchronous importResult sidecar. Wait until
	# the internal compiler has atomically copied the exact recorded bytes before
	# accepting registry inspection or opening the clip in the preview decoder.
	$compiledRecording = Join-Path $RunRoot 'project/Build/Assets/Validation/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpFoxRecording.teanim'
	$sourceHash = Get-Sha256 $destination
	$deadline = (Get-Date).AddSeconds(30)
	while ((Get-Date) -lt $deadline) {
		if ((Test-Path -LiteralPath $compiledRecording -PathType Leaf) -and
			(Get-Sha256 $compiledRecording) -ceq $sourceHash) {
			break
		}
		Start-Sleep -Milliseconds 100
	}
	if (-not (Test-Path -LiteralPath $compiledRecording -PathType Leaf) -or
		(Get-Sha256 $compiledRecording) -cne $sourceHash) {
		throw 'The queued recording import did not atomically publish identical compiled clip bytes.'
	}
	$listed = Invoke-McpTool 'asset.list' ([ordered]@{ search = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpFoxRecording.teanim' })
	$matches = @((Get-NestedValue $listed 'data.assets') | Where-Object { [string]$_.path -ceq 'Validation/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpFoxRecording.teanim' })
	if ($matches.Count -ne 1 -or [string](Get-Property $matches[0] 'guid') -cne $recordingAssetId) {
		throw 'The published recording was not uniquely registered under its deterministic asset ID.'
	}
	$inspection = Invoke-McpTool 'animation.asset.inspect' ([ordered]@{
		assetId = $recordingAssetId
		maximumItems = 64
		correlationId = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpRecordingPublishedInspection'
	})
	if ([string](Get-NestedValue $inspection 'data.registeredAssetId') -cne $recordingAssetId -or
		[string](Get-NestedValue $inspection 'data.importer') -cne 'AnimationClipAssetImporter' -or
		([string](Get-NestedValue $inspection 'data.sourcePath')).Replace('\', '/') -cne 'Validation/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp/AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpFoxRecording.teanim' -or
		[UInt64](Get-NestedValue $inspection 'data.generation') -ne $Generation) {
		throw 'The published recording did not retain its checked clip identity, importer, path, and animation generation.'
	}
	# Reopen the recorder output through the production clip decoder and isolated
	# preview world while Play is still active. This proves the new file evaluates
	# real joint data and that preview transport does not need to touch the Play
	# world in order to seek, reverse, tick, and close the recorded clip.
	$recordedPreview = Invoke-AnimationPreviewMatrix @([ordered]@{
		id = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpRecordedFox'
		subassets = @([ordered]@{
			kind = 'animation_clip'
			assetId = $recordingAssetId
			displayName = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpFoxRecording.teanim'
			publishedGeneration = $Generation
		})
	}) $Generation
	$Generation = [UInt64]$recordedPreview.generation
	return [ordered]@{
		generation = $Generation
		approval = $approval
		assetId = $recordingAssetId
		path = $destination
		sha256 = Get-Sha256 $destination
		bytes = (Get-Item -LiteralPath $destination).Length
		targetTick = $targetTick
		publicationGeneration = $boundGeneration
		boundTick = $boundTick
		bindCurrentPublication = $true
		boundJointStableIds = $boundJointIds
		jointCount = $jointIds.Count
		seek = Get-Property $seek 'data'
		initialDiagnostics = [ordered]@{
			summary = Get-NestedValue $readyDiagnostics 'data.summary'
		}
		settledDiagnostics = [ordered]@{
			summary = Get-NestedValue $settledDiagnostics 'data.summary'
		}
		import = Get-Property $imported 'data'
		inspection = Get-Property $inspection 'data'
		preview = @($recordedPreview.models)[0]
	}
}

function Initialize-ValidationProject([string]$ProjectRoot, [string]$CachePath, [object[]]$Models) {
	# A real project identity, startup scene, and saved build profile are required by
	# export.validate/export.start. Keep the gameplay project minimal, but retain
	# managed compilation because the shipped launcher fail-closes unless its
	# NativeAOT ABI is present beside the TEPAK runtime.
	foreach ($folder in @('Assets/Models', 'Assets/Scenes', 'BuildProfiles', 'ProjectSettings', 'Scenes')) {
		[System.IO.Directory]::CreateDirectory((Join-Path $ProjectRoot $folder)) | Out-Null
	}
	$projectManifest = [ordered]@{
		name = 'Animation Validation Models'
		version = '1.0.0'
		startupScene = 'Scenes/ValidationModels.tescene'
	}
	Write-Json (Join-Path $ProjectRoot 'project.json') $projectManifest
	Write-Json (Join-Path $ProjectRoot 'game.json') $projectManifest
	$sceneDocument = [ordered]@{
		name = 'Animation Validation Models'
		sceneSettings = [ordered]@{ renderSettings = [ordered]@{ ambientLight = @(0.08, 0.09, 0.11); exposure = 1.0 } }
		objects = @([ordered]@{
			uuid = '43000000000000000000000000000430'
			name = 'Validation Capture Camera'
			components = [ordered]@{
				# The Khronos Fox geometry is authored in a large source-space
				# range and the runtime fixture scales it to meter-like scene size.
				# Center the camera at the scaled character height so Game View and
				# authored-camera captures frame the model instead of its feet.
				Transform = [ordered]@{ Position = @(0.0, 0.4, 4.0); Rotation = @(1.0, 0.0, 0.0, 0.0); Scale = @(1.0, 1.0, 1.0) }
				CameraComponent = [ordered]@{ verticalFovDegrees = 60.0; aspectRatio = 1.7777777777777777; nearPlane = 0.1; farPlane = 100.0; up = @(0.0, 1.0, 0.0) }
			}
		})
	}
	# Export resolves the project-level startup scene, while editor automation
	# resolves an asset-relative scene. Keep identical documents in both declared
	# locations so neither surface falls back to an implicit or empty scene.
	Write-Json (Join-Path $ProjectRoot 'Scenes/ValidationModels.tescene') $sceneDocument
	Write-Json (Join-Path $ProjectRoot 'Assets/Scenes/ValidationModels.tescene') $sceneDocument
	Write-Json (Join-Path $ProjectRoot 'BuildProfiles/validation-models.json') ([ordered]@{
		name = 'Validation Models Windows'
		id = 'validation-models-windows'
		platform = 'windows'
		outputDirectory = 'Builds/ValidationModels'
		startupScene = 'Scenes/ValidationModels.tescene'
		compressionPreset = 'normal'
		tepakWorkerCount = 0
		enableManagedCompilation = $true
		includeDebugSymbols = $true
		stripNativeAotOutput = $false
		enableVulkanValidationLayers = $true
		enableRenderDebugDiagnostics = $true
		includedScenes = @()
	})
	Write-Json (Join-Path $ProjectRoot 'ProjectSettings/ProjectSettings.terra') ([ordered]@{ schemaVersion = 1; projectName = 'Animation Validation Models' })
	Write-Json (Join-Path $ProjectRoot 'ProjectSettings/RenderSettings.json') ([ordered]@{ schemaVersion = 1; renderer = 'vulkan' })

	$inputs = @()
	$modelIndex = 0
	foreach ($model in $Models) {
		# Validation IDs are intentionally descriptive and can make otherwise valid
		# compiler staging paths exceed legacy Windows MAX_PATH. Use a compact,
		# deterministic project folder while preserving the full ID in evidence.
		$modelFolder = 'm{0:D2}' -f $modelIndex
		$modelIndex++
		$files = @((Get-Property $model 'files'))
		foreach ($file in $files) {
			$relativePayload = [string](Get-Property $file 'path')
			$source = Join-Path (Join-Path (Join-Path $CachePath ([string]$model.id)) 'payload') $relativePayload
			$assetRelative = (Join-Path (Join-Path 'Models' $modelFolder) $relativePayload).Replace('\', '/')
			$destination = Join-Path (Join-Path $ProjectRoot 'Assets') $assetRelative
			[System.IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
			Copy-Item -LiteralPath $source -Destination $destination -Force
			$inputs += [ordered]@{ modelId = [string]$model.id; assetRelative = $assetRelative; source = $source; destination = $destination }
		}
	}
	return $inputs
}

function Wait-ForEditorMainWindow([System.Diagnostics.Process]$Editor, [int]$TimeoutSeconds) {
	$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
	while ((Get-Date) -lt $deadline) {
		$Editor.Refresh()
		if ($Editor.HasExited) { throw 'Installed editor exited before its native main window became available.' }
		if ($Editor.MainWindowHandle -ne [IntPtr]::Zero -and -not [string]::IsNullOrWhiteSpace([string]$Editor.MainWindowTitle)) {
			return [ordered]@{
				processId = [int]$Editor.Id
				windowHandle = ('0x{0:x}' -f [UInt64]$Editor.MainWindowHandle.ToInt64())
				windowTitle = [string]$Editor.MainWindowTitle
			}
		}
		Start-Sleep -Milliseconds 250
	}
	throw "Installed editor did not expose a native main window within $TimeoutSeconds seconds."
}

function Write-AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpManualChecklist([string]$RunRoot, [object[]]$Models, $NativeWindow) {
	$items = [System.Collections.Generic.List[object]]::new()
	foreach ($global in @(
		[ordered]@{ id = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpAuthorControllerTransition'; input = 'mouse'; instruction = 'In Animation Controller Editor, show the exact Fox-bound controller, two distinct clip states, Boolean condition, deterministic transition, and root-motion policy/result controls.' },
		[ordered]@{ id = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpAuthorSkeletonRetargetIk'; input = 'mouse'; instruction = 'In Skeleton and Retarget editors, show the duplicated Fox skeleton stable IDs plus the Quaternius-to-Fox mapping, reachable IK/contact rule, and accepted quality result.' },
		[ordered]@{ id = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpAuthorMotionMatch'; input = 'mouse'; instruction = 'In Motion Matching Database Editor, show the real Fox clip sources, built sample inventory, metadata, and a successful query/result view.' },
		[ordered]@{ id = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpAuthorRagdoll'; input = 'mouse'; instruction = 'In Ragdoll Editor, show the Fox skeleton binding, generated body/collider records, joint blend, activate/recover controls, and stable visual result.' },
		[ordered]@{ id = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpRecordingAndClipReview'; input = 'mouse'; instruction = 'Approve the typed recording request, then open the published AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpFoxRecording.teanim in Animation Clip Editor and show its Recording provenance/channels.' },
		[ordered]@{ id = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpAnimationWindowsControls'; input = 'mouse'; instruction = 'Open and exercise Animation Clip, Skeleton, Controller, Blend Tree, State Machine, Transition, Layer/Mask, Retarget, Motion Matching, Ragdoll, and Diagnostics windows using their real controls.' },
		[ordered]@{ id = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpPlaySceneGameCameraCullingShadows'; input = 'mouse'; instruction = 'In Play Mode, inspect Scene View, Game View, authored camera, and custom camera while toggling/observing animation culling, visibility return, deformation, lighting, and shadows.' },
		[ordered]@{ id = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpExportSourceFreeRuntimeReview'; input = 'keyboard'; instruction = 'Use the installed editor export approval/controls, then inspect the completed source-free rendered/headless parity artifacts and the non-vacuous Vulkan capture.' },
		[ordered]@{ id = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpPlayRestoration'; input = 'keyboard'; instruction = 'After the full matrix, leave Play Mode stopped and verify authored scene/editor state was restored without preview leakage.' }
	)) {
		$global['required'] = $true
		$items.Add($global)
	}
	foreach ($model in @($Models)) {
		foreach ($scenario in @((Get-Property $model 'validationScenarios'))) {
			$items.Add([ordered]@{
				id = "$([string]$model.id)::$([string]$scenario)"
				input = 'mouse'
				required = $true
				modelId = [string]$model.id
				scenario = [string]$scenario
				instruction = "Use the installed editor to perform exact real-model scenario '$scenario' for '$($model.id)'. Show the named source/package identity and an action-bound visual result; expected features: $(@($model.expectedFeatures) -join ', ')."
			})
		}
	}
	$checklist = [ordered]@{
		schemaVersion = 3
		task = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp'
		instruction = 'Use real human or Win32SendInput mouse/keyboard input in this installed editor. Append one clicks.jsonl object for every exact item ID. Each passing action requires non-empty observed text and at least one distinct, newly captured native-window-screenshot PNG bound by actionId/actionSequence. PrintWindow process/window metadata and strictly increasing capture timestamps are mandatory. Only after every machine and UI action is complete write {"completed":true,"defects":[]} to manual-complete.json.'
		nativeWindow = $NativeWindow
		items = @($items)
	}
	$path = Join-Path $RunRoot 'editor/manual-checklist.json'
	Write-Json $path $checklist
	return $checklist
}

function Read-AndValidateAnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpManualEvidence(
	[string]$RunRoot,
	$Checklist,
	[System.Diagnostics.Process]$Editor) {
	$completionPath = Join-Path $RunRoot 'editor/manual-complete.json'
	$clickPath = Join-Path $RunRoot 'editor/clicks.jsonl'
	if (-not (Test-Path -LiteralPath $completionPath -PathType Leaf)) { throw 'Required manual-complete.json is missing.' }
	$completionText = Get-Content -Raw -LiteralPath $completionPath
	$completionStrict = ConvertFrom-JsonPreservingArrays $completionText 'manual-complete.json'
	try { $completion = $completionText | ConvertFrom-Json } catch { throw "manual-complete.json is invalid JSON: $($_.Exception.Message)" }
	if ($completionStrict -isnot [System.Collections.IDictionary] -or $completion -is [System.Array]) { throw 'manual-complete.json must contain one JSON object.' }
	$completed = Get-RequiredPreservedJsonMember $completionStrict 'completed' 'manual-complete.json'
	if ($completed -isnot [bool] -or -not $completed) { throw 'manual-complete.json completed must be the JSON Boolean true.' }
	$defectsStrict = Get-RequiredPreservedJsonMember $completionStrict 'defects' 'manual-complete.json'
	if ($defectsStrict -isnot [System.Array]) { throw 'manual-complete.json defects must be a JSON array.' }
	$defects = @((Get-Property $completion 'defects'))
	foreach ($defect in $defects) {
		if ($null -eq $defect -or $defect -is [string] -or $defect -is [System.Array]) { throw 'Every manual defect must be a JSON object.' }
		$defectId = [string](Get-RequiredJsonProperty $defect 'id' 'Manual defect')
		$observed = [string](Get-RequiredJsonProperty $defect 'observed' "Manual defect '$defectId'")
		$status = [string](Get-RequiredJsonProperty $defect 'status' "Manual defect '$defectId'")
		if ([string]::IsNullOrWhiteSpace($defectId) -or [string]::IsNullOrWhiteSpace($observed) -or $status -cne 'fixed') {
			throw 'Every reported manual defect must have non-empty id/observed fields and exact status fixed.'
		}
	}

	if (-not (Test-Path -LiteralPath $clickPath -PathType Leaf)) { throw 'Required editor/clicks.jsonl transcript is missing.' }
	$declaredItems = @((Get-Property $Checklist 'items'))
	$declaredIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
	foreach ($declared in $declaredItems) {
		$id = [string](Get-Property $declared 'id')
		if ([string]::IsNullOrWhiteSpace($id) -or -not $declaredIds.Add($id)) { throw "Manual checklist contains an empty or duplicate ID '$id'." }
	}
	$recordIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
	$screenshotPaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
	$screenshotHashes = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
	$pixelHashes = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
	$records = [System.Collections.Generic.List[object]]::new()
	$lastScreenshotTime = [DateTimeOffset]::MinValue
	$expectedSequence = 1
	$lineNumber = 0
	$Editor.Refresh()
	if ($Editor.HasExited) { throw 'Installed editor exited before manual evidence validation.' }
	foreach ($line in @(Get-Content -LiteralPath $clickPath)) {
		$lineNumber++
		if ([string]::IsNullOrWhiteSpace($line)) { continue }
		$recordStrict = ConvertFrom-JsonPreservingArrays $line "clicks.jsonl line $lineNumber"
		try { $record = $line | ConvertFrom-Json } catch { throw "clicks.jsonl line $lineNumber is invalid JSON." }
		if ($recordStrict -isnot [System.Collections.IDictionary] -or $record -is [System.Array]) { throw "clicks.jsonl line $lineNumber must contain one JSON object." }
		$id = [string](Get-RequiredJsonProperty $record 'id' "clicks.jsonl line $lineNumber")
		if (-not $declaredIds.Contains($id)) { throw "clicks.jsonl line $lineNumber uses undeclared checklist ID '$id'." }
		if (-not $recordIds.Add($id)) { throw "clicks.jsonl repeats checklist ID '$id'." }
		$declared = @($declaredItems | Where-Object { [string]$_.id -ceq $id })[0]
		$input = [string](Get-RequiredJsonProperty $record 'input' "Click record '$id'")
		if ($input -cne [string]$declared.input) { throw "Click record '$id' input does not match its exact checklist declaration." }
		$delivery = [string](Get-RequiredJsonProperty $record 'inputDelivery' "Click record '$id'")
		if ($delivery -cnotin @('human', 'Win32SendInput')) { throw "Click record '$id' inputDelivery must be human or Win32SendInput." }
		$passed = Get-RequiredPreservedJsonMember $recordStrict 'passed' "Click record '$id'"
		if ($passed -isnot [bool] -or -not $passed) { throw "Click record '$id' passed must be the JSON Boolean true." }
		$observed = [string](Get-RequiredJsonProperty $record 'observed' "Click record '$id'")
		if ([string]::IsNullOrWhiteSpace($observed)) { throw "Click record '$id' requires a non-empty observed result." }
		$evidenceStrict = Get-RequiredPreservedJsonMember $recordStrict 'evidence' "Click record '$id'"
		if ($evidenceStrict -isnot [System.Array] -or $evidenceStrict.Count -lt 1) { throw "Click record '$id' evidence must be a non-empty JSON array." }
		$evidence = @((Get-RequiredJsonProperty $record 'evidence' "Click record '$id'"))
		$normalizedEvidence = [System.Collections.Generic.List[object]]::new()
		foreach ($artifactRecord in $evidence) {
			if ($null -eq $artifactRecord -or $artifactRecord -is [string] -or $artifactRecord -is [System.Array]) { throw "Click record '$id' evidence entries must be objects." }
			$kind = [string](Get-RequiredJsonProperty $artifactRecord 'kind' "Click record '$id' evidence")
			if ($kind -cne 'native-window-screenshot') { throw "Click record '$id' has unsupported evidence kind '$kind'." }
			$actionId = [string](Get-RequiredJsonProperty $artifactRecord 'actionId' "Click record '$id' screenshot")
			if ($actionId -cne $id) { throw "Click record '$id' screenshot actionId is not bound to that exact action." }
			$sequence = Get-RequiredJsonProperty $artifactRecord 'actionSequence' "Click record '$id' screenshot"
			if ($sequence -isnot [int] -or [int]$sequence -ne $expectedSequence) { throw "Click record '$id' screenshot actionSequence must be '$expectedSequence'." }
			$expectedSequence++
			$relativePath = [string](Get-RequiredJsonProperty $artifactRecord 'path' "Click record '$id' screenshot")
			if ([string]::IsNullOrWhiteSpace($relativePath) -or [System.IO.Path]::IsPathRooted($relativePath) -or $relativePath -match '[:*?]' -or
				@($relativePath -split '[\\/]' | Where-Object { $_ -in @('', '.', '..') }).Count -gt 0 -or
				[System.IO.Path]::GetExtension($relativePath) -cne '.png') {
				throw "Click record '$id' screenshot path must be one safe run-root-relative .png path."
			}
			$normalizedPath = $relativePath.Replace('\', '/')
			if (-not $screenshotPaths.Add($normalizedPath)) { throw "Manual screenshot '$relativePath' is reused across actions." }
			$absolutePath = Get-AbsolutePath $relativePath $RunRoot
			Assert-AtOrBelow $RunRoot $absolutePath 'Manual screenshot evidence'
			if (-not (Test-Path -LiteralPath $absolutePath -PathType Leaf) -or (Get-Item -LiteralPath $absolutePath).Length -eq 0) { throw "Manual screenshot is missing or empty: '$relativePath'." }

			$window = Get-RequiredJsonProperty $artifactRecord 'nativeWindow' "Click record '$id' screenshot"
			if ($null -eq $window -or $window -is [string] -or $window -is [System.Array]) { throw "Click record '$id' nativeWindow must be a JSON object." }
			$processId = Get-RequiredJsonProperty $window 'processId' "Click record '$id' nativeWindow"
			if ($processId -isnot [int] -or [int]$processId -ne [int]$Editor.Id) { throw "Click record '$id' screenshot processId does not match the launched editor." }
			$expectedHandle = '0x{0:x}' -f [UInt64]$Editor.MainWindowHandle.ToInt64()
			if ([string](Get-RequiredJsonProperty $window 'windowHandle' "Click record '$id' nativeWindow") -cne $expectedHandle) { throw "Click record '$id' screenshot windowHandle does not match the launched editor." }
			if ([string](Get-RequiredJsonProperty $window 'windowTitle' "Click record '$id' nativeWindow") -cne [string]$Editor.MainWindowTitle) { throw "Click record '$id' screenshot windowTitle does not match the launched editor." }
			if ([string](Get-RequiredJsonProperty $window 'captureMethod' "Click record '$id' nativeWindow") -cne 'PrintWindow') { throw "Click record '$id' captureMethod must be exactly PrintWindow." }
			$dpiAware = Get-RequiredJsonProperty $window 'dpiAware' "Click record '$id' nativeWindow"
			if ($dpiAware -isnot [bool] -or -not $dpiAware) { throw "Click record '$id' dpiAware must be the JSON Boolean true." }
			$capturedAtText = [string](Get-RequiredJsonProperty $window 'capturedAtUtc' "Click record '$id' nativeWindow")
			$capturedAt = [DateTimeOffset]::MinValue
			if (-not [DateTimeOffset]::TryParse($capturedAtText, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind, [ref]$capturedAt)) { throw "Click record '$id' capturedAtUtc is not a round-trip timestamp." }
			$processStartedAt = [DateTimeOffset]::new(([DateTime]$Editor.StartTime).ToUniversalTime())
			if ($capturedAt.ToUniversalTime() -lt $processStartedAt -or $capturedAt.ToUniversalTime() -gt [DateTimeOffset]::UtcNow.AddSeconds(5)) { throw "Click record '$id' timestamp is outside the editor lifetime." }
			if ($capturedAt.ToUniversalTime() -le $lastScreenshotTime) { throw "Click record '$id' timestamp must be strictly later than the prior capture." }
			$lastScreenshotTime = $capturedAt.ToUniversalTime()

			$bytes = [System.IO.File]::ReadAllBytes($absolutePath)
			$statistics = Get-PngVisualStatistics $bytes
			$width = Get-RequiredJsonProperty $window 'pixelWidth' "Click record '$id' nativeWindow"
			$height = Get-RequiredJsonProperty $window 'pixelHeight' "Click record '$id' nativeWindow"
			if ($width -isnot [int] -or $height -isnot [int] -or [int]$width -ne [int]$statistics.width -or [int]$height -ne [int]$statistics.height) {
				throw "Click record '$id' declared dimensions do not match its PNG."
			}
			if ([UInt64]$statistics.visibleNonBlackPixelCount -eq 0 -or [bool]$statistics.isRgbaUniform) { throw "Click record '$id' screenshot is black, invisible, or uniform." }
			$fileHash = Get-Sha256 $absolutePath
			if (-not $screenshotHashes.Add($fileHash) -or -not $pixelHashes.Add([string]$statistics.pixelSha256)) { throw "Click record '$id' reuses screenshot bytes or identical decoded pixels." }
			$normalizedEvidence.Add([ordered]@{
				path = $normalizedPath
				kind = $kind
				actionId = $actionId
				actionSequence = [int]$sequence
				sha256 = $fileHash
				nativeWindow = $window
				visualStatistics = $statistics
			})
		}
		$records.Add([ordered]@{ id = $id; input = $input; inputDelivery = $delivery; passed = $true; observed = $observed; evidence = @($normalizedEvidence) })
	}
	foreach ($requiredId in $declaredIds) {
		if (-not $recordIds.Contains($requiredId)) { throw "Manual checklist item '$requiredId' lacks exactly one passing action record." }
	}
	if ($recordIds.Count -ne $declaredIds.Count) { throw 'Manual transcript action set does not exactly match the AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp checklist.' }
	return [ordered]@{
		schemaVersion = 2
		completed = $true
		recordCount = $records.Count
		screenshotCount = $expectedSequence - 1
		records = @($records)
		defects = @($defects)
		editor = [ordered]@{ processId = [int]$Editor.Id; windowHandle = ('0x{0:x}' -f [UInt64]$Editor.MainWindowHandle.ToInt64()); windowTitle = [string]$Editor.MainWindowTitle }
	}
}

function Wait-ForManualEvidence([string]$RunRoot, $Checklist, [System.Diagnostics.Process]$Editor) {
	$proofPath = Join-Path $RunRoot 'editor/manual-complete.json'
	$deadline = (Get-Date).AddSeconds($ManualEvidenceTimeoutSeconds)
	Write-Host "Complete every exact action in editor/manual-checklist.json and append its proof to editor/clicks.jsonl."
	Write-Host "Then write {`"completed`":true,`"defects`":[]} to: $proofPath"
	while ((Get-Date) -lt $deadline) {
		$Editor.Refresh()
		if ($Editor.HasExited) { throw "Installed editor exited before manual proof with code $($Editor.ExitCode)." }
		if (Test-Path -LiteralPath $proofPath -PathType Leaf) {
			try {
				$candidate = Get-Content -Raw -LiteralPath $proofPath | ConvertFrom-Json
				$completed = Get-Property $candidate 'completed'
				if ($completed -is [bool] -and $completed) { break }
			}
			catch { }
		}
		Start-Sleep -Milliseconds 500
	}
	if (-not (Test-Path -LiteralPath $proofPath -PathType Leaf)) { throw "Timed out after $ManualEvidenceTimeoutSeconds seconds waiting for real installed-editor manual proof." }
	$validated = Read-AndValidateAnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpManualEvidence $RunRoot $Checklist $Editor
	$validatedPath = Join-Path $RunRoot 'editor/manual-evidence.json'
	Write-Json $validatedPath $validated
	$validated['evidencePath'] = $validatedPath
	$validated['evidenceSha256'] = Get-Sha256 $validatedPath
	return $validated
}

function Publish-AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntimeRuntimeEvidence(
	[string]$RunRoot,
	[string]$ExportRuntimeResultsPath,
	$MachineEvidence) {
	# AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntime has an independent evidence location from the broader AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp editor
	# matrix. Copy only the approved export and source-free runtime proof after
	# both parity lanes pass; the disposable source project is intentionally not
	# duplicated into this evidence tree.
	$runId = Split-Path -Leaf $RunRoot
	$AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntimeRoot = Join-Path $ExportRuntimeResultsPath $runId
	Assert-AtOrBelow $ExportRuntimeResultsPath $AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntimeRoot 'AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntime export-runtime evidence root'
	if (Test-Path -LiteralPath $AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntimeRoot) {
		# Manual AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp proof can be retried after this independently published AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntime
		# lane has already passed. Reuse only an exact, readable success record for
		# this same run root; any partial, foreign, or malformed directory still
		# fails closed instead of being silently overwritten.
		$existingSummaryPath = Join-Path $AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntimeRoot 'result.json'
		if (-not (Test-Path -LiteralPath $existingSummaryPath -PathType Leaf)) {
			throw "Existing AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntime export-runtime evidence for run '$runId' has no result.json."
		}
		try {
			$existingSummary = Get-Content -Raw -LiteralPath $existingSummaryPath | ConvertFrom-Json
		} catch {
			throw "Existing AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntime export-runtime evidence for run '$runId' has invalid result.json: $($_.Exception.Message)"
		}
		$expectedRunRoot = [System.IO.Path]::GetFullPath($RunRoot).TrimEnd('\', '/')
		$recordedRunRoot = [System.IO.Path]::GetFullPath([string](Get-Property $existingSummary 'sourceAnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpRunRoot')).TrimEnd('\', '/')
		if ([int](Get-Property $existingSummary 'schemaVersion') -ne 1 -or
			[string](Get-Property $existingSummary 'task') -cne 'AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntime' -or
			[string](Get-Property $existingSummary 'status') -cne 'machine-parity-passed' -or
			-not $recordedRunRoot.Equals($expectedRunRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
			throw "Existing AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntime export-runtime evidence for run '$runId' is not an exact successful record for this run."
		}
		return [ordered]@{ root = $AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntimeRoot; result = $existingSummaryPath; resultSha256 = Get-Sha256 $existingSummaryPath }
	}
	[System.IO.Directory]::CreateDirectory($AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntimeRoot) | Out-Null
	foreach ($relativeDirectory in @('export', 'runtime')) {
		$source = Join-Path $RunRoot $relativeDirectory
		if (-not (Test-Path -LiteralPath $source -PathType Container)) { throw "AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntime source evidence directory '$relativeDirectory' is missing." }
		Copy-Item -LiteralPath $source -Destination $AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntimeRoot -Recurse
	}
	$editorDestination = Join-Path $AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntimeRoot 'editor'
	[System.IO.Directory]::CreateDirectory($editorDestination) | Out-Null
	foreach ($fileName in @('mcp.jsonl', 'approval-requests.jsonl')) {
		$source = Join-Path (Join-Path $RunRoot 'editor') $fileName
		if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntime required editor evidence '$fileName' is missing." }
		Copy-Item -LiteralPath $source -Destination (Join-Path $editorDestination $fileName)
	}
	$summaryPath = Join-Path $AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntimeRoot 'result.json'
	Write-Json $summaryPath ([ordered]@{
		schemaVersion = 1
		task = 'AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntime'
		feature = 'complete-animation-system'
		status = 'machine-parity-passed'
		failClosed = $true
		sourceAnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpRunRoot = $RunRoot
		manualAnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpGate = 'pending_or_separately_recorded'
		machineEvidence = $MachineEvidence
		reason = 'Approved acquired-model TEPAK export, isolated source-free bootstrap, rendered Vulkan capture, and 120-tick rendered/headless authoritative parity passed. This record does not claim the separate real-click AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp matrix passed.'
	})
	return [ordered]@{ root = $AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntimeRoot; result = $summaryPath; resultSha256 = Get-Sha256 $summaryPath }
}

$repoRoot = (Get-Location).Path; $manifestPath = Get-AbsolutePath $Manifest $repoRoot; $cachePath = Get-AbsolutePath $CacheRoot $repoRoot; $installPath = Get-AbsolutePath $InstallRoot $repoRoot; $resultsPath = Get-AbsolutePath $ResultsRoot $repoRoot; $exportRuntimeResultsPath = Get-AbsolutePath $ExportRuntimeResultsRoot $repoRoot
$result = [ordered]@{ schemaVersion = 3; tool = 'TestValidationModels'; tasks = @('AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp', 'AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntime'); feature = 'complete-animation-system'; status = 'failed'; failClosed = $true; runRoot = ''; models = @(); reason = '' }
$editor = $null
try {
	Assert-AtOrBelow (Join-Path $repoRoot 'out') $cachePath 'Validation model cache root'
	Assert-AtOrBelow (Join-Path $repoRoot 'out') $resultsPath 'Validation model results root'
	Assert-AtOrBelow (Join-Path $repoRoot 'out') $exportRuntimeResultsPath 'AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntime export-runtime results root'
	if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "Validation-model manifest is missing: '$manifestPath'." }
	$manifestText = [System.IO.File]::ReadAllText($manifestPath, $script:Utf8NoBom)
	$models = Assert-Manifest ($manifestText | ConvertFrom-Json)
	$modelProvenance = [System.Collections.Generic.List[object]]::new()
	foreach ($model in $models) {
		$entry = Join-Path $cachePath ([string]$model.id)
		Assert-AtOrBelow $cachePath $entry 'Model cache entry'
		$provenance = Join-Path $entry 'provenance.json'
		if (-not (Test-Path -LiteralPath $provenance -PathType Leaf)) { throw "Verified provenance is missing for '$($model.id)'." }
		$record = Get-Content -Raw -LiteralPath $provenance | ConvertFrom-Json
		$manifestEntryHash = Get-TextSha256 (($model | ConvertTo-Json -Depth 64 -Compress))
		$licensePath = Join-Path (Join-Path $entry 'licenses') ([string]$model.licenseFileName)
		Assert-AtOrBelow (Join-Path $entry 'licenses') $licensePath 'Model license evidence'
		if ([string]$record.archiveSha256 -cne [string]$model.archiveSha256 -or [string]$record.modelId -ne [string]$model.id -or [string]$record.manifestEntrySha256 -cne $manifestEntryHash -or [string]$record.licenseSha256 -cne [string]$model.licenseSha256 -or [string]$record.license -cne [string]$model.license -or [string]$record.attribution -cne [string]$model.attribution) { throw "Provenance mismatch for '$($model.id)'." }
		if (-not (Test-Path -LiteralPath $licensePath -PathType Leaf) -or (Get-Sha256 $licensePath) -cne [string]$model.licenseSha256) { throw "Pinned license evidence is missing or changed for '$($model.id)'." }
		$payloadEvidence = [System.Collections.Generic.List[object]]::new()
		foreach ($file in @((Get-Property $model 'files'))) {
			$payload = Join-Path (Join-Path $entry 'payload') ([string]$file.path)
			Assert-AtOrBelow (Join-Path $entry 'payload') $payload 'Model payload'
			if (-not (Test-Path -LiteralPath $payload -PathType Leaf)) { throw "Verified payload is missing for '$($model.id)': '$($file.path)'." }
			if ((Get-Sha256 $payload) -cne [string]$file.sha256) { throw "Verified payload hash changed for '$($model.id)': '$($file.path)'." }
			$payloadEvidence.Add([ordered]@{ path = [string]$file.path; sha256 = [string]$file.sha256; bytes = [UInt64](Get-Item -LiteralPath $payload).Length })
		}
		$modelProvenance.Add([ordered]@{ id = [string]$model.id; archiveSha256 = [string]$model.archiveSha256; manifestEntrySha256 = $manifestEntryHash; provenancePath = $provenance; provenanceSha256 = Get-Sha256 $provenance; licensePath = $licensePath; licenseSha256 = [string]$model.licenseSha256; payloads = @($payloadEvidence) })
	}
	if ($DryRun) { $result.status = 'ready-for-installed-validation'; $result.models = @($modelProvenance); $result.reason = 'All four manifest-pinned payloads and their model-specific license/provenance records are valid; no installed binary or editor was launched.'; $result; return }
	if (-not (Test-Path -LiteralPath $installPath -PathType Container)) { throw "Installed tree is missing: '$installPath'." }; if (-not (Test-LoopbackPortAvailable $McpPort)) { throw "MCP loopback port $McpPort is occupied." }
	$editorPath = Resolve-InstalledBinary $installPath @('Editor/TerraEngine-Editor.exe', 'Editor/TerraEngine-Editor') 'Installed editor'
	$installedLauncherPath = Resolve-InstalledBinary $installPath @('Launcher/TerraEngine-Launcher.exe', 'Launcher/TerraEngine-Launcher') 'Installed launcher'
	# Long real-click authoring sessions can outlive an invoking automation host.
	# A caller may explicitly resume the same disposable run so assets already
	# created through the installed editor remain valid evidence. The normal path
	# still creates a new timestamped project, and the containment check below
	# prevents a resume from targeting an unrelated or user-owned project.
	$resuming = -not [string]::IsNullOrWhiteSpace($ResumeRunRoot)
	$runRoot = if ($resuming) {
		Get-AbsolutePath $ResumeRunRoot $repoRoot
	} else {
		Join-Path $resultsPath ([DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
	}
	Assert-AtOrBelow $resultsPath $runRoot 'Validation result root'
	if ($resuming -and -not (Test-Path -LiteralPath (Join-Path $runRoot 'project') -PathType Container)) {
		throw "The requested validation run cannot be resumed because its disposable project is missing: '$runRoot'."
	}
	foreach ($folder in @('project', 'preferences', 'editor/cameras', 'editor', 'export', 'runtime')) { [System.IO.Directory]::CreateDirectory((Join-Path $runRoot $folder)) | Out-Null }
	$result.runRoot = $runRoot
	# Copy only hash-verified payload files into the disposable project. The cache,
	# user preferences, and any existing project remain read-only and untouched.
	$projectInputs = @(Initialize-ValidationProject (Join-Path $runRoot 'project') $cachePath $models)
	# The fixed port and write mode do not implicitly enable the bridge. Record
	# the complete editor preference contract so an isolated validation launch
	# actually binds its loopback endpoint without changing user preferences.
	$preferencesPath = Join-Path $runRoot 'preferences/editor-preferences.json'
	Write-Json $preferencesPath ([ordered]@{
		isolated = $true
		suppressRecentProjects = $true
		editorMcpEnabled = $true
		editorMcpWriteModeDefault = 'writes_enabled'
		editorMcpRecentSession = 'AnimationValidationModels'
		editorMcpListenPort = $McpPort
	})
	$script:McpEndpoint = "http://127.0.0.1:$McpPort/mcp"; $script:McpTranscript = Join-Path $runRoot 'editor/mcp.jsonl'
	$start = [System.Diagnostics.ProcessStartInfo]::new(); $start.FileName = $editorPath; $start.Arguments = "--project `"$(Join-Path $runRoot 'project')`""; $start.WorkingDirectory = Split-Path -Parent $editorPath; $start.UseShellExecute = $false; $start.EnvironmentVariables['TERRAENGINE_EDITOR_PREFS_PATH'] = $preferencesPath; $start.EnvironmentVariables['TERRAENGINE_SUPPRESS_PROJECT_RECENTS'] = '1'; $editor = [System.Diagnostics.Process]::Start($start); if ($null -eq $editor) { throw 'Installed editor failed to start.' }
	$deadline = (Get-Date).AddSeconds($EditorStartupTimeoutSeconds); $tools = @(); $toolDescriptors = @(); while ((Get-Date) -lt $deadline) { $editor.Refresh(); if ($editor.HasExited) { throw "Installed editor exited before MCP readiness with code $($editor.ExitCode)." }; try { $ready = Invoke-McpRpc 'initialize' ([ordered]@{ protocolVersion = '2025-03-26'; clientInfo = [ordered]@{ name = 'AnimationValidationModels'; version = '1' } }); if ([string]$ready.result.serverInfo.name -eq 'TerraEngine Editor') { $candidateDescriptors = @((Invoke-McpRpc 'tools/list').result.tools); $projectImport = @($candidateDescriptors | Where-Object { [string]$_.name -eq 'asset.import' }); $sceneOpen = @($candidateDescriptors | Where-Object { [string]$_.name -eq 'scene.open' }); if ($projectImport.Count -eq 1 -and [bool](Get-NestedValue $projectImport[0] 'availability.available') -and $sceneOpen.Count -eq 1 -and [bool](Get-NestedValue $sceneOpen[0] 'availability.available')) { $toolDescriptors = $candidateDescriptors; $tools = @($toolDescriptors | ForEach-Object { [string]$_.name }); break } } } catch {}; Start-Sleep -Milliseconds 500 }; if ($tools.Count -eq 0) { throw 'Installed editor MCP and project-scoped tools did not become ready.' }
	foreach ($tool in @('asset.list', 'asset.import', 'asset.inspect', 'prefab.instantiate', 'scene.open', 'scene.save', 'scene.list_objects', 'component.get', 'component.set_property', 'animation.asset.import', 'animation.asset.reimport', 'animation.asset.inspect', 'animation.controller.edit', 'animation.controller.validate', 'animation.advanced.edit', 'animation.preview.play', 'animation.recording.start', 'animation.runtime.parameters', 'animation.runtime.diagnostics', 'animation.camera.capture', 'animation.job.get', 'play.state', 'play.enter', 'play.exit', 'game_view.capture', 'camera.capture_custom', 'camera.capture_object', 'export.validate', 'export.start', 'jobs.get')) { Assert-Tool $tools $tool }
	Assert-TypedMutationDescriptor @($toolDescriptors | Where-Object { [string]$_.name -eq 'animation.asset.import' })[0] 'animation.asset.import'
	Assert-TypedMutationDescriptor @($toolDescriptors | Where-Object { [string]$_.name -eq 'animation.asset.reimport' })[0] 'animation.asset.reimport'
	$recordingDescriptor = @($toolDescriptors | Where-Object { [string]$_.name -eq 'animation.recording.start' })[0]
	# Recording binds a protected immutable pose snapshot before publishing the
	# durable job, so it intentionally advertises a narrower boundary than the
	# generic animation-authoring mutation tools.
	Assert-TypedMutationDescriptor $recordingDescriptor 'animation.recording.start' 'animation.job.get' 'animation_protected_snapshot_capture_then_atomic_publish'
	Assert-RecordingBindCurrentDescriptor $recordingDescriptor
	$exportDescriptor = @($toolDescriptors | Where-Object { [string]$_.name -eq 'export.start' })[0]
	Assert-TypedMutationDescriptor $exportDescriptor 'export.start' 'jobs.get' 'editor_protected_commit'
	$nativeWindow = Wait-ForEditorMainWindow $editor $EditorStartupTimeoutSeconds
	Write-Json (Join-Path $runRoot 'editor/native-window.json') ([ordered]@{
		schemaVersion = 1
		task = 'AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp'
		processId = $nativeWindow.processId
		windowHandle = $nativeWindow.windowHandle
		windowTitle = $nativeWindow.windowTitle
		executable = $editorPath
		startedAtUtc = $editor.StartTime.ToUniversalTime().ToString('o')
	})
	# The project startup path already opens ValidationModels.tescene. Query that
	# live scene directly because scene.open is an approval-protected mutation;
	# invoking it here would turn a read-only readiness assertion into a pending
	# user action and would make unattended validation stop before import begins.
	$sceneObjects = Invoke-McpTool 'scene.list_objects' ([ordered]@{ includeComponents = $true })
	$captureCamera = @((Get-NestedValue $sceneObjects 'data.objects') | Where-Object { [string](Get-Property $_ 'name') -eq 'Validation Capture Camera' })
	if ($captureCamera.Count -ne 1) { throw 'The installed editor did not load the authored validation capture scene.' }
	$animationModelAssets = [System.Collections.Generic.List[object]]::new()
	$animationGeneration = [UInt64]0
	foreach ($model in $models) {
		$input = @($projectInputs | Where-Object { [string]$_.modelId -eq [string]$model.id })[0]
		$relative = [string]$input.assetRelative
		# The generic project-write surface provides approval-free machine evidence
		# for actual import and forced reimport. Typed animation mutations remain
		# high-impact: automation-only runs prove the pending boundary, while a manual
		# run discovers the durable job only after a real editor UI approval.
		$genericImportEnvelope = Invoke-McpTool 'asset.import' ([ordered]@{ path = $relative; force = $false })
		$genericImport = Get-GenericImportEvidence $genericImportEnvelope $model $false
		$genericReimportEnvelope = Invoke-McpTool 'asset.import' ([ordered]@{ path = $relative; force = $true })
		$genericReimport = Get-GenericImportEvidence $genericReimportEnvelope $model $true
		$stableSubassets = @($genericImport.subassetIdentities).Count -eq @($genericReimport.subassetIdentities).Count -and @(
			Compare-Object -ReferenceObject @($genericImport.subassetIdentities) -DifferenceObject @($genericReimport.subassetIdentities) -CaseSensitive
		).Count -eq 0
		if ($genericImport.packageGuid -ne $genericReimport.packageGuid -or -not $stableSubassets -or $genericReimport.publishedGeneration -le $genericImport.publishedGeneration) { throw "Forced reimport for '$($model.id)' did not preserve identity while advancing the published generation." }

		$importCorrelation = "validation-$($model.id)-typed-import"
		$typedImport = Invoke-McpToolEnvelope 'animation.asset.import' ([ordered]@{ sourcePath = $relative; idempotencyKey = $importCorrelation; correlationId = $importCorrelation })
		$typedImportEvidence = if ($AutomationOnly) { Assert-PendingTypedApproval $typedImport 'animation.asset.import' } else { Wait-ForApprovedAnimationJob $typedImport 'animation.asset.import' $importCorrelation $runRoot }
		$reimportCorrelation = "validation-$($model.id)-typed-reimport"
		$typedReimport = Invoke-McpToolEnvelope 'animation.asset.reimport' ([ordered]@{ sourcePath = $relative; idempotencyKey = $reimportCorrelation; correlationId = $reimportCorrelation })
		$typedReimportEvidence = if ($AutomationOnly) { Assert-PendingTypedApproval $typedReimport 'animation.asset.reimport' } else { Wait-ForApprovedAnimationJob $typedReimport 'animation.asset.reimport' $reimportCorrelation $runRoot }
		$listed = Invoke-McpTool 'asset.list' ([ordered]@{ search = [System.IO.Path]::GetFileName($relative) })
		$asset = @(@((Get-NestedValue $listed 'data.assets')) | Where-Object { [string]$_.path -eq $relative })
		if ($asset.Count -ne 1 -or [string]::IsNullOrWhiteSpace([string]$asset[0].guid)) { throw "Published asset metadata for '$($model.id)' was not uniquely discoverable." }
		$assetId = [string]$asset[0].guid
		if ($assetId -ne $genericReimport.packageGuid) { throw "AssetDatabase identity for '$($model.id)' did not match the published package GUID." }
		$inspect = Invoke-McpTool 'asset.inspect' ([ordered]@{ asset = "Assets/$relative"; operation = 'inspect' })
		$animationInspect = Invoke-McpTool 'animation.asset.inspect' ([ordered]@{ assetId = $assetId; maximumItems = 64; correlationId = "validation-$($model.id)-inspect" })
		if ($null -eq (Get-Property $inspect 'data') -or $null -eq (Get-Property $animationInspect 'data')) { throw "Inspection proof for '$($model.id)' omitted data." }
		$animationGeneration = [UInt64](Get-NestedValue $animationInspect 'data.generation')
		if ($animationGeneration -lt 1) { throw "Animation inspection for '$($model.id)' omitted the service generation." }
		$subassets = @((Get-NestedValue $animationInspect 'data.subassets'))
		if ($subassets.Count -lt 1) { throw "Animation inspection for '$($model.id)' published no generated package children." }
		$assetModel = [ordered]@{
			id = [string]$model.id
			assetId = $assetId
			subassets = $subassets
		}
		$animationModelAssets.Add($assetModel)
		# Per-import verification needs the always-published immutable summary only.
		# Detailed rows are intentionally lease-gated and cannot be produced by an
		# isolated synchronous MCP request without a concurrent visible consumer.
		$diagnostics = Invoke-McpTool 'animation.runtime.diagnostics' ([ordered]@{ assetId = $assetId; correlationId = "validation-$($model.id)-diagnostics"; maximumItems = 64; includeDetails = $false })
		if ($null -eq (Get-Property $diagnostics 'data')) { throw "Runtime diagnostic proof for '$($model.id)' omitted data." }
		$result.models += [ordered]@{ id = $model.id; assetPath = $relative; assetId = $assetId; genericImport = $genericImport; genericReimport = [ordered]@{ surface = $genericReimport.surface; force = $genericReimport.force; status = $genericReimport.status; packageGuid = $genericReimport.packageGuid; publishedGeneration = $genericReimport.publishedGeneration; publishedAtomically = $genericReimport.publishedAtomically; subassetGuids = $genericReimport.subassetGuids; subassetIdentities = $genericReimport.subassetIdentities; requiredKindCounts = $genericReimport.requiredKindCounts; verifiedImportFeatures = $genericReimport.verifiedImportFeatures; pendingScenarioFeatures = $genericReimport.pendingScenarioFeatures; stableAssetId = $true; stableSubassetIds = $stableSubassets; generationAdvanced = $true }; typedImportApproval = $typedImportEvidence; typedReimportApproval = $typedReimportEvidence; inspected = $true; diagnosticsObserved = $true; generatedAnimationSubassets = @($subassets | Where-Object { [string]$_.kind -in @('skeleton', 'animation_clip', 'morph_target', 'deformable_mesh', 'prefab') }); expectedFeatures = @($model.expectedFeatures); machineVerifiedImportFeatures = @($genericReimport.verifiedImportFeatures); scenarioFeaturesPending = @($genericReimport.pendingScenarioFeatures); validationScenarios = @($model.validationScenarios) }
	}

	# Export parity must run an actual cooked animator, not a startup-scene string.
	# Prefer the verified Fox package because it has multiple clips, root-motion
	# coverage, and a skeleton small enough for the bounded 64-joint recorder API.
	$foxAssets = @($animationModelAssets | Where-Object { [string]$_.id -ceq 'khronos-fox-multiple-clips' })
	if ($foxAssets.Count -ne 1) { throw 'The generated Fox package was not uniquely available for the runtime fixture.' }
	$foxSkeletonIds = @(Get-AnimationSubassets $foxAssets[0] 'skeleton' | ForEach-Object { [string]$_.assetId })
	$runtimeFixturePrefab = Find-PlayableValidationPrefab (Join-Path $runRoot 'project') $foxSkeletonIds
	$manualChecklist = Write-AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpManualChecklist $runRoot $models $nativeWindow

	if ($AutomationOnly) {
		$authoringEvidence = [ordered]@{ completed = $false; status = 'skipped'; reason = 'AutomationOnly cannot create and inspect production animation assets through real editor controls.' }
		Write-Json (Join-Path $runRoot 'editor/authoring-status.json') $authoringEvidence
	} else {
		$authoringReady = Wait-ForAnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpAuthoringReady $runRoot @($animationModelAssets) $runtimeFixturePrefab $nativeWindow $editor $animationGeneration
		$authoringEvidence = Invoke-AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpAuthoredMutationMatrix $authoringReady $animationGeneration $runRoot
		$animationGeneration = [UInt64]$authoringEvidence.generation
	}

	$previewEvidence = Invoke-AnimationPreviewMatrix @($animationModelAssets) $animationGeneration
	$animationGeneration = [UInt64]$previewEvidence.generation
	Write-Json (Join-Path $runRoot 'editor/preview-evidence.json') $previewEvidence
	foreach ($modelResult in @($result.models)) {
		$preview = @($previewEvidence.models | Where-Object { [string]$_.id -ceq [string]$modelResult.id })
		if ($preview.Count -eq 1) { $modelResult['preview'] = $preview[0] }
	}

	# Instantiate the complete Fox graph through the editor's protected prefab
	# transaction, switch it to Manual scheduling for a stable recorder binding,
	# save, and re-read the serialized startup scene before Play or export.
	$runtimeFixtureInstance = Invoke-McpTool 'prefab.instantiate' ([ordered]@{
		prefab = [string]$runtimeFixturePrefab.assetPath
		name = 'Validation Animated Runtime Hero'
	})
	$runtimeFixtureInstanceId = [string](Get-NestedValue $runtimeFixtureInstance 'data.instanceId')
	if ($runtimeFixtureInstanceId -notmatch '^[0-9a-f]{32}$') {
		throw 'The full-graph generated prefab instantiation omitted a stable runtime instance identity.'
	}
	$instantiatedScene = Invoke-McpTool 'scene.list_objects' ([ordered]@{ includeComponents = $true })
	$runtimeMeshChildren = @((Get-NestedValue $instantiatedScene 'data.objects') | Where-Object {
		[string](Get-Property $_ 'parentUuid') -ceq $runtimeFixtureInstanceId -and
		@((Get-Property $_ 'components')) -contains 'MeshRenderComponent'
	})
	if ($runtimeMeshChildren.Count -ne 1) {
		throw 'The Fox runtime fixture did not expose exactly one direct renderable child.'
	}
	$runtimeMeshObjectId = [string](Get-Property $runtimeMeshChildren[0] 'uuid')
	# Fox uses a deliberately large authored coordinate range. Preserve the cooked
	# package unchanged and scale only its renderable scene child so the model
	# matrix used by every deformation path frames the complete character.
	[void](Invoke-McpTool 'component.set_property' ([ordered]@{
		object = $runtimeMeshObjectId
		component = 'Transform'
		property = 'Scale'
		value = @(0.01, 0.01, 0.01)
	}))
	[void](Invoke-McpTool 'component.set_property' ([ordered]@{
		object = $runtimeMeshObjectId
		component = 'Transform'
		property = 'Rotation'
		# Fox is authored along the Z axis. Turn the complete render child so
		# the validation cameras observe its full side silhouette instead of a
		# technically non-black but heavily self-occluded head-on view.
		value = @(0.7071067811865476, 0.0, 0.7071067811865475, 0.0)
	}))
	[void](Invoke-McpTool 'component.set_property' ([ordered]@{
		object = $runtimeFixtureInstanceId
		component = 'AnimatorComponent'
		property = 'updatePolicy'
		value = 2
	}))
	# Recording consumes a complete CPU pose. Pin this fixture to the deterministic
	# CPU fallback so GPU residency or a mode switch cannot remove that authority
	# between the runtime command and the protected recorder capture.
	[void](Invoke-McpTool 'component.set_property' ([ordered]@{
		object = $runtimeFixtureInstanceId
		component = 'AnimatorComponent'
		property = 'requestedMode'
		value = 3
	}))
	$runtimeAnimatorComponent = Invoke-McpTool 'component.get' ([ordered]@{ object = $runtimeFixtureInstanceId; component = 'AnimatorComponent' })
	$runtimeTransformComponent = Invoke-McpTool 'component.get' ([ordered]@{ object = $runtimeMeshObjectId; component = 'Transform' })
	if ([int](Get-NestedValue $runtimeAnimatorComponent 'data.properties.updatePolicy') -ne 2 -or
		[int](Get-NestedValue $runtimeAnimatorComponent 'data.properties.requestedMode') -ne 3) {
		throw 'The Fox runtime fixture did not retain its Manual deterministic-CPU recording policy.'
	}
	$runtimeScale = @((Get-NestedValue $runtimeTransformComponent 'data.properties.Scale'))
	if ($runtimeScale.Count -ne 3 -or @($runtimeScale | Where-Object { [Math]::Abs([double]$_ - 0.01) -gt 0.000001 }).Count -ne 0) {
		throw 'The Fox runtime fixture did not retain its validation-scene scale.'
	}
	$runtimeRotation = @((Get-NestedValue $runtimeTransformComponent 'data.properties.Rotation'))
	$expectedRuntimeRotation = @(0.7071067811865476, 0.0, 0.7071067811865475, 0.0)
	if ($runtimeRotation.Count -ne 4) {
		throw 'The Fox runtime fixture did not retain its validation-scene orientation.'
	}
	for ($rotationIndex = 0; $rotationIndex -lt 4; $rotationIndex++) {
		if ([Math]::Abs([double]$runtimeRotation[$rotationIndex] - [double]$expectedRuntimeRotation[$rotationIndex]) -gt 0.000001) {
			throw 'The Fox runtime fixture did not retain its validation-scene orientation.'
		}
	}
	$runtimeFixtureSave = Invoke-McpTool 'scene.save'
	$savedStartupScenePath = Join-Path $runRoot 'project/Assets/Scenes/ValidationModels.tescene'
	if (-not (Test-Path -LiteralPath $savedStartupScenePath -PathType Leaf)) {
		throw 'The installed editor did not persist the authored runtime startup scene.'
	}
	$savedStartupScene = Get-Content -Raw -LiteralPath $savedStartupScenePath | ConvertFrom-Json
	$savedAnimator = Find-AnimatorComponentInObjectGraph @((Get-Property $savedStartupScene 'objects'))
	if ($null -eq $savedAnimator -or
		[string](Get-Property $savedAnimator 'defaultClipAssetId') -cne [string]$runtimeFixturePrefab.defaultClipAssetId -or
		[string](Get-Property $savedAnimator 'skeletonOverrideAssetId') -cne [string]$runtimeFixturePrefab.skeletonAssetId -or
		[int](Get-Property $savedAnimator 'updatePolicy') -ne 2 -or
		[int](Get-Property $savedAnimator 'requestedMode') -ne 3) {
		throw 'The persisted startup scene does not retain the selected cooked clip/skeleton and Manual deterministic-CPU AnimatorComponent binding.'
	}
	$runtimeFixture = [ordered]@{
		prefab = $runtimeFixturePrefab
		instanceId = $runtimeFixtureInstanceId
		meshObjectId = $runtimeMeshObjectId
		scene = 'Assets/Scenes/ValidationModels.tescene'
		sceneSha256 = Get-Sha256 $savedStartupScenePath
		save = Get-Property $runtimeFixtureSave 'data'
	}
	if ($AutomationOnly) {
		$manual = [ordered]@{ completed = $false; status = 'skipped'; reason = 'AutomationOnly was requested because real mouse/keyboard and screen proof were unavailable.' }
		Write-Json (Join-Path $runRoot 'editor/manual-status.json') $manual
	} else {
		# The live checklist includes Play, camera, export, and runtime review.
		# Those results do not exist yet, so defer the operator proof until every
		# machine-driven action below has produced evidence that can be inspected.
		$manual = $null
	}

	$before = Invoke-McpTool 'play.state'
	if ([string](Get-NestedValue $before 'data.currentState') -ne 'stopped') { throw 'Installed editor must be stopped before scripted Play proof.' }
	$playEntered = $false
	$playValidationError = ''
	$playValidationStage = 'play.enter'
	$captureDirectory = Join-Path $runRoot 'editor/captures'
	$captureEvidence = [ordered]@{ status = 'not-run'; rendererAvailable = $true; visualContentVerified = $false; files = @() }
	$recordingEvidence = [ordered]@{ completed = $false; status = 'not-run' }
	try {
		[void](Invoke-McpTool 'play.enter' ([ordered]@{ expectedState = 'stopped'; correlationId = 'validation-models-enter' }))
		$playEntered = $true
		$playValidationStage = 'recording'
		if ($AutomationOnly) {
			$recordingEvidence = [ordered]@{ completed = $false; status = 'skipped'; reason = 'A checked full skeleton joint list requires the installed-editor authoring step.' }
		} else {
			$recordingEvidence = Invoke-AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcpRuntimeRecording $runRoot $runtimeFixtureInstanceId $authoringReady $animationGeneration
			$animationGeneration = [UInt64]$recordingEvidence.generation
			Write-Json (Join-Path $runRoot 'editor/recording-evidence.json') $recordingEvidence
		}
		$playValidationStage = 'camera-capture'
		$gameViewPath = Join-Path $captureDirectory 'game-view.png'
		$customCameraPath = Join-Path $captureDirectory 'custom-camera.png'
		$sceneCameraPath = Join-Path $captureDirectory 'scene-camera.png'
		[void](Invoke-McpPngCapture 'camera.capture_custom' ([ordered]@{ name = 'Validation Scene View'; position = @(-1.0, 0.7, 4); target = @(0, 0.4, 0); up = @(0, 1, 0); projection = 'perspective'; verticalFovDegrees = 60.0; nearPlane = 0.1; farPlane = 100.0 }) $customCameraPath)
		[void](Invoke-McpPngCapture 'camera.capture_object' ([ordered]@{ object = 'Validation Capture Camera'; includeDisabled = $false; transparentBackground = $false }) $sceneCameraPath)
		# Move the live Game View camera only inside Play Mode. play.exit restores
		# the authored snapshot, while this offset guarantees the Game View proof
		# is a distinct camera result rather than a duplicate object-camera image.
		[void](Invoke-McpTool 'component.set_property' ([ordered]@{
			object = 'Validation Capture Camera'
			component = 'Transform'
			property = 'Position'
			value = @(1.0, 0.4, 4.0)
		}))
		# play.enter publishes state before the next normal render necessarily
		# completes. Retry only while the latest Game View is still blank so the
		# evidence binds a real post-enter frame without accepting stale pixels.
		$gameViewDeadline = (Get-Date).AddSeconds(5)
		do {
			[void](Invoke-McpPngCapture 'game_view.capture' ([ordered]@{ includeOverlays = $false; freshness = 'nextFrame' }) $gameViewPath)
			$gameViewStatistics = Get-PngVisualStatistics ([System.IO.File]::ReadAllBytes($gameViewPath))
			if ([UInt64]$gameViewStatistics.visibleNonBlackPixelCount -gt 0 -and -not [bool]$gameViewStatistics.isRgbaUniform) {
				break
			}
			if ((Get-Date) -ge $gameViewDeadline) {
				throw "Installed-editor capture '$gameViewPath' remained black, invisible, or uniform after Play frames were allowed to publish."
			}
			Start-Sleep -Milliseconds 100
		} while ($true)
		[void](Invoke-McpTool 'animation.camera.capture' ([ordered]@{ source = 'custom'; name = 'Animation Validation'; position = @(-1.0, 0.7, 4); lookAt = @(0, 0.4, 0); up = @(0, 1, 0); projection = 'perspective'; verticalFovDegrees = 60.0; nearPlane = 0.1; farPlane = 100.0; freshness = 'latestCompleted'; allowBackendFallback = $true }))
		$playValidationStage = 'camera-visual-validation'
		$visualFiles = [System.Collections.Generic.List[object]]::new()
		$captureByteHashes = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
		$capturePixelHashes = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
		foreach ($capturePath in @($gameViewPath, $customCameraPath, $sceneCameraPath)) {
			$statistics = Get-PngVisualStatistics ([System.IO.File]::ReadAllBytes($capturePath))
			if ([UInt64]$statistics.visibleNonBlackPixelCount -eq 0 -or [bool]$statistics.isRgbaUniform) {
				throw "Installed-editor capture '$capturePath' is black, invisible, or uniform."
			}
			$byteHash = Get-Sha256 $capturePath
			if (-not $captureByteHashes.Add($byteHash) -or -not $capturePixelHashes.Add([string]$statistics.pixelSha256)) {
				throw "Installed-editor camera captures reuse identical bytes or decoded pixels; fresh view-specific visible-deformation evidence is required."
			}
			$visualFiles.Add([ordered]@{ path = $capturePath; sha256 = $byteHash; visualStatistics = $statistics })
		}
		$captureEvidence.status = 'captured'
		$captureEvidence.visualContentVerified = $true
		$captureEvidence.files = @($visualFiles)
		$captureEvidence.reason = 'Three distinct installed-editor camera surfaces produced non-black, non-uniform PNG readbacks; semantic culling/shadow correctness remains bound to the exact manual actions.'
	} catch {
		$playValidationError = $_.Exception.Message
		$captureEvidence.status = 'failed'
		$captureEvidence.rendererAvailable = $null
		$captureEvidence['failureStage'] = $playValidationStage
		$captureEvidence.reason = $playValidationError
	} finally {
		if ($playEntered) { [void](Invoke-McpTool 'play.exit' ([ordered]@{ expectedState = 'playing'; correlationId = 'validation-models-exit' })) }
	}
	if (-not [string]::IsNullOrWhiteSpace($playValidationError) -and -not $AutomationOnly) {
		throw "Installed-editor Play/recording/camera proof failed: $playValidationError"
	}
	$afterPlay = Invoke-McpTool 'play.state'
	if ([string](Get-NestedValue $afterPlay 'data.currentState') -cne 'stopped') { throw 'The installed editor did not restore stopped state after AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp Play validation.' }
	# Manual policy is a recorder-only fixture constraint. Restore normal
	# EveryFrame playback before saving/exporting so the TEPAK parity lanes still
	# exercise advancing animation rather than a static pose.
	[void](Invoke-McpTool 'component.set_property' ([ordered]@{
		object = $runtimeFixtureInstanceId
		component = 'AnimatorComponent'
		property = 'updatePolicy'
		value = 0
	}))
	[void](Invoke-McpTool 'component.set_property' ([ordered]@{
		object = $runtimeFixtureInstanceId
		component = 'AnimatorComponent'
		property = 'requestedMode'
		value = 0
	}))
	$restoredAnimator = Invoke-McpTool 'component.get' ([ordered]@{ object = $runtimeFixtureInstanceId; component = 'AnimatorComponent' })
	if ([int](Get-NestedValue $restoredAnimator 'data.properties.updatePolicy') -ne 0 -or
		[int](Get-NestedValue $restoredAnimator 'data.properties.requestedMode') -ne 0) {
		throw 'The Fox runtime fixture did not restore EveryFrame/Auto policy after recording.'
	}
	[void](Invoke-McpTool 'scene.save')
	$runtimeFixture.sceneSha256 = Get-Sha256 $savedStartupScenePath
	$runtimeFixture['recordingPolicy'] = 'manual'
	$runtimeFixture['recordingExecutionMode'] = 'force_cpu_deterministic_fallback'
	$runtimeFixture['exportPolicy'] = 'every_frame'
	$runtimeFixture['exportExecutionMode'] = 'auto'

	$validation = Invoke-McpTool 'export.validate' ([ordered]@{ profile = 'Validation Models Windows' })
	if ([bool](Get-NestedValue $validation 'data.canExport') -ne $true -or [int](Get-NestedValue $validation 'data.errorCount') -ne 0) { throw 'The acquired-model export profile failed pre-publication validation.' }
	$exportCorrelation = 'validation-models-export'
	# Correlation and idempotency keys are intentionally identical and retained in
	# the approval/job evidence. That makes the approved TEPAK job one binding,
	# rather than a similarly named export discovered after the fact.
	$export = Invoke-McpToolEnvelope 'export.start' ([ordered]@{ profile = 'Validation Models Windows'; correlationId = $exportCorrelation; idempotencyKey = $exportCorrelation })

	$machineEvidence = [ordered]@{
		installedTreeOnly = $true
		manifestPath = $manifestPath
		manifestSha256 = Get-TextSha256 $manifestText
		acquiredModelPayloads = @($modelProvenance)
		mcpTranscript = $script:McpTranscript
		animationGeneration = $animationGeneration
		authoring = $authoringEvidence
		preview = $previewEvidence
		recording = $recordingEvidence
		capture = $captureEvidence
		exportValidation = [ordered]@{ canExport = $true; errorCount = [int](Get-NestedValue $validation 'data.errorCount'); warningCount = [int](Get-NestedValue $validation 'data.warningCount'); profile = [string](Get-NestedValue $validation 'data.profile'); profileId = [string](Get-NestedValue $validation 'data.profileId') }
		runtimeFixture = $runtimeFixture
		performanceRegressionTesting = [ordered]@{ status = 'skipped'; reason = 'The operator-required GPU power limit makes performance regression measurements non-representative.' }
		authoritativeFixedTickParity = $false
		visualRuntimeParity = $false
		authoritativeParityReason = 'The approved source-free export and its rendered/headless parity lanes have not executed yet.'
	}
	if ($AutomationOnly) {
		$exportApproval = Assert-PendingTypedApproval $export 'export.start'
		$machineEvidence['export'] = [ordered]@{ approval = $exportApproval; executed = $false; bootstrapExecuted = $false; reason = 'Automation-only validation preserves the required export approval boundary.' }
		$result.status = 'partial-automation-only'
		$result.reason = 'Installed-editor import/reimport, exact animation.preview.play sequences, diagnostics, Manual-policy Play, and camera checks ran; production UI authoring, controller/advanced edits, recording approval, export/bootstrap, real-click proof, and authoritative runtime parity remain incomplete.'
	} else {
		$approvedExport = Wait-ForApprovedExportJob $export $exportCorrelation $runRoot
		$exportJobId = [string](Get-Property $approvedExport.approval 'jobId')
		$outputDirectory = [string](Get-NestedValue $approvedExport 'job.result.outputDirectory')
		if ([string]::IsNullOrWhiteSpace($outputDirectory) -or -not (Test-Path -LiteralPath $outputDirectory -PathType Container)) { throw 'The completed export job omitted a readable output directory.' }
		Assert-AtOrBelow (Join-Path $runRoot 'project') $outputDirectory 'Approved export output'

		# Copy the completed output beside, rather than beneath, the disposable
		# source project so bootstrap cannot find loose source/editor files through a
		# parent-directory fallback.
		$isolatedRuntime = Join-Path $runRoot 'runtime/isolated'
		Assert-AtOrBelow (Join-Path $runRoot 'runtime') $isolatedRuntime 'Isolated runtime output'
		# A resumed validation attempt may find Settings or other runtime-owned
		# directories produced by the previous exported launcher. Recreate this
		# disposable run-local directory before copying so stale output cannot make
		# Copy-Item nest the new export or contaminate the source-free inventory.
		if (Test-Path -LiteralPath $isolatedRuntime) {
			Remove-Item -LiteralPath $isolatedRuntime -Recurse -Force
		}
		Copy-Item -LiteralPath $outputDirectory -Destination $isolatedRuntime -Recurse -Force
		$isolatedRuntimePath = (Resolve-Path -LiteralPath $isolatedRuntime).Path.TrimEnd('\', '/')
		$runtimeInventory = [System.Collections.Generic.List[object]]::new()
		foreach ($directory in @(Get-ChildItem -LiteralPath $isolatedRuntime -Directory -Recurse -Force)) {
			$relativeDirectory = $directory.FullName.Substring($isolatedRuntimePath.Length + 1).Replace('\', '/')
			# A source-free Windows export has only the NativeAOT Managed payload
			# below the root. Rejecting every other directory prevents a launcher
			# from finding copied Assets, Editor, source, or build intermediates.
			if (($relativeDirectory -split '/')[0] -cne 'Managed') {
				throw "Isolated runtime contains forbidden loose directory '$relativeDirectory'."
			}
		}
		$allowedRuntimeExtensions = @('.dll', '.exe', '.pdb', '.tepak')
		foreach ($runtimeFile in @(Get-ChildItem -LiteralPath $isolatedRuntime -File -Recurse -Force)) {
			$relativeFile = $runtimeFile.FullName.Substring($isolatedRuntimePath.Length + 1).Replace('\', '/')
			$extension = [System.IO.Path]::GetExtension($runtimeFile.Name).ToLowerInvariant()
			if ($extension -cnotin $allowedRuntimeExtensions) {
				throw "Isolated runtime contains forbidden loose source/config payload '$relativeFile'."
			}
			$runtimeInventory.Add([ordered]@{
				path = $relativeFile
				bytes = [UInt64]$runtimeFile.Length
				sha256 = Get-Sha256 $runtimeFile.FullName
			})
		}
		$runtimeInventoryPath = Join-Path $runRoot 'runtime/source-free-inventory.json'
		Write-Json $runtimeInventoryPath ([ordered]@{
			schemaVersion = 1
			sourceProject = (Join-Path $runRoot 'project')
			isolatedRuntime = $isolatedRuntime
			allowedExtensions = $allowedRuntimeExtensions
			files = @($runtimeInventory)
		})
		$packages = @(Get-ChildItem -LiteralPath $isolatedRuntime -Filter '*.tepak' -File)
		if ($packages.Count -ne 1) { throw "Expected one TEPAK in isolated runtime output, found $($packages.Count)." }
		$package = $packages[0]
		$publishedLauncher = Join-Path $isolatedRuntime ($package.BaseName + '.exe')
		if (-not (Test-Path -LiteralPath $publishedLauncher -PathType Leaf)) { throw "Exported launcher matching '$($package.Name)' is missing." }
		$normalBootstrap = Invoke-InstalledProcess $publishedLauncher @('--validate-export-bootstrap') $isolatedRuntime (Join-Path $runRoot 'runtime/bootstrap')
		if ($normalBootstrap.exitCode -ne 0) { throw 'The source-free exported launcher failed archive/bootstrap validation.' }

		# Run the exact exported executable twice from the isolated output. The
		# rendered lane must return a real Vulkan PNG, while the headless lane must
		# remain CPU fallback. Their authoritative hashes are compared for all 120
		# fixed ticks, so a visually valid frame cannot hide gameplay divergence.
		$renderedProcess = Invoke-InstalledProcess $publishedLauncher @(
			'--validate-export-animation-parity'
		) $isolatedRuntime (Join-Path $runRoot 'runtime/rendered-parity') 180
		$renderedReport = Read-AnimationParityReport $renderedProcess 'rendered-runtime' $true
		$renderedVisual = Get-Property $renderedReport 'visualCapture'
		$renderedPngBytes = [Convert]::FromBase64String([string](Get-Property $renderedVisual 'pngBase64'))
		$renderedPngPath = Join-Path $runRoot 'runtime/rendered-runtime.png'
		[System.IO.File]::WriteAllBytes($renderedPngPath, $renderedPngBytes)
		$renderedVisualStatistics = Get-PngVisualStatistics $renderedPngBytes
		if ([int]$renderedVisualStatistics.width -ne [int](Get-Property $renderedVisual 'width') -or
			[int]$renderedVisualStatistics.height -ne [int](Get-Property $renderedVisual 'height') -or
			[UInt64]$renderedVisualStatistics.visibleNonBlackPixelCount -eq 0 -or
			[bool]$renderedVisualStatistics.isRgbaUniform) {
			throw 'The source-free rendered runtime capture is black, invisible, uniform, or dimensionally inconsistent.'
		}
		$renderedVisual.PSObject.Properties.Remove('pngBase64')
		$renderedReportPath = Join-Path $runRoot 'runtime/rendered-parity-report.json'
		Write-Json $renderedReportPath $renderedReport

		$headlessProcess = Invoke-InstalledProcess $publishedLauncher @(
			'--validate-export-animation-parity',
			'--headless-cpu-fallback'
		) $isolatedRuntime (Join-Path $runRoot 'runtime/headless-parity') 180
		$headlessReport = Read-AnimationParityReport $headlessProcess 'headless-cpu-fallback' $false
		$headlessReportPath = Join-Path $runRoot 'runtime/headless-parity-report.json'
		Write-Json $headlessReportPath $headlessReport
		$parityComparison = Compare-AnimationParityReports $renderedReport $headlessReport
		$parityComparison['renderedPng'] = $renderedPngPath
		$parityComparison['renderedPngSha256'] = Get-Sha256 $renderedPngPath
		$parityComparison['renderedVisualStatistics'] = $renderedVisualStatistics
		$parityComparison['renderedReport'] = $renderedReportPath
		$parityComparison['renderedReportSha256'] = Get-Sha256 $renderedReportPath
		$parityComparison['headlessReport'] = $headlessReportPath
		$parityComparison['headlessReportSha256'] = Get-Sha256 $headlessReportPath
		Write-Json (Join-Path $runRoot 'runtime/parity-comparison.json') $parityComparison

		$machineEvidence.authoritativeFixedTickParity = $true
		$machineEvidence.visualRuntimeParity = $true
		$machineEvidence.authoritativeParityReason = 'Rendered and headless source-free runtimes matched all authoritative hashes for 120 fixed ticks; the rendered lane also produced a non-vacuous Vulkan PNG.'
		$machineEvidence['export'] = [ordered]@{
			approval = $approvedExport.approval
			executed = $true
			jobId = $exportJobId
			output = $outputDirectory
			isolatedRuntime = $isolatedRuntime
			sourceFreeInventory = $runtimeInventoryPath
			sourceFreeInventorySha256 = Get-Sha256 $runtimeInventoryPath
			sourceFreeFileCount = $runtimeInventory.Count
			installedLauncherReference = $installedLauncherPath
			publishedLauncher = $publishedLauncher
			publishedLauncherSha256 = Get-Sha256 $publishedLauncher
			tepak = $package.FullName
			tepakSha256 = Get-Sha256 $package.FullName
			normalBootstrap = $normalBootstrap
			parity = $parityComparison
		}
		# Preserve AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntime's complete source-free export and runtime artifacts before
		# waiting on the separate AnimationValidationRealModelInstalledEditorMatrixRealClicksAnimationWindowsMcp mouse/keyboard matrix. A later manual timeout
		# therefore cannot fabricate or erase the already observed parity outcome.
		$machineEvidence['AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntimeEvidence'] = Publish-AnimationValidationExportAcquiredModelProjectTepakLaunchInstalledRuntimeHeadlessRuntimeRuntimeEvidence $runRoot $exportRuntimeResultsPath $machineEvidence
		# Ask for real installed-editor mouse/keyboard proof only after Play,
		# cameras, approved export, isolated bootstrap, and both runtime parity
		# lanes are complete. This keeps every checklist assertion temporally true.
		$manual = Wait-ForManualEvidence $runRoot $manualChecklist $editor
		$result.status = 'passed'
		$result.reason = 'Real-model import/reimport, controller and advanced authoring edits, isolated previews, deterministic recording, Play/cameras, strict action-bound PrintWindow proof, approved source-free export, rendered Vulkan output, and exact 120-tick rendered/headless parity all passed.'
	}
	Write-Json (Join-Path $runRoot 'result.json') ([ordered]@{ status = $result.status; reason = $result.reason; models = $result.models; manualProof = $manual; machineEvidence = $machineEvidence })
	$result
}
catch { $result.reason = $_.Exception.Message; $result | ConvertTo-Json -Depth 32; exit 1 }
finally { if ($null -ne $editor -and -not $editor.HasExited) { [void]$editor.CloseMainWindow(); if (-not $editor.WaitForExit(60000)) { $editor.Kill(); [void]$editor.WaitForExit(10000) } } }
