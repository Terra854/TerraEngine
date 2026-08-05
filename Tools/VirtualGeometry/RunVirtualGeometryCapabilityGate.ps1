[CmdletBinding()]
param(
	[string]$ScenarioDirectory = 'Tools/VirtualGeometry/CapabilityScenarios',
	[string]$OutputDirectory = 'out/evidence/virtual-geometry/production-gates',
	[string]$InstalledEditor = 'out/build/RelWithDebInfo/Build/Editor/TerraEngine-Editor.exe',
	[string]$Project = 'Tests/Fixtures/Animation/ImportValidation',
	[string]$Preferences = 'out/validation/virtual-geometry/live/editor-preferences.json',
	[string]$Asset = 'Assets/Models/ImportHero.gltf',
	[string]$FixturePrefab = 'Assets/Models/ImportHero_Generated/ImportHero.teprefab',
	[string]$FixturePrefabName = 'Virtual Geometry Installed Validation Hero',
	[string]$BuildDirectory = 'out/build/RelWithDebInfo',
	[string]$InstalledRoot = 'out/build/RelWithDebInfo/Build',
	[string]$CodecInputMesh = '',
	[string]$HardwareImage = '',
	[string]$ComputeImage = '',
	[string]$HybridImage = '',
	[string]$IndexedImage = '',
	[string]$FullReferenceEvidence = '',
	[string]$MillionDecisionEvidence = '',
	[string]$BenchmarkBaseline = '',
	[string]$BenchmarkCandidate = '',
	[string]$PackageEvidence = '',
	[string]$RuntimeAdapter = '',
	[int]$McpPort = 43156,
	[int]$StartupTimeoutSeconds = 90,
	[switch]$DryRun,
	[switch]$SkipReleaseGates,
	[switch]$SkipBuildInstallPackage,
	[switch]$SkipFixturePrefabSetup
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$utf8NoBom = [Text.UTF8Encoding]::new($false)

function Resolve-RepositoryPath([string]$Path)
{
	if([IO.Path]::IsPathRooted($Path))
	{
		return [IO.Path]::GetFullPath($Path)
	}
	return [IO.Path]::GetFullPath((Join-Path $repositoryRoot $Path))
}

function Get-OptionalProperty($Value, [string]$Name)
{
	if($null -eq $Value)
	{
		return $null
	}
	$property = $Value.PSObject.Properties[$Name]
	if($null -eq $property)
	{
		return $null
	}
	return $property.Value
}

function Write-JsonFile([string]$Path, $Value)
{
	$parent = Split-Path -Parent $Path
	if(-not [string]::IsNullOrWhiteSpace($parent))
	{
		[IO.Directory]::CreateDirectory($parent) | Out-Null
	}
	[IO.File]::WriteAllText(
		$Path,
		($Value | ConvertTo-Json -Depth 64),
		$utf8NoBom)
}

function Get-FileIdentity([string]$Path)
{
	return [ordered]@{
		path = Get-RelativeArtifactPath $outputRoot $Path
		bytes = (Get-Item -LiteralPath $Path).Length
		sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).
			Hash.ToLowerInvariant()
	}
}

function Get-RelativeArtifactPath([string]$BasePath, [string]$ArtifactPath)
{
	# Windows PowerShell can run on a .NET Framework version that predates
	# Path.GetRelativePath, so derive the child path after validating scope.
	$baseFullPath = [IO.Path]::GetFullPath($BasePath).TrimEnd(
		[IO.Path]::DirectorySeparatorChar,
		[IO.Path]::AltDirectorySeparatorChar)
	$artifactFullPath = [IO.Path]::GetFullPath($ArtifactPath)
	$childPrefix = $baseFullPath + [IO.Path]::DirectorySeparatorChar
	if(-not $artifactFullPath.StartsWith(
		$childPrefix, [StringComparison]::OrdinalIgnoreCase))
	{
		throw "Production-gate artifact escaped the output directory: $artifactFullPath"
	}
	return $artifactFullPath.Substring($childPrefix.Length)
}

function Test-ScenarioArtifact([string]$ScenarioOutput, $Descriptor)
{
	$relativePath = if($Descriptor -is [string])
	{
		[string]$Descriptor
	}
	else
	{
		[string](Get-OptionalProperty $Descriptor 'path')
	}
	$kind = if($Descriptor -is [string])
	{
		'file'
	}
	else
	{
		[string](Get-OptionalProperty $Descriptor 'kind')
	}
	$path = Join-Path $ScenarioOutput $relativePath
	if([string]::IsNullOrWhiteSpace($relativePath) -or
		-not (Test-Path -LiteralPath $path -PathType Leaf))
	{
		return [ordered]@{
			path = $relativePath
			status = 'missing'
			reason = 'The required artifact was not published.'
		}
	}
	$item = Get-Item -LiteralPath $path
	$record = [ordered]@{
		path = $relativePath
		status = 'accepted'
		bytes = $item.Length
		sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).
			Hash.ToLowerInvariant()
		kind = $kind
	}
	if($item.Length -eq 0)
	{
		$record.status = 'invalid'
		$record.reason = 'Empty files are not evidence.'
		return $record
	}
	if($kind -eq 'png')
	{
		$bytes = [IO.File]::ReadAllBytes($path)
		if($bytes.Length -lt 8 -or $bytes[0] -ne 0x89 -or
			$bytes[1] -ne 0x50 -or $bytes[2] -ne 0x4e -or
			$bytes[3] -ne 0x47)
		{
			$record.status = 'invalid'
			$record.reason = 'The image does not have a PNG signature.'
		}
		return $record
	}
	if($kind -in @('json', 'editor-envelope'))
	{
		try
		{
			$document = Get-Content -Raw -LiteralPath $path |
				ConvertFrom-Json
		}
		catch
		{
			$record.status = 'invalid'
			$record.reason = "The JSON artifact is unreadable: $($_.Exception.Message)"
			return $record
		}
		if($kind -eq 'editor-envelope')
		{
			$ok = Get-OptionalProperty $document 'ok'
			$data = Get-OptionalProperty $document 'data'
			if($ok -ne $true -or $null -eq $data)
			{
				$record.status = 'incomplete'
				$record.reason =
					'Editor evidence requires an accepted envelope with typed data, not a status-only placeholder.'
			}
		}
	}
	return $record
}

$scenarioRoot = Resolve-RepositoryPath $ScenarioDirectory
$outputRoot = Resolve-RepositoryPath $OutputDirectory
if(-not (Test-Path -LiteralPath $scenarioRoot -PathType Container))
{
	throw "Scenario directory missing: $scenarioRoot"
}
$scenarioFiles = @(Get-ChildItem -LiteralPath $scenarioRoot -Filter '*.json' -File |
	Sort-Object Name)
if($scenarioFiles.Count -eq 0)
{
	throw 'No capability scenarios were found.'
}

$scenarios = @()
foreach($scenarioFile in $scenarioFiles)
{
	$scenario = Get-Content -Raw -LiteralPath $scenarioFile.FullName |
		ConvertFrom-Json
	if([int](Get-OptionalProperty $scenario 'schemaVersion') -ne 2 -or
		[string]::IsNullOrWhiteSpace([string](Get-OptionalProperty $scenario 'id')))
	{
		throw "Invalid capability scenario: $($scenarioFile.FullName)"
	}
	$scenarios += $scenario
}
$requiredScenarioIds = @(
	'animated-geometry', 'capability-contract', 'codec-storage',
	'device-submission', 'hybrid-rasterization', 'multi-view-rendering',
	'production-readiness', 'streaming-publication', 'vendor-portability',
	'visibility-parity')
$scenarioIds = @($scenarios | ForEach-Object {
	[string](Get-OptionalProperty $_ 'id')
} | Sort-Object)
if(($scenarioIds -join ',') -ne (($requiredScenarioIds | Sort-Object) -join ','))
{
	throw "Capability scenarios must cover the complete production set exactly once; found: $($scenarioIds -join ',')."
}
$manifestText = ($scenarioFiles | ForEach-Object {
	Get-Content -Raw -LiteralPath $_.FullName
}) -join "`n"
if($manifestText -match '(?i)d3d12|dx12|dxil')
{
	throw 'Production capability scenarios must remain Vulkan-only.'
}

if($DryRun)
{
	$releaseScript = Join-Path $PSScriptRoot 'InvokeVirtualGeometryReleaseGates.ps1'
	& powershell -NoProfile -ExecutionPolicy Bypass -File $releaseScript -DryRun
	if($LASTEXITCODE -ne 0)
	{
		throw 'Release-gate dry-run validation failed.'
	}
	Write-Output "validated production capability scenarios and Vulkan-only release gates (dry run)"
	exit 0
}

$releaseGateStatus = 'skipped'
if(-not $SkipReleaseGates)
{
	$releaseArguments = @(
		'-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
		(Join-Path $PSScriptRoot 'InvokeVirtualGeometryReleaseGates.ps1'),
		'-OutputDirectory', $OutputDirectory,
		'-BuildDirectory', $BuildDirectory,
		'-InstalledRoot', $InstalledRoot
	)
	foreach($argument in @(
		@('-CodecInputMesh', $CodecInputMesh),
		@('-HardwareImage', $HardwareImage),
		@('-ComputeImage', $ComputeImage),
		@('-HybridImage', $HybridImage),
		@('-IndexedImage', $IndexedImage),
		@('-FullReferenceEvidence', $FullReferenceEvidence),
		@('-MillionDecisionEvidence', $MillionDecisionEvidence),
		@('-BenchmarkBaseline', $BenchmarkBaseline),
		@('-BenchmarkCandidate', $BenchmarkCandidate),
		@('-PackageEvidence', $PackageEvidence),
		@('-RuntimeAdapter', $RuntimeAdapter)))
	{
		if(-not [string]::IsNullOrWhiteSpace([string]$argument[1]))
		{
			$releaseArguments += $argument
		}
	}
	if($SkipBuildInstallPackage)
	{
		$releaseArguments += '-SkipBuildInstallPackage'
	}
	& powershell @releaseArguments
	$releaseGateStatus = if($LASTEXITCODE -eq 0) { 'accepted' } else { 'incomplete' }
}

$editorPath = Resolve-RepositoryPath $InstalledEditor
$projectPath = Resolve-RepositoryPath $Project
$preferencesPath = Resolve-RepositoryPath $Preferences
foreach($requiredFile in @($editorPath, $preferencesPath))
{
	if(-not (Test-Path -LiteralPath $requiredFile -PathType Leaf))
	{
		throw "Required installed production-gate input is missing: $requiredFile"
	}
}
if(-not (Test-Path -LiteralPath $projectPath -PathType Container))
{
	throw "Required production-gate project is missing: $projectPath"
}

[IO.Directory]::CreateDirectory($outputRoot) | Out-Null
# Keep the caller's preference file immutable. The production gate needs its own
# listener, so copy the normal editor preferences into the evidence directory
# and override only the MCP fields required by this installed-editor run.
$runPreferences = Get-Content -Raw -LiteralPath $preferencesPath |
	ConvertFrom-Json
$runPreferences | Add-Member -NotePropertyName editorMcpEnabled `
	-NotePropertyValue $true -Force
$runPreferences | Add-Member -NotePropertyName editorMcpListenPort `
	-NotePropertyValue $McpPort -Force
$runPreferences | Add-Member -NotePropertyName editorMcpWriteModeDefault `
	-NotePropertyValue 'writes_enabled' -Force
$runPreferences | Add-Member -NotePropertyName editorMcpRecentSession `
	-NotePropertyValue 'virtual-geometry-production-gate' -Force
$preferencesPath = Join-Path $outputRoot 'editor-preferences.json'
Write-JsonFile $preferencesPath $runPreferences
$endpoint = "http://127.0.0.1:$McpPort/mcp"
$sequence = 0
$transcriptPath = Join-Path $outputRoot 'mcp-transcript.jsonl'
[IO.File]::WriteAllText($transcriptPath, '', $utf8NoBom)

function Add-Transcript($Value)
{
	[IO.File]::AppendAllText(
		$transcriptPath,
		($Value | ConvertTo-Json -Depth 64 -Compress) + "`n",
		$utf8NoBom)
}

function Invoke-McpRpc([string]$Method, $Parameters = @{})
{
	$script:sequence++
	$request = [ordered]@{
		jsonrpc = '2.0'
		id = $script:sequence
		method = $Method
		params = $Parameters
	}
	try
	{
		$reply = (
			Invoke-WebRequest `
				-UseBasicParsing `
				-Uri $endpoint `
				-Method Post `
				-ContentType 'application/json' `
				-Body ($request | ConvertTo-Json -Depth 64 -Compress) `
				-TimeoutSec 30
		).Content | ConvertFrom-Json
		Add-Transcript ([ordered]@{
			timestamp = [DateTime]::UtcNow.ToString('o')
			request = $request
			response = $reply
		})
		return $reply
	}
	catch
	{
		Add-Transcript ([ordered]@{
			timestamp = [DateTime]::UtcNow.ToString('o')
			request = $request
			error = $_.Exception.Message
		})
		throw
	}
}

function Get-McpEnvelope($Reply, [string]$Tool)
{
	$errorValue = Get-OptionalProperty $Reply 'error'
	if($null -ne $errorValue)
	{
		throw "MCP returned an error for '$Tool'."
	}
	$result = Get-OptionalProperty $Reply 'result'
	$content = @((Get-OptionalProperty $result 'content'))
	$textItems = @($content | Where-Object {
		[string](Get-OptionalProperty $_ 'type') -eq 'text'
	})
	if($textItems.Count -ne 1)
	{
		throw "MCP tool '$Tool' did not return one typed JSON envelope."
	}
	return [string](Get-OptionalProperty $textItems[0] 'text') | ConvertFrom-Json
}

function Invoke-McpTool([string]$Name, $Arguments = @{})
{
	$reply = Invoke-McpRpc 'tools/call' ([ordered]@{
		name = $Name
		arguments = $Arguments
	})
	$envelope = Get-McpEnvelope $reply $Name
	return [ordered]@{
		reply = $reply
		envelope = $envelope
	}
}

function Invoke-McpToolWithReadinessRetry([string]$Name, $Arguments = @{})
{
	# The installed editor can accept MCP calls a few frames before the first
	# Game-view stats publication. Retry only that explicit transient state;
	# every other rejection remains immediate and fail-closed.
	for($attempt = 0; $attempt -lt 20; ++$attempt)
	{
		$result = Invoke-McpTool $Name $Arguments
		$ok = [bool](Get-OptionalProperty $result.envelope 'ok')
		$errorValue = Get-OptionalProperty $result.envelope 'error'
		$category = [string](Get-OptionalProperty $errorValue 'category')
		if($ok -or $Name -ne 'renderer.virtualGeometry.stats' -or
			$category -ne 'missing_target')
		{
			return $result
		}
		Start-Sleep -Milliseconds 250
	}
	return Invoke-McpTool $Name $Arguments
}

function Get-DefaultToolArguments([string]$Tool, $PriorOperation)
{
	switch($Tool)
	{
		'renderer.virtualGeometry.capabilityEvidence'
		{
			return [ordered]@{ viewType = 'Game' }
		}
		'renderer.virtualGeometry.stats'
		{
			return [ordered]@{ viewType = 'Game' }
		}
		'asset.virtualGeometry.inspect'
		{
			return [ordered]@{
				# Inspection resolves the source asset through the asset
				# database, so the contract calls this selector `asset`.
				asset = $Asset
				includeRuntimeState = $true
				viewType = 'Game'
			}
		}
		'asset.virtualGeometry.rebuild'
		{
			return [ordered]@{ target = $Asset }
		}
		'script.compile'
		{
			# Virtual-geometry rebuild is intentionally synchronous at the
			# protected asset-registration boundary. Start a real durable compile
			# job for the production-readiness cancellation exercise instead of inventing a
			# rebuild job identifier that the editor never published.
			return [ordered]@{
				profile = 'Debug'
			correlationId = 'virtual-geometry-production-gate-cancel'
			}
		}
		'jobs.cancel'
		{
			$jobId = Get-OptionalProperty (
				Get-OptionalProperty $PriorOperation 'data') 'jobId'
			if($null -eq $jobId)
			{
				return $null
			}
			return [ordered]@{ jobId = $jobId }
		}
		default
		{
			return [ordered]@{}
		}
	}
}

function Save-Capture(
	[string]$Tool,
	[string]$OutputPath)
{
	$arguments = if($Tool -eq 'camera.capture_custom')
	{
		[ordered]@{
			name = 'Virtual Geometry installed production gate'
			position = @(0.0, 0.5, 4.0)
			target = @(0.0, 0.5, 0.0)
			up = @(0.0, 1.0, 0.0)
			projection = 'perspective'
			verticalFovDegrees = 60.0
			nearPlane = 0.1
			farPlane = 100.0
			width = 480
			height = 270
			format = 'image/png'
			freshness = 'nextFrame'
			includeOverlays = $false
		}
	}
	else
	{
		[ordered]@{
			width = 480
			height = 270
			format = 'image/png'
			freshness = 'nextFrame'
			includeOverlays = $false
			transparentBackground = $false
		}
	}
	$result = Invoke-McpTool $Tool $arguments
	$content = @((Get-OptionalProperty (
		Get-OptionalProperty $result.reply 'result') 'content'))
	$imageItems = @($content | Where-Object {
		[string](Get-OptionalProperty $_ 'type') -eq 'image' -and
		[string](Get-OptionalProperty $_ 'mimeType') -eq 'image/png'
	})
	if($imageItems.Count -ne 1)
	{
		throw "Capture '$Tool' did not return one PNG image."
	}
	$bytes = [Convert]::FromBase64String(
		[string](Get-OptionalProperty $imageItems[0] 'data'))
	if($bytes.Length -lt 8 -or $bytes[0] -ne 0x89 -or
		$bytes[1] -ne 0x50 -or $bytes[2] -ne 0x4e -or
		$bytes[3] -ne 0x47)
	{
		throw "Capture '$Tool' returned invalid PNG data."
	}
	$parent = Split-Path -Parent $OutputPath
	[IO.Directory]::CreateDirectory($parent) | Out-Null
	[IO.File]::WriteAllBytes($OutputPath, $bytes)
	return $result.envelope
}

$editor = $null
$stdoutTask = $null
$stderrTask = $null
$scenarioRows = @()
try
{
	$start = [Diagnostics.ProcessStartInfo]::new()
	$start.FileName = $editorPath
	$start.Arguments = "--project `"$projectPath`""
	$start.WorkingDirectory = Split-Path -Parent $editorPath
	$start.UseShellExecute = $false
	$start.RedirectStandardOutput = $true
	$start.RedirectStandardError = $true
	$start.EnvironmentVariables['TERRAENGINE_EDITOR_PREFS_SESSION_PATH'] =
		$preferencesPath
	$start.EnvironmentVariables['TERRAENGINE_SUPPRESS_PROJECT_RECENTS'] = '1'
	$editor = [Diagnostics.Process]::Start($start)
	if($null -eq $editor)
	{
		throw 'Installed editor failed to start.'
	}
	$stdoutTask = $editor.StandardOutput.ReadToEndAsync()
	$stderrTask = $editor.StandardError.ReadToEndAsync()

	$deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
	$availableTools = @()
	while([DateTime]::UtcNow -lt $deadline)
	{
		$editor.Refresh()
		if($editor.HasExited)
		{
			throw "Installed editor exited before MCP readiness with code $($editor.ExitCode)."
		}
		try
		{
			$initialize = Invoke-McpRpc 'initialize' ([ordered]@{
				protocolVersion = '2025-03-26'
				clientInfo = [ordered]@{
					name = 'virtual-geometry-production-gate'
					version = '1'
				}
			})
			if([string]$initialize.result.serverInfo.name -eq
				'TerraEngine Editor')
			{
				$descriptors = @((Invoke-McpRpc 'tools/list').result.tools)
				$availableTools = @($descriptors | ForEach-Object {
					[string]$_.name
				})
				if($availableTools.Count -ne 0)
				{
					break
				}
			}
		}
		catch
		{
			# Startup probes are expected to fail until the listener is bound.
		}
		Start-Sleep -Milliseconds 500
	}
	if($availableTools.Count -eq 0)
	{
		throw 'Installed editor MCP did not become ready.'
	}

	# The default import-validation fixture startup scene intentionally contains no renderable
	# hero. Instantiate its generated prefab so custom-camera captures exercise
	# real geometry instead of accepting a syntactically valid black image.
	# Other projects can either supply their own generated prefab through the
	# parameters above, or skip this optional setup when their startup scene is
	# already renderable. The defaults preserve the established production gate.
	if(-not $SkipFixturePrefabSetup -and $availableTools -contains 'prefab.instantiate')
	{
		$setupResult = Invoke-McpTool 'prefab.instantiate' ([ordered]@{
			prefab = $FixturePrefab
			name = $FixturePrefabName
		})
		Write-JsonFile (Join-Path $outputRoot 'production-gate-setup.json') (
			[ordered]@{
				status = [string](Get-OptionalProperty $setupResult.envelope 'status')
				operation = 'prefab.instantiate'
				envelope = $setupResult.envelope
			})
	}

	foreach($scenario in $scenarios)
	{
		$scenarioId = [string](Get-OptionalProperty $scenario 'id')
		$scenarioOutput = Join-Path $outputRoot $scenarioId
		[IO.Directory]::CreateDirectory($scenarioOutput) | Out-Null
		$requiredTools = @(
			(Get-OptionalProperty $scenario 'requiredTools') |
				Where-Object { $null -ne $_ })
		$missingTools = @($requiredTools | Where-Object {
			$availableTools -notcontains [string]$_
		})
		$operationRows = @()
		$priorEnvelope = $null
		$operations = @(
			(Get-OptionalProperty $scenario 'operations') |
				Where-Object { $null -ne $_ })
		foreach($operation in $operations)
		{
			$tool = [string](Get-OptionalProperty $operation 'tool')
			$operationId = [string](Get-OptionalProperty $operation 'id')
			$arguments = Get-DefaultToolArguments $tool $priorEnvelope
			if($null -eq $arguments)
			{
				$operationRows += [ordered]@{
					id = $operationId
					tool = $tool
					status = 'unavailable'
					reason = 'A prior operation did not publish the required identity.'
				}
				continue
			}
			try
			{
				$result = Invoke-McpToolWithReadinessRetry $tool $arguments
				$priorEnvelope = $result.envelope
				$operationEvidence = [string](Get-OptionalProperty (
					$operation) 'evidence')
				if([string]::IsNullOrWhiteSpace($operationEvidence))
				{
					$operationEvidence = "editor/$operationId.json"
				}
				$artifactPath = Join-Path $scenarioOutput $operationEvidence
				Write-JsonFile $artifactPath $result.envelope
				$operationRows += [ordered]@{
					id = $operationId
					tool = $tool
					status = if([bool](Get-OptionalProperty (
						$result.envelope) 'ok')) { 'executed' } else { 'rejected' }
					artifact = Get-FileIdentity $artifactPath
				}
			}
			catch
			{
				$operationRows += [ordered]@{
					id = $operationId
					tool = $tool
					status = 'failed'
					reason = $_.Exception.Message
				}
			}
		}

		$captureRows = @()
		$captures = @(
			(Get-OptionalProperty $scenario 'captures') |
				Where-Object { $null -ne $_ })
		foreach($capture in $captures)
		{
			$captureId = [string](Get-OptionalProperty $capture 'id')
			$capturePath = Join-Path $scenarioOutput (
				[string](Get-OptionalProperty $capture 'evidence'))
			try
			{
				$envelope = Save-Capture (
					[string](Get-OptionalProperty $capture 'tool')) $capturePath
				$captureRows += [ordered]@{
					id = $captureId
					status = 'captured'
					artifact = Get-FileIdentity $capturePath
					envelopeStatus = [string](Get-OptionalProperty $envelope 'status')
				}
			}
			catch
			{
				$captureRows += [ordered]@{
					id = $captureId
					status = 'failed'
					reason = $_.Exception.Message
				}
			}
		}

		# Scenario manifests name the artifacts needed for an auditable result.
		# Verify them after every operation/capture so a successful MCP envelope
		# cannot be mistaken for a complete installed-tree gate.
		$requiredArtifacts = @(
			(Get-OptionalProperty $scenario 'requiredArtifacts') |
				Where-Object { $null -ne $_ })
		# Every manifest-declared output is evidence, even when an author omitted
		# it from requiredArtifacts. Merge those paths into the audit so an
		# operation such as production-readiness cancellation cannot silently go unchecked.
		$declaredOutputs = @(
			@($operations | ForEach-Object {
				$path = [string](Get-OptionalProperty $_ 'evidence')
				if([string]::IsNullOrWhiteSpace($path))
				{
					$path = "editor/$([string](Get-OptionalProperty $_ 'id')).json"
				}
				[pscustomobject]@{ path = $path; kind = 'editor-envelope' }
			}) +
			@($captures | ForEach-Object {
				[pscustomobject]@{
					path = [string](Get-OptionalProperty $_ 'evidence')
					kind = 'png'
				}
			}))
		$knownArtifactPaths = @($requiredArtifacts | ForEach-Object {
			[string](Get-OptionalProperty $_ 'path')
		})
		$requiredArtifacts += @($declaredOutputs | Where-Object {
			$_.path -notin $knownArtifactPaths
		})
		$artifactRows = @($requiredArtifacts | ForEach-Object {
			Test-ScenarioArtifact $scenarioOutput $_
		})
		$missingArtifacts = @($artifactRows | Where-Object {
			$_.status -ne 'accepted'
		} | ForEach-Object { $_.path })

		$scenarioSucceeded = $missingTools.Count -eq 0 -and
			@($operationRows | Where-Object {
				$_.status -notin @('executed', 'unavailable')
			}).Count -eq 0 -and
			@($captureRows | Where-Object {
				$_.status -ne 'captured'
			}).Count -eq 0 -and
			$missingArtifacts.Count -eq 0
		$row = [ordered]@{
			scenario = $scenarioId
			status = if($scenarioSucceeded) { 'executed' } else { 'incomplete' }
			missingTools = $missingTools
			operations = $operationRows
			captures = $captureRows
			requiredArtifacts = $requiredArtifacts
			artifacts = $artifactRows
			missingArtifacts = $missingArtifacts
			restoration = [ordered]@{
				status = 'editor-quit-requested'
				note = 'The installed editor is closed in the finally block; scenario-owned project files are not rewritten by this runner.'
			}
			unavailablePolicy = [string](Get-OptionalProperty (
				$scenario) 'unavailablePolicy')
		note = 'Executed means evidence was collected; capability and benchmark claims remain fail-closed until their assertions pass.'
		}
		Write-JsonFile (Join-Path $scenarioOutput 'scenario-result.json') $row
		$scenarioRows += $row
	}
}
finally
{
	if($null -ne $editor)
	{
		$editor.Refresh()
		if(-not $editor.HasExited)
		{
			try
			{
				$null = Invoke-McpTool 'editor.quit' @{}
				$null = $editor.WaitForExit(10000)
			}
			catch
			{
				# A forced stop below remains local to this installed test process.
			}
		}
		$editor.Refresh()
		if(-not $editor.HasExited)
		{
			# Windows PowerShell exposes only the parameterless overload on
			# older .NET runtimes, and this editor process owns no child helper
			# that needs recursive termination at this point.
			$editor.Kill()
			$editor.WaitForExit()
		}
	}
	if($null -ne $stdoutTask)
	{
		[IO.File]::WriteAllText(
			(Join-Path $outputRoot 'installed-editor.stdout.log'),
			$stdoutTask.GetAwaiter().GetResult(),
			$utf8NoBom)
	}
	if($null -ne $stderrTask)
	{
		[IO.File]::WriteAllText(
			(Join-Path $outputRoot 'installed-editor.stderr.log'),
			$stderrTask.GetAwaiter().GetResult(),
			$utf8NoBom)
	}
}

$overall = [ordered]@{
	schemaVersion = 2
	status = if($releaseGateStatus -in @('accepted', 'skipped') -and
		@($scenarioRows | Where-Object {
		$_.status -ne 'executed'
	}).Count -eq 0) { 'executed' } else { 'incomplete' }
	generatedUtc = [DateTime]::UtcNow.ToString('o')
	installedEditor = $editorPath
	project = $projectPath
	backendScope = @('Vulkan')
	releaseGates = [ordered]@{
		status = $releaseGateStatus
		artifact = if(Test-Path -LiteralPath (
			Join-Path $outputRoot 'release-gates.json') -PathType Leaf)
		{
			Get-FileIdentity (Join-Path $outputRoot 'release-gates.json')
		}
		else
		{
			$null
		}
	}
	scenarios = $scenarioRows
	note = 'No unavailable or unmeasured capability is promoted to a pass.'
}
Write-JsonFile (Join-Path $outputRoot 'production-gate.json') $overall
$overall | ConvertTo-Json -Depth 64
if($overall.status -ne 'executed')
{
	exit 2
}
