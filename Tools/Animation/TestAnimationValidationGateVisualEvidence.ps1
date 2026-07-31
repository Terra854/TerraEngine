# TestAnimationValidationGateVisualEvidence.ps1 - Focused pixel-evidence self-test.
#
# This script imports only reviewed helper functions from the phase gate, opens
# one disposable native WinForms window, and verifies fail-closed behavior. It
# never launches TerraEngine, builds a target, or mutates project evidence.

[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-TestCondition {
	param(
		[Parameter(Mandatory = $true)][bool]$Condition,
		[Parameter(Mandatory = $true)][string]$Message
	)
	if (-not $Condition) { throw $Message }
}

function New-TestScenarioWithFixtureOverlays {
	param(
		[Parameter(Mandatory = $true)]$BaseScenario,
		[Parameter(Mandatory = $true)][AllowEmptyCollection()]$FixtureOverlays
	)
	# JSON round-tripping produces the same PSCustomObject shape consumed by the
	# production parser while keeping each negative test isolated from the base.
	$copy = $BaseScenario | ConvertTo-Json -Depth 30 | ConvertFrom-Json
	$copy | Add-Member -NotePropertyName fixtureOverlays -NotePropertyValue ([object[]]@($FixtureOverlays)) -Force
	return $copy
}

function Assert-FixtureOverlayScenarioRejected {
	param(
		[Parameter(Mandatory = $true)]$Scenario,
		[Parameter(Mandatory = $true)][string]$ScenarioPath,
		[Parameter(Mandatory = $true)][string]$RepoRoot,
		[Parameter(Mandatory = $true)][string]$ExpectedMessage,
		[Parameter(Mandatory = $true)][string]$FailureMessage
	)
	[System.IO.File]::WriteAllText($ScenarioPath, ($Scenario | ConvertTo-Json -Depth 30), [System.Text.UTF8Encoding]::new($false))
	$rejected = $false
	try {
		[void](Read-ScenarioManifest -Path $ScenarioPath -ExpectedPhase 'self-test' -RepoRoot $RepoRoot)
	}
	catch {
		$rejected = $_.Exception.Message -match $ExpectedMessage
	}
	Assert-TestCondition -Condition $rejected -Message $FailureMessage
}

function Assert-ArgumentBindingScenarioRejected {
	param(
		[Parameter(Mandatory = $true)]$Scenario,
		[Parameter(Mandatory = $true)][string]$ScenarioPath,
		[Parameter(Mandatory = $true)][string]$RepoRoot,
		[Parameter(Mandatory = $true)][string]$ExpectedMessage,
		[Parameter(Mandatory = $true)][string]$FailureMessage
	)
	[System.IO.File]::WriteAllText($ScenarioPath, ($Scenario | ConvertTo-Json -Depth 40), [System.Text.UTF8Encoding]::new($false))
	$rejected = $false
	try {
		[void](Read-ScenarioManifest -Path $ScenarioPath -ExpectedPhase 'self-test' -RepoRoot $RepoRoot)
	}
	catch {
		$rejected = $_.Exception.Message -match $ExpectedMessage
	}
	Assert-TestCondition -Condition $rejected -Message $FailureMessage
}

function Assert-RecordingOutputScenarioRejected {
	param(
		[Parameter(Mandatory = $true)]$Scenario,
		[Parameter(Mandatory = $true)][string]$ScenarioPath,
		[Parameter(Mandatory = $true)][string]$RepoRoot,
		[Parameter(Mandatory = $true)][string]$ExpectedMessage,
		[Parameter(Mandatory = $true)][string]$FailureMessage
	)
	[System.IO.File]::WriteAllText($ScenarioPath, ($Scenario | ConvertTo-Json -Depth 50), [System.Text.UTF8Encoding]::new($false))
	$rejected = $false
	try {
		[void](Read-ScenarioManifest -Path $ScenarioPath -ExpectedPhase 'self-test' -RepoRoot $RepoRoot)
	}
	catch {
		$rejected = $_.Exception.Message -match $ExpectedMessage
	}
	Assert-TestCondition -Condition $rejected -Message $FailureMessage
}

function Assert-OperationEvidenceRejected {
	param(
		[Parameter(Mandatory = $true)]$Evidence,
		[Parameter(Mandatory = $true)][string]$EvidencePath,
		[Parameter(Mandatory = $true)]$Scenario,
		[Parameter(Mandatory = $true)][string]$RunRoot,
		[Parameter(Mandatory = $true)][string]$ExpectedMessage,
		[Parameter(Mandatory = $true)][string]$FailureMessage
	)
	[System.IO.File]::WriteAllText($EvidencePath, ($Evidence | ConvertTo-Json -Depth 80), [System.Text.UTF8Encoding]::new($false))
	$rejected = $false
	$actualMessage = $null
	try {
		[void](Read-AndValidateOperationEvidence -Path $EvidencePath -Scenario $Scenario -RunRoot $RunRoot)
	}
	catch {
		$actualMessage = $_.Exception.Message
		$rejected = $actualMessage -match $ExpectedMessage
	}
	Assert-TestCondition -Condition $rejected -Message "$FailureMessage Actual rejection: '$actualMessage'."
}

function Assert-PlayModeEvidenceRejected {
	param(
		[Parameter(Mandatory = $true)]$Evidence,
		[Parameter(Mandatory = $true)][string]$EvidencePath,
		[Parameter(Mandatory = $true)]$Scenario,
		[Parameter(Mandatory = $true)][string]$RunRoot,
		[Parameter(Mandatory = $true)][string]$ProjectRoot,
		[Parameter(Mandatory = $true)][string]$ExpectedMessage,
		[Parameter(Mandatory = $true)][string]$FailureMessage
	)
	[System.IO.File]::WriteAllText($EvidencePath, ($Evidence | ConvertTo-Json -Depth 100), [System.Text.UTF8Encoding]::new($false))
	$rejected = $false
	$actualMessage = $null
	try {
		[void](Read-AndValidatePlayModeEvidence -Path $EvidencePath -Scenario $Scenario -RunRoot $RunRoot -ProjectRoot $ProjectRoot)
	}
	catch {
		$actualMessage = $_.Exception.Message
		$rejected = $actualMessage -match $ExpectedMessage
	}
	Assert-TestCondition -Condition $rejected -Message "$FailureMessage Actual rejection: '$actualMessage'."
}

function New-TestPng {
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[Parameter(Mandatory = $true)][System.Drawing.Color]$Background,
		[System.Drawing.Color]$ChangedPixel = [System.Drawing.Color]::Empty,
		[int]$Width = 4,
		[int]$Height = 4
	)
	$bitmap = [System.Drawing.Bitmap]::new($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
	$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
	try {
		$graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
		$graphics.Clear($Background)
		if (-not $ChangedPixel.IsEmpty) { $bitmap.SetPixel(1, 1, $ChangedPixel) }
		$bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
	}
	finally {
		$graphics.Dispose()
		$bitmap.Dispose()
	}
	return [System.IO.File]::ReadAllBytes($Path)
}

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$runnerPath = Join-Path $repoRoot 'Tools/Animation/RunAnimationValidationGate.ps1'
$tokens = $null
$parseErrors = $null
$runnerAst = [System.Management.Automation.Language.Parser]::ParseFile($runnerPath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -gt 0) { throw "Phase-gate source did not parse: $($parseErrors[0].Message)" }

# Dot-source only the production functions under test. Extracting their AST
# extents avoids executing the phase gate's build, install, or editor main path.
$requiredFunctions = @(
	'Get-AbsolutePath', 'Assert-PathBelow', 'Get-OptionalProperty',
	'Get-RequiredJsonProperty', 'Test-IsJsonArray', 'Assert-NoDuplicateJsonObjectKeys', 'ConvertFrom-JsonPreservingArrays',
	'Get-RequiredPreservedJsonMember', 'Get-RelativeForwardPath', 'Write-Utf8Text',
	'Get-Sha256', 'Get-VulkanValidationLogEvidence', 'Get-StringSha256', 'Get-FileInventory', 'Get-InventoryDigest', 'Assert-NoReparsePoints',
	'Copy-IsolatedFixture', 'Assert-FixtureOverlayRelativePath', 'Test-FixtureOverlayPathOverlap',
	'Assert-FixtureOverlayDestinationPathSafe', 'Get-FixtureOverlaySourceState', 'Copy-FixtureOverlays',
	'Get-NestedValue', 'Test-NestedJsonFieldPresent', 'Assert-ArgumentBindingObjectPath', 'Get-CaseSensitiveJsonPathValue',
	'Set-CaseSensitiveJsonNullPathValue', 'Copy-JsonValuePreservingArrays', 'ConvertTo-CanonicalJsonNode',
	'Get-CanonicalJsonText', 'Get-CanonicalJsonSha256', 'Get-OrdinalSortedStrings',
	'Assert-ExactPreservedJsonObjectKeys', 'Get-JsonScalarType',
	'Assert-ArgumentBindingValueType', 'Get-ScenarioInvocationArguments', 'Resolve-ScenarioArgumentBindings',
	'Initialize-ScenarioOperationApprovals', 'Initialize-ScenarioArgumentBindings', 'Assert-PlayModeRecordingOutputPath',
	'Initialize-ScenarioPlayModeRecordingOutputs', 'Get-FirstEvidenceValue',
	'Read-FinalAnimationPerformanceEvidence', 'Assert-OperationExpectation', 'New-ScenarioExpectationRecord', 'Read-AndValidateOperationEvidence',
	'Get-ToolEnvelope', 'Assert-ExactRuntimeJsonObjectKeys', 'Read-McpJsonResource',
	'Get-PendingApprovalResourceEntry', 'Get-RecentApprovalResourceEntry', 'Invoke-ApprovedScenarioOperation',
	'Read-McpTranscriptForOperationEvidence', 'Get-TranscriptJsonResourceAtSequence',
	'Read-AndValidateScenarioApprovalEvidence',
	'Get-PngDimensions', 'Get-PngVisualStatistics', 'Get-CaptureVisualAssessment',
	'Initialize-NativeValidationBridge', 'Install-VerifiedDefaultEditorLayout',
	'Get-ScenarioPreservationDigest', 'Get-StableScenarioPreservationDigest',
	'ConvertTo-ExtendedLengthPath', 'Get-LongPathFileState', 'Get-PlayModeSnapshotDigest',
	'Get-PlayModeProjectSnapshot', 'Start-PlayModeRecordingOutputContract',
	'Compare-PlayModeProjectSnapshots', 'Complete-PlayModeRecordingOutputContract',
	'Get-AuthoredProjectState', 'Get-StableAuthoredProjectState', 'Assert-PlayModeSnapshotRelativePath',
	'Assert-PlayModeSnapshotIntegrity', 'Read-PlayModeSnapshotEvidence',
	'Get-TranscriptToolEnvelopeAtSequence', 'Read-AndValidatePlayModeEvidence',
	'Read-ScenarioManifest', 'Get-CameraEvidenceValidation', 'Read-ManualActionRecords',
	'Get-NativeInputCapability', 'Invoke-GateOwnedInput', 'New-GateNativeActionEvidence',
	'Read-AndValidateManualEvidence',
	'New-AcceptanceMatrixUpdate', 'Apply-AcceptanceMatrixUpdate', 'Restore-AcceptanceMatrixUpdate'
)
$functionDefinitions = @($runnerAst.FindAll({
	param($node)
	return $node -is [System.Management.Automation.Language.FunctionDefinitionAst]
}, $true))
foreach ($functionName in $requiredFunctions) {
	$definition = @($functionDefinitions | Where-Object { $_.Name -eq $functionName })
	if ($definition.Count -ne 1) { throw "Expected one production definition for '$functionName'." }
	. ([scriptblock]::Create($definition[0].Extent.Text))
}

# The live path must create reviewed pre-Play state before manual inspection,
# then save and hash the operator-visible authored state before entering Play.
$runnerSource = [System.IO.File]::ReadAllText($runnerPath)
$sequenceMarkers = @(
	"Write-JsonFile -Path (Join-Path `$runRoot 'editor/manual-checklist.json')",
	"Invoke-ScenarioStage -Scenario `$scenario -Stage 'prePlay'",
	'`$manualResult = Wait-AndValidateClickEvidence',
	'`$initialPlay = Invoke-McpTool',
	'`$manualSceneSave = Invoke-McpTool',
	'`$authoredBeforeStability = Get-StableAuthoredProjectState',
	'`$enter = Invoke-McpTool'
)
$previousIndex = -1
foreach ($marker in $sequenceMarkers) {
	$markerText = $marker.Replace('`$', '$')
	$index = $runnerSource.IndexOf($markerText, $previousIndex + 1, [System.StringComparison]::Ordinal)
	Assert-TestCondition -Condition ($index -gt $previousIndex) -Message "Live evidence sequence is missing or misorders '$markerText'."
	$previousIndex = $index
}

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Initialize-NativeValidationBridge
$initialThreadDpiContext = [TerraEngine.Animation.Validation.NativeWindowEvidence]::GetCurrentThreadDpiAwarenessContextName()
$script:ReferenceWidth = 1920
$script:ReferenceHeight = 1080
$script:Utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$script:DeferredPerformanceAcceptanceRows = @()
$script:DeferredPerformanceSuccessCriteria = @('animation-outcome-creator-import-representative-animated-gltf-fbx-package-obtain-correctly-referenced-playable-prefab')
$script:FeatureName = 'complete-animation-system'
$script:MaximumFixtureOverlayCount = 16
$script:MaximumFixtureOverlayFiles = 4096
$script:MaximumFixtureOverlayBytes = [UInt64]1073741824
$script:MaximumArgumentBindingsPerOperation = 16
$script:MaximumArgumentBindingsPerScenario = 160
$script:MaximumArgumentBindingPathSegments = 8
$script:HighImpactScenarioTools = @(
	'animation.asset.import',
	'animation.asset.reimport',
	'animation.motion_database.build',
	'animation.recording.start',
	'animation.recording.stop'
)
$script:ApprovalPollIntervalMilliseconds = 1
$script:MaximumPlayModeRecordingOutputs = 16
$script:MaximumPlayModeRecordingOutputBytes = [UInt64]536870912
$script:MaximumPlayModeSnapshotFiles = 100000
$script:MaximumPlayModeSnapshotDirectories = 50000
$script:MaximumPlayModeSnapshotBytes = [UInt64]34359738368
$script:PlayModeAuthoredExcludedTopLevelDirectories = @('Build', 'Library', 'Logs', 'Temp', 'UserSettings', 'bin', 'obj', '.git', 'out')
$script:ScenarioPreservationStableSamples = 3
$script:ScenarioPreservationStableIntervalMilliseconds = 1
$script:ScenarioPreservationStabilizationTimeoutMilliseconds = 1000
$temporaryParent = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\', '/')
$temporaryRoot = [System.IO.Path]::Combine($temporaryParent, "TerraEngine-AnimationVisualEvidence-$([Guid]::NewGuid().ToString('N'))")
$temporaryPrefix = $temporaryParent + [System.IO.Path]::DirectorySeparatorChar
if (-not $temporaryRoot.StartsWith($temporaryPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
	throw 'Visual-evidence self-test directory escaped the operating-system temporary directory.'
}
[System.IO.Directory]::CreateDirectory($temporaryRoot) | Out-Null

	# A malformed-reimport proof must not begin while the preceding valid import
	# still has ordinary watcher work pending. The helper accepts a byte-stable
	# package root and retains its stabilization provenance for operation evidence.
	$preservationProjectRoot = Join-Path $temporaryRoot 'preservation-project'
	$preservationRoot = Join-Path $preservationProjectRoot 'Assets/Models/Hero_Generated'
	[System.IO.Directory]::CreateDirectory($preservationRoot) | Out-Null
	[System.IO.File]::WriteAllText((Join-Path $preservationRoot 'hero.teprefab'), 'last-known-good', [System.Text.UTF8Encoding]::new($false))
	$stablePreservation = Get-StableScenarioPreservationDigest -ProjectRoot $preservationProjectRoot -RelativePath 'Assets/Models/Hero_Generated'
	$stablePreservationPassed = $stablePreservation.kind -eq 'directory' -and $stablePreservation.files -eq 1 -and $stablePreservation.stabilization.stableSamples -eq 3 -and @($stablePreservation.stabilization.observedDigests).Count -eq 3
	Assert-TestCondition -Condition $stablePreservationPassed -Message 'Stable last-known-good package root did not retain the required quiescence evidence.'

$evidenceForm = $null
$inputTestProcess = $null
$inputTestJob = $null
$jobTestProcess = $null
$jobTestObject = $null
$stabilityTestProcess = $null
$overlayJunctionPath = $null
try {
	$black = Get-PngVisualStatistics -Bytes (New-TestPng -Path (Join-Path $temporaryRoot 'black.png') -Background ([System.Drawing.Color]::Black))
	$uniformGreen = Get-PngVisualStatistics -Bytes (New-TestPng -Path (Join-Path $temporaryRoot 'green.png') -Background ([System.Drawing.Color]::Green))
	$varied = Get-PngVisualStatistics -Bytes (New-TestPng -Path (Join-Path $temporaryRoot 'varied.png') -Background ([System.Drawing.Color]::Black) -ChangedPixel ([System.Drawing.Color]::Red))
	$visibleCapture = [pscustomobject]@{ visualExpectation = [pscustomobject]@{ contentPolicy = 'visible-geometry-required' } }
	$emptyAllowedCapture = [pscustomobject]@{ visualExpectation = [pscustomobject]@{ contentPolicy = 'empty-scene-allowed' } }
	$blackAssessment = Get-CaptureVisualAssessment -Capture $visibleCapture -Statistics $black
	$greenAssessment = Get-CaptureVisualAssessment -Capture $visibleCapture -Statistics $uniformGreen
	$variedAssessment = Get-CaptureVisualAssessment -Capture $visibleCapture -Statistics $varied
	$emptyAssessment = Get-CaptureVisualAssessment -Capture $emptyAllowedCapture -Statistics $black

	Assert-TestCondition -Condition ([bool]$black.isSolidBlack -and [bool]$black.isRgbUniform) -Message 'Solid-black fixture was not classified as vacuous.'
	Assert-TestCondition -Condition (-not [bool]$blackAssessment.passed) -Message 'Visible-geometry policy accepted a solid-black fixture.'
	Assert-TestCondition -Condition ([bool]$uniformGreen.isRgbUniform -and -not [bool]$greenAssessment.passed) -Message 'Visible-geometry policy accepted a uniform nonblack fixture.'
	Assert-TestCondition -Condition (-not [bool]$varied.isRgbUniform -and [bool]$variedAssessment.passed) -Message 'Nonuniform visible-content fixture did not pass the pixel sanity check.'
	Assert-TestCondition -Condition ([bool]$emptyAssessment.passed) -Message 'Explicit empty-scene policy rejected the intended empty fixture.'
	Assert-TestCondition -Condition ([string]$black.pixelSha256 -cne [string]$varied.pixelSha256) -Message 'Canonical pixel digests did not distinguish different fixtures.'
	$cleanVulkanStdout = Join-Path $temporaryRoot 'vulkan-clean.stdout.log'
	$cleanVulkanStderr = Join-Path $temporaryRoot 'vulkan-clean.stderr.log'
	Write-Utf8Text -Path $cleanVulkanStdout -Value '[INFO][Vulkan] Renderer initialized without validation errors.'
	Write-Utf8Text -Path $cleanVulkanStderr -Value ''
	$cleanVulkanEvidence = Get-VulkanValidationLogEvidence -StdoutPath $cleanVulkanStdout -StderrPath $cleanVulkanStderr
	Assert-TestCondition -Condition ([bool]$cleanVulkanEvidence.passed -and [int]$cleanVulkanEvidence.errorLineCount -eq 0) -Message 'A clean Vulkan log was rejected.'
	$invalidVulkanStdout = Join-Path $temporaryRoot 'vulkan-invalid.stdout.log'
	Write-Utf8Text -Path $invalidVulkanStdout -Value '[ERROR][Vulkan] Invalid descriptor. VUID-vkCmdDrawMeshTasksIndirectEXT-None-08114'
	$invalidVulkanEvidence = Get-VulkanValidationLogEvidence -StdoutPath $invalidVulkanStdout -StderrPath $cleanVulkanStderr
	Assert-TestCondition -Condition (
		-not [bool]$invalidVulkanEvidence.passed -and
		[int]$invalidVulkanEvidence.errorLineCount -eq 1 -and
		@($invalidVulkanEvidence.vuidIds) -ccontains 'VUID-vkCmdDrawMeshTasksIndirectEXT-None-08114'
	) -Message 'A Vulkan validation-layer descriptor error was not rejected with its VUID.'

	$scenarioPath = Join-Path $temporaryRoot 'scenario.json'
	$scenario = [ordered]@{
		schemaVersion = 2
		phase = 'self-test'
		requiredTools = @()
		requiredEvidence = @('editor/mcp-operations.json', 'editor/operations/inspect.json')
		operations = @(
			[ordered]@{
				id = 'inspect'
				stage = 'prePlay'
				order = 4
				tool = 'diagnostics.get_summary'
				evidencePath = 'editor/operations/inspect.json'
				expect = [ordered]@{ ok = $true; requiredFields = @('data') }
			}
		)
		captures = @(
			[ordered]@{ id = 'scene'; view = 'SceneView'; stage = 'prePlay'; order = 1; tool = 'camera.capture_custom'; arguments = [ordered]@{} },
			[ordered]@{ id = 'game'; view = 'GameView'; stage = 'play'; order = 2; tool = 'game_view.capture'; arguments = [ordered]@{} },
			[ordered]@{ id = 'ingame'; view = 'InGameCamera'; stage = 'postPlay'; order = 3; tool = 'camera.capture_object'; arguments = [ordered]@{} }
		)
		visualExpectations = @(
			[ordered]@{ captureId = 'scene'; contentPolicy = 'visible-geometry-required' },
			[ordered]@{ captureId = 'game'; contentPolicy = 'visible-geometry-required' },
			[ordered]@{ captureId = 'ingame'; contentPolicy = 'visible-geometry-required' }
		)
		visualIdentityExceptions = @(
			[ordered]@{ captureIds = @('game', 'ingame'); reason = 'Game and authored in-game views intentionally share one active camera for parity validation.' }
		)
		manualChecklist = @(
			[ordered]@{ id = 'mouse'; input = 'mouse'; required = $true; instruction = 'Click the first target and then click the second target.' },
			[ordered]@{ id = 'keyboard'; input = 'keyboard'; required = $true; instruction = 'keyboard' }
		)
	}
	[System.IO.File]::WriteAllText($scenarioPath, ($scenario | ConvertTo-Json -Depth 20), [System.Text.UTF8Encoding]::new($false))
	$validatedScenario = Read-ScenarioManifest -Path $scenarioPath -ExpectedPhase 'self-test'
	Assert-TestCondition -Condition ($null -ne $validatedScenario.captures[0].visualExpectation) -Message 'Valid scenario did not bind its per-capture expectation.'
	Assert-TestCondition -Condition ([int]$validatedScenario.manualChecklist[0].minimumInputEvents -eq 2) -Message 'Multi-action checklist prose did not derive a multi-event minimum.'
	$emptyDiagnosticsEnvelope = '{"data":{"diagnostics":[]}}' | ConvertFrom-Json
	Assert-TestCondition -Condition (
		(Test-NestedJsonFieldPresent -Object $emptyDiagnosticsEnvelope -Path 'data.diagnostics') -and
		-not (Test-NestedJsonFieldPresent -Object $emptyDiagnosticsEnvelope -Path 'data.missing')
	) -Message 'An explicitly present empty JSON array was misclassified as a missing required result field.'
	# Failed operations commonly constrain only ok/errorCategory. This verifies
	# that omitting the optional requiredFields member does not create a bogus
	# empty-path validation request.
	$expectedFailureEnvelope = '{"ok":false,"error":{"category":"validation"}}' | ConvertFrom-Json
	$expectedFailure = '{"ok":false,"errorCategory":"Validation"}' | ConvertFrom-Json
	Assert-OperationExpectation -Envelope $expectedFailureEnvelope -Expectation $expectedFailure -Id 'expected-validation-failure'

	$explicitMinimumScenario = $scenario | ConvertTo-Json -Depth 20 | ConvertFrom-Json
	$explicitMinimumScenario.manualChecklist[0] | Add-Member -NotePropertyName minimumInputEvents -NotePropertyValue 8
	[System.IO.File]::WriteAllText($scenarioPath, ($explicitMinimumScenario | ConvertTo-Json -Depth 20), [System.Text.UTF8Encoding]::new($false))
	$explicitMinimumValidated = Read-ScenarioManifest -Path $scenarioPath -ExpectedPhase 'self-test'
	Assert-TestCondition -Condition ([int]$explicitMinimumValidated.manualChecklist[0].minimumInputEvents -eq 8) -Message 'Explicit minimumInputEvents was overwritten by heuristic derivation.'
	foreach ($invalidMinimum in @(0, 2.5, 129)) {
		$invalidMinimumScenario = $scenario | ConvertTo-Json -Depth 20 | ConvertFrom-Json
		$invalidMinimumScenario.manualChecklist[0] | Add-Member -NotePropertyName minimumInputEvents -NotePropertyValue $invalidMinimum
		[System.IO.File]::WriteAllText($scenarioPath, ($invalidMinimumScenario | ConvertTo-Json -Depth 20), [System.Text.UTF8Encoding]::new($false))
		$invalidMinimumRejected = $false
		try { [void](Read-ScenarioManifest -Path $scenarioPath -ExpectedPhase 'self-test') } catch { $invalidMinimumRejected = $_.Exception.Message -match 'integer from 1 through 128' }
		Assert-TestCondition -Condition $invalidMinimumRejected -Message "Invalid minimumInputEvents '$invalidMinimum' was accepted."
	}
	[System.IO.File]::WriteAllText($scenarioPath, ($scenario | ConvertTo-Json -Depth 20), [System.Text.UTF8Encoding]::new($false))

	$emptyRequiredEvidence = $scenario | ConvertTo-Json -Depth 20 | ConvertFrom-Json
	$emptyRequiredEvidence.requiredEvidence = @()
	[System.IO.File]::WriteAllText($scenarioPath, ($emptyRequiredEvidence | ConvertTo-Json -Depth 20), [System.Text.UTF8Encoding]::new($false))
	$emptyRequiredEvidenceRejected = $false
	try { [void](Read-ScenarioManifest -Path $scenarioPath -ExpectedPhase 'self-test') } catch { $emptyRequiredEvidenceRejected = $_.Exception.Message -match 'non-empty JSON array' }
	Assert-TestCondition -Condition $emptyRequiredEvidenceRejected -Message 'Scenario with no explicit requiredEvidence contract did not fail closed.'

	$unsafeRequiredEvidence = $scenario | ConvertTo-Json -Depth 20 | ConvertFrom-Json
	$unsafeRequiredEvidence.requiredEvidence = @('../escape.json')
	[System.IO.File]::WriteAllText($scenarioPath, ($unsafeRequiredEvidence | ConvertTo-Json -Depth 20), [System.Text.UTF8Encoding]::new($false))
	$unsafeRequiredEvidenceRejected = $false
	try { [void](Read-ScenarioManifest -Path $scenarioPath -ExpectedPhase 'self-test') } catch { $unsafeRequiredEvidenceRejected = $_.Exception.Message -match 'safe relative file path' }
	Assert-TestCondition -Condition $unsafeRequiredEvidenceRejected -Message 'Scenario requiredEvidence path traversal did not fail closed.'

	$duplicateKeyRejected = $false
	try { [void](ConvertFrom-JsonPreservingArrays -Json '{"passed":false,"passed":true}' -Description 'Duplicate-key fixture') } catch { $duplicateKeyRejected = $_.Exception.Message -match 'Duplicate JSON object member' }
	Assert-TestCondition -Condition $duplicateKeyRejected -Message 'Contradictory duplicate JSON object members did not fail closed.'

	# Build a binding-only extension of the otherwise unchanged base scenario.
	# The five primitive bindings prove nested, case-sensitive, no-coercion
	# resolution without requiring a live MCP server in this focused self-test.
	$bindingUuid = '0123456789abcdef8123456789abcdef'
	$bindingSourceOperation = [ordered]@{
		id = 'binding-source'
		stage = 'prePlay'
		order = 5
		tool = 'fixture.source'
		arguments = [ordered]@{}
		expect = [ordered]@{
			ok = $true
			requiredFields = @(
				'data.uuidValue', 'data.stringValue', 'data.integerValue',
				'data.numberValue', 'data.booleanValue'
			)
		}
		evidencePath = 'editor/operations/binding-source.json'
	}
	$bindingDestinationOperation = [ordered]@{
		id = 'binding-destination'
		stage = 'prePlay'
		order = 6
		tool = 'fixture.destination'
		arguments = [ordered]@{
			target = [ordered]@{
				identity = $null
				label = $null
				metrics = [ordered]@{ count = $null; ratio = $null }
				enabled = $null
			}
		}
		argumentBindings = @(
			[ordered]@{ sourceOperationId = 'binding-source'; sourceResponsePath = 'data.uuidValue'; destinationArgumentPath = 'target.identity'; valueType = 'uuid' },
			[ordered]@{ sourceOperationId = 'binding-source'; sourceResponsePath = 'data.stringValue'; destinationArgumentPath = 'target.label'; valueType = 'string' },
			# Use an explicit offset so this focused test proves dynamic recording
			# end ticks are derived from the reviewed source publication tick.
			[ordered]@{ sourceOperationId = 'binding-source'; sourceResponsePath = 'data.integerValue'; destinationArgumentPath = 'target.metrics.count'; valueType = 'integer'; integerOffset = 60 },
			[ordered]@{ sourceOperationId = 'binding-source'; sourceResponsePath = 'data.numberValue'; destinationArgumentPath = 'target.metrics.ratio'; valueType = 'number' },
			[ordered]@{ sourceOperationId = 'binding-source'; sourceResponsePath = 'data.booleanValue'; destinationArgumentPath = 'target.enabled'; valueType = 'boolean' }
		)
		expect = [ordered]@{ ok = $true; requiredFields = @('data.accepted') }
		evidencePath = 'editor/operations/binding-destination.json'
	}
	$bindingScenario = $scenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	$bindingScenario.operations = @($bindingScenario.operations) + @(
		[pscustomobject]$bindingSourceOperation,
		[pscustomobject]$bindingDestinationOperation
	)
	$bindingScenario.requiredEvidence = @($bindingScenario.requiredEvidence) + @(
		'editor/operations/binding-source.json',
		'editor/operations/binding-destination.json'
	)
	[System.IO.File]::WriteAllText($scenarioPath, ($bindingScenario | ConvertTo-Json -Depth 40), [System.Text.UTF8Encoding]::new($false))
	$validatedBindingScenario = Read-ScenarioManifest -Path $scenarioPath -ExpectedPhase 'self-test' -RepoRoot $repoRoot
	$validatedBindingSource = @($validatedBindingScenario.operations | Where-Object { [string]$_.id -ceq 'binding-source' })[0]
	$validatedBindingDestination = @($validatedBindingScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0]
	Assert-TestCondition -Condition (@($validatedBindingDestination.argumentBindings).Count -eq 5) -Message 'Valid primitive argumentBindings did not preserve their true JSON array.'
	$singleBindingScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	$singleBindingDestination = @($singleBindingScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0]
	$singleBindingDestination.argumentBindings = @($singleBindingDestination.argumentBindings[0])
	[System.IO.File]::WriteAllText($scenarioPath, ($singleBindingScenario | ConvertTo-Json -Depth 40), [System.Text.UTF8Encoding]::new($false))
	$validatedSingleBindingScenario = Read-ScenarioManifest -Path $scenarioPath -ExpectedPhase 'self-test' -RepoRoot $repoRoot
	Assert-TestCondition -Condition (@(@($validatedSingleBindingScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].argumentBindings).Count -eq 1) -Message 'A true one-element argumentBindings array did not preserve its shape.'
	$emptyBindingScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	@($emptyBindingScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].argumentBindings = @()
	[System.IO.File]::WriteAllText($scenarioPath, ($emptyBindingScenario | ConvertTo-Json -Depth 40), [System.Text.UTF8Encoding]::new($false))
	$validatedEmptyBindingScenario = Read-ScenarioManifest -Path $scenarioPath -ExpectedPhase 'self-test' -RepoRoot $repoRoot
	Assert-TestCondition -Condition (@(@($validatedEmptyBindingScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].argumentBindings).Count -eq 0) -Message 'An explicit empty argumentBindings JSON array was rejected or changed shape.'
	$crossStageBindingScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	@($crossStageBindingScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].stage = 'play'
	[System.IO.File]::WriteAllText($scenarioPath, ($crossStageBindingScenario | ConvertTo-Json -Depth 40), [System.Text.UTF8Encoding]::new($false))
	$validatedCrossStageBindingScenario = Read-ScenarioManifest -Path $scenarioPath -ExpectedPhase 'self-test' -RepoRoot $repoRoot
	$validatedCrossStageDestination = @($validatedCrossStageBindingScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0]
	Assert-TestCondition -Condition ([string]$validatedCrossStageDestination.stage -ceq 'play') -Message 'A valid prePlay-to-play argument binding was rejected.'

	$bindingSourceEnvelope = [pscustomobject][ordered]@{
		ok = $true
		data = [pscustomobject][ordered]@{
			uuidValue = $bindingUuid
			stringValue = 'bound-string'
			integerValue = [int]42
			numberValue = [double]1.25
			booleanValue = $true
		}
	}
	$bindingSourceEnvelopeHash = Get-StringSha256 -Value ($bindingSourceEnvelope | ConvertTo-Json -Depth 20 -Compress)
	$bindingRegistry = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
	$bindingRegistry['binding-source'] = [ordered]@{
		envelope = $bindingSourceEnvelope
		mcpSequence = 41
		envelopeSha256 = $bindingSourceEnvelopeHash
		evidenceSha256 = ('1' * 64)
	}
	$script:McpSequence = 700
	$resolvedBindingArguments = Get-ScenarioInvocationArguments -Definition $validatedBindingDestination -Kind operation
	$bindingResolution = Resolve-ScenarioArgumentBindings -Definition $validatedBindingDestination -Arguments $resolvedBindingArguments -BindingRegistry $bindingRegistry
	Assert-TestCondition -Condition ([string](Get-CaseSensitiveJsonPathValue -Object $resolvedBindingArguments -Path 'target.identity').value -ceq $bindingUuid) -Message 'UUID argument binding did not resolve at its nested destination.'
	Assert-TestCondition -Condition ((Get-CaseSensitiveJsonPathValue -Object $resolvedBindingArguments -Path 'target.label').value -is [string]) -Message 'String argument binding changed JSON type.'
	Assert-TestCondition -Condition ((Get-CaseSensitiveJsonPathValue -Object $resolvedBindingArguments -Path 'target.metrics.count').value -is [Int64] -and (Get-CaseSensitiveJsonPathValue -Object $resolvedBindingArguments -Path 'target.metrics.count').value -eq 102) -Message 'Integer argument binding did not apply its reviewed offset without changing the JSON integer type.'
	Assert-TestCondition -Condition ((Get-CaseSensitiveJsonPathValue -Object $resolvedBindingArguments -Path 'target.metrics.ratio').value -is [double]) -Message 'Number argument binding changed JSON type.'
	Assert-TestCondition -Condition ((Get-CaseSensitiveJsonPathValue -Object $resolvedBindingArguments -Path 'target.enabled').value -is [bool]) -Message 'Boolean argument binding changed JSON type.'
	Assert-TestCondition -Condition ([string]$bindingResolution.argumentsSha256 -ceq (Get-CanonicalJsonSha256 -Value $resolvedBindingArguments)) -Message 'Resolved argument canonical hash is not reproducible.'
	Assert-TestCondition -Condition (@($bindingResolution.bindings).Count -eq 5 -and [int]$bindingResolution.bindings[0].sourceMcpSequence -eq 41) -Message 'Argument binding provenance omitted its source sequence.'
	Assert-TestCondition -Condition ($null -eq (Get-CaseSensitiveJsonPathValue -Object $validatedBindingDestination.arguments -Path 'target.identity').value) -Message 'Deep argument cloning mutated the reviewed scenario placeholder.'
	Assert-TestCondition -Condition ($script:McpSequence -eq 700) -Message 'Argument resolution issued an MCP call or silent retry.'
	$canonicalLeft = [ordered]@{ z = [int]2; a = [ordered]@{ y = $true; b = 'value' } }
	$canonicalRight = [ordered]@{ a = [ordered]@{ b = 'value'; y = $true }; z = [int]2 }
	Assert-TestCondition -Condition ((Get-CanonicalJsonSha256 -Value $canonicalLeft) -ceq (Get-CanonicalJsonSha256 -Value $canonicalRight)) -Message 'Canonical argument hashing depended on object member order.'
	Assert-TestCondition -Condition ((Get-CanonicalJsonSha256 -Value @([int]1, [int]2)) -cne (Get-CanonicalJsonSha256 -Value @([int]2, [int]1))) -Message 'Canonical argument hashing ignored JSON array order.'
	Assert-TestCondition -Condition ((Get-CanonicalJsonSha256 -Value @()) -cne (Get-CanonicalJsonSha256 -Value @([int]1)) -and (Get-CanonicalJsonSha256 -Value ([int]1)) -cne (Get-CanonicalJsonSha256 -Value '1')) -Message 'Canonical argument hashing collapsed distinct JSON array or scalar types.'

	foreach ($wrongType in @(
		[ordered]@{ sourcePath = 'uuidValue'; value = 'not-a-uuid'; expected = 'Terra canonical lowercase 32-hex UUID' },
		[ordered]@{ sourcePath = 'uuidValue'; value = '01234567-89ab-cdef-8123-456789abcdef'; expected = 'Terra canonical lowercase 32-hex UUID' },
		[ordered]@{ sourcePath = 'uuidValue'; value = '0123456789ABCDEF8123456789ABCDEF'; expected = 'Terra canonical lowercase 32-hex UUID' },
		[ordered]@{ sourcePath = 'stringValue'; value = [int]7; expected = "JSON type 'integer', expected 'string'" },
		[ordered]@{ sourcePath = 'integerValue'; value = [double]7.5; expected = "JSON type 'number', expected 'integer'" },
		[ordered]@{ sourcePath = 'numberValue'; value = '7.5'; expected = "JSON type 'string', expected 'number'" },
		[ordered]@{ sourcePath = 'booleanValue'; value = 'true'; expected = "JSON type 'string', expected 'boolean'" }
	)) {
		$wrongEnvelope = Copy-JsonValuePreservingArrays -Value $bindingSourceEnvelope
		$wrongEnvelope['data'][[string]$wrongType.sourcePath] = $wrongType.value
		$wrongRegistry = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
		$wrongRegistry['binding-source'] = [ordered]@{ envelope = $wrongEnvelope; mcpSequence = 41; envelopeSha256 = ('2' * 64) }
		$wrongTypeRejected = $false
		try {
			$wrongArguments = Get-ScenarioInvocationArguments -Definition $validatedBindingDestination -Kind operation
			[void](Resolve-ScenarioArgumentBindings -Definition $validatedBindingDestination -Arguments $wrongArguments -BindingRegistry $wrongRegistry)
		}
		catch { $wrongTypeRejected = $_.Exception.Message -match [regex]::Escape([string]$wrongType.expected) }
		Assert-TestCondition -Condition $wrongTypeRejected -Message "Argument binding accepted wrong runtime type for '$($wrongType.sourcePath)'."
	}
	$emptyBindingRegistry = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
	$missingSuccessfulSourceRejected = $false
	try {
		$missingSourceArguments = Get-ScenarioInvocationArguments -Definition $validatedBindingDestination -Kind operation
		[void](Resolve-ScenarioArgumentBindings -Definition $validatedBindingDestination -Arguments $missingSourceArguments -BindingRegistry $emptyBindingRegistry)
	}
	catch { $missingSuccessfulSourceRejected = $_.Exception.Message -match 'has not completed successful expectation and evidence validation' }
	Assert-TestCondition -Condition $missingSuccessfulSourceRejected -Message 'Argument binding resolved from an operation absent from the successful in-memory registry.'
	$nullEnvelope = Copy-JsonValuePreservingArrays -Value $bindingSourceEnvelope
	$nullEnvelope['data']['uuidValue'] = $null
	$nullRegistry = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
	$nullRegistry['binding-source'] = [ordered]@{ envelope = $nullEnvelope; mcpSequence = 41; envelopeSha256 = ('3' * 64) }
	$nullSourceRejected = $false
	try {
		$nullArguments = Get-ScenarioInvocationArguments -Definition $validatedBindingDestination -Kind operation
		[void](Resolve-ScenarioArgumentBindings -Definition $validatedBindingDestination -Arguments $nullArguments -BindingRegistry $nullRegistry)
	}
	catch { $nullSourceRejected = $_.Exception.Message -match 'missing or null' }
	Assert-TestCondition -Condition $nullSourceRejected -Message 'Argument binding accepted a null source value.'
	$caseEnvelope = Copy-JsonValuePreservingArrays -Value $bindingSourceEnvelope
	$caseEnvelope['data']['UuidValue'] = $caseEnvelope['data']['uuidValue']
	[void]$caseEnvelope['data'].Remove('uuidValue')
	$caseRegistry = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
	$caseRegistry['binding-source'] = [ordered]@{ envelope = $caseEnvelope; mcpSequence = 41; envelopeSha256 = ('4' * 64) }
	$caseSourceRejected = $false
	try {
		$caseArguments = Get-ScenarioInvocationArguments -Definition $validatedBindingDestination -Kind operation
		[void](Resolve-ScenarioArgumentBindings -Definition $validatedBindingDestination -Arguments $caseArguments -BindingRegistry $caseRegistry)
	}
	catch { $caseSourceRejected = $_.Exception.Message -match 'missing or null' }
	Assert-TestCondition -Condition $caseSourceRejected -Message 'Argument binding source lookup was not case-sensitive.'

	$nonArrayBindingScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	$nonArrayBindingDestination = @($nonArrayBindingScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0]
	$nonArrayBindingDestination.argumentBindings = $nonArrayBindingDestination.argumentBindings[0]
	Assert-ArgumentBindingScenarioRejected -Scenario $nonArrayBindingScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'argumentBindings must be a JSON array' -FailureMessage 'An argumentBindings object was accepted as an array.'
	$extraBindingFieldScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	$extraBinding = @($extraBindingFieldScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].argumentBindings[0]
	$extraBinding | Add-Member -NotePropertyName fallback -NotePropertyValue 'forbidden'
	Assert-ArgumentBindingScenarioRejected -Scenario $extraBindingFieldScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'must contain exactly' -FailureMessage 'Argument binding accepted an unknown extra field.'
	$missingBindingFieldScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	$missingBinding = @($missingBindingFieldScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].argumentBindings[0]
	$missingBinding.PSObject.Properties.Remove('valueType')
	Assert-ArgumentBindingScenarioRejected -Scenario $missingBindingFieldScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'must contain exactly' -FailureMessage 'Argument binding accepted a missing required field.'

	$tooManyPerOperationScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	$tooManyPerOperationDestination = @($tooManyPerOperationScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0]
	$tooManyArguments = [ordered]@{}
	$tooManyBindings = @()
	for ($bindingIndex = 0; $bindingIndex -lt 17; ++$bindingIndex) {
		$tooManyArguments["value$bindingIndex"] = $null
		$tooManyBindings += [ordered]@{ sourceOperationId = 'binding-source'; sourceResponsePath = 'data.stringValue'; destinationArgumentPath = "value$bindingIndex"; valueType = 'string' }
	}
	$tooManyPerOperationDestination.arguments = [pscustomobject]$tooManyArguments
	$tooManyPerOperationDestination.argumentBindings = @($tooManyBindings)
	Assert-ArgumentBindingScenarioRejected -Scenario $tooManyPerOperationScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'at most 16 argument bindings' -FailureMessage 'More than sixteen bindings on one operation were accepted.'

	$tooManyPerScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	$tooManyPerScenario.operations = @($tooManyPerScenario.operations | Where-Object { [string]$_.id -cne 'binding-destination' })
	for ($destinationIndex = 0; $destinationIndex -lt 11; ++$destinationIndex) {
		$manyArguments = [ordered]@{}
		$manyBindings = @()
		for ($bindingIndex = 0; $bindingIndex -lt 15; ++$bindingIndex) {
			$manyArguments["value$bindingIndex"] = $null
			$manyBindings += [ordered]@{ sourceOperationId = 'binding-source'; sourceResponsePath = 'data.stringValue'; destinationArgumentPath = "value$bindingIndex"; valueType = 'string' }
		}
		$manyEvidencePath = "editor/operations/binding-many-$destinationIndex.json"
		$tooManyPerScenario.operations += [pscustomobject][ordered]@{
			id = "binding-many-$destinationIndex"
			stage = 'prePlay'
			order = 6 + $destinationIndex
			tool = 'fixture.destination'
			arguments = [pscustomobject]$manyArguments
			argumentBindings = @($manyBindings)
			expect = [pscustomobject][ordered]@{ ok = $true; requiredFields = @('data.accepted') }
			evidencePath = $manyEvidencePath
		}
		$tooManyPerScenario.requiredEvidence += $manyEvidencePath
	}
	Assert-ArgumentBindingScenarioRejected -Scenario $tooManyPerScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'at most 160 argument bindings' -FailureMessage 'More than one hundred sixty bindings in one scenario were accepted.'

	foreach ($unsafeBindingPath in @(
		[ordered]@{ property = 'sourceResponsePath'; value = 'data.items.0.id'; expected = 'unsafe or non-object segment' },
		[ordered]@{ property = 'sourceResponsePath'; value = 'data.items[0].id'; expected = 'unsafe or non-object segment' },
		[ordered]@{ property = 'sourceResponsePath'; value = 'data.*.id'; expected = 'unsafe or non-object segment' },
		[ordered]@{ property = 'sourceResponsePath'; value = 'data.a.b.c.d.e.f.g.h'; expected = 'one through 8' },
		[ordered]@{ property = 'destinationArgumentPath'; value = 'target..identity'; expected = 'unsafe or non-object segment' },
		[ordered]@{ property = 'destinationArgumentPath'; value = 'target[0].identity'; expected = 'unsafe or non-object segment' }
	)) {
		$unsafePathScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
		$unsafePathBinding = @($unsafePathScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].argumentBindings[0]
		$unsafePathBinding.($unsafeBindingPath.property) = [string]$unsafeBindingPath.value
		Assert-ArgumentBindingScenarioRejected -Scenario $unsafePathScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage ([regex]::Escape([string]$unsafeBindingPath.expected)) -FailureMessage "Unsafe argument binding path '$($unsafeBindingPath.value)' was accepted."
	}

	$notDataScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	@($notDataScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].argumentBindings[0].sourceResponsePath = 'result.uuidValue'
	Assert-ArgumentBindingScenarioRejected -Scenario $notDataScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage "must begin with exact 'data.'" -FailureMessage 'Argument binding source outside data.* was accepted.'
	$caseMismatchedDeclaredSourceScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	@($caseMismatchedDeclaredSourceScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].argumentBindings[0].sourceResponsePath = 'data.UuidValue'
	Assert-ArgumentBindingScenarioRejected -Scenario $caseMismatchedDeclaredSourceScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'must declare exact required field' -FailureMessage 'A case-mismatched sourceResponsePath was treated as the declared required field.'
	foreach ($invalidSource in @(
		[ordered]@{ id = 'missing-operation'; expected = 'does not exist' },
		[ordered]@{ id = 'binding-destination'; expected = 'distinct earlier operation' },
		[ordered]@{ id = 'scene'; expected = 'is a capture' }
	)) {
		$invalidSourceScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
		@($invalidSourceScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].argumentBindings[0].sourceOperationId = [string]$invalidSource.id
		Assert-ArgumentBindingScenarioRejected -Scenario $invalidSourceScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage ([regex]::Escape([string]$invalidSource.expected)) -FailureMessage "Invalid binding source '$($invalidSource.id)' was accepted."
	}
	$laterSourceScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	$laterSource = @($laterSourceScenario.operations | Where-Object { [string]$_.id -ceq 'binding-source' })[0] | ConvertTo-Json -Depth 20 | ConvertFrom-Json
	$laterSource.id = 'binding-later-source'
	$laterSource.order = 7
	$laterSource.evidencePath = 'editor/operations/binding-later-source.json'
	$laterSourceScenario.operations += $laterSource
	$laterSourceScenario.requiredEvidence += $laterSource.evidencePath
	@($laterSourceScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].argumentBindings[0].sourceOperationId = 'binding-later-source'
	Assert-ArgumentBindingScenarioRejected -Scenario $laterSourceScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'distinct earlier operation' -FailureMessage 'Argument binding accepted a later source operation.'
	$laterStageScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	@($laterStageScenario.operations | Where-Object { [string]$_.id -ceq 'binding-source' })[0].stage = 'postPlay'
	Assert-ArgumentBindingScenarioRejected -Scenario $laterStageScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'distinct earlier operation' -FailureMessage 'Argument binding accepted a later-stage source despite its lower numeric order.'
	$failedSourceScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	@($failedSourceScenario.operations | Where-Object { [string]$_.id -ceq 'binding-source' })[0].expect.ok = $false
	Assert-ArgumentBindingScenarioRejected -Scenario $failedSourceScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'successful ok=true' -FailureMessage 'Argument binding accepted an expected-failure source operation.'
	$unrequiredSourceScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	@($unrequiredSourceScenario.operations | Where-Object { [string]$_.id -ceq 'binding-source' })[0].expect.requiredFields = @('data.stringValue')
	Assert-ArgumentBindingScenarioRejected -Scenario $unrequiredSourceScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'must declare exact required field' -FailureMessage 'Argument binding source path absent from exact requiredFields was accepted.'

	$absentDestinationScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	@($absentDestinationScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].arguments.target.PSObject.Properties.Remove('identity')
	Assert-ArgumentBindingScenarioRejected -Scenario $absentDestinationScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'does not exist with exact case' -FailureMessage 'Argument binding accepted an absent destination placeholder.'
	$nonnullDestinationScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	@($nonnullDestinationScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].arguments.target.identity = 'preset'
	Assert-ArgumentBindingScenarioRejected -Scenario $nonnullDestinationScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'must be exact JSON null' -FailureMessage 'Argument binding overwrote a non-null destination.'
	$caseDestinationScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	@($caseDestinationScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].argumentBindings[0].destinationArgumentPath = 'target.Identity'
	Assert-ArgumentBindingScenarioRejected -Scenario $caseDestinationScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'does not exist with exact case' -FailureMessage 'Argument binding destination lookup was not case-sensitive.'
	$duplicateDestinationScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	$duplicateDestinationOperation = @($duplicateDestinationScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0]
	$duplicateDestinationOperation.argumentBindings = @($duplicateDestinationOperation.argumentBindings) + @($duplicateDestinationOperation.argumentBindings[0])
	Assert-ArgumentBindingScenarioRejected -Scenario $duplicateDestinationScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'duplicates or overlaps' -FailureMessage 'Duplicate binding destination was accepted.'
	$ancestorDestinationScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	$ancestorDestinationOperation = @($ancestorDestinationScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0]
	$ancestorBinding = $ancestorDestinationOperation.argumentBindings[2] | ConvertTo-Json -Depth 10 | ConvertFrom-Json
	$ancestorBinding.destinationArgumentPath = 'target.metrics'
	$ancestorDestinationOperation.argumentBindings = @($ancestorDestinationOperation.argumentBindings) + @($ancestorBinding)
	Assert-ArgumentBindingScenarioRejected -Scenario $ancestorDestinationScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'duplicates or overlaps' -FailureMessage 'Ancestor-overlapping binding destination was accepted.'
	$unsupportedTypeScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	@($unsupportedTypeScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].argumentBindings[0].valueType = 'String'
	Assert-ArgumentBindingScenarioRejected -Scenario $unsupportedTypeScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'valueType.*unsupported' -FailureMessage 'Case-mismatched argument binding valueType was accepted.'
	$nonIntegerOffsetScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	@($nonIntegerOffsetScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].argumentBindings[2].integerOffset = 1.5
	Assert-ArgumentBindingScenarioRejected -Scenario $nonIntegerOffsetScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'integerOffset must be a JSON integer' -FailureMessage 'A fractional argument binding integerOffset was accepted.'
	$wrongOffsetTypeScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	@($wrongOffsetTypeScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].argumentBindings[0] | Add-Member -NotePropertyName integerOffset -NotePropertyValue 1
	Assert-ArgumentBindingScenarioRejected -Scenario $wrongOffsetTypeScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage "integerOffset requires valueType 'integer'" -FailureMessage 'A non-integer argument binding accepted integerOffset.'
	$oversizedOffsetScenario = $bindingScenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	@($oversizedOffsetScenario.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].argumentBindings[2].integerOffset = 864001
	Assert-ArgumentBindingScenarioRejected -Scenario $oversizedOffsetScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'integerOffset must stay within the recording four-hour fixed-tick bound' -FailureMessage 'An unbounded argument binding integerOffset was accepted.'

	# Durable Play Mode recording is the sole creation-only exception to exact
	# authored-state restoration. Its manifest contract names the stop operation
	# and the exact response field that proves where publication occurred.
	$recordingProducer = [ordered]@{
		id = 'recording-stop'
		stage = 'play'
		order = 5
		tool = 'animation.recording.stop'
		arguments = [ordered]@{ jobId = 'fixture-job' }
		approval = [ordered]@{ policy = 'high_impact' }
		expect = [ordered]@{ ok = $true; requiredFields = @('data.destinationPath') }
		evidencePath = 'editor/operations/recording-stop.json'
	}
	$recordingDeclaration = [ordered]@{
		path = 'Assets/Captured/SelfTestRecording.teanim'
		producerOperationId = 'recording-stop'
		producerResponsePath = 'data.destinationPath'
	}
	$recordingScenario = $scenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	$recordingScenario.operations = @($recordingScenario.operations) + @([pscustomobject]$recordingProducer)
	$recordingScenario.requiredEvidence = @($recordingScenario.requiredEvidence) + @('editor/operations/recording-stop.json')
	$recordingScenario | Add-Member -NotePropertyName playModeRecordingOutputs -NotePropertyValue @([pscustomobject]$recordingDeclaration)
	[System.IO.File]::WriteAllText($scenarioPath, ($recordingScenario | ConvertTo-Json -Depth 50), [System.Text.UTF8Encoding]::new($false))
	$validatedRecordingScenario = Read-ScenarioManifest -Path $scenarioPath -ExpectedPhase 'self-test' -RepoRoot $repoRoot
	Assert-TestCondition -Condition (@($validatedRecordingScenario.playModeRecordingOutputs).Count -eq 1) -Message 'A valid one-element playModeRecordingOutputs JSON array changed shape.'
	$missingApprovalMarkerScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
	@($missingApprovalMarkerScenario.operations | Where-Object { [string]$_.id -ceq 'recording-stop' })[0].PSObject.Properties.Remove('approval')
	Assert-RecordingOutputScenarioRejected -Scenario $missingApprovalMarkerScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'requires an exact operation-level approval marker' -FailureMessage 'A high-impact recording operation without its top-level approval marker was accepted.'
	$missingMotionBuildApprovalScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
	$missingMotionBuildApprovalOperation = @($missingMotionBuildApprovalScenario.operations | Where-Object { [string]$_.id -ceq 'recording-stop' })[0]
	$missingMotionBuildApprovalOperation.tool = 'animation.motion_database.build'
	$missingMotionBuildApprovalOperation.PSObject.Properties.Remove('approval')
	Assert-RecordingOutputScenarioRejected -Scenario $missingMotionBuildApprovalScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'requires an exact operation-level approval marker' -FailureMessage 'A high-impact motion-database build without its top-level approval marker was accepted.'
	$unsupportedApprovalMarkerScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
	@($unsupportedApprovalMarkerScenario.operations | Where-Object { [string]$_.id -ceq 'recording-stop' })[0].tool = 'animation.runtime.query'
	Assert-RecordingOutputScenarioRejected -Scenario $unsupportedApprovalMarkerScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'only for a reviewed mutating animation tool' -FailureMessage 'A non-HighImpact animation tool accepted a high-impact approval marker.'
	$argumentApprovalMarkerScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
	$argumentApprovalMarkerOperation = @($argumentApprovalMarkerScenario.operations | Where-Object { [string]$_.id -ceq 'recording-stop' })[0]
	$argumentApprovalMarkerOperation.arguments | Add-Member -NotePropertyName approval -NotePropertyValue ([pscustomobject]@{ policy = 'high_impact' })
	Assert-RecordingOutputScenarioRejected -Scenario $argumentApprovalMarkerScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'operation-level member, not an MCP argument' -FailureMessage 'Approval execution policy metadata leaked into MCP arguments.'
	$wrongApprovalPolicyScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
	@($wrongApprovalPolicyScenario.operations | Where-Object { [string]$_.id -ceq 'recording-stop' })[0].approval.policy = 'automatic'
	Assert-RecordingOutputScenarioRejected -Scenario $wrongApprovalPolicyScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage "exact JSON string 'high_impact'" -FailureMessage 'A wrong approval policy was accepted.'
	$extraApprovalPolicyFieldScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
	@($extraApprovalPolicyFieldScenario.operations | Where-Object { [string]$_.id -ceq 'recording-stop' })[0].approval | Add-Member -NotePropertyName autoApprove -NotePropertyValue $true
	Assert-RecordingOutputScenarioRejected -Scenario $extraApprovalPolicyFieldScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'must contain exactly' -FailureMessage 'An approval marker with an automatic-approval field was accepted.'
	$captureApprovalMarkerScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
	$captureApprovalMarkerScenario.captures[0] | Add-Member -NotePropertyName approval -NotePropertyValue ([pscustomobject]@{ policy = 'high_impact' })
	Assert-RecordingOutputScenarioRejected -Scenario $captureApprovalMarkerScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'capture.*may not declare an approval policy' -FailureMessage 'A camera capture accepted operation approval metadata.'
	$pairedRecordingScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
	$recordingSidecarDeclaration = $pairedRecordingScenario.playModeRecordingOutputs[0] | ConvertTo-Json -Depth 10 | ConvertFrom-Json
	$recordingSidecarDeclaration.path = 'Assets/Captured/SelfTestRecording.teanim.meta'
	$pairedRecordingScenario.playModeRecordingOutputs = @($pairedRecordingScenario.playModeRecordingOutputs) + @($recordingSidecarDeclaration)
	[System.IO.File]::WriteAllText($scenarioPath, ($pairedRecordingScenario | ConvertTo-Json -Depth 50), [System.Text.UTF8Encoding]::new($false))
	$validatedPairedRecordingScenario = Read-ScenarioManifest -Path $scenarioPath -ExpectedPhase 'self-test' -RepoRoot $repoRoot
	Assert-TestCondition -Condition (@($validatedPairedRecordingScenario.playModeRecordingOutputs).Count -eq 2) -Message 'An exact .teanim/.teanim.meta declaration pair was rejected or changed shape.'
	$unpairedSidecarScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
	$unpairedSidecarScenario.playModeRecordingOutputs[0].path = 'Assets/Captured/SelfTestRecording.teanim.meta'
	Assert-RecordingOutputScenarioRejected -Scenario $unpairedSidecarScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'must pair with exact declared' -FailureMessage 'An unpaired recording metadata sidecar was accepted.'
	$mismatchedSidecarScenario = $pairedRecordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
	$mismatchedSidecarScenario.playModeRecordingOutputs[1].path = 'Assets/Captured/OtherRecording.teanim.meta'
	Assert-RecordingOutputScenarioRejected -Scenario $mismatchedSidecarScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'must pair with exact declared|shared only by an exact' -FailureMessage 'A recording metadata sidecar was paired to a different clip path.'
	$explicitEmptyRecordingScenario = $scenario | ConvertTo-Json -Depth 40 | ConvertFrom-Json
	$explicitEmptyRecordingScenario | Add-Member -NotePropertyName playModeRecordingOutputs -NotePropertyValue @()
	[System.IO.File]::WriteAllText($scenarioPath, ($explicitEmptyRecordingScenario | ConvertTo-Json -Depth 40), [System.Text.UTF8Encoding]::new($false))
	$validatedEmptyRecordingScenario = Read-ScenarioManifest -Path $scenarioPath -ExpectedPhase 'self-test' -RepoRoot $repoRoot
	Assert-TestCondition -Condition (@($validatedEmptyRecordingScenario.playModeRecordingOutputs).Count -eq 0) -Message 'An explicit empty playModeRecordingOutputs JSON array was rejected.'

	$nonArrayRecordingScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
	$nonArrayRecordingScenario.playModeRecordingOutputs = $nonArrayRecordingScenario.playModeRecordingOutputs[0]
	Assert-RecordingOutputScenarioRejected -Scenario $nonArrayRecordingScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'playModeRecordingOutputs must be a JSON array' -FailureMessage 'A recording-output object was accepted instead of a JSON array.'
	$tooManyRecordingScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
	$tooManyRecordingScenario.playModeRecordingOutputs = @(0..16 | ForEach-Object {
		[pscustomobject]@{ path = "Assets/Captured/Recording$_.teanim"; producerOperationId = 'recording-stop'; producerResponsePath = 'data.destinationPath' }
	})
	Assert-RecordingOutputScenarioRejected -Scenario $tooManyRecordingScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'at most 16 entries' -FailureMessage 'More than sixteen recording outputs were accepted.'
	$extraRecordingFieldScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
	$extraRecordingFieldScenario.playModeRecordingOutputs[0] | Add-Member -NotePropertyName allowOverwrite -NotePropertyValue $true
	Assert-RecordingOutputScenarioRejected -Scenario $extraRecordingFieldScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'must contain exactly' -FailureMessage 'A recording-output declaration accepted an extra field.'
	$missingRecordingFieldScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
	$missingRecordingFieldScenario.playModeRecordingOutputs[0].PSObject.Properties.Remove('producerResponsePath')
	Assert-RecordingOutputScenarioRejected -Scenario $missingRecordingFieldScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'must contain exactly' -FailureMessage 'A recording-output declaration accepted a missing field.'
	$typedRecordingFieldScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
	$typedRecordingFieldScenario.playModeRecordingOutputs[0].producerOperationId = 5
	Assert-RecordingOutputScenarioRejected -Scenario $typedRecordingFieldScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'must be a JSON string' -FailureMessage 'A recording-output declaration coerced a non-string producer ID.'
	foreach ($unsafeRecordingPath in @(
		'Assets/Captured/../Escape.teanim',
		'Assets\Captured\Backslash.teanim',
		'C:/Assets/Captured/Absolute.teanim',
		'UserSettings/Captured/Excluded.teanim',
		'Assets/Captured/Nested/TooBroad.teanim',
		'Assets/Captured/Wrong.tanim',
		'Assets/Captured/Wild*.teanim',
		'Assets/Captured/Sidecar.meta',
		'Assets/Captured/con.teanim'
	)) {
		$unsafeRecordingScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
		$unsafeRecordingScenario.playModeRecordingOutputs[0].path = $unsafeRecordingPath
		Assert-RecordingOutputScenarioRejected -Scenario $unsafeRecordingScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'Assets/Captured|Windows-unsafe' -FailureMessage "Unsafe or over-broad recording output '$unsafeRecordingPath' was accepted."
	}
	$aliasedRecordingScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
	$secondRecordingDeclaration = $aliasedRecordingScenario.playModeRecordingOutputs[0] | ConvertTo-Json -Depth 10 | ConvertFrom-Json
	$secondRecordingDeclaration.path = 'Assets/Captured/selftestrecording.teanim'
	$aliasedRecordingScenario.playModeRecordingOutputs = @($aliasedRecordingScenario.playModeRecordingOutputs) + @($secondRecordingDeclaration)
	Assert-RecordingOutputScenarioRejected -Scenario $aliasedRecordingScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'duplicated or aliases' -FailureMessage 'Case-insensitive Windows aliases were accepted as distinct recording outputs.'
	foreach ($invalidProducer in @(
		[ordered]@{ id = 'missing-producer'; expected = 'does not exist' },
		[ordered]@{ id = 'scene'; expected = 'is a capture' }
	)) {
		$invalidProducerScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
		$invalidProducerScenario.playModeRecordingOutputs[0].producerOperationId = [string]$invalidProducer.id
		Assert-RecordingOutputScenarioRejected -Scenario $invalidProducerScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage ([regex]::Escape([string]$invalidProducer.expected)) -FailureMessage "Invalid recording producer '$($invalidProducer.id)' was accepted."
	}
	$wrongProducerStageScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
	@($wrongProducerStageScenario.operations | Where-Object { [string]$_.id -ceq 'recording-stop' })[0].stage = 'postPlay'
	Assert-RecordingOutputScenarioRejected -Scenario $wrongProducerStageScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'play-stage animation.recording.stop' -FailureMessage 'A postPlay recording producer was accepted.'
	$wrongProducerToolScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
	@($wrongProducerToolScenario.operations | Where-Object { [string]$_.id -ceq 'recording-stop' })[0].tool = 'animation.recording.start'
	Assert-RecordingOutputScenarioRejected -Scenario $wrongProducerToolScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'play-stage animation.recording.stop' -FailureMessage 'animation.recording.start was accepted as a durable output producer.'
	$failedRecordingProducerScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
	@($failedRecordingProducerScenario.operations | Where-Object { [string]$_.id -ceq 'recording-stop' })[0].expect.ok = $false
	Assert-RecordingOutputScenarioRejected -Scenario $failedRecordingProducerScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'explicit expect.ok=true|successful ok=true' -FailureMessage 'An expected-failure recording stop was accepted as a producer.'
	$unrequiredRecordingPathScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
	@($unrequiredRecordingPathScenario.operations | Where-Object { [string]$_.id -ceq 'recording-stop' })[0].expect.requiredFields = @('data.published')
	Assert-RecordingOutputScenarioRejected -Scenario $unrequiredRecordingPathScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage 'must declare exact required field' -FailureMessage 'A producer response path omitted from requiredFields was accepted.'
	foreach ($wrongResponsePath in @('result.destinationPath', 'data.DestinationPath', 'data.result.destinationPath')) {
		$wrongResponseScenario = $recordingScenario | ConvertTo-Json -Depth 50 | ConvertFrom-Json
		$wrongResponseScenario.playModeRecordingOutputs[0].producerResponsePath = $wrongResponsePath
		Assert-RecordingOutputScenarioRejected -Scenario $wrongResponseScenario -ScenarioPath $scenarioPath -RepoRoot $repoRoot -ExpectedMessage "exact 'data.destinationPath'" -FailureMessage "Wrong recording producer response path '$wrongResponsePath' was accepted."
	}

	$recordingProjectRoot = Join-Path $temporaryRoot 'recording-project'
	[System.IO.Directory]::CreateDirectory((Join-Path $recordingProjectRoot 'Assets/Captured')) | Out-Null
	[System.IO.Directory]::CreateDirectory((Join-Path $recordingProjectRoot 'Scenes')) | Out-Null
	[System.IO.Directory]::CreateDirectory((Join-Path $recordingProjectRoot 'UserSettings')) | Out-Null
	[System.IO.File]::WriteAllText((Join-Path $recordingProjectRoot 'Scenes/Original.tescene'), 'original-authored-state', [System.Text.UTF8Encoding]::new($false))
	[System.IO.File]::WriteAllText((Join-Path $recordingProjectRoot 'UserSettings/EditorLayout.ini'), 'transient-before', [System.Text.UTF8Encoding]::new($false))
	$deepRelativeDirectory = 'Assets/' + ((1..9 | ForEach-Object { "segment-$('{0:d2}' -f $_)-abcdefghijklmnopqrstuvwxyz" }) -join '/')
	$deepDirectoryNormal = Join-Path $recordingProjectRoot $deepRelativeDirectory
	$deepDirectoryExtended = ConvertTo-ExtendedLengthPath -Path $deepDirectoryNormal
	[System.IO.Directory]::CreateDirectory($deepDirectoryExtended) | Out-Null
	# PowerShell 5.1's Join-Path provider rejects the Win32 extended-length
	# prefix even though the underlying .NET file APIs accept it.
	$deepFileExtended = [System.IO.Path]::Combine($deepDirectoryExtended, 'deep-authored-file.bin')
	[System.IO.File]::WriteAllBytes($deepFileExtended, [byte[]](1, 2, 3, 4))
	Assert-TestCondition -Condition ($deepFileExtended.Length -gt 260) -Message 'Long-path recording fixture did not exceed the legacy MAX_PATH boundary.'
	# Rewrite one authored file from a separate process while the production
	# helper samples the project. Ten required observations keep the helper
	# active long enough to see both the pre-write and post-write inventories.
	$stabilityProbePath = Join-Path $recordingProjectRoot 'Scenes/StabilityProbe.tescene'
	[System.IO.File]::WriteAllText($stabilityProbePath, 'before', [System.Text.UTF8Encoding]::new($false))
	$stabilityProcessInfo = [System.Diagnostics.ProcessStartInfo]::new()
	$stabilityProcessInfo.FileName = (Get-Process -Id $PID).Path
	$stabilityProcessInfo.Arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "Start-Sleep -Milliseconds 150; [System.IO.File]::WriteAllText($env:TERRAENGINE_STABILITY_PROBE_PATH, ''after'', [System.Text.UTF8Encoding]::new($false))"'
	$stabilityProcessInfo.UseShellExecute = $false
	$stabilityProcessInfo.CreateNoWindow = $true
	$stabilityProcessInfo.EnvironmentVariables['TERRAENGINE_STABILITY_PROBE_PATH'] = $stabilityProbePath
	$stabilityTestProcess = [System.Diagnostics.Process]::Start($stabilityProcessInfo)
	if ($null -eq $stabilityTestProcess) { throw 'Could not start the authored-state stability rewrite fixture.' }
	$stableAuthoredState = Get-StableAuthoredProjectState -ProjectRoot $recordingProjectRoot -RequiredConsecutiveObservations 10 -SampleIntervalMilliseconds 50 -TimeoutSeconds 5
	if (-not $stabilityTestProcess.WaitForExit(5000)) { throw 'Authored-state stability rewrite fixture did not exit.' }
	if ($stabilityTestProcess.ExitCode -ne 0) { throw "Authored-state stability rewrite fixture exited with code $($stabilityTestProcess.ExitCode)." }
	$finalAuthoredState = Get-AuthoredProjectState -ProjectRoot $recordingProjectRoot
	$stabilityHashes = @($stableAuthoredState.samples | Where-Object { [bool]$_.succeeded } | ForEach-Object { [string]$_.sha256 } | Select-Object -Unique)
	Assert-TestCondition -Condition (
		$stabilityHashes.Count -ge 2 -and
		[string]$stableAuthoredState.snapshot.sha256 -ceq [string]$finalAuthoredState.sha256 -and
		[System.IO.File]::ReadAllText($stabilityProbePath) -ceq 'after'
	) -Message 'Bounded authored-state stabilization accepted a transient inventory instead of the completed asynchronous rewrite.'
	$stabilityTestProcess.Dispose()
	$stabilityTestProcess = $null
	$recordingBeforeSnapshot = Get-PlayModeProjectSnapshot -ProjectRoot $recordingProjectRoot
	Assert-TestCondition -Condition (@($recordingBeforeSnapshot.files | Where-Object { [string]$_.path -like 'Assets/segment-*/deep-authored-file.bin' }).Count -eq 1) -Message 'Iterative Play Mode snapshot omitted a greater-than-260-character authored file.'
	Assert-TestCondition -Condition (@($recordingBeforeSnapshot.files | Where-Object { [string]$_.path -like 'UserSettings/*' }).Count -eq 0 -and [string]$recordingBeforeSnapshot.excludedTransientState.path -ceq 'UserSettings') -Message 'Play Mode snapshot did not truthfully exclude transient UserSettings state.'
	Assert-TestCondition -Condition (@($recordingBeforeSnapshot.directories) -ccontains 'Assets' -and @($recordingBeforeSnapshot.directories) -ccontains 'Assets/Captured') -Message "Iterative Play Mode snapshot omitted existing Assets/Captured directories: [$(@($recordingBeforeSnapshot.directories) -join ', ')]."
	$recordingPreflight = @(Start-PlayModeRecordingOutputContract -Scenario $validatedPairedRecordingScenario -ProjectRoot $recordingProjectRoot -BeforeSnapshot $recordingBeforeSnapshot)
	Assert-TestCondition -Condition ($recordingPreflight.Count -eq 2 -and @($recordingPreflight | Where-Object { [bool]$_.outputExisted }).Count -eq 0) -Message 'Valid paired creation-only recording preflight did not pass.'
	$recordingOutputPath = Join-Path $recordingProjectRoot 'Assets/Captured/SelfTestRecording.teanim'
	$recordingSidecarPath = $recordingOutputPath + '.meta'
	[System.IO.File]::WriteAllBytes($recordingOutputPath, [byte[]](5, 4, 3, 2, 1))
	[System.IO.File]::WriteAllBytes($recordingSidecarPath, [byte[]](9, 8, 7))
	[System.IO.File]::WriteAllText((Join-Path $recordingProjectRoot 'UserSettings/EditorLayout.ini'), 'transient-after', [System.Text.UTF8Encoding]::new($false))
	$recordingAfterSnapshot = Get-PlayModeProjectSnapshot -ProjectRoot $recordingProjectRoot
	$recordingProducerEnvelope = [pscustomobject][ordered]@{ ok = $true; data = [pscustomobject][ordered]@{ destinationPath = [System.IO.Path]::GetFullPath($recordingOutputPath) } }
	$recordingProducerEnvelopeHash = Get-StringSha256 -Value ($recordingProducerEnvelope | ConvertTo-Json -Depth 20 -Compress)
	$recordingRegistry = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
	$recordingRegistry['recording-stop'] = [ordered]@{
		envelope = $recordingProducerEnvelope
		mcpSequence = 12
		envelopeSha256 = $recordingProducerEnvelopeHash
		evidenceSha256 = ('7' * 64)
	}
	$recordingOperationInventory = @([pscustomobject][ordered]@{
		id = 'recording-stop'
		kind = 'operation'
		stage = 'play'
		tool = 'animation.recording.stop'
		mcpSequence = 12
		expectationPassed = $true
		evidence = [pscustomobject][ordered]@{ path = 'editor/operations/recording-stop.json'; sha256 = ('7' * 64) }
	})
	$recordingCompletion = Complete-PlayModeRecordingOutputContract -Scenario $validatedPairedRecordingScenario -ProjectRoot $recordingProjectRoot -BeforeSnapshot $recordingBeforeSnapshot -AfterSnapshot $recordingAfterSnapshot -BindingRegistry $recordingRegistry -OperationInventory $recordingOperationInventory -PlayEnterSequence 10 -PlayExitSequence 20
	$recordingClipProof = @($recordingCompletion.outputs | Where-Object { [string]$_.path -ceq 'Assets/Captured/SelfTestRecording.teanim' })
	$recordingSidecarProof = @($recordingCompletion.outputs | Where-Object { [string]$_.path -ceq 'Assets/Captured/SelfTestRecording.teanim.meta' })
	Assert-TestCondition -Condition (
		[bool]$recordingCompletion.comparison.passed -and @($recordingCompletion.outputs).Count -eq 2 -and
		$recordingClipProof.Count -eq 1 -and $recordingSidecarProof.Count -eq 1 -and
		[string]$recordingClipProof[0].sha256 -ceq (Get-LongPathFileState -Path $recordingOutputPath).sha256 -and
		[string]$recordingSidecarProof[0].sha256 -ceq (Get-LongPathFileState -Path $recordingSidecarPath).sha256
	) -Message 'Valid creation-only recording clip and independently hashed metadata sidecar did not produce exact producer/content proofs.'
	$preexistingRecordingRejected = $false
	try { [void](Start-PlayModeRecordingOutputContract -Scenario $validatedPairedRecordingScenario -ProjectRoot $recordingProjectRoot -BeforeSnapshot $recordingBeforeSnapshot) } catch { $preexistingRecordingRejected = $_.Exception.Message -match 'already exists' }
	Assert-TestCondition -Condition $preexistingRecordingRejected -Message 'A pre-existing recording output passed creation-only preflight.'
	$missingRecordingParentRoot = Join-Path $temporaryRoot 'recording-missing-parent'
	[System.IO.Directory]::CreateDirectory((Join-Path $missingRecordingParentRoot 'Assets')) | Out-Null
	$missingParentSnapshot = Get-PlayModeProjectSnapshot -ProjectRoot $missingRecordingParentRoot
	$missingRecordingParentRejected = $false
	try { [void](Start-PlayModeRecordingOutputContract -Scenario $validatedPairedRecordingScenario -ProjectRoot $missingRecordingParentRoot -BeforeSnapshot $missingParentSnapshot) } catch { $missingRecordingParentRejected = $_.Exception.Message -match 'existing Assets/Captured parent' }
	Assert-TestCondition -Condition $missingRecordingParentRejected -Message 'Recording preflight silently created a missing output parent.'
	$recordingReparseRoot = Join-Path $temporaryRoot 'recording-reparse-project'
	$recordingReparseTarget = Join-Path $temporaryRoot 'recording-reparse-target'
	[System.IO.Directory]::CreateDirectory((Join-Path $recordingReparseRoot 'Assets')) | Out-Null
	[System.IO.Directory]::CreateDirectory($recordingReparseTarget) | Out-Null
	$overlayJunctionPath = Join-Path $recordingReparseRoot 'Assets/Captured'
	[void](New-Item -ItemType Junction -Path $overlayJunctionPath -Target $recordingReparseTarget -ErrorAction Stop)
	$recordingReparseRejected = $false
	try { [void](Get-PlayModeProjectSnapshot -ProjectRoot $recordingReparseRoot) } catch { $recordingReparseRejected = $_.Exception.Message -match 'reparse points' }
	Assert-TestCondition -Condition $recordingReparseRejected -Message 'Play Mode inventory traversed a recording-output reparse parent.'
	[System.IO.Directory]::Delete($overlayJunctionPath)
	$overlayJunctionPath = $null

	foreach ($snapshotMutation in @(
		[ordered]@{ name = 'original mutation'; mutate = {
			param($copy)
			@($copy.files | Where-Object { [string]$_.path -ceq 'Scenes/Original.tescene' })[0].sha256 = ('1' * 64)
		}; expected = 'mutated original authored file' },
		[ordered]@{ name = 'original deletion'; mutate = {
			param($copy)
			$copy.files = @($copy.files | Where-Object { [string]$_.path -cne 'Scenes/Original.tescene' })
		}; expected = 'deleted or renamed original authored file' },
		[ordered]@{ name = 'undeclared addition'; mutate = {
			param($copy)
			$copy.files = @($copy.files) + @([pscustomobject]@{ path = 'Assets/Captured/Undeclared.teanim'; bytes = 1; sha256 = ('2' * 64) })
		}; expected = 'additions do not exactly match' },
		[ordered]@{ name = 'new directory'; mutate = {
			param($copy)
			$copy.directories = @($copy.directories) + @('Assets/Captured/UnexpectedDirectory')
		}; expected = 'added, deleted, renamed, or case-changed an authored directory' },
		[ordered]@{ name = 'missing output'; mutate = {
			param($copy)
			$copy.files = @($copy.files | Where-Object { [string]$_.path -cne 'Assets/Captured/SelfTestRecording.teanim' })
		}; expected = 'additions do not exactly match' },
		[ordered]@{ name = 'missing metadata sidecar'; mutate = {
			param($copy)
			$copy.files = @($copy.files | Where-Object { [string]$_.path -cne 'Assets/Captured/SelfTestRecording.teanim.meta' })
		}; expected = 'additions do not exactly match' },
		[ordered]@{ name = 'empty output'; mutate = {
			param($copy)
			@($copy.files | Where-Object { [string]$_.path -ceq 'Assets/Captured/SelfTestRecording.teanim' })[0].bytes = 0
		}; expected = 'is empty' },
		[ordered]@{ name = 'oversize output'; mutate = {
			param($copy)
			@($copy.files | Where-Object { [string]$_.path -ceq 'Assets/Captured/SelfTestRecording.teanim' })[0].bytes = [UInt64]536870913
		}; expected = 'exceeds 536870912 bytes' },
		[ordered]@{ name = 'empty metadata sidecar'; mutate = {
			param($copy)
			@($copy.files | Where-Object { [string]$_.path -ceq 'Assets/Captured/SelfTestRecording.teanim.meta' })[0].bytes = 0
		}; expected = 'is empty' }
	)) {
		$mutatedAfter = $recordingAfterSnapshot | ConvertTo-Json -Depth 80 | ConvertFrom-Json
		& $snapshotMutation.mutate $mutatedAfter
		$snapshotMutationRejected = $false
		try { [void](Compare-PlayModeProjectSnapshots -Before $recordingBeforeSnapshot -After $mutatedAfter -Declarations @($validatedPairedRecordingScenario.playModeRecordingOutputs)) } catch { $snapshotMutationRejected = $_.Exception.Message -match [regex]::Escape([string]$snapshotMutation.expected) }
		Assert-TestCondition -Condition $snapshotMutationRejected -Message "Creation-only comparison accepted $($snapshotMutation.name)."
	}
	$missingRecordingRegistry = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
	$missingProducerPublicationRejected = $false
	try { [void](Complete-PlayModeRecordingOutputContract -Scenario $validatedPairedRecordingScenario -ProjectRoot $recordingProjectRoot -BeforeSnapshot $recordingBeforeSnapshot -AfterSnapshot $recordingAfterSnapshot -BindingRegistry $missingRecordingRegistry -OperationInventory $recordingOperationInventory -PlayEnterSequence 10 -PlayExitSequence 20) } catch { $missingProducerPublicationRejected = $_.Exception.Message -match 'did not complete successful expectation' }
	Assert-TestCondition -Condition $missingProducerPublicationRejected -Message 'Recording output linked to a producer absent from the successful in-memory registry.'
	$relativeProducerRegistry = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
	$relativeProducerEnvelope = Copy-JsonValuePreservingArrays -Value $recordingProducerEnvelope
	$relativeProducerEnvelope['data']['destinationPath'] = 'Assets/Captured/SelfTestRecording.teanim'
	$relativeProducerRegistry['recording-stop'] = [ordered]@{ envelope = $relativeProducerEnvelope; mcpSequence = 12; envelopeSha256 = ('8' * 64); evidenceSha256 = ('7' * 64) }
	$relativeProducerRejected = $false
	try { [void](Complete-PlayModeRecordingOutputContract -Scenario $validatedPairedRecordingScenario -ProjectRoot $recordingProjectRoot -BeforeSnapshot $recordingBeforeSnapshot -AfterSnapshot $recordingAfterSnapshot -BindingRegistry $relativeProducerRegistry -OperationInventory $recordingOperationInventory -PlayEnterSequence 10 -PlayExitSequence 20) } catch { $relativeProducerRejected = $_.Exception.Message -match 'did not return an absolute path' }
	Assert-TestCondition -Condition $relativeProducerRejected -Message 'Recording producer relative response path was accepted.'
	$outsideProducerRegistry = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
	$outsideProducerEnvelope = Copy-JsonValuePreservingArrays -Value $recordingProducerEnvelope
	$outsideProducerEnvelope['data']['destinationPath'] = Join-Path $temporaryRoot 'outside.teanim'
	$outsideProducerRegistry['recording-stop'] = [ordered]@{ envelope = $outsideProducerEnvelope; mcpSequence = 12; envelopeSha256 = ('9' * 64); evidenceSha256 = ('7' * 64) }
	$outsideProducerRejected = $false
	try { [void](Complete-PlayModeRecordingOutputContract -Scenario $validatedPairedRecordingScenario -ProjectRoot $recordingProjectRoot -BeforeSnapshot $recordingBeforeSnapshot -AfterSnapshot $recordingAfterSnapshot -BindingRegistry $outsideProducerRegistry -OperationInventory $recordingOperationInventory -PlayEnterSequence 10 -PlayExitSequence 20) } catch { $outsideProducerRejected = $_.Exception.Message -match 'must remain below' }
	Assert-TestCondition -Condition $outsideProducerRejected -Message 'Recording producer response escaped the project root.'
	$caseProducerRegistry = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
	$caseProducerEnvelope = Copy-JsonValuePreservingArrays -Value $recordingProducerEnvelope
	$caseProducerEnvelope['data']['destinationPath'] = ([string]$recordingProducerEnvelope.data.destinationPath).Replace('\Assets\', '\assets\')
	$caseProducerRegistry['recording-stop'] = [ordered]@{ envelope = $caseProducerEnvelope; mcpSequence = 12; envelopeSha256 = ('a' * 64); evidenceSha256 = ('7' * 64) }
	$caseProducerRejected = $false
	try { [void](Complete-PlayModeRecordingOutputContract -Scenario $validatedPairedRecordingScenario -ProjectRoot $recordingProjectRoot -BeforeSnapshot $recordingBeforeSnapshot -AfterSnapshot $recordingAfterSnapshot -BindingRegistry $caseProducerRegistry -OperationInventory $recordingOperationInventory -PlayEnterSequence 10 -PlayExitSequence 20) } catch { $caseProducerRejected = $_.Exception.Message -match 'does not exactly match declared' }
	Assert-TestCondition -Condition $caseProducerRejected -Message 'Recording producer response path comparison was not case-sensitive.'
	$producerBoundaryRejected = $false
	try { [void](Complete-PlayModeRecordingOutputContract -Scenario $validatedPairedRecordingScenario -ProjectRoot $recordingProjectRoot -BeforeSnapshot $recordingBeforeSnapshot -AfterSnapshot $recordingAfterSnapshot -BindingRegistry $recordingRegistry -OperationInventory $recordingOperationInventory -PlayEnterSequence 12 -PlayExitSequence 20) } catch { $producerBoundaryRejected = $_.Exception.Message -match 'strictly between play.enter and play.exit' }
	Assert-TestCondition -Condition $producerBoundaryRejected -Message 'Recording producer sequence outside the strict Play Mode boundary was accepted.'

	# Fixture overlays model the reviewed cooked inputs that are not part of the
	# reusable base project. Use a fake repository root so this self-test never
	# writes into Tests/Fixtures/Animation or any developer-owned project.
	$overlayRepoRoot = Join-Path $temporaryRoot 'overlay-repository'
	$overlayAnimationRoot = Join-Path $overlayRepoRoot 'Tests/Fixtures/Animation'
	$overlaySourceRoot = Join-Path $overlayAnimationRoot 'SharedCooked'
	[System.IO.Directory]::CreateDirectory((Join-Path $overlaySourceRoot 'Nested')) | Out-Null
	[System.IO.File]::WriteAllText((Join-Path $overlaySourceRoot 'walk.tanim'), 'walk-cooked-payload', [System.Text.UTF8Encoding]::new($false))
	[System.IO.File]::WriteAllBytes((Join-Path $overlaySourceRoot 'Nested/rig.bin'), [byte[]](1, 3, 3, 7))
	$validOverlay = [ordered]@{
		source = 'Tests/Fixtures/Animation/SharedCooked'
		destination = 'Assets/Animation/SharedCooked'
	}
	$overlayScenario = New-TestScenarioWithFixtureOverlays -BaseScenario $scenario -FixtureOverlays @($validOverlay)
	[System.IO.File]::WriteAllText($scenarioPath, ($overlayScenario | ConvertTo-Json -Depth 30), [System.Text.UTF8Encoding]::new($false))
	$validatedOverlayScenario = Read-ScenarioManifest -Path $scenarioPath -ExpectedPhase 'self-test' -RepoRoot $overlayRepoRoot
	Assert-TestCondition -Condition (@($validatedOverlayScenario.fixtureOverlays).Count -eq 1) -Message 'One valid fixture overlay did not preserve its JSON-array declaration.'
	Assert-TestCondition -Condition ([int]$validatedOverlayScenario.fixtureOverlays[0].validatedSourceFiles -eq 2) -Message 'Fixture overlay validation did not inventory both source files.'

	$overlayBaseFixture = Join-Path $temporaryRoot 'overlay-base-fixture'
	$overlayProject = Join-Path $temporaryRoot 'overlay-project'
	[System.IO.Directory]::CreateDirectory((Join-Path $overlayBaseFixture 'Scenes')) | Out-Null
	[System.IO.File]::WriteAllText((Join-Path $overlayBaseFixture 'Scenes/base.terra'), 'base-project', [System.Text.UTF8Encoding]::new($false))
	[void](Copy-IsolatedFixture -Source $overlayBaseFixture -Destination $overlayProject)
	$overlayProvenance = @(Copy-FixtureOverlays -Overlays @($validatedOverlayScenario.fixtureOverlays) -RepoRoot $overlayRepoRoot -ProjectRoot $overlayProject)
	Assert-TestCondition -Condition ($overlayProvenance.Count -eq 1 -and [int]$overlayProvenance[0].fileCount -eq 2) -Message 'Valid fixture overlay copy did not emit complete provenance.'
	Assert-TestCondition -Condition ([string]$overlayProvenance[0].sourceInventorySha256 -ceq [string]$overlayProvenance[0].destinationInventorySha256) -Message 'Fixture overlay source and destination inventory hashes differ.'
	Assert-TestCondition -Condition ((Get-Sha256 -Path (Join-Path $overlayProject 'Assets/Animation/SharedCooked/walk.tanim')) -ceq (Get-Sha256 -Path (Join-Path $overlaySourceRoot 'walk.tanim'))) -Message 'Fixture overlay file was not copied byte-for-byte.'
	Assert-TestCondition -Condition ((Get-Sha256 -Path (Join-Path $overlayProject 'Scenes/base.terra')) -ceq (Get-Sha256 -Path (Join-Path $overlayBaseFixture 'Scenes/base.terra'))) -Message 'Fixture overlay copy changed the isolated base project.'

	# Fault the second destination hash after both File.Copy calls have succeeded.
	# The production rollback must remove both gate-created files while retaining
	# every file that belonged to the base fixture.
	$rollbackProject = Join-Path $temporaryRoot 'overlay-rollback-project'
	[void](Copy-IsolatedFixture -Source $overlayBaseFixture -Destination $rollbackProject)
	$script:FixtureOverlayRollbackOriginalGetSha256 = ${function:Get-Sha256}
	$script:FixtureOverlayRollbackFaultPath = [System.IO.Path]::GetFullPath((Join-Path $rollbackProject 'Assets/Animation/SharedCooked/walk.tanim'))
	$rollbackRejected = $false
	try {
		Set-Item -LiteralPath Function:Get-Sha256 -Value {
			param([Parameter(Mandatory = $true)][string]$Path)
			$fullPath = [System.IO.Path]::GetFullPath($Path)
			if ($fullPath.Equals($script:FixtureOverlayRollbackFaultPath, [System.StringComparison]::OrdinalIgnoreCase) -and
				(Test-Path -LiteralPath $fullPath -PathType Leaf)) {
				return ('0' * 64)
			}
			return & $script:FixtureOverlayRollbackOriginalGetSha256 -Path $Path
		}
		try {
			[void](Copy-FixtureOverlays -Overlays @($validatedOverlayScenario.fixtureOverlays) -RepoRoot $overlayRepoRoot -ProjectRoot $rollbackProject)
		}
		catch {
			$rollbackRejected = $_.Exception.Message -match 'not byte-for-byte identical'
		}
	}
	finally {
		Set-Item -LiteralPath Function:Get-Sha256 -Value $script:FixtureOverlayRollbackOriginalGetSha256
	}
	Assert-TestCondition -Condition $rollbackRejected -Message 'Injected late overlay hash failure did not fail closed.'
	Assert-TestCondition -Condition (-not (Test-Path -LiteralPath (Join-Path $rollbackProject 'Assets/Animation/SharedCooked/Nested/rig.bin'))) -Message 'Overlay rollback left the first successfully copied file behind.'
	Assert-TestCondition -Condition (-not (Test-Path -LiteralPath (Join-Path $rollbackProject 'Assets/Animation/SharedCooked/walk.tanim'))) -Message 'Overlay rollback left the hash-failed file behind.'
	Assert-TestCondition -Condition (Test-Path -LiteralPath (Join-Path $rollbackProject 'Scenes/base.terra') -PathType Leaf) -Message 'Overlay rollback removed a base-project file.'

	$emptyOverlayScenario = New-TestScenarioWithFixtureOverlays -BaseScenario $scenario -FixtureOverlays @()
	[System.IO.File]::WriteAllText($scenarioPath, ($emptyOverlayScenario | ConvertTo-Json -Depth 30), [System.Text.UTF8Encoding]::new($false))
	$emptyOverlayValidated = Read-ScenarioManifest -Path $scenarioPath -ExpectedPhase 'self-test' -RepoRoot $overlayRepoRoot
	Assert-TestCondition -Condition (@($emptyOverlayValidated.fixtureOverlays).Count -eq 0) -Message 'An explicit empty fixtureOverlays JSON array was not accepted.'

	$nonArrayOverlayScenario = $scenario | ConvertTo-Json -Depth 30 | ConvertFrom-Json
	$nonArrayOverlayScenario | Add-Member -NotePropertyName fixtureOverlays -NotePropertyValue ([pscustomobject]$validOverlay) -Force
	Assert-FixtureOverlayScenarioRejected -Scenario $nonArrayOverlayScenario -ScenarioPath $scenarioPath -RepoRoot $overlayRepoRoot -ExpectedMessage 'must be a JSON array' -FailureMessage 'A fixtureOverlays object was accepted as an array.'

	$tooManyOverlays = @()
	for ($overlayIndex = 0; $overlayIndex -lt 17; ++$overlayIndex) {
		$tooManyOverlays += [ordered]@{ source = 'Tests/Fixtures/Animation/SharedCooked'; destination = "Assets/Overlay$overlayIndex" }
	}
	$maximumOverlayScenario = New-TestScenarioWithFixtureOverlays -BaseScenario $scenario -FixtureOverlays @($tooManyOverlays | Select-Object -First 16)
	[System.IO.File]::WriteAllText($scenarioPath, ($maximumOverlayScenario | ConvertTo-Json -Depth 30), [System.Text.UTF8Encoding]::new($false))
	$maximumOverlayValidated = Read-ScenarioManifest -Path $scenarioPath -ExpectedPhase 'self-test' -RepoRoot $overlayRepoRoot
	Assert-TestCondition -Condition (@($maximumOverlayValidated.fixtureOverlays).Count -eq 16) -Message 'The declared maximum of sixteen non-overlapping fixture overlays was rejected.'
	Assert-FixtureOverlayScenarioRejected -Scenario (New-TestScenarioWithFixtureOverlays -BaseScenario $scenario -FixtureOverlays $tooManyOverlays) -ScenarioPath $scenarioPath -RepoRoot $overlayRepoRoot -ExpectedMessage 'at most 16' -FailureMessage 'More than sixteen fixture overlays were accepted.'

	$unknownOverlayKey = [ordered]@{ source = $validOverlay.source; destination = $validOverlay.destination; overwrite = $false }
	Assert-FixtureOverlayScenarioRejected -Scenario (New-TestScenarioWithFixtureOverlays -BaseScenario $scenario -FixtureOverlays @($unknownOverlayKey)) -ScenarioPath $scenarioPath -RepoRoot $overlayRepoRoot -ExpectedMessage "exactly 'source' and 'destination'" -FailureMessage 'A fixture overlay with an unknown key was accepted.'
	$missingOverlayKey = [ordered]@{ source = $validOverlay.source }
	Assert-FixtureOverlayScenarioRejected -Scenario (New-TestScenarioWithFixtureOverlays -BaseScenario $scenario -FixtureOverlays @($missingOverlayKey)) -ScenarioPath $scenarioPath -RepoRoot $overlayRepoRoot -ExpectedMessage "exactly 'source' and 'destination'" -FailureMessage 'A fixture overlay with a missing destination was accepted.'

	$duplicateOverlayJson = '{"schemaVersion":2,"phase":"self-test","fixtureOverlays":[{"source":"Tests/Fixtures/Animation/SharedCooked","source":"Tests/Fixtures/Animation/SharedCooked","destination":"Assets/Animation/SharedCooked"}]}'
	[System.IO.File]::WriteAllText($scenarioPath, $duplicateOverlayJson, [System.Text.UTF8Encoding]::new($false))
	$duplicateOverlayKeyRejected = $false
	try { [void](Read-ScenarioManifest -Path $scenarioPath -ExpectedPhase 'self-test' -RepoRoot $overlayRepoRoot) } catch { $duplicateOverlayKeyRejected = $_.Exception.Message -match 'Duplicate JSON object member' }
	Assert-TestCondition -Condition $duplicateOverlayKeyRejected -Message 'A duplicate fixture overlay object key was accepted.'

	foreach ($unsafeSource in @(
		'', '../SharedCooked', 'Tests/Fixtures/Animation/../Outside',
		'C:/Tests/Fixtures/Animation/SharedCooked', '//server/share/SharedCooked',
		'//?/C:/Tests/Fixtures/Animation/SharedCooked', 'Tests\Fixtures\Animation\SharedCooked',
		'Tests/Fixtures/Other/SharedCooked'
	)) {
		$unsafeOverlay = [ordered]@{ source = $unsafeSource; destination = 'Assets/Animation/Safe' }
		Assert-FixtureOverlayScenarioRejected -Scenario (New-TestScenarioWithFixtureOverlays -BaseScenario $scenario -FixtureOverlays @($unsafeOverlay)) -ScenarioPath $scenarioPath -RepoRoot $overlayRepoRoot -ExpectedMessage 'Fixture overlay source' -FailureMessage "Unsafe fixture overlay source '$unsafeSource' was accepted."
	}
	foreach ($unsafeDestination in @(
		'', '../Assets/Animation', 'Assets/../Animation', 'C:/Assets/Animation',
		'//server/share/Animation', '//?/C:/Assets/Animation', 'Assets\Animation'
	)) {
		$unsafeOverlay = [ordered]@{ source = $validOverlay.source; destination = $unsafeDestination }
		Assert-FixtureOverlayScenarioRejected -Scenario (New-TestScenarioWithFixtureOverlays -BaseScenario $scenario -FixtureOverlays @($unsafeOverlay)) -ScenarioPath $scenarioPath -RepoRoot $overlayRepoRoot -ExpectedMessage 'Fixture overlay destination' -FailureMessage "Unsafe fixture overlay destination '$unsafeDestination' was accepted."
	}

	$missingSourceOverlay = [ordered]@{ source = 'Tests/Fixtures/Animation/MissingCooked'; destination = 'Assets/MissingCooked' }
	Assert-FixtureOverlayScenarioRejected -Scenario (New-TestScenarioWithFixtureOverlays -BaseScenario $scenario -FixtureOverlays @($missingSourceOverlay)) -ScenarioPath $scenarioPath -RepoRoot $overlayRepoRoot -ExpectedMessage 'source was not found' -FailureMessage 'A missing fixture overlay source was accepted.'
	$emptySourceRoot = Join-Path $overlayAnimationRoot 'EmptyCooked'
	[System.IO.Directory]::CreateDirectory($emptySourceRoot) | Out-Null
	$emptySourceOverlay = [ordered]@{ source = 'Tests/Fixtures/Animation/EmptyCooked'; destination = 'Assets/EmptyCooked' }
	Assert-FixtureOverlayScenarioRejected -Scenario (New-TestScenarioWithFixtureOverlays -BaseScenario $scenario -FixtureOverlays @($emptySourceOverlay)) -ScenarioPath $scenarioPath -RepoRoot $overlayRepoRoot -ExpectedMessage 'is empty' -FailureMessage 'An empty fixture overlay directory was accepted.'
	$emptyFilePath = Join-Path $overlayAnimationRoot 'empty.tanim'
	[System.IO.File]::WriteAllBytes($emptyFilePath, [byte[]]@())
	$emptyFileOverlay = [ordered]@{ source = 'Tests/Fixtures/Animation/empty.tanim'; destination = 'Assets/empty.tanim' }
	Assert-FixtureOverlayScenarioRejected -Scenario (New-TestScenarioWithFixtureOverlays -BaseScenario $scenario -FixtureOverlays @($emptyFileOverlay)) -ScenarioPath $scenarioPath -RepoRoot $overlayRepoRoot -ExpectedMessage 'empty file' -FailureMessage 'An empty fixture overlay file was accepted.'

	$overlapOverlays = @(
		[ordered]@{ source = $validOverlay.source; destination = 'Assets/Animation' },
		[ordered]@{ source = $validOverlay.source; destination = 'Assets/Animation/Nested' }
	)
	Assert-FixtureOverlayScenarioRejected -Scenario (New-TestScenarioWithFixtureOverlays -BaseScenario $scenario -FixtureOverlays $overlapOverlays) -ScenarioPath $scenarioPath -RepoRoot $overlayRepoRoot -ExpectedMessage 'destination.*conflicts' -FailureMessage 'Overlapping fixture overlay destinations were accepted.'

	$originalMaximumOverlayFiles = $script:MaximumFixtureOverlayFiles
	try {
		$script:MaximumFixtureOverlayFiles = 1
		Assert-FixtureOverlayScenarioRejected -Scenario $overlayScenario -ScenarioPath $scenarioPath -RepoRoot $overlayRepoRoot -ExpectedMessage 'maximum file count' -FailureMessage 'Fixture overlay file-count limit was not enforced.'
	}
	finally {
		$script:MaximumFixtureOverlayFiles = $originalMaximumOverlayFiles
	}
	$originalMaximumOverlayBytes = $script:MaximumFixtureOverlayBytes
	try {
		$script:MaximumFixtureOverlayBytes = [UInt64]3
		Assert-FixtureOverlayScenarioRejected -Scenario $overlayScenario -ScenarioPath $scenarioPath -RepoRoot $overlayRepoRoot -ExpectedMessage 'maximum byte count' -FailureMessage 'Fixture overlay byte-count limit was not enforced.'
	}
	finally {
		$script:MaximumFixtureOverlayBytes = $originalMaximumOverlayBytes
	}

	$overlayJunctionTarget = Join-Path $overlayAnimationRoot 'JunctionTarget'
	$overlayJunctionPath = Join-Path $overlayAnimationRoot 'LinkedCooked'
	[System.IO.Directory]::CreateDirectory($overlayJunctionTarget) | Out-Null
	[System.IO.File]::WriteAllText((Join-Path $overlayJunctionTarget 'linked.tanim'), 'linked-cooked-payload', [System.Text.UTF8Encoding]::new($false))
	[void](New-Item -ItemType Junction -Path $overlayJunctionPath -Target $overlayJunctionTarget -ErrorAction Stop)
	$junctionOverlay = [ordered]@{ source = 'Tests/Fixtures/Animation/LinkedCooked'; destination = 'Assets/LinkedCooked' }
	Assert-FixtureOverlayScenarioRejected -Scenario (New-TestScenarioWithFixtureOverlays -BaseScenario $scenario -FixtureOverlays @($junctionOverlay)) -ScenarioPath $scenarioPath -RepoRoot $overlayRepoRoot -ExpectedMessage 'symbolic links or junctions' -FailureMessage 'A reparse-point fixture overlay source was accepted.'
	[System.IO.Directory]::Delete($overlayJunctionPath)
	$overlayJunctionPath = $null

	$destinationJunctionProject = Join-Path $temporaryRoot 'overlay-destination-junction-project'
	$destinationJunctionTarget = Join-Path $temporaryRoot 'overlay-destination-junction-target'
	[System.IO.Directory]::CreateDirectory((Join-Path $destinationJunctionProject 'Assets')) | Out-Null
	[System.IO.Directory]::CreateDirectory($destinationJunctionTarget) | Out-Null
	$overlayJunctionPath = Join-Path $destinationJunctionProject 'Assets/Animation'
	[void](New-Item -ItemType Junction -Path $overlayJunctionPath -Target $destinationJunctionTarget -ErrorAction Stop)
	$destinationJunctionRejected = $false
	try {
		[void](Copy-FixtureOverlays -Overlays @($validatedOverlayScenario.fixtureOverlays) -RepoRoot $overlayRepoRoot -ProjectRoot $destinationJunctionProject)
	}
	catch {
		$destinationJunctionRejected = $_.Exception.Message -match 'symbolic links or junctions'
	}
	Assert-TestCondition -Condition $destinationJunctionRejected -Message 'A fixture overlay destination-parent junction was accepted.'
	Assert-TestCondition -Condition (-not (Test-Path -LiteralPath (Join-Path $destinationJunctionTarget 'SharedCooked/walk.tanim'))) -Message 'Rejected destination-parent junction redirected an overlay write outside the project.'
	[System.IO.Directory]::Delete($overlayJunctionPath)
	$overlayJunctionPath = $null

	$conflictingBaseFixture = Join-Path $temporaryRoot 'overlay-conflicting-base'
	$conflictingProject = Join-Path $temporaryRoot 'overlay-conflicting-project'
	[System.IO.Directory]::CreateDirectory((Join-Path $conflictingBaseFixture 'Assets/Animation/SharedCooked')) | Out-Null
	[System.IO.File]::WriteAllText((Join-Path $conflictingBaseFixture 'Assets/Animation/SharedCooked/base-owned.txt'), 'base-owned', [System.Text.UTF8Encoding]::new($false))
	[void](Copy-IsolatedFixture -Source $conflictingBaseFixture -Destination $conflictingProject)
	$baseConflictRejected = $false
	try {
		[void](Copy-FixtureOverlays -Overlays @($validatedOverlayScenario.fixtureOverlays) -RepoRoot $overlayRepoRoot -ProjectRoot $conflictingProject)
	}
	catch {
		$baseConflictRejected = $_.Exception.Message -match 'overlaps base-project file'
	}
	Assert-TestCondition -Condition $baseConflictRejected -Message 'A fixture overlay destination overlapping a base-project file was accepted.'
	Assert-TestCondition -Condition (-not (Test-Path -LiteralPath (Join-Path $conflictingProject 'Assets/Animation/SharedCooked/walk.tanim'))) -Message 'Rejected fixture overlay copied files before completing collision preflight.'
	$baseFixtureCopyIndex = $runnerSource.IndexOf('$fixtureCopy = Copy-IsolatedFixture', [System.StringComparison]::Ordinal)
	$overlayCopyIndex = $runnerSource.IndexOf('$fixtureOverlayEvidence = @(Copy-FixtureOverlays', [System.StringComparison]::Ordinal)
	$layoutInstallIndex = $runnerSource.IndexOf('$layoutEvidence = @(Install-VerifiedDefaultEditorLayout', [System.StringComparison]::Ordinal)
	Assert-TestCondition -Condition ($baseFixtureCopyIndex -ge 0 -and $overlayCopyIndex -gt $baseFixtureCopyIndex -and $layoutInstallIndex -gt $overlayCopyIndex) -Message 'Live gate does not copy fixture overlays between the isolated base fixture and verified editor-layout install.'
	Assert-TestCondition -Condition ($runnerSource.Contains('$phaseEvidence.inputs | Add-Member -NotePropertyName fixtureOverlays')) -Message 'Live phase evidence does not record fixture overlay provenance.'

	# The durable MCP summary must contain one typed, expectation-bound result for
	# every reviewed operation and capture. File existence alone is not evidence.
	$editorDirectory = Join-Path $temporaryRoot 'editor'
	[System.IO.Directory]::CreateDirectory($editorDirectory) | Out-Null
	$operationEvidenceRecords = [System.Collections.Generic.List[object]]::new()
	foreach ($definition in @(@($validatedScenario.operations) + @($validatedScenario.captures))) {
		$kind = if (@($validatedScenario.captures | Where-Object { [string]$_.id -eq [string]$definition.id }).Count -eq 1) { 'capture' } else { 'operation' }
		$fakeEnvelope = [ordered]@{ ok = $true; status = 'completed'; data = [ordered]@{ fixture = [string]$definition.id } }
		$fakeEnvelopeHash = Get-StringSha256 -Value ($fakeEnvelope | ConvertTo-Json -Depth 20 -Compress)
		$artifactReference = $null
		if ($kind -eq 'operation') {
			$operationArtifactPath = Join-Path $temporaryRoot ([string]$definition.evidencePath)
			[System.IO.Directory]::CreateDirectory((Split-Path -Parent $operationArtifactPath)) | Out-Null
			$operationArtifact = [ordered]@{
				schemaVersion = 1
				phase = 'self-test'
				id = [string]$definition.id
				stage = [string]$definition.stage
				order = [int]$definition.order
				tool = [string]$definition.tool
				arguments = [ordered]@{}
				expected = New-ScenarioExpectationRecord -Expectation (Get-OptionalProperty -Object $definition -Name 'expect')
				expectationPassed = $true
				envelopeSha256 = $fakeEnvelopeHash
				envelope = $fakeEnvelope
				fileTransition = $null
			}
			[System.IO.File]::WriteAllText($operationArtifactPath, ($operationArtifact | ConvertTo-Json -Depth 30), [System.Text.UTF8Encoding]::new($false))
			$artifactReference = [ordered]@{ path = [string]$definition.evidencePath; sha256 = Get-Sha256 -Path $operationArtifactPath }
		}
		$operationEvidenceRecords.Add([ordered]@{
			id = [string]$definition.id
			kind = $kind
			stage = [string]$definition.stage
			order = [int]$definition.order
			tool = [string]$definition.tool
			expectationPassed = $true
			expected = New-ScenarioExpectationRecord -Expectation (Get-OptionalProperty -Object $definition -Name 'expect')
			observed = [ordered]@{ ok = $true; status = 'completed'; errorCategory = $null; envelopeSha256 = $fakeEnvelopeHash }
			evidence = $artifactReference
			fileTransition = $null
		})
	}
	$reconnectEvidencePath = Join-Path $editorDirectory 'mcp-reconnect.json'
	$reconnectEvidenceDocument = [ordered]@{
		schemaVersion = 1
		endpoint = 'http://127.0.0.1:43143/mcp'
		connectionBoundary = [ordered]@{ requestedAtUtc = [DateTimeOffset]::UtcNow.AddMilliseconds(-4).ToString('o'); sequence = 1; disableKeepAlive = $true; serverName = 'TerraEngine Editor' }
		freshInitialize = [ordered]@{ requestedAtUtc = [DateTimeOffset]::UtcNow.AddMilliseconds(-3).ToString('o'); sequence = 2; serverName = 'TerraEngine Editor' }
		postReconnect = [ordered]@{ toolListSequence = 3; requiredToolsPresent = $true; advertisedTools = @('play.state'); playStateSequence = 4; playState = 'stopped'; operationPassed = $true }
		passed = $true
		completedAtUtc = [DateTimeOffset]::UtcNow.ToString('o')
	}
	[System.IO.File]::WriteAllText($reconnectEvidencePath, ($reconnectEvidenceDocument | ConvertTo-Json -Depth 20), [System.Text.UTF8Encoding]::new($false))
	$operationEvidencePath = Join-Path $editorDirectory 'mcp-operations.json'
	$operationEvidenceDocument = [ordered]@{
		schemaVersion = 1
		phase = 'self-test'
		declaredItemCount = $operationEvidenceRecords.Count
		operations = @($operationEvidenceRecords)
		reconnectPassed = $true
		reconnectEvidence = [ordered]@{ path = 'editor/mcp-reconnect.json'; sha256 = Get-Sha256 -Path $reconnectEvidencePath }
	}
	[System.IO.File]::WriteAllText($operationEvidencePath, ($operationEvidenceDocument | ConvertTo-Json -Depth 30), [System.Text.UTF8Encoding]::new($false))
	$validatedOperationEvidence = Read-AndValidateOperationEvidence -Path $operationEvidencePath -Scenario $validatedScenario -RunRoot $temporaryRoot
	Assert-TestCondition -Condition ($validatedOperationEvidence.operationCount -eq 4) -Message 'Valid typed MCP operation evidence did not pass.'
	$emptyOperationEvidence = $operationEvidenceDocument | ConvertTo-Json -Depth 30 | ConvertFrom-Json
	$emptyOperationEvidence.operations = @()
	[System.IO.File]::WriteAllText($operationEvidencePath, ($emptyOperationEvidence | ConvertTo-Json -Depth 30), [System.Text.UTF8Encoding]::new($false))
	$emptyOperationEvidenceRejected = $false
	try { [void](Read-AndValidateOperationEvidence -Path $operationEvidencePath -Scenario $validatedScenario -RunRoot $temporaryRoot) } catch { $emptyOperationEvidenceRejected = $_.Exception.Message -match 'non-empty JSON array' }
	Assert-TestCondition -Condition $emptyOperationEvidenceRejected -Message 'Empty MCP operation evidence satisfied the required-evidence contract.'
	[System.IO.File]::WriteAllText($operationEvidencePath, ($operationEvidenceDocument | ConvertTo-Json -Depth 30), [System.Text.UTF8Encoding]::new($false))

	$invalidReconnectEvidence = $reconnectEvidenceDocument | ConvertTo-Json -Depth 20 | ConvertFrom-Json
	$invalidReconnectEvidence.connectionBoundary.disableKeepAlive = $false
	[System.IO.File]::WriteAllText($reconnectEvidencePath, ($invalidReconnectEvidence | ConvertTo-Json -Depth 20), [System.Text.UTF8Encoding]::new($false))
	$invalidReconnectSummary = $operationEvidenceDocument | ConvertTo-Json -Depth 30 | ConvertFrom-Json
	$invalidReconnectSummary.reconnectEvidence.sha256 = Get-Sha256 -Path $reconnectEvidencePath
	[System.IO.File]::WriteAllText($operationEvidencePath, ($invalidReconnectSummary | ConvertTo-Json -Depth 30), [System.Text.UTF8Encoding]::new($false))
	$falseReconnectRejected = $false
	try { [void](Read-AndValidateOperationEvidence -Path $operationEvidencePath -Scenario $validatedScenario -RunRoot $temporaryRoot) } catch { $falseReconnectRejected = $_.Exception.Message -match 'does not prove an ordered connection-close boundary' }
	Assert-TestCondition -Condition $falseReconnectRejected -Message 'Reconnect evidence without a forced HTTP close boundary was accepted.'
	[System.IO.File]::WriteAllText($reconnectEvidencePath, ($reconnectEvidenceDocument | ConvertTo-Json -Depth 20), [System.Text.UTF8Encoding]::new($false))
	$operationEvidenceDocument.reconnectEvidence.sha256 = Get-Sha256 -Path $reconnectEvidencePath
	[System.IO.File]::WriteAllText($operationEvidencePath, ($operationEvidenceDocument | ConvertTo-Json -Depth 30), [System.Text.UTF8Encoding]::new($false))
	$legacyLiveDowngradeRejected = $false
	try {
		[void](Read-AndValidateOperationEvidence -Path $operationEvidencePath -Scenario $validatedScenario -RunRoot $temporaryRoot -MinimumSchemaVersion 2)
	}
	catch { $legacyLiveDowngradeRejected = $_.Exception.Message -match 'requires schemaVersion 2 or later' }
	Assert-TestCondition -Condition $legacyLiveDowngradeRejected -Message 'The live validation path accepted downgraded schema-v1 evidence for a no-binding scenario.'
	$bindingLegacyDowngradeRejected = $false
	try {
		[void](Read-AndValidateOperationEvidence -Path $operationEvidencePath -Scenario $validatedBindingScenario -RunRoot $temporaryRoot)
	}
	catch { $bindingLegacyDowngradeRejected = $_.Exception.Message -match 'argument bindings require schemaVersion 2' }
	Assert-TestCondition -Condition $bindingLegacyDowngradeRejected -Message 'A binding scenario accepted legacy evidence without transcript or resolved-argument provenance.'

	# Build a complete schema-v2 summary, per-operation artifact set, and MCP
	# transcript for the binding scenario. This exercises the same provenance
	# reconstruction used during final evidence locking.
	$bindingEvidenceRoot = Join-Path $temporaryRoot 'binding-evidence'
	$bindingEditorRoot = Join-Path $bindingEvidenceRoot 'editor'
	$bindingOperationsRoot = Join-Path $bindingEditorRoot 'operations'
	[System.IO.Directory]::CreateDirectory($bindingOperationsRoot) | Out-Null
	$bindingReconnectPath = Join-Path $bindingEditorRoot 'mcp-reconnect.json'
	$bindingReconnectDocument = [ordered]@{
		schemaVersion = 1
		endpoint = 'http://127.0.0.1:43143/mcp'
		connectionBoundary = [ordered]@{ requestedAtUtc = [DateTimeOffset]::UtcNow.AddMilliseconds(-4).ToString('o'); sequence = 201; disableKeepAlive = $true; serverName = 'TerraEngine Editor' }
		freshInitialize = [ordered]@{ requestedAtUtc = [DateTimeOffset]::UtcNow.AddMilliseconds(-3).ToString('o'); sequence = 202; serverName = 'TerraEngine Editor' }
		postReconnect = [ordered]@{ toolListSequence = 203; requiredToolsPresent = $true; advertisedTools = @('play.state'); playStateSequence = 204; playState = 'stopped'; operationPassed = $true }
		passed = $true
		completedAtUtc = [DateTimeOffset]::UtcNow.ToString('o')
	}
	[System.IO.File]::WriteAllText($bindingReconnectPath, ($bindingReconnectDocument | ConvertTo-Json -Depth 20), [System.Text.UTF8Encoding]::new($false))
	$bindingExecutionDefinitions = [System.Collections.Generic.List[object]]::new()
	foreach ($stageName in @('prePlay', 'play', 'postPlay')) {
		$stageDefinitions = @(@($validatedBindingScenario.operations) + @($validatedBindingScenario.captures) |
			Where-Object { [string]$_.stage -ceq $stageName } |
			Sort-Object { [int]$_.order })
		foreach ($stageDefinition in $stageDefinitions) { $bindingExecutionDefinitions.Add($stageDefinition) }
	}
	$bindingEvidenceRecords = [System.Collections.Generic.List[object]]::new()
	$bindingTranscriptLines = [System.Collections.Generic.List[string]]::new()
	$bindingEvidenceRegistry = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
	$bindingSequence = 100
	foreach ($definition in $bindingExecutionDefinitions) {
		$bindingSequence++
		$id = [string]$definition.id
		$kind = if (@($validatedBindingScenario.captures | Where-Object { [string]$_.id -ceq $id }).Count -eq 1) { 'capture' } else { 'operation' }
		$resolvedArguments = Get-ScenarioInvocationArguments -Definition $definition -Kind $kind
		$resolved = if ($kind -eq 'operation') {
			Resolve-ScenarioArgumentBindings -Definition $definition -Arguments $resolvedArguments -BindingRegistry $bindingEvidenceRegistry
		}
		else {
			[ordered]@{ argumentsSha256 = Get-CanonicalJsonSha256 -Value $resolvedArguments; bindings = @() }
		}
		$envelope = if ($id -ceq 'binding-source') {
			$bindingSourceEnvelope
		}
		elseif ($id -ceq 'binding-destination') {
			[ordered]@{ ok = $true; status = 'completed'; data = [ordered]@{ accepted = $true } }
		}
		else {
			[ordered]@{ ok = $true; status = 'completed'; data = [ordered]@{ fixture = $id } }
		}
		$envelopeHash = Get-StringSha256 -Value ($envelope | ConvertTo-Json -Depth 80 -Compress)
		$transcriptResponse = [ordered]@{
			jsonrpc = '2.0'
			id = $bindingSequence
			result = [ordered]@{
				content = @([ordered]@{ type = 'text'; text = ($envelope | ConvertTo-Json -Depth 80 -Compress) })
			}
		}
		$bindingTranscriptLines.Add(([ordered]@{
			sequence = $bindingSequence
			timestamp = [DateTimeOffset]::UtcNow.ToString('o')
			durationMs = 1
			transport = [ordered]@{ disableKeepAlive = $false; connectionIntent = 'default-persistent' }
			request = [ordered]@{
				jsonrpc = '2.0'
				id = $bindingSequence
				method = 'tools/call'
				params = [ordered]@{ name = [string]$definition.tool; arguments = $resolvedArguments }
			}
			response = $transcriptResponse
		} | ConvertTo-Json -Depth 80 -Compress))

		$artifactReference = $null
		if ($kind -eq 'operation') {
			$artifactPath = Join-Path $bindingEvidenceRoot ([string]$definition.evidencePath)
			[System.IO.Directory]::CreateDirectory((Split-Path -Parent $artifactPath)) | Out-Null
			$artifact = [ordered]@{
				schemaVersion = 2
				phase = 'self-test'
				id = $id
				stage = [string]$definition.stage
				order = [int]$definition.order
				tool = [string]$definition.tool
				mcpSequence = $bindingSequence
				arguments = $resolvedArguments
				resolvedArguments = $resolvedArguments
				resolvedArgumentsSha256 = [string]$resolved.argumentsSha256
				argumentBindings = @($resolved.bindings)
				expected = New-ScenarioExpectationRecord -Expectation (Get-OptionalProperty -Object $definition -Name 'expect')
				expectationPassed = $true
				envelopeSha256 = $envelopeHash
				envelope = $envelope
				approval = $null
				fileTransition = $null
			}
			[System.IO.File]::WriteAllText($artifactPath, ($artifact | ConvertTo-Json -Depth 80), [System.Text.UTF8Encoding]::new($false))
			$artifactReference = [ordered]@{ path = [string]$definition.evidencePath; sha256 = Get-Sha256 -Path $artifactPath }
		}
		$bindingEvidenceRecords.Add([ordered]@{
			id = $id
			kind = $kind
			stage = [string]$definition.stage
			order = [int]$definition.order
			tool = [string]$definition.tool
			mcpSequence = $bindingSequence
			resolvedArguments = $resolvedArguments
			resolvedArgumentsSha256 = [string]$resolved.argumentsSha256
			argumentBindings = @($resolved.bindings)
			expectationPassed = $true
			expected = New-ScenarioExpectationRecord -Expectation (Get-OptionalProperty -Object $definition -Name 'expect')
			observed = [ordered]@{
				ok = $true
				status = Get-NestedValue -Object $envelope -Path 'status'
				errorCategory = $null
				envelopeSha256 = $envelopeHash
			}
			evidence = $artifactReference
			approval = $null
			fileTransition = $null
		})
		if ($kind -eq 'operation') {
			$bindingEvidenceRegistry[$id] = [ordered]@{
				envelope = $envelope
				mcpSequence = $bindingSequence
				envelopeSha256 = $envelopeHash
				evidenceSha256 = [string]$artifactReference.sha256
				approval = $null
			}
		}
	}
	$bindingTranscriptPath = Join-Path $bindingEditorRoot 'mcp.jsonl'
	[System.IO.File]::WriteAllLines($bindingTranscriptPath, [string[]]@($bindingTranscriptLines), [System.Text.UTF8Encoding]::new($false))
	$bindingOperationEvidencePath = Join-Path $bindingEditorRoot 'mcp-operations.json'
	$bindingOperationEvidenceDocument = [ordered]@{
		schemaVersion = 2
		phase = 'self-test'
		declaredItemCount = $bindingEvidenceRecords.Count
		operations = @($bindingEvidenceRecords)
		transcriptEvidence = [ordered]@{ path = 'editor/mcp.jsonl'; sha256 = Get-Sha256 -Path $bindingTranscriptPath }
		reconnectPassed = $true
		reconnectEvidence = [ordered]@{ path = 'editor/mcp-reconnect.json'; sha256 = Get-Sha256 -Path $bindingReconnectPath }
	}
	[System.IO.File]::WriteAllText($bindingOperationEvidencePath, ($bindingOperationEvidenceDocument | ConvertTo-Json -Depth 80), [System.Text.UTF8Encoding]::new($false))
	$validatedBindingEvidence = Read-AndValidateOperationEvidence -Path $bindingOperationEvidencePath -Scenario $validatedBindingScenario -RunRoot $bindingEvidenceRoot
	Assert-TestCondition -Condition ([int]$validatedBindingEvidence.schemaVersion -eq 2 -and [int]$validatedBindingEvidence.argumentBindingCount -eq 5) -Message 'Valid schema-v2 argument binding evidence did not pass.'

	$bindingEvidenceBaselineJson = $bindingOperationEvidenceDocument | ConvertTo-Json -Depth 80
	$bindingTranscriptBaseline = [string[]]@(Get-Content -LiteralPath $bindingTranscriptPath)
	# Invoke-McpRpc writes both the parsed response and a same-sequence catch
	# record when a JSON-RPC error is returned. Unreferenced response/error pairs
	# are valid transcript history and must not break later successful evidence.
	$duplicateErrorPairLines = [System.Collections.Generic.List[string]]::new()
	$duplicateErrorResponse = [ordered]@{
		sequence = 90
		timestamp = [DateTimeOffset]::UtcNow.AddMilliseconds(-2).ToString('o')
		request = [ordered]@{ jsonrpc = '2.0'; id = 90; method = 'tools/call'; params = [ordered]@{ name = 'fixture.failed'; arguments = [ordered]@{} } }
		response = [ordered]@{ jsonrpc = '2.0'; id = 90; error = [ordered]@{ code = -32000; message = 'expected fixture error' } }
	}
	$duplicateErrorCatch = [ordered]@{
		sequence = 90
		timestamp = [DateTimeOffset]::UtcNow.AddMilliseconds(-1).ToString('o')
		request = $duplicateErrorResponse.request
		error = 'JSON-RPC error -32000: expected fixture error'
	}
	$duplicateErrorPairLines.Add(($duplicateErrorResponse | ConvertTo-Json -Depth 80 -Compress))
	$duplicateErrorPairLines.Add(($duplicateErrorCatch | ConvertTo-Json -Depth 80 -Compress))
	foreach ($baselineLine in $bindingTranscriptBaseline) { $duplicateErrorPairLines.Add($baselineLine) }
	[System.IO.File]::WriteAllLines($bindingTranscriptPath, [string[]]@($duplicateErrorPairLines), [System.Text.UTF8Encoding]::new($false))
	$duplicateErrorPairEvidence = $bindingEvidenceBaselineJson | ConvertFrom-Json
	$duplicateErrorPairEvidence.transcriptEvidence.sha256 = Get-Sha256 -Path $bindingTranscriptPath
	[System.IO.File]::WriteAllText($bindingOperationEvidencePath, ($duplicateErrorPairEvidence | ConvertTo-Json -Depth 80), [System.Text.UTF8Encoding]::new($false))
	[void](Read-AndValidateOperationEvidence -Path $bindingOperationEvidencePath -Scenario $validatedBindingScenario -RunRoot $bindingEvidenceRoot -MinimumSchemaVersion 2)
	[System.IO.File]::WriteAllLines($bindingTranscriptPath, $bindingTranscriptBaseline, [System.Text.UTF8Encoding]::new($false))

	$duplicateSuccessfulResponseLines = [System.Collections.Generic.List[string]]::new()
	$duplicateSuccessfulResponseLines.Add($bindingTranscriptBaseline[0])
	$duplicateSuccessfulResponseLines.Add($bindingTranscriptBaseline[0])
	foreach ($baselineLine in @($bindingTranscriptBaseline | Select-Object -Skip 1)) { $duplicateSuccessfulResponseLines.Add($baselineLine) }
	[System.IO.File]::WriteAllLines($bindingTranscriptPath, [string[]]@($duplicateSuccessfulResponseLines), [System.Text.UTF8Encoding]::new($false))
	$duplicateSuccessfulResponseEvidence = $bindingEvidenceBaselineJson | ConvertFrom-Json
	$duplicateSuccessfulResponseEvidence.transcriptEvidence.sha256 = Get-Sha256 -Path $bindingTranscriptPath
	Assert-OperationEvidenceRejected -Evidence $duplicateSuccessfulResponseEvidence -EvidencePath $bindingOperationEvidencePath -Scenario $validatedBindingScenario -RunRoot $bindingEvidenceRoot -ExpectedMessage 'exactly one successful transcript response' -FailureMessage 'Schema-v2 validation accepted two transcript responses for one claimed MCP sequence.'
	[System.IO.File]::WriteAllLines($bindingTranscriptPath, $bindingTranscriptBaseline, [System.Text.UTF8Encoding]::new($false))

	$reorderedBindingEvidence = $bindingEvidenceBaselineJson | ConvertFrom-Json
	$reorderedBindingEvidence.operations = @($reorderedBindingEvidence.operations[1], $reorderedBindingEvidence.operations[0]) + @($reorderedBindingEvidence.operations | Select-Object -Skip 2)
	Assert-OperationEvidenceRejected -Evidence $reorderedBindingEvidence -EvidencePath $bindingOperationEvidencePath -Scenario $validatedBindingScenario -RunRoot $bindingEvidenceRoot -ExpectedMessage 'is reordered' -FailureMessage 'Schema-v2 operation evidence accepted reordered records.'

	$reorderedTranscriptLines = [string[]]@($bindingTranscriptBaseline)
	$firstTranscriptLine = $reorderedTranscriptLines[0]
	$reorderedTranscriptLines[0] = $reorderedTranscriptLines[1]
	$reorderedTranscriptLines[1] = $firstTranscriptLine
	[System.IO.File]::WriteAllLines($bindingTranscriptPath, $reorderedTranscriptLines, [System.Text.UTF8Encoding]::new($false))
	$reorderedTranscriptEvidence = $bindingEvidenceBaselineJson | ConvertFrom-Json
	$reorderedTranscriptEvidence.transcriptEvidence.sha256 = Get-Sha256 -Path $bindingTranscriptPath
	Assert-OperationEvidenceRejected -Evidence $reorderedTranscriptEvidence -EvidencePath $bindingOperationEvidencePath -Scenario $validatedBindingScenario -RunRoot $bindingEvidenceRoot -ExpectedMessage 'monotonically ordered' -FailureMessage 'Schema-v2 validation accepted a reordered MCP transcript.'
	[System.IO.File]::WriteAllLines($bindingTranscriptPath, $bindingTranscriptBaseline, [System.Text.UTF8Encoding]::new($false))

	$sequenceTamperEvidence = $bindingEvidenceBaselineJson | ConvertFrom-Json
	$sequenceTamperDestination = @($sequenceTamperEvidence.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0]
	$sequenceTamperDestination.mcpSequence = [int](@($sequenceTamperEvidence.operations | Where-Object { [string]$_.id -ceq 'binding-source' })[0].mcpSequence)
	Assert-OperationEvidenceRejected -Evidence $sequenceTamperEvidence -EvidencePath $bindingOperationEvidencePath -Scenario $validatedBindingScenario -RunRoot $bindingEvidenceRoot -ExpectedMessage 'reordered MCP sequence' -FailureMessage 'Schema-v2 evidence accepted a destination sequence reordered before its source.'

	$provenanceTamperEvidence = $bindingEvidenceBaselineJson | ConvertFrom-Json
	$provenanceTamperBinding = @($provenanceTamperEvidence.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].argumentBindings[0]
	$provenanceTamperBinding.selectedValueSha256 = ('0' * 64)
	Assert-OperationEvidenceRejected -Evidence $provenanceTamperEvidence -EvidencePath $bindingOperationEvidencePath -Scenario $validatedBindingScenario -RunRoot $bindingEvidenceRoot -ExpectedMessage 'binding provenance' -FailureMessage 'Schema-v2 evidence accepted a tampered selected-value provenance hash.'

	$sourceHashTamperEvidence = $bindingEvidenceBaselineJson | ConvertFrom-Json
	$sourceHashTamperBinding = @($sourceHashTamperEvidence.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].argumentBindings[0]
	$sourceHashTamperBinding.sourceEnvelopeSha256 = ('f' * 64)
	Assert-OperationEvidenceRejected -Evidence $sourceHashTamperEvidence -EvidencePath $bindingOperationEvidencePath -Scenario $validatedBindingScenario -RunRoot $bindingEvidenceRoot -ExpectedMessage 'binding provenance' -FailureMessage 'Schema-v2 evidence accepted a tampered source-envelope provenance hash.'

	# The aggregate summary is not the only provenance authority: mutate the
	# destination's own artifact and update its outer file hash to prove the
	# validator reconstructs and checks the embedded binding records as well.
	$bindingDestinationArtifactPath = Join-Path $bindingEvidenceRoot ([string]$validatedBindingDestination.evidencePath)
	$bindingDestinationArtifactBaseline = Get-Content -Raw -LiteralPath $bindingDestinationArtifactPath
	$artifactProvenanceTamper = $bindingDestinationArtifactBaseline | ConvertFrom-Json
	$artifactProvenanceTamper.argumentBindings[0].selectedValueSha256 = ('e' * 64)
	[System.IO.File]::WriteAllText($bindingDestinationArtifactPath, ($artifactProvenanceTamper | ConvertTo-Json -Depth 80), [System.Text.UTF8Encoding]::new($false))
	$artifactProvenanceTamperEvidence = $bindingEvidenceBaselineJson | ConvertFrom-Json
	$artifactProvenanceTamperRecord = @($artifactProvenanceTamperEvidence.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0]
	$artifactProvenanceTamperRecord.evidence.sha256 = Get-Sha256 -Path $bindingDestinationArtifactPath
	Assert-OperationEvidenceRejected -Evidence $artifactProvenanceTamperEvidence -EvidencePath $bindingOperationEvidencePath -Scenario $validatedBindingScenario -RunRoot $bindingEvidenceRoot -ExpectedMessage 'schema-v2 argument binding provenance' -FailureMessage 'Schema-v2 validation trusted a tampered per-operation binding provenance record.'
	[System.IO.File]::WriteAllText($bindingDestinationArtifactPath, $bindingDestinationArtifactBaseline, [System.Text.UTF8Encoding]::new($false))

	$resolvedArgumentTamperEvidence = $bindingEvidenceBaselineJson | ConvertFrom-Json
	$resolvedArgumentTamperDestination = @($resolvedArgumentTamperEvidence.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0]
	$resolvedArgumentTamperDestination.resolvedArguments.target.identity = '11111111111111118111111111111111'
	Assert-OperationEvidenceRejected -Evidence $resolvedArgumentTamperEvidence -EvidencePath $bindingOperationEvidencePath -Scenario $validatedBindingScenario -RunRoot $bindingEvidenceRoot -ExpectedMessage 'exact resolved arguments' -FailureMessage 'Schema-v2 evidence accepted tampered resolved arguments.'
	$resolvedHashTamperEvidence = $bindingEvidenceBaselineJson | ConvertFrom-Json
	@($resolvedHashTamperEvidence.operations | Where-Object { [string]$_.id -ceq 'binding-destination' })[0].resolvedArgumentsSha256 = ('a' * 64)
	Assert-OperationEvidenceRejected -Evidence $resolvedHashTamperEvidence -EvidencePath $bindingOperationEvidencePath -Scenario $validatedBindingScenario -RunRoot $bindingEvidenceRoot -ExpectedMessage 'exact resolved arguments' -FailureMessage 'Schema-v2 evidence accepted a tampered resolved-argument hash.'

	$transcriptArgumentTamperLines = [System.Collections.Generic.List[string]]::new()
	foreach ($transcriptLine in $bindingTranscriptBaseline) {
		$transcriptEntry = $transcriptLine | ConvertFrom-Json
		if ([string]$transcriptEntry.request.params.name -ceq 'fixture.destination') {
			$transcriptEntry.request.params.arguments.target.label = 'tampered-transcript-value'
		}
		$transcriptArgumentTamperLines.Add(($transcriptEntry | ConvertTo-Json -Depth 80 -Compress))
	}
	[System.IO.File]::WriteAllLines($bindingTranscriptPath, [string[]]@($transcriptArgumentTamperLines), [System.Text.UTF8Encoding]::new($false))
	$transcriptArgumentTamperEvidence = $bindingEvidenceBaselineJson | ConvertFrom-Json
	$transcriptArgumentTamperEvidence.transcriptEvidence.sha256 = Get-Sha256 -Path $bindingTranscriptPath
	Assert-OperationEvidenceRejected -Evidence $transcriptArgumentTamperEvidence -EvidencePath $bindingOperationEvidencePath -Scenario $validatedBindingScenario -RunRoot $bindingEvidenceRoot -ExpectedMessage 'does not preserve scenario item' -FailureMessage 'Schema-v2 evidence accepted tampered transcript request arguments.'
	[System.IO.File]::WriteAllLines($bindingTranscriptPath, $bindingTranscriptBaseline, [System.Text.UTF8Encoding]::new($false))

	$sourceEnvelopeTamperLines = [System.Collections.Generic.List[string]]::new()
	foreach ($transcriptLine in $bindingTranscriptBaseline) {
		$transcriptEntry = $transcriptLine | ConvertFrom-Json
		if ([string]$transcriptEntry.request.params.name -ceq 'fixture.source') {
			$tamperedSourceEnvelope = [string]$transcriptEntry.response.result.content[0].text | ConvertFrom-Json
			$tamperedSourceEnvelope.data.uuidValue = '11111111111111118111111111111111'
			$transcriptEntry.response.result.content[0].text = $tamperedSourceEnvelope | ConvertTo-Json -Depth 40 -Compress
		}
		$sourceEnvelopeTamperLines.Add(($transcriptEntry | ConvertTo-Json -Depth 80 -Compress))
	}
	[System.IO.File]::WriteAllLines($bindingTranscriptPath, [string[]]@($sourceEnvelopeTamperLines), [System.Text.UTF8Encoding]::new($false))
	$sourceEnvelopeTamperEvidence = $bindingEvidenceBaselineJson | ConvertFrom-Json
	$sourceEnvelopeTamperEvidence.transcriptEvidence.sha256 = Get-Sha256 -Path $bindingTranscriptPath
	Assert-OperationEvidenceRejected -Evidence $sourceEnvelopeTamperEvidence -EvidencePath $bindingOperationEvidencePath -Scenario $validatedBindingScenario -RunRoot $bindingEvidenceRoot -ExpectedMessage 'response-envelope digest does not match' -FailureMessage 'Schema-v2 evidence accepted a tampered source transcript envelope.'
	[System.IO.File]::WriteAllLines($bindingTranscriptPath, $bindingTranscriptBaseline, [System.Text.UTF8Encoding]::new($false))
	[System.IO.File]::WriteAllText($bindingOperationEvidencePath, $bindingEvidenceBaselineJson, [System.Text.UTF8Encoding]::new($false))

	# High-impact operations do not complete at their initial tools/call. Preserve
	# the exact waiting request, pending resource, every recent-resource poll, and
	# the retained dispatch result so validation can reject denial, tampering, or
	# reuse of an approval from another operation.
	$approvalEvidenceRoot = Join-Path $temporaryRoot 'approval-evidence'
	$approvalEditorRoot = Join-Path $approvalEvidenceRoot 'editor'
	$approvalPromptRoot = Join-Path $approvalEditorRoot 'approvals'
	$approvalOperationRoot = Join-Path $approvalEditorRoot 'operations'
	$approvalProjectRoot = Join-Path $temporaryRoot 'approval-project'
	[System.IO.Directory]::CreateDirectory($approvalPromptRoot) | Out-Null
	[System.IO.Directory]::CreateDirectory($approvalOperationRoot) | Out-Null
	[System.IO.Directory]::CreateDirectory((Join-Path $approvalProjectRoot 'Assets/Captured')) | Out-Null
	$approvalOutputPath = [System.IO.Path]::GetFullPath((Join-Path $approvalProjectRoot 'Assets/Captured/SelfTestRecording.teanim'))
	$approvalDefinition = @($validatedRecordingScenario.operations | Where-Object { [string]$_.id -ceq 'recording-stop' })[0] |
		ConvertTo-Json -Depth 50 | ConvertFrom-Json
	$approvalDeclarations = @($validatedPairedRecordingScenario.playModeRecordingOutputs | ForEach-Object {
		$_ | ConvertTo-Json -Depth 20 | ConvertFrom-Json
	})
	$approvalScenario = [pscustomobject][ordered]@{
		phase = 'self-test'
		operations = @($approvalDefinition)
		captures = @()
		playModeRecordingOutputs = @($approvalDeclarations)
	}
	$approvalArguments = Get-ScenarioInvocationArguments -Definition $approvalDefinition -Kind 'operation'
	$approvalArgumentsSha256 = Get-CanonicalJsonSha256 -Value $approvalArguments
	$approvalId = 'approval-701'
	$approvalRequestId = 'self-test-approval-request'
	$approvalImpactFingerprint = '1a2b3c4d'
	$approvalArgumentsFingerprint = '5e6f7788'
	$approvalCapability = 'animation.recording.stop'
	$approvalProjectGeneration = 11
	# Scene generations are native UInt64 identities. Use a value above
	# Int64.MaxValue so the approval chain exercises Windows PowerShell's exact
	# Decimal representation instead of only the smaller Int64 path.
	$approvalSceneGeneration = [decimal]::Parse('16126822243653173321', [Globalization.CultureInfo]::InvariantCulture)
	$approvalWorldGeneration = 13
	$approvalInitialSequence = 10
	$approvalPendingSequence = 11
	$approvalCompletionSequence = 12
	$approvalInitialEnvelope = [ordered]@{
		ok = $false
		requestId = $approvalRequestId
		status = 'waiting_approval'
		message = 'Manual approval is required.'
		data = [ordered]@{
			approvalId = $approvalId
			capability = $approvalCapability
			impactFingerprint = $approvalImpactFingerprint
			projectGeneration = $approvalProjectGeneration
			sceneGeneration = $approvalSceneGeneration
			worldGeneration = $approvalWorldGeneration
		}
		error = $null
	}
	$approvalDispatchEnvelope = [ordered]@{
		ok = $true
		status = 'completed'
		data = [ordered]@{ destinationPath = $approvalOutputPath }
	}
	$approvalPendingEntry = [ordered]@{
		approvalId = $approvalId
		requestId = $approvalRequestId
		capability = $approvalCapability
		summary = 'Stop recording and publish the captured animation clip.'
		state = 'pending'
	}
	$approvalRecentEntry = [ordered]@{
		approvalId = $approvalId
		requestId = $approvalRequestId
		capability = $approvalCapability
		summary = 'Stop recording and publish the captured animation clip.'
		state = 'approved'
		approved = $true
		dispatchCompleted = $true
		dispatchResult = $approvalDispatchEnvelope
		argumentsFingerprint = $approvalArgumentsFingerprint
		impactFingerprint = $approvalImpactFingerprint
		projectGeneration = $approvalProjectGeneration
		sceneGeneration = $approvalSceneGeneration
		worldGeneration = $approvalWorldGeneration
	}
	$approvalPendingDocument = [ordered]@{ ok = $true; approvals = @($approvalPendingEntry) }
	$approvalRecentDocument = [ordered]@{ ok = $true; approvals = @($approvalRecentEntry) }
	$approvalPendingText = $approvalPendingDocument | ConvertTo-Json -Depth 80 -Compress
	$approvalRecentText = $approvalRecentDocument | ConvertTo-Json -Depth 80 -Compress
	$approvalInitialEnvelopeSha256 = Get-StringSha256 -Value ($approvalInitialEnvelope | ConvertTo-Json -Depth 80 -Compress)
	$approvalDispatchEnvelopeSha256 = Get-StringSha256 -Value ($approvalDispatchEnvelope | ConvertTo-Json -Depth 80 -Compress)
	$approvalPendingDocumentSha256 = Get-StringSha256 -Value $approvalPendingText
	$approvalRecentDocumentSha256 = Get-StringSha256 -Value $approvalRecentText
	$approvalPendingEntrySha256 = Get-CanonicalJsonSha256 -Value $approvalPendingEntry
	$approvalCompletionEntrySha256 = Get-CanonicalJsonSha256 -Value $approvalRecentEntry
	$approvalPromptPath = Join-Path $approvalPromptRoot 'recording-stop.json'
	$approvalPromptDocument = [ordered]@{
		schemaVersion = 1
		phase = 'self-test'
		operationId = 'recording-stop'
		policy = 'high_impact'
		capability = $approvalCapability
		approvalId = $approvalId
		requestId = $approvalRequestId
		argumentsSha256 = $approvalArgumentsSha256
		impactFingerprint = $approvalImpactFingerprint
		projectGeneration = $approvalProjectGeneration
		sceneGeneration = $approvalSceneGeneration
		worldGeneration = $approvalWorldGeneration
		status = 'approved'
		initial = [ordered]@{
			mcpSequence = $approvalInitialSequence
			envelopeSha256 = $approvalInitialEnvelopeSha256
			envelope = $approvalInitialEnvelope
		}
		pending = [ordered]@{
			mcpSequence = $approvalPendingSequence
			resourceUri = 'terraengine://approvals/pending'
			documentSha256 = $approvalPendingDocumentSha256
			entrySha256 = $approvalPendingEntrySha256
			entry = $approvalPendingEntry
		}
		recentPolls = @([ordered]@{
			mcpSequence = $approvalCompletionSequence
			resourceUri = 'terraengine://approvals/recent'
			documentSha256 = $approvalRecentDocumentSha256
			entrySha256 = $approvalCompletionEntrySha256
			state = 'approved'
			approved = $true
			dispatchCompleted = $true
		})
		completion = [ordered]@{
			mcpSequence = $approvalCompletionSequence
			entrySha256 = $approvalCompletionEntrySha256
			state = 'approved'
			approved = $true
			dispatchCompleted = $true
			dispatchResultSha256 = $approvalDispatchEnvelopeSha256
			dispatchResult = $approvalDispatchEnvelope
		}
	}
	[System.IO.File]::WriteAllText($approvalPromptPath, ($approvalPromptDocument | ConvertTo-Json -Depth 80), [System.Text.UTF8Encoding]::new($false))
	$approvalSummary = [ordered]@{
		policy = 'high_impact'
		capability = $approvalCapability
		approvalId = $approvalId
		requestId = $approvalRequestId
		argumentsSha256 = $approvalArgumentsSha256
		argumentsFingerprint = $approvalArgumentsFingerprint
		impactFingerprint = $approvalImpactFingerprint
		projectGeneration = $approvalProjectGeneration
		sceneGeneration = $approvalSceneGeneration
		worldGeneration = $approvalWorldGeneration
		initialMcpSequence = $approvalInitialSequence
		initialEnvelopeSha256 = $approvalInitialEnvelopeSha256
		pendingMcpSequence = $approvalPendingSequence
		pendingDocumentSha256 = $approvalPendingDocumentSha256
		pendingEntrySha256 = $approvalPendingEntrySha256
		completionMcpSequence = $approvalCompletionSequence
		completionEntrySha256 = $approvalCompletionEntrySha256
		dispatchResultSha256 = $approvalDispatchEnvelopeSha256
		evidence = [ordered]@{
			path = 'editor/approvals/recording-stop.json'
			sha256 = Get-Sha256 -Path $approvalPromptPath
		}
	}
	$approvalTranscriptEntries = @(
		[ordered]@{
			sequence = 5
			timestamp = [DateTimeOffset]::UtcNow.AddMilliseconds(-5).ToString('o')
			request = [ordered]@{ jsonrpc = '2.0'; id = 5; method = 'tools/call'; params = [ordered]@{ name = 'play.enter'; arguments = [ordered]@{} } }
			response = [ordered]@{ jsonrpc = '2.0'; id = 5; result = [ordered]@{ content = @([ordered]@{ type = 'text'; text = (([ordered]@{ ok = $true; data = [ordered]@{ currentState = 'playing' } }) | ConvertTo-Json -Depth 80 -Compress) }) } }
		},
		[ordered]@{
			sequence = $approvalInitialSequence
			timestamp = [DateTimeOffset]::UtcNow.AddMilliseconds(-4).ToString('o')
			request = [ordered]@{ jsonrpc = '2.0'; id = $approvalInitialSequence; method = 'tools/call'; params = [ordered]@{ name = $approvalCapability; arguments = $approvalArguments } }
			response = [ordered]@{ jsonrpc = '2.0'; id = $approvalInitialSequence; result = [ordered]@{ content = @([ordered]@{ type = 'text'; text = ($approvalInitialEnvelope | ConvertTo-Json -Depth 80 -Compress) }) } }
		},
		[ordered]@{
			sequence = $approvalPendingSequence
			timestamp = [DateTimeOffset]::UtcNow.AddMilliseconds(-3).ToString('o')
			request = [ordered]@{ jsonrpc = '2.0'; id = $approvalPendingSequence; method = 'resources/read'; params = [ordered]@{ uri = 'terraengine://approvals/pending' } }
			response = [ordered]@{ jsonrpc = '2.0'; id = $approvalPendingSequence; result = [ordered]@{ contents = @([ordered]@{ uri = 'terraengine://approvals/pending'; mimeType = 'application/json'; text = $approvalPendingText }) } }
		},
		[ordered]@{
			sequence = $approvalCompletionSequence
			timestamp = [DateTimeOffset]::UtcNow.AddMilliseconds(-2).ToString('o')
			request = [ordered]@{ jsonrpc = '2.0'; id = $approvalCompletionSequence; method = 'resources/read'; params = [ordered]@{ uri = 'terraengine://approvals/recent' } }
			response = [ordered]@{ jsonrpc = '2.0'; id = $approvalCompletionSequence; result = [ordered]@{ contents = @([ordered]@{ uri = 'terraengine://approvals/recent'; mimeType = 'application/json'; text = $approvalRecentText }) } }
		},
		[ordered]@{
			sequence = 15
			timestamp = [DateTimeOffset]::UtcNow.AddMilliseconds(-1).ToString('o')
			request = [ordered]@{ jsonrpc = '2.0'; id = 15; method = 'tools/call'; params = [ordered]@{ name = 'play.exit'; arguments = [ordered]@{} } }
			response = [ordered]@{ jsonrpc = '2.0'; id = 15; result = [ordered]@{ content = @([ordered]@{ type = 'text'; text = (([ordered]@{ ok = $true; data = [ordered]@{ currentState = 'stopped'; restoration = [ordered]@{ completed = $true; partialFailures = @() } } }) | ConvertTo-Json -Depth 80 -Compress) }) } }
		}
	)
	$approvalTranscriptPath = Join-Path $approvalEditorRoot 'mcp.jsonl'
	$approvalTranscriptLines = [string[]]@($approvalTranscriptEntries | ForEach-Object { $_ | ConvertTo-Json -Depth 80 -Compress })
	[System.IO.File]::WriteAllLines($approvalTranscriptPath, $approvalTranscriptLines, [System.Text.UTF8Encoding]::new($false))
	$approvalReconnectDocument = [ordered]@{
		schemaVersion = 1
		passed = $true
		connectionBoundary = [ordered]@{ sequence = 20; serverName = 'TerraEngine Editor'; disableKeepAlive = $true }
		freshInitialize = [ordered]@{ sequence = 21; serverName = 'TerraEngine Editor' }
		postReconnect = [ordered]@{
			toolListSequence = 22
			playStateSequence = 23
			requiredToolsPresent = $true
			operationPassed = $true
			playState = 'stopped'
		}
	}
	$approvalReconnectPath = Join-Path $approvalEditorRoot 'mcp-reconnect.json'
	[System.IO.File]::WriteAllText($approvalReconnectPath, ($approvalReconnectDocument | ConvertTo-Json -Depth 30), [System.Text.UTF8Encoding]::new($false))
	$approvalFinalEnvelopeSha256 = Get-StringSha256 -Value ($approvalDispatchEnvelope | ConvertTo-Json -Depth 80 -Compress)
	$approvalOperationArtifact = [ordered]@{
		schemaVersion = 2
		phase = 'self-test'
		id = 'recording-stop'
		stage = 'play'
		order = 5
		tool = $approvalCapability
		mcpSequence = $approvalInitialSequence
		arguments = $approvalArguments
		resolvedArguments = $approvalArguments
		resolvedArgumentsSha256 = $approvalArgumentsSha256
		argumentBindings = @()
		expected = New-ScenarioExpectationRecord -Expectation $approvalDefinition.expect
		expectationPassed = $true
		envelopeSha256 = $approvalFinalEnvelopeSha256
		envelope = $approvalDispatchEnvelope
		approval = $approvalSummary
		fileTransition = $null
	}
	$approvalOperationArtifactPath = Join-Path $approvalOperationRoot 'recording-stop.json'
	[System.IO.File]::WriteAllText($approvalOperationArtifactPath, ($approvalOperationArtifact | ConvertTo-Json -Depth 80), [System.Text.UTF8Encoding]::new($false))
	$approvalOperationRecord = [ordered]@{
		id = 'recording-stop'
		kind = 'operation'
		stage = 'play'
		order = 5
		tool = $approvalCapability
		mcpSequence = $approvalInitialSequence
		resolvedArguments = $approvalArguments
		resolvedArgumentsSha256 = $approvalArgumentsSha256
		argumentBindings = @()
		expectationPassed = $true
		expected = New-ScenarioExpectationRecord -Expectation $approvalDefinition.expect
		observed = [ordered]@{
			ok = $true
			status = 'completed'
			errorCategory = $null
			envelopeSha256 = $approvalFinalEnvelopeSha256
		}
		evidence = [ordered]@{
			path = 'editor/operations/recording-stop.json'
			sha256 = Get-Sha256 -Path $approvalOperationArtifactPath
		}
		approval = $approvalSummary
		fileTransition = $null
	}
	$approvalOperationEvidenceDocument = [ordered]@{
		schemaVersion = 2
		phase = 'self-test'
		declaredItemCount = 1
		operations = @($approvalOperationRecord)
		transcriptEvidence = [ordered]@{ path = 'editor/mcp.jsonl'; sha256 = Get-Sha256 -Path $approvalTranscriptPath }
		reconnectPassed = $true
		reconnectEvidence = [ordered]@{ path = 'editor/mcp-reconnect.json'; sha256 = Get-Sha256 -Path $approvalReconnectPath }
	}
	$approvalOperationEvidencePath = Join-Path $approvalEditorRoot 'mcp-operations.json'
	[System.IO.File]::WriteAllText($approvalOperationEvidencePath, ($approvalOperationEvidenceDocument | ConvertTo-Json -Depth 80), [System.Text.UTF8Encoding]::new($false))
	$validatedApprovalEvidence = Read-AndValidateOperationEvidence -Path $approvalOperationEvidencePath -Scenario $approvalScenario -RunRoot $approvalEvidenceRoot -MinimumSchemaVersion 2
	Assert-TestCondition -Condition ([int]$validatedApprovalEvidence.approvalCount -eq 1) -Message 'A valid schema-v2 high-impact approval chain did not pass.'
	Assert-TestCondition -Condition ($approvalSceneGeneration -gt [decimal][long]::MaxValue) -Message 'The approval self-test did not exercise the unsigned 64-bit generation range.'

	$approvalAggregateBaseline = Get-Content -Raw -LiteralPath $approvalOperationEvidencePath
	$approvalArtifactBaseline = Get-Content -Raw -LiteralPath $approvalOperationArtifactPath
	$approvalPromptBaseline = Get-Content -Raw -LiteralPath $approvalPromptPath
	$approvalTranscriptBaseline = [string[]]@(Get-Content -LiteralPath $approvalTranscriptPath)
	$missingApprovalEvidence = $approvalAggregateBaseline | ConvertFrom-Json
	$missingApprovalEvidence.operations[0].approval = $null
	Assert-OperationEvidenceRejected -Evidence $missingApprovalEvidence -EvidencePath $approvalOperationEvidencePath -Scenario $approvalScenario -RunRoot $approvalEvidenceRoot -ExpectedMessage 'omits its required high-impact approval provenance' -FailureMessage 'A high-impact operation without approval evidence was accepted.'
	[System.IO.File]::WriteAllText($approvalOperationEvidencePath, $approvalAggregateBaseline, [System.Text.UTF8Encoding]::new($false))

	$wrongInitialLines = [System.Collections.Generic.List[string]]::new()
	foreach ($approvalTranscriptLine in $approvalTranscriptBaseline) {
		$approvalTranscriptEntry = $approvalTranscriptLine | ConvertFrom-Json
		if ([int]$approvalTranscriptEntry.sequence -eq $approvalInitialSequence) {
			$wrongInitialEnvelope = [string]$approvalTranscriptEntry.response.result.content[0].text | ConvertFrom-Json
			$wrongInitialEnvelope.status = 'completed'
			$approvalTranscriptEntry.response.result.content[0].text = $wrongInitialEnvelope | ConvertTo-Json -Depth 80 -Compress
		}
		$wrongInitialLines.Add(($approvalTranscriptEntry | ConvertTo-Json -Depth 80 -Compress))
	}
	[System.IO.File]::WriteAllLines($approvalTranscriptPath, [string[]]@($wrongInitialLines), [System.Text.UTF8Encoding]::new($false))
	$wrongInitialEvidence = $approvalAggregateBaseline | ConvertFrom-Json
	$wrongInitialEvidence.transcriptEvidence.sha256 = Get-Sha256 -Path $approvalTranscriptPath
	Assert-OperationEvidenceRejected -Evidence $wrongInitialEvidence -EvidencePath $approvalOperationEvidencePath -Scenario $approvalScenario -RunRoot $approvalEvidenceRoot -ExpectedMessage 'initial waiting_approval proof|initial envelope is not its exact pending request' -FailureMessage 'A high-impact operation with a non-pending initial result was accepted.'
	[System.IO.File]::WriteAllLines($approvalTranscriptPath, $approvalTranscriptBaseline, [System.Text.UTF8Encoding]::new($false))

	$tamperedPendingLines = [System.Collections.Generic.List[string]]::new()
	foreach ($approvalTranscriptLine in $approvalTranscriptBaseline) {
		$approvalTranscriptEntry = $approvalTranscriptLine | ConvertFrom-Json
		if ([int]$approvalTranscriptEntry.sequence -eq $approvalPendingSequence) {
			$tamperedPendingDocument = [string]$approvalTranscriptEntry.response.result.contents[0].text | ConvertFrom-Json
			$tamperedPendingDocument.approvals[0].summary = 'Tampered pending prompt.'
			$approvalTranscriptEntry.response.result.contents[0].text = $tamperedPendingDocument | ConvertTo-Json -Depth 80 -Compress
		}
		$tamperedPendingLines.Add(($approvalTranscriptEntry | ConvertTo-Json -Depth 80 -Compress))
	}
	[System.IO.File]::WriteAllLines($approvalTranscriptPath, [string[]]@($tamperedPendingLines), [System.Text.UTF8Encoding]::new($false))
	$tamperedPendingEvidence = $approvalAggregateBaseline | ConvertFrom-Json
	$tamperedPendingEvidence.transcriptEvidence.sha256 = Get-Sha256 -Path $approvalTranscriptPath
	Assert-OperationEvidenceRejected -Evidence $tamperedPendingEvidence -EvidencePath $approvalOperationEvidencePath -Scenario $approvalScenario -RunRoot $approvalEvidenceRoot -ExpectedMessage 'pending resource proof was tampered' -FailureMessage 'A tampered pending-approval resource was accepted.'
	[System.IO.File]::WriteAllLines($approvalTranscriptPath, $approvalTranscriptBaseline, [System.Text.UTF8Encoding]::new($false))

	$tamperedDispatchPrompt = $approvalPromptBaseline | ConvertFrom-Json
	$tamperedDispatchPrompt.completion.dispatchResult.data.destinationPath = [System.IO.Path]::GetFullPath((Join-Path $approvalProjectRoot 'Assets/Captured/Tampered.teanim'))
	$tamperedDispatchPrompt.completion.dispatchResultSha256 = Get-StringSha256 -Value ($tamperedDispatchPrompt.completion.dispatchResult | ConvertTo-Json -Depth 80 -Compress)
	[System.IO.File]::WriteAllText($approvalPromptPath, ($tamperedDispatchPrompt | ConvertTo-Json -Depth 80), [System.Text.UTF8Encoding]::new($false))
	$tamperedDispatchEvidence = $approvalAggregateBaseline | ConvertFrom-Json
	$tamperedDispatchEvidence.operations[0].approval.evidence.sha256 = Get-Sha256 -Path $approvalPromptPath
	Assert-OperationEvidenceRejected -Evidence $tamperedDispatchEvidence -EvidencePath $approvalOperationEvidencePath -Scenario $approvalScenario -RunRoot $approvalEvidenceRoot -ExpectedMessage 'completion result is denied, incomplete, replayed, or tampered' -FailureMessage 'A prompt artifact with a tampered retained dispatch result was accepted.'
	[System.IO.File]::WriteAllText($approvalPromptPath, $approvalPromptBaseline, [System.Text.UTF8Encoding]::new($false))

	$deniedRecentEntry = $approvalRecentEntry | ConvertTo-Json -Depth 80 | ConvertFrom-Json
	$deniedRecentEntry.state = 'denied'
	$deniedRecentEntry.approved = $false
	$deniedRecentEntry.dispatchResult = $null
	$deniedRecentDocument = [ordered]@{ ok = $true; approvals = @($deniedRecentEntry) }
	$deniedRecentText = $deniedRecentDocument | ConvertTo-Json -Depth 80 -Compress
	$deniedRecentLines = [System.Collections.Generic.List[string]]::new()
	foreach ($approvalTranscriptLine in $approvalTranscriptBaseline) {
		$approvalTranscriptEntry = $approvalTranscriptLine | ConvertFrom-Json
		if ([int]$approvalTranscriptEntry.sequence -eq $approvalCompletionSequence) {
			$approvalTranscriptEntry.response.result.contents[0].text = $deniedRecentText
		}
		$deniedRecentLines.Add(($approvalTranscriptEntry | ConvertTo-Json -Depth 80 -Compress))
	}
	[System.IO.File]::WriteAllLines($approvalTranscriptPath, [string[]]@($deniedRecentLines), [System.Text.UTF8Encoding]::new($false))
	$deniedEntrySha256 = Get-CanonicalJsonSha256 -Value $deniedRecentEntry
	$deniedPrompt = $approvalPromptBaseline | ConvertFrom-Json
	$deniedPrompt.status = 'denied'
	$deniedPrompt.recentPolls[0].documentSha256 = Get-StringSha256 -Value $deniedRecentText
	$deniedPrompt.recentPolls[0].entrySha256 = $deniedEntrySha256
	$deniedPrompt.recentPolls[0].state = 'denied'
	$deniedPrompt.recentPolls[0].approved = $false
	$deniedPrompt.completion.entrySha256 = $deniedEntrySha256
	$deniedPrompt.completion.state = 'denied'
	$deniedPrompt.completion.approved = $false
	$deniedPrompt.completion.dispatchResult = $null
	$deniedPrompt.completion.dispatchResultSha256 = ('0' * 64)
	[System.IO.File]::WriteAllText($approvalPromptPath, ($deniedPrompt | ConvertTo-Json -Depth 80), [System.Text.UTF8Encoding]::new($false))
	$deniedEvidence = $approvalAggregateBaseline | ConvertFrom-Json
	$deniedEvidence.transcriptEvidence.sha256 = Get-Sha256 -Path $approvalTranscriptPath
	$deniedEvidence.operations[0].approval.completionEntrySha256 = $deniedEntrySha256
	$deniedEvidence.operations[0].approval.dispatchResultSha256 = ('0' * 64)
	$deniedEvidence.operations[0].approval.evidence.sha256 = Get-Sha256 -Path $approvalPromptPath
	Assert-OperationEvidenceRejected -Evidence $deniedEvidence -EvidencePath $approvalOperationEvidencePath -Scenario $approvalScenario -RunRoot $approvalEvidenceRoot -ExpectedMessage 'artifact identity or final state was tampered|completion result is denied' -FailureMessage 'A denied high-impact approval was accepted as a successful operation.'
	[System.IO.File]::WriteAllLines($approvalTranscriptPath, $approvalTranscriptBaseline, [System.Text.UTF8Encoding]::new($false))
	[System.IO.File]::WriteAllText($approvalPromptPath, $approvalPromptBaseline, [System.Text.UTF8Encoding]::new($false))
	[System.IO.File]::WriteAllText($approvalOperationArtifactPath, $approvalArtifactBaseline, [System.Text.UTF8Encoding]::new($false))
	[System.IO.File]::WriteAllText($approvalOperationEvidencePath, $approvalAggregateBaseline, [System.Text.UTF8Encoding]::new($false))

	$approvalReplayTranscript = Read-McpTranscriptForOperationEvidence `
		-Reference ([pscustomobject][ordered]@{ path = 'editor/mcp.jsonl'; sha256 = Get-Sha256 -Path $approvalTranscriptPath }) `
		-RunRoot $approvalEvidenceRoot
	$approvalReplaySummaryText = $approvalSummary | ConvertTo-Json -Depth 80
	$approvalReplaySummary = $approvalReplaySummaryText | ConvertFrom-Json
	$approvalReplaySummaryStrict = ConvertFrom-JsonPreservingArrays -Json $approvalReplaySummaryText -Description 'Approval replay self-test summary'
	$approvalReplayInitialEntry = @($approvalReplayTranscript.entries | Where-Object { [int]$_.sequence -eq $approvalInitialSequence })[0]
	$approvalReplayInitialEnvelope = Get-ToolEnvelope -Response $approvalReplayInitialEntry.response -Tool $approvalCapability
	$approvalReplayIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
	[void](Read-AndValidateScenarioApprovalEvidence -Summary $approvalReplaySummary -SummaryStrict $approvalReplaySummaryStrict -Definition $approvalDefinition -ExpectedArguments $approvalArguments -InitialEnvelope $approvalReplayInitialEnvelope -InitialMcpSequence $approvalInitialSequence -TranscriptState $approvalReplayTranscript -RunRoot $approvalEvidenceRoot -ScenarioPhase 'self-test' -ApprovalIds $approvalReplayIds)
	$approvalReplayRejected = $false
	try {
		[void](Read-AndValidateScenarioApprovalEvidence -Summary $approvalReplaySummary -SummaryStrict $approvalReplaySummaryStrict -Definition $approvalDefinition -ExpectedArguments $approvalArguments -InitialEnvelope $approvalReplayInitialEnvelope -InitialMcpSequence $approvalInitialSequence -TranscriptState $approvalReplayTranscript -RunRoot $approvalEvidenceRoot -ScenarioPhase 'self-test' -ApprovalIds $approvalReplayIds)
	}
	catch {
		$approvalReplayRejected = $_.Exception.Message -match 'replays approval ID'
	}
	Assert-TestCondition -Condition $approvalReplayRejected -Message 'The same approval ID was accepted twice by one validation registry.'

	# Build the complete before/after/summary chain around the approved recording
	# operation. This exercises the same schema-v2 validation used by the live
	# phase gate, including the explicit .teanim metadata sidecar.
	$approvalBeforeSnapshot = Get-AuthoredProjectState -ProjectRoot $approvalProjectRoot
	$approvalCreationOnlyPreflight = @(Start-PlayModeRecordingOutputContract -Scenario $approvalScenario -ProjectRoot $approvalProjectRoot -BeforeSnapshot $approvalBeforeSnapshot)
	$approvalBeforeEvidencePath = Join-Path $approvalEditorRoot 'play-mode-before.json'
	$approvalBeforeEvidenceDocument = [ordered]@{
		schemaVersion = 1
		phase = 'self-test'
		kind = 'before-play'
		capturedAtUtc = [DateTimeOffset]::UtcNow.AddSeconds(-1).ToString('o')
		playModeRecordingOutputs = @($approvalScenario.playModeRecordingOutputs)
		creationOnlyPreflight = @($approvalCreationOnlyPreflight)
		snapshot = $approvalBeforeSnapshot
	}
	[System.IO.File]::WriteAllText($approvalBeforeEvidencePath, ($approvalBeforeEvidenceDocument | ConvertTo-Json -Depth 100), [System.Text.UTF8Encoding]::new($false))
	$approvalBeforeReference = [ordered]@{ path = 'editor/play-mode-before.json'; sha256 = Get-Sha256 -Path $approvalBeforeEvidencePath }
	$approvalOutputBytes = [System.Text.Encoding]::UTF8.GetBytes('self-test durable recording payload')
	$approvalMetadataBytes = [System.Text.Encoding]::UTF8.GetBytes('self-test durable recording metadata')
	[System.IO.File]::WriteAllBytes($approvalOutputPath, $approvalOutputBytes)
	[System.IO.File]::WriteAllBytes(($approvalOutputPath + '.meta'), $approvalMetadataBytes)
	$approvalAfterSnapshot = Get-AuthoredProjectState -ProjectRoot $approvalProjectRoot
	$approvalParsedOperationDocument = Get-Content -Raw -LiteralPath $approvalOperationEvidencePath | ConvertFrom-Json
	$approvalParsedOperationArtifact = Get-Content -Raw -LiteralPath $approvalOperationArtifactPath | ConvertFrom-Json
	$approvalBindingRegistry = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
	$approvalBindingRegistry['recording-stop'] = [pscustomobject][ordered]@{
		envelope = $approvalParsedOperationArtifact.envelope
		mcpSequence = [int]$approvalParsedOperationDocument.operations[0].mcpSequence
		envelopeSha256 = [string]$approvalParsedOperationDocument.operations[0].observed.envelopeSha256
		evidenceSha256 = [string]$approvalParsedOperationDocument.operations[0].evidence.sha256
		approval = $approvalParsedOperationDocument.operations[0].approval
	}
	Assert-TestCondition -Condition ([int]$approvalParsedOperationDocument.operations[0].mcpSequence -eq 10) -Message 'Approval Play Mode fixture lost its initial producer sequence.'
	Assert-TestCondition -Condition ([int]$approvalParsedOperationDocument.operations[0].approval.completionMcpSequence -eq 12) -Message 'Approval Play Mode fixture lost its completion sequence.'
	Assert-TestCondition -Condition (
		(Get-CanonicalJsonText -Value $approvalBindingRegistry['recording-stop'].approval) -ceq
		(Get-CanonicalJsonText -Value $approvalParsedOperationDocument.operations[0].approval)
	) -Message 'Approval Play Mode fixture registry changed producer approval provenance.'
	$approvalPublishedFixture = Get-OptionalProperty -Object $approvalBindingRegistry['recording-stop'] -Name 'approval'
	$approvalRecordedFixture = Get-OptionalProperty -Object $approvalParsedOperationDocument.operations[0] -Name 'approval'
	Assert-TestCondition -Condition (
		(Get-CanonicalJsonText -Value $approvalPublishedFixture) -ceq
		(Get-CanonicalJsonText -Value $approvalRecordedFixture)
	) -Message 'Approval Play Mode fixture helper lookup changed producer approval provenance.'
	$approvalRecordingState = Complete-PlayModeRecordingOutputContract `
		-Scenario $approvalScenario `
		-ProjectRoot $approvalProjectRoot `
		-BeforeSnapshot $approvalBeforeSnapshot `
		-AfterSnapshot $approvalAfterSnapshot `
		-BindingRegistry $approvalBindingRegistry `
		-OperationInventory @($approvalParsedOperationDocument.operations) `
		-PlayEnterSequence 5 `
		-PlayExitSequence 15
	$approvalTranscriptReference = [ordered]@{ path = 'editor/mcp.jsonl'; sha256 = Get-Sha256 -Path $approvalTranscriptPath }
	$approvalOperationReference = [ordered]@{ path = 'editor/mcp-operations.json'; sha256 = Get-Sha256 -Path $approvalOperationEvidencePath }
	$approvalEnterTranscriptEntry = @($approvalReplayTranscript.entries | Where-Object { [int]$_.sequence -eq 5 })[0]
	$approvalExitTranscriptEntry = @($approvalReplayTranscript.entries | Where-Object { [int]$_.sequence -eq 15 })[0]
	$approvalEnterEnvelope = Get-ToolEnvelope -Response $approvalEnterTranscriptEntry.response -Tool 'play.enter'
	$approvalExitEnvelope = Get-ToolEnvelope -Response $approvalExitTranscriptEntry.response -Tool 'play.exit'
	$approvalPlayBoundary = [ordered]@{
		enterMcpSequence = 5
		exitMcpSequence = 15
		enterEnvelopeSha256 = Get-StringSha256 -Value ($approvalEnterEnvelope | ConvertTo-Json -Depth 80 -Compress)
		exitEnvelopeSha256 = Get-StringSha256 -Value ($approvalExitEnvelope | ConvertTo-Json -Depth 80 -Compress)
	}
	$approvalRecordingProofs = [System.Collections.Generic.List[object]]::new()
	foreach ($approvalRecordingProof in @($approvalRecordingState.outputs)) {
		$approvalRecordingProofs.Add([ordered]@{
			path = [string]$approvalRecordingProof.path
			bytes = [UInt64]$approvalRecordingProof.bytes
			sha256 = [string]$approvalRecordingProof.sha256
			producerOperationId = [string]$approvalRecordingProof.producerOperationId
			producerResponsePath = [string]$approvalRecordingProof.producerResponsePath
			producerMcpSequence = [int]$approvalRecordingProof.producerMcpSequence
			producerCompletionMcpSequence = [int]$approvalRecordingProof.producerCompletionMcpSequence
			producerEnvelopeSha256 = [string]$approvalRecordingProof.producerEnvelopeSha256
			producerApproval = $approvalRecordingProof.producerApproval
			producerArtifact = $approvalRecordingProof.producerArtifact
			operationEvidenceSha256 = [string]$approvalOperationReference.sha256
			transcriptSha256 = [string]$approvalTranscriptReference.sha256
		})
	}
	$approvalAfterEvidencePath = Join-Path $approvalEditorRoot 'play-mode-after.json'
	$approvalAfterEvidenceDocument = [ordered]@{
		schemaVersion = 1
		phase = 'self-test'
		kind = 'after-play'
		capturedAtUtc = [DateTimeOffset]::UtcNow.ToString('o')
		beforeEvidence = $approvalBeforeReference
		operationEvidence = $approvalOperationReference
		transcriptEvidence = $approvalTranscriptReference
		playBoundary = $approvalPlayBoundary
		comparison = $approvalRecordingState.comparison
		recordingOutputs = @($approvalRecordingProofs)
		snapshot = $approvalAfterSnapshot
	}
	[System.IO.File]::WriteAllText($approvalAfterEvidencePath, ($approvalAfterEvidenceDocument | ConvertTo-Json -Depth 100), [System.Text.UTF8Encoding]::new($false))
	$approvalAfterReference = [ordered]@{ path = 'editor/play-mode-after.json'; sha256 = Get-Sha256 -Path $approvalAfterEvidencePath }
	$approvalPlayEvidenceDocument = [ordered]@{
		schemaVersion = 2
		phase = 'self-test'
		entered = $true
		exited = $true
		authoredStateRestored = $true
		creationOnly = $true
		beforeSha256 = [string]$approvalBeforeSnapshot.sha256
		afterSha256 = [string]$approvalAfterSnapshot.sha256
		excludedTransientState = $approvalBeforeSnapshot.excludedTransientState
		beforeEvidence = $approvalBeforeReference
		afterEvidence = $approvalAfterReference
		operationEvidence = $approvalOperationReference
		transcriptEvidence = $approvalTranscriptReference
		playBoundary = $approvalPlayBoundary
		recordingOutputs = @($approvalRecordingProofs)
		enter = $approvalEnterEnvelope.data
		exit = $approvalExitEnvelope.data
	}
	$approvalPlayEvidencePath = Join-Path $approvalEditorRoot 'play-mode.json'
	[System.IO.File]::WriteAllText($approvalPlayEvidencePath, ($approvalPlayEvidenceDocument | ConvertTo-Json -Depth 100), [System.Text.UTF8Encoding]::new($false))
	$validatedApprovalPlayEvidence = Read-AndValidatePlayModeEvidence -Path $approvalPlayEvidencePath -Scenario $approvalScenario -RunRoot $approvalEvidenceRoot -ProjectRoot $approvalProjectRoot
	Assert-TestCondition -Condition ([int]$validatedApprovalPlayEvidence.recordingOutputCount -eq 2 -and [bool]$validatedApprovalPlayEvidence.creationOnly) -Message 'Valid paired recording Play Mode evidence did not pass.'

	$approvalBeforeEvidenceBaseline = Get-Content -Raw -LiteralPath $approvalBeforeEvidencePath
	$approvalAfterEvidenceBaseline = Get-Content -Raw -LiteralPath $approvalAfterEvidencePath
	$approvalPlayEvidenceBaseline = Get-Content -Raw -LiteralPath $approvalPlayEvidencePath
	$tamperedBeforeEvidence = $approvalBeforeEvidenceBaseline | ConvertFrom-Json
	$tamperedBeforeEvidence.snapshot.filesSha256 = ('0' * 64)
	[System.IO.File]::WriteAllText($approvalBeforeEvidencePath, ($tamperedBeforeEvidence | ConvertTo-Json -Depth 100), [System.Text.UTF8Encoding]::new($false))
	$tamperedBeforePlayEvidence = $approvalPlayEvidenceBaseline | ConvertFrom-Json
	$tamperedBeforePlayEvidence.beforeEvidence.sha256 = Get-Sha256 -Path $approvalBeforeEvidencePath
	Assert-PlayModeEvidenceRejected -Evidence $tamperedBeforePlayEvidence -EvidencePath $approvalPlayEvidencePath -Scenario $approvalScenario -RunRoot $approvalEvidenceRoot -ProjectRoot $approvalProjectRoot -ExpectedMessage 'snapshot digest metadata does not match' -FailureMessage 'Tampered before-play snapshot digest metadata was accepted.'
	[System.IO.File]::WriteAllText($approvalBeforeEvidencePath, $approvalBeforeEvidenceBaseline, [System.Text.UTF8Encoding]::new($false))

	$tamperedAfterEvidence = $approvalAfterEvidenceBaseline | ConvertFrom-Json
	$tamperedAfterEvidence.recordingOutputs[0].producerCompletionMcpSequence = 11
	[System.IO.File]::WriteAllText($approvalAfterEvidencePath, ($tamperedAfterEvidence | ConvertTo-Json -Depth 100), [System.Text.UTF8Encoding]::new($false))
	$tamperedAfterPlayEvidence = $approvalPlayEvidenceBaseline | ConvertFrom-Json
	$tamperedAfterPlayEvidence.afterEvidence.sha256 = Get-Sha256 -Path $approvalAfterEvidencePath
	Assert-PlayModeEvidenceRejected -Evidence $tamperedAfterPlayEvidence -EvidencePath $approvalPlayEvidencePath -Scenario $approvalScenario -RunRoot $approvalEvidenceRoot -ProjectRoot $approvalProjectRoot -ExpectedMessage 'recording-output comparison or producer provenance was tampered' -FailureMessage 'Tampered after-play producer completion provenance was accepted.'
	[System.IO.File]::WriteAllText($approvalAfterEvidencePath, $approvalAfterEvidenceBaseline, [System.Text.UTF8Encoding]::new($false))

	$tamperedPlaySummary = $approvalPlayEvidenceBaseline | ConvertFrom-Json
	$tamperedPlaySummary.enter.currentState = 'paused'
	Assert-PlayModeEvidenceRejected -Evidence $tamperedPlaySummary -EvidencePath $approvalPlayEvidencePath -Scenario $approvalScenario -RunRoot $approvalEvidenceRoot -ProjectRoot $approvalProjectRoot -ExpectedMessage 'summary enter/exit data does not match' -FailureMessage 'Tampered Play Mode boundary summary data was accepted.'

	[System.IO.File]::WriteAllBytes($approvalOutputPath, [System.Text.Encoding]::UTF8.GetBytes('tampered after evidence lock'))
	$currentOutputTamperEvidence = $approvalPlayEvidenceBaseline | ConvertFrom-Json
	Assert-PlayModeEvidenceRejected -Evidence $currentOutputTamperEvidence -EvidencePath $approvalPlayEvidencePath -Scenario $approvalScenario -RunRoot $approvalEvidenceRoot -ProjectRoot $approvalProjectRoot -ExpectedMessage 'changed after its after-play snapshot' -FailureMessage 'A recording output changed after evidence lock without detection.'
	[System.IO.File]::WriteAllBytes($approvalOutputPath, $approvalOutputBytes)
	[System.IO.File]::WriteAllText($approvalPlayEvidencePath, $approvalPlayEvidenceBaseline, [System.Text.UTF8Encoding]::new($false))

	# The layout copied into each disposable project must be the exact installed
	# default, and that installed default must still match repository source.
	$layoutFixtureRoot = Join-Path $temporaryRoot 'layout-chain'
	$sourceLayoutRoot = Join-Path $layoutFixtureRoot 'source'
	$installedLayoutRoot = Join-Path $layoutFixtureRoot 'installed'
	$layoutProjectRoot = Join-Path $layoutFixtureRoot 'project'
	[System.IO.Directory]::CreateDirectory($sourceLayoutRoot) | Out-Null
	[System.IO.Directory]::CreateDirectory($installedLayoutRoot) | Out-Null
	foreach ($layoutName in @('EditorLayout.terra', 'EditorLayout.ini')) {
		$layoutBytes = [System.Text.Encoding]::UTF8.GetBytes("default-$layoutName")
		[System.IO.File]::WriteAllBytes((Join-Path $sourceLayoutRoot $layoutName), $layoutBytes)
		[System.IO.File]::WriteAllBytes((Join-Path $installedLayoutRoot $layoutName), $layoutBytes)
	}
	$layoutChain = @(Install-VerifiedDefaultEditorLayout -SourceTemplateRoot $sourceLayoutRoot -InstalledTemplateRoot $installedLayoutRoot -ProjectRoot $layoutProjectRoot)
	Assert-TestCondition -Condition ($layoutChain.Count -eq 2 -and @($layoutChain | Where-Object { -not $_.sourceMatchesInstalled -or -not $_.installedMatchesProject }).Count -eq 0) -Message 'Verified source-to-installed-to-project layout chain rejected matching bytes.'
	[System.IO.File]::AppendAllText((Join-Path $installedLayoutRoot 'EditorLayout.ini'), 'tampered', [System.Text.UTF8Encoding]::new($false))
	$installedLayoutMismatchRejected = $false
	try { [void](Install-VerifiedDefaultEditorLayout -SourceTemplateRoot $sourceLayoutRoot -InstalledTemplateRoot $installedLayoutRoot -ProjectRoot $layoutProjectRoot) } catch { $installedLayoutMismatchRejected = $_.Exception.Message -match 'does not match the repository source template' }
	Assert-TestCondition -Condition $installedLayoutMismatchRejected -Message 'A changed installed default layout was copied into the isolated project.'
	Copy-Item -LiteralPath (Join-Path $sourceLayoutRoot 'EditorLayout.ini') -Destination (Join-Path $installedLayoutRoot 'EditorLayout.ini') -Force
	Remove-Item -LiteralPath (Join-Path $installedLayoutRoot 'EditorLayout.terra') -Force
	$missingInstalledLayoutRejected = $false
	try { [void](Install-VerifiedDefaultEditorLayout -SourceTemplateRoot $sourceLayoutRoot -InstalledTemplateRoot $installedLayoutRoot -ProjectRoot $layoutProjectRoot) } catch { $missingInstalledLayoutRejected = $_.Exception.Message -match 'is missing' }
	Assert-TestCondition -Condition $missingInstalledLayoutRejected -Message 'A missing installed default layout did not fail closed.'

	# Manual proof now comes from a real gate-owned PrintWindow capture of a live
	# HWND. Caller JSON contains only ordered action requests and observations.
	$manualDirectory = Join-Path $temporaryRoot 'editor/manual'
	[System.IO.Directory]::CreateDirectory($manualDirectory) | Out-Null
	$evidenceForm = [System.Windows.Forms.Form]::new()
	$evidenceForm.Text = 'TerraEngine Gate Native Evidence Self Test'
	$evidenceForm.Size = [System.Drawing.Size]::new(640, 480)
	$evidenceForm.StartPosition = [System.Windows.Forms.FormStartPosition]::Manual
	$evidenceForm.Location = [System.Drawing.Point]::new(80, 80)
	$evidenceForm.BackColor = [System.Drawing.Color]::FromArgb(26, 42, 72)
	$evidenceLabel = [System.Windows.Forms.Label]::new()
	$evidenceLabel.Dock = [System.Windows.Forms.DockStyle]::Fill
	$evidenceLabel.TextAlign = [System.Drawing.ContentAlignment]::MiddleCenter
	$evidenceLabel.Font = [System.Drawing.Font]::new('Segoe UI', 24, [System.Drawing.FontStyle]::Bold)
	$evidenceLabel.ForeColor = [System.Drawing.Color]::White
	$evidenceLabel.Text = 'Mouse action capture'
	$evidenceForm.Controls.Add($evidenceLabel)
	$evidenceForm.Show()
	$evidenceForm.Activate()
	[System.Windows.Forms.Application]::DoEvents()
	Start-Sleep -Milliseconds 150

	$currentProcess = Get-Process -Id $PID
	$fakeEditor = [pscustomobject]@{
		Id = [int]$PID
		MainWindowHandle = [IntPtr]$evidenceForm.Handle
		MainWindowTitle = $evidenceForm.Text
		StartTime = $currentProcess.StartTime
		HasExited = $false
	}
	$fakeEditor | Add-Member -MemberType ScriptMethod -Name Refresh -Value { }
	$manualStageStartedAtUtc = [DateTimeOffset]::UtcNow
	Start-Sleep -Milliseconds 5
	$mouseEventOneAt = [DateTimeOffset]::UtcNow
	Start-Sleep -Milliseconds 5
	$mouseEventTwoAt = [DateTimeOffset]::UtcNow
	Start-Sleep -Milliseconds 5
	$keyboardEventAt = [DateTimeOffset]::UtcNow
	$clickRecords = @(
		[ordered]@{
			id = 'mouse'; input = 'mouse'; inputDelivery = 'human'; passed = $true; observed = 'Selected both intended native test items.'
			inputRequests = @(
				[ordered]@{ eventId = 'select-first'; kind = 'mouse-click'; description = 'Select the first target.'; performedAtUtc = $mouseEventOneAt.ToString('o') },
				[ordered]@{ eventId = 'select-second'; kind = 'mouse-click'; description = 'Select the second target.'; performedAtUtc = $mouseEventTwoAt.ToString('o') }
			)
		},
		[ordered]@{
			id = 'keyboard'; input = 'keyboard'; inputDelivery = 'human'; passed = $true; observed = 'Invoked the intended native test shortcut.'
			inputRequests = @([ordered]@{ eventId = 'invoke-shortcut'; kind = 'key-chord'; description = 'Invoke the test shortcut.'; performedAtUtc = $keyboardEventAt.ToString('o') })
		}
	)
	$clickPath = Join-Path $temporaryRoot 'editor/clicks.jsonl'
	$nativeEvidencePath = Join-Path $temporaryRoot 'editor/native-actions.jsonl'
	$completionPath = Join-Path $temporaryRoot 'editor/manual-complete.json'
	$clickLines = @($clickRecords | ForEach-Object { $_ | ConvertTo-Json -Depth 20 -Compress })
	[System.IO.File]::WriteAllLines($clickPath, $clickLines, [System.Text.UTF8Encoding]::new($false))
	Start-Sleep -Milliseconds 20
	$mouseNative = New-GateNativeActionEvidence -Action $clickRecords[0] -ActionSequence 1 -RunRoot $temporaryRoot -EditorProcess $fakeEditor
	$evidenceForm.Size = [System.Drawing.Size]::new(660, 480)
	$evidenceForm.BackColor = [System.Drawing.Color]::FromArgb(72, 32, 48)
	$evidenceLabel.BackColor = [System.Drawing.Color]::FromArgb(72, 32, 48)
	$evidenceLabel.Text = 'Keyboard action capture'
	$evidenceForm.Refresh()
	$evidenceForm.Update()
	[System.Windows.Forms.Application]::DoEvents()
	Start-Sleep -Milliseconds 20
	$keyboardNative = New-GateNativeActionEvidence -Action $clickRecords[1] -ActionSequence 2 -RunRoot $temporaryRoot -EditorProcess $fakeEditor
	$nativeRecords = @($mouseNative, $keyboardNative)
	$nativeLines = @($nativeRecords | ForEach-Object { $_ | ConvertTo-Json -Depth 30 -Compress })
	[System.IO.File]::WriteAllLines($nativeEvidencePath, $nativeLines, [System.Text.UTF8Encoding]::new($false))
	[System.IO.File]::WriteAllText($completionPath, (([ordered]@{ completed = $true; defects = @() }) | ConvertTo-Json -Depth 10), [System.Text.UTF8Encoding]::new($false))
	$manualEvidence = Read-AndValidateManualEvidence -Scenario $validatedScenario -ClickPath $clickPath -NativeEvidencePath $nativeEvidencePath -CompletionPath $completionPath -RunRoot $temporaryRoot -EditorProcess $fakeEditor -ManualStageStartedAtUtc $manualStageStartedAtUtc
	Assert-TestCondition -Condition ($manualEvidence.recordCount -eq 2) -Message 'Real gate-owned native-window evidence rejected a valid mouse/keyboard transcript.'
	Assert-TestCondition -Condition (@($manualEvidence.records[0].evidence).Count -eq 1) -Message 'Strict native-window evidence did not retain normalized PrintWindow proof.'
	Assert-TestCondition -Condition (
		[string]$manualEvidence.records[0].evidence[0].nativeWindow.dpiAwarenessContext -ceq 'PER_MONITOR_AWARE_V2' -and
		[bool]$manualEvidence.records[0].evidence[0].nativeWindow.dpiAware
	) -Message 'Native PrintWindow evidence did not retain its runtime-verified Per-Monitor-V2 context.'
	$contextAfterSuccessfulCapture = [TerraEngine.Animation.Validation.NativeWindowEvidence]::GetCurrentThreadDpiAwarenessContextName()
	Assert-TestCondition -Condition ([string]$contextAfterSuccessfulCapture -ceq [string]$initialThreadDpiContext) -Message 'A successful native capture leaked its thread DPI awareness context.'
	$scopedSnapshot = [TerraEngine.Animation.Validation.NativeWindowEvidence]::GetWindowSnapshot([int]$PID, [Int64]$evidenceForm.Handle.ToInt64())
	Assert-TestCondition -Condition (
		[bool]$scopedSnapshot.DpiAwareCapture -and
		[string]$scopedSnapshot.DpiAwarenessContext -ceq 'PER_MONITOR_AWARE_V2' -and
		[int]$scopedSnapshot.PixelWidth -gt 0 -and
		[int]$scopedSnapshot.PixelHeight -gt 0
	) -Message 'DPI-scoped GetWindowRect did not return a runtime-verified physical snapshot.'
	Assert-TestCondition -Condition (
		[TerraEngine.Animation.Validation.NativeWindowEvidence]::GetCurrentThreadDpiAwarenessContextName() -ceq [string]$initialThreadDpiContext
	) -Message 'GetWindowSnapshot did not restore the prior thread DPI awareness context.'
	$failureRestorationObserved = $false
	try {
		$invalidDpiCapturePath = [string]::Concat((Join-Path $temporaryRoot 'invalid-dpi-capture'), [char]0, '.png')
		[void][TerraEngine.Animation.Validation.NativeWindowEvidence]::CaptureWindow([int]$PID, [Int64]$evidenceForm.Handle.ToInt64(), $invalidDpiCapturePath)
	}
	catch {
		$failureRestorationObserved = [TerraEngine.Animation.Validation.NativeWindowEvidence]::GetCurrentThreadDpiAwarenessContextName() -ceq [string]$initialThreadDpiContext
	}
	Assert-TestCondition -Condition $failureRestorationObserved -Message 'A failing scoped native capture did not restore the prior thread DPI awareness context.'

	# Exercise real SendInput when an interactive input desktop is available. On a
	# locked or service desktop, prove the exact same API fails with an explicit
	# capability classification instead of manufacturing a successful receipt.
	$inputCapability = Get-NativeInputCapability
	$dpiClickEffectVerified = $false
	Assert-TestCondition -Condition (-not [string]::IsNullOrWhiteSpace([string]$inputCapability.reason) -and [bool]$inputCapability.sessionLocked -eq (-not [bool]$inputCapability.inputAvailable)) -Message 'Input-desktop classification is internally inconsistent.'
	# Host the input target on a different native UI thread/process so the test
	# actually exercises AttachThreadInput rather than the same-thread fast path.
	$inputTargetLogicalWidth = 640
	$inputTargetLogicalHeight = 360
	$inputWindowScript = @'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
$form = [System.Windows.Forms.Form]::new()
$form.Text = 'TerraEngine DPI Input Target - Ready'
$form.ClientSize = [System.Drawing.Size]::new(640, 360)
$form.StartPosition = [System.Windows.Forms.FormStartPosition]::Manual
$form.Location = [System.Drawing.Point]::new(120, 120)
$button = [System.Windows.Forms.Button]::new()
$button.Text = 'DPI click oracle'
$button.Dock = [System.Windows.Forms.DockStyle]::Right
$button.Width = 24
$button.Add_Click({ $form.Text = 'TerraEngine DPI Input Target - Clicked' })
$form.Controls.Add($button)
[System.Windows.Forms.Application]::Run($form)
'@
	$inputWindowEncoded = [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($inputWindowScript))
	$inputStartInfo = [System.Diagnostics.ProcessStartInfo]::new()
	$inputStartInfo.FileName = (Get-Process -Id $PID).Path
	$inputStartInfo.Arguments = "-NoProfile -NonInteractive -STA -EncodedCommand $inputWindowEncoded"
	$inputStartInfo.UseShellExecute = $false
	$inputStartInfo.CreateNoWindow = $true
	$inputTestProcess = [System.Diagnostics.Process]::Start($inputStartInfo)
	$inputTestJob = [TerraEngine.Animation.Validation.NativeWindowEvidence]::CreateKillOnCloseProcessJob([int]$inputTestProcess.Id)
	$inputWindowDeadline = [DateTimeOffset]::UtcNow.AddSeconds(5)
	do {
		$inputTestProcess.Refresh()
		if ($inputTestProcess.HasExited) { throw 'SendInput focus-target process exited before creating its HWND.' }
		$inputWindowHandle = [Int64]$inputTestProcess.MainWindowHandle.ToInt64()
		if ($inputWindowHandle -ne 0) { break }
		Start-Sleep -Milliseconds 50
	} while ([DateTimeOffset]::UtcNow -lt $inputWindowDeadline)
	Assert-TestCondition -Condition ($inputWindowHandle -ne 0) -Message 'SendInput focus-target process did not create a native HWND.'
	$contextBeforeClientSnapshot = [TerraEngine.Animation.Validation.NativeWindowEvidence]::GetCurrentThreadDpiAwarenessContextName()
	$inputClientSnapshot = [TerraEngine.Animation.Validation.NativeWindowEvidence]::GetClientSnapshot([int]$inputTestProcess.Id, $inputWindowHandle)
	Assert-TestCondition -Condition (
		[bool]$inputClientSnapshot.DpiAwareGeometry -and
		[string]$inputClientSnapshot.DpiAwarenessContext -ceq 'PER_MONITOR_AWARE_V2' -and
		[int]$inputClientSnapshot.PixelWidth -ge 320 -and
		[int]$inputClientSnapshot.PixelHeight -ge 200
	) -Message 'Foreign-HWND client geometry was not read as runtime-verified physical pixels.'
	Assert-TestCondition -Condition (
		[TerraEngine.Animation.Validation.NativeWindowEvidence]::GetCurrentThreadDpiAwarenessContextName() -ceq [string]$contextBeforeClientSnapshot
	) -Message 'GetClientSnapshot leaked its Per-Monitor-V2 thread context.'
	$inputPhysicalScaleX = [double]$inputClientSnapshot.PixelWidth / [double]$inputTargetLogicalWidth
	$inputPhysicalScaleY = [double]$inputClientSnapshot.PixelHeight / [double]$inputTargetLogicalHeight
	$inputUsesNonDefaultPhysicalScale = [Math]::Abs($inputPhysicalScaleX - 1.0) -gt 0.01 -or
		[Math]::Abs($inputPhysicalScaleY - 1.0) -gt 0.01
	# The target process is intentionally DPI-unaware, so GetDpiForWindow can
	# report 96 even while Windows virtualizes its 640-pixel logical client to
	# (for example) 800 physical pixels. Derive the safe button inset from the
	# runtime-observed physical/logical scale instead of trusting that DPI value.
	$oracleInset = [Math]::Max(2, [int][Math]::Round(8.0 * $inputPhysicalScaleX))
	$oracleClientX = [int]$inputClientSnapshot.PixelWidth - $oracleInset
	$oracleClientY = [int][Math]::Floor([double]$inputClientSnapshot.PixelHeight / 2.0)
	$outOfBoundsContext = [TerraEngine.Animation.Validation.NativeWindowEvidence]::GetCurrentThreadDpiAwarenessContextName()
	$outOfBoundsReceipt = [TerraEngine.Animation.Validation.NativeWindowEvidence]::SendMouseClick(
		[int]$inputTestProcess.Id,
		$inputWindowHandle,
		[int]$inputClientSnapshot.PixelWidth,
		$oracleClientY,
		'left')
	if ([bool]$outOfBoundsReceipt.InputAvailable) {
		Assert-TestCondition -Condition (
			-not [bool]$outOfBoundsReceipt.Succeeded -and
			[bool]$outOfBoundsReceipt.DpiAwareGeometry -and
			[string]$outOfBoundsReceipt.DpiAwarenessContext -ceq 'PER_MONITOR_AWARE_V2' -and
			[string]$outOfBoundsReceipt.FailureReason -match 'outside the live editor client bounds'
		) -Message 'A DPI-scoped mouse request accepted the exclusive physical client bound.'
		Assert-TestCondition -Condition (
			[TerraEngine.Animation.Validation.NativeWindowEvidence]::GetCurrentThreadDpiAwarenessContextName() -ceq [string]$outOfBoundsContext
		) -Message 'An early-return mouse geometry failure leaked its Per-Monitor-V2 thread context.'
	}
	else {
		Assert-TestCondition -Condition (
			-not [bool]$outOfBoundsReceipt.Succeeded -and
			-not [bool]$outOfBoundsReceipt.ForegroundAcquisitionAttempted -and
			[int]$outOfBoundsReceipt.EventCountSubmitted -eq 0 -and
			-not [string]::IsNullOrWhiteSpace([string]$outOfBoundsReceipt.FailureReason)
		) -Message 'An input-desktop transition converted an unavailable exclusive-bound probe into a successful receipt.'
	}
	# Windows may briefly refuse a foreground transition while the test host and
	# the foreign STA process finish activating. Retry the identical physical
	# coordinate a small, bounded number of times; every attempt still produces
	# a complete receipt and must restore its temporary PMv2/thread-input state.
	$nativeMouseInputReceipts = @()
	$nativeMouseInputReceipt = $null
	$mouseAttemptLimit = if ([bool]$inputCapability.inputAvailable) { 3 } else { 1 }
	for ($mouseAttempt = 1; $mouseAttempt -le $mouseAttemptLimit; $mouseAttempt++) {
		$nativeMouseInputReceipt = [TerraEngine.Animation.Validation.NativeWindowEvidence]::SendMouseClick([int]$inputTestProcess.Id, $inputWindowHandle, $oracleClientX, $oracleClientY, 'left')
		$nativeMouseInputReceipts += $nativeMouseInputReceipt
		if ([bool]$nativeMouseInputReceipt.Succeeded) {
			$clickEffectDeadline = [DateTimeOffset]::UtcNow.AddMilliseconds(750)
			do {
				$inputTestProcess.Refresh()
				if ($inputTestProcess.MainWindowTitle -match 'Clicked') {
					$dpiClickEffectVerified = $true
					break
				}
				Start-Sleep -Milliseconds 25
			} while ([DateTimeOffset]::UtcNow -lt $clickEffectDeadline)
			if ($dpiClickEffectVerified) { break }
		}
		if ($mouseAttempt -lt $mouseAttemptLimit) { Start-Sleep -Milliseconds 75 }
	}
	# Input-desktop and foreground ownership can change between the initial
	# capability probe and a bounded retry. Prefer the receipt that actually
	# succeeded, otherwise retain the latest receipt that reached PMv2 geometry;
	# only fall back to a fully unavailable receipt when no geometry attempt ran.
	$successfulMouseReceipts = @($nativeMouseInputReceipts | Where-Object { [bool]$_.Succeeded })
	$dpiGeometryMouseReceipts = @($nativeMouseInputReceipts | Where-Object { [bool]$_.DpiAwareGeometry })
	if ($successfulMouseReceipts.Count -gt 0) {
		$nativeMouseInputReceipt = $successfulMouseReceipts[-1]
	}
	elseif ($dpiGeometryMouseReceipts.Count -gt 0) {
		$nativeMouseInputReceipt = $dpiGeometryMouseReceipts[-1]
	}
	else {
		$nativeMouseInputReceipt = $nativeMouseInputReceipts[-1]
	}
	$nativeKeyboardInputReceipt = $null
	$threadInputBalanced = $true
	foreach ($attemptReceipt in $nativeMouseInputReceipts) {
		if (
			-not [bool]$attemptReceipt.ThreadInputDetached -or
			[int]$attemptReceipt.ThreadInputAttachCount -ne [int]$attemptReceipt.ThreadInputDetachCount -or
			[int]$attemptReceipt.ThreadInputDetachAttempts -ne [int]$attemptReceipt.ThreadInputAttachCount
		) {
			$threadInputBalanced = $false
			break
		}
	}
	if ([bool]$nativeMouseInputReceipt.InputAvailable) {
		$dpiMouseGeometryValidated = [bool]$nativeMouseInputReceipt.DpiAwareGeometry -and
			[string]$nativeMouseInputReceipt.DpiAwarenessContext -ceq 'PER_MONITOR_AWARE_V2' -and
			[int]$nativeMouseInputReceipt.Dpi -eq [int]$inputClientSnapshot.Dpi -and
			[int]$nativeMouseInputReceipt.ClientPixelWidth -eq [int]$inputClientSnapshot.PixelWidth -and
			[int]$nativeMouseInputReceipt.ClientPixelHeight -eq [int]$inputClientSnapshot.PixelHeight -and
			[int]$nativeMouseInputReceipt.RequestedClientX -eq $oracleClientX -and
			[int]$nativeMouseInputReceipt.RequestedClientY -eq $oracleClientY -and
			[bool]$nativeMouseInputReceipt.PhysicalCursorPositionVerified -and
			[int]$nativeMouseInputReceipt.ScreenX -eq [int]$nativeMouseInputReceipt.PositionedScreenX -and
			[int]$nativeMouseInputReceipt.ScreenY -eq [int]$nativeMouseInputReceipt.PositionedScreenY
		Assert-TestCondition -Condition $dpiMouseGeometryValidated -Message (
			'Mouse input did not retain exact Per-Monitor-V2 physical client-to-screen geometry. Receipt: {0}' -f
				($nativeMouseInputReceipt | ConvertTo-Json -Depth 8 -Compress)
		)
		Assert-TestCondition -Condition (
			[TerraEngine.Animation.Validation.NativeWindowEvidence]::GetCurrentThreadDpiAwarenessContextName() -ceq [string]$contextBeforeClientSnapshot
		) -Message 'SendMouseClick leaked its Per-Monitor-V2 thread context.'
		Assert-TestCondition -Condition ($threadInputBalanced -and [int]$nativeMouseInputReceipt.ThreadInputAttachAttempts -ge 1) -Message 'Mouse foreground acquisition did not exercise or leaked a temporary AttachThreadInput relationship.'
		$sendInputOutcomeValidated = if ([bool]$nativeMouseInputReceipt.Succeeded) {
			[bool]$nativeMouseInputReceipt.ForegroundAcquired -and
				([bool]$nativeMouseInputReceipt.ForegroundAcquiredBeforeInput -xor [bool]$nativeMouseInputReceipt.ForegroundAcquiredByDeclaredMouseInput) -and
				[Int64]$nativeMouseInputReceipt.ForegroundWindow -eq $inputWindowHandle -and
				[int]$nativeMouseInputReceipt.EventCountSubmitted -eq [int]$nativeMouseInputReceipt.EventCountRequested -and
				[int]$nativeMouseInputReceipt.PostInputForegroundChecks -ge 1 -and
				[int]$nativeMouseInputReceipt.PostInputForegroundChecks -le 8
		}
		else {
			-not [string]::IsNullOrWhiteSpace([string]$nativeMouseInputReceipt.FailureReason) -and
				[string]$nativeMouseInputReceipt.FailureReason -match 'did not submit every requested mouse event|target HWND did not become foreground|thread-input attachment'
		}
		Assert-TestCondition -Condition $sendInputOutcomeValidated -Message 'Declared mouse input neither activated/interacted with the target nor failed with an explicit receipt.'
		if ([bool]$nativeMouseInputReceipt.Succeeded) {
			Assert-TestCondition -Condition $dpiClickEffectVerified -Message 'Accepted activating mouse input did not perform its declared button interaction.'
		}
		if ($inputUsesNonDefaultPhysicalScale -and -not [bool]$nativeMouseInputReceipt.Succeeded) {
			# An interactive desktop can still deny a foreground transition (for
			# example while the Codex host owns the foreground lock timeout).
			# The production bridge must fail closed in that case; geometry and
			# context restoration remain independently testable without claiming
			# that the far-right button received the click.
			Assert-TestCondition -Condition (
				[string]$nativeMouseInputReceipt.FailureReason -match 'target HWND did not become foreground|thread-input attachment'
			) -Message (
				'Non-100% physical scaling neither completed the far-right click nor reported an explicit foreground failure. Receipt: {0}' -f
					($nativeMouseInputReceipt | ConvertTo-Json -Depth 8 -Compress)
			)
		}
		$nativeKeyboardInputReceipt = [TerraEngine.Animation.Validation.NativeWindowEvidence]::SendKeyChord([int]$inputTestProcess.Id, $inputWindowHandle, [int[]]@(0x87))
		$keyboardThreadInputBalanced = [bool]$nativeKeyboardInputReceipt.ThreadInputDetached -and
			[int]$nativeKeyboardInputReceipt.ThreadInputAttachCount -eq [int]$nativeKeyboardInputReceipt.ThreadInputDetachCount -and
			[int]$nativeKeyboardInputReceipt.ThreadInputDetachAttempts -eq [int]$nativeKeyboardInputReceipt.ThreadInputAttachCount
		$keyboardOutcomeValidated = if ([bool]$nativeKeyboardInputReceipt.Succeeded) {
			[bool]$nativeKeyboardInputReceipt.ForegroundAcquiredBeforeInput -and
				-not [bool]$nativeKeyboardInputReceipt.ForegroundAcquiredByDeclaredMouseInput -and
				[int]$nativeKeyboardInputReceipt.EventCountSubmitted -eq [int]$nativeKeyboardInputReceipt.EventCountRequested
		}
		else {
			-not [bool]$nativeKeyboardInputReceipt.ForegroundAcquiredBeforeInput -and
				[int]$nativeKeyboardInputReceipt.EventCountSubmitted -eq 0 -and
				-not [string]::IsNullOrWhiteSpace([string]$nativeKeyboardInputReceipt.FailureReason)
		}
		Assert-TestCondition -Condition ($keyboardThreadInputBalanced -and $keyboardOutcomeValidated) -Message 'Keyboard input was submitted without prior target foreground or leaked thread-input attachment.'

		# Exercise the passing serialized-receipt contract only when Windows
		# actually granted foreground ownership and the foreign HWND observed the
		# click. Explicit foreground denial was already validated fail-closed
		# above and must not be converted into synthetic passing manual evidence.
		if ([bool]$nativeMouseInputReceipt.Succeeded -and $dpiClickEffectVerified) {
			$win32Scenario = [pscustomobject][ordered]@{
			manualChecklist = @([pscustomobject][ordered]@{
				id = 'pmv2-oracle'
				input = 'mouse'
				required = $true
				instruction = 'Click the far-right DPI oracle.'
				minimumInputEvents = 1
			})
		}
		$win32Action = [pscustomobject][ordered]@{
			id = 'pmv2-oracle'
			input = 'mouse'
			inputDelivery = 'Win32SendInput'
			passed = $true
			observed = 'The far-right DPI click oracle activated.'
			inputRequests = @([pscustomobject][ordered]@{
				eventId = 'pmv2-far-right-click'
				kind = 'mouse-click'
				description = 'Click the far-right physical client-pixel oracle.'
				clientX = $oracleClientX
				clientY = $oracleClientY
				button = 'left'
			})
		}
		$win32ClickPath = Join-Path $temporaryRoot 'editor/pmv2-clicks.jsonl'
		$win32NativePath = Join-Path $temporaryRoot 'editor/pmv2-native-actions.jsonl'
		$win32CompletionPath = Join-Path $temporaryRoot 'editor/pmv2-manual-complete.json'
		$win32StageStartedAtUtc = [DateTimeOffset]::UtcNow
		[System.IO.File]::WriteAllText($win32ClickPath, ($win32Action | ConvertTo-Json -Depth 20 -Compress), [System.Text.UTF8Encoding]::new($false))
		$win32NativeRecord = New-GateNativeActionEvidence -Action $win32Action -ActionSequence 1 -RunRoot $temporaryRoot -EditorProcess $inputTestProcess
		$win32NativeBaseline = $win32NativeRecord | ConvertTo-Json -Depth 40 -Compress
		[System.IO.File]::WriteAllText($win32NativePath, $win32NativeBaseline, [System.Text.UTF8Encoding]::new($false))
		# Completion is the final durable marker: all declared input receipts and
		# their gate-owned captures must already exist before it becomes visible.
		[System.IO.File]::WriteAllText($win32CompletionPath, (([ordered]@{ completed = $true; defects = @() }) | ConvertTo-Json -Depth 10), [System.Text.UTF8Encoding]::new($false))
		$validatedWin32Evidence = Read-AndValidateManualEvidence -Scenario $win32Scenario -ClickPath $win32ClickPath -NativeEvidencePath $win32NativePath -CompletionPath $win32CompletionPath -RunRoot $temporaryRoot -EditorProcess $inputTestProcess -ManualStageStartedAtUtc $win32StageStartedAtUtc
		Assert-TestCondition -Condition ([int]$validatedWin32Evidence.recordCount -eq 1) -Message 'A valid PMv2 Win32SendInput receipt did not pass the manual-evidence contract.'
		foreach ($dpiReceiptMutation in @(
			[ordered]@{ property = 'dpiAwareGeometry'; value = $false; message = 'A DPI-unaware mouse geometry receipt was accepted.' },
			[ordered]@{ property = 'dpiAwarenessContext'; value = 'SYSTEM_AWARE'; message = 'A non-PMv2 mouse geometry receipt was accepted.' },
			[ordered]@{ property = 'requestedClientX'; value = ($oracleClientX - 1); message = 'A mouse receipt not bound to its declared physical client coordinate was accepted.' },
			[ordered]@{ property = 'physicalCursorPositionVerified'; value = $false; message = 'An unverified physical cursor position was accepted.' }
		)) {
			$tamperedWin32Record = $win32NativeBaseline | ConvertFrom-Json
			$tamperedWin32Record.inputReceipts[0].($dpiReceiptMutation.property) = $dpiReceiptMutation.value
			[System.IO.File]::WriteAllText($win32NativePath, ($tamperedWin32Record | ConvertTo-Json -Depth 40 -Compress), [System.Text.UTF8Encoding]::new($false))
			$dpiReceiptRejected = $false
			try {
				[void](Read-AndValidateManualEvidence -Scenario $win32Scenario -ClickPath $win32ClickPath -NativeEvidencePath $win32NativePath -CompletionPath $win32CompletionPath -RunRoot $temporaryRoot -EditorProcess $inputTestProcess -ManualStageStartedAtUtc $win32StageStartedAtUtc)
			}
			catch {
				$dpiReceiptRejected = $_.Exception.Message -match 'incomplete or did not submit every event'
			}
			Assert-TestCondition -Condition $dpiReceiptRejected -Message ([string]$dpiReceiptMutation.message)
			}
			[System.IO.File]::WriteAllText($win32NativePath, $win32NativeBaseline, [System.Text.UTF8Encoding]::new($false))
		}
	}
	else {
		Assert-TestCondition -Condition ($threadInputBalanced -and -not [bool]$nativeMouseInputReceipt.Succeeded -and -not [bool]$nativeMouseInputReceipt.ForegroundAcquisitionAttempted -and [int]$nativeMouseInputReceipt.EventCountSubmitted -eq 0 -and -not [string]::IsNullOrWhiteSpace([string]$nativeMouseInputReceipt.FailureReason)) -Message 'Unavailable or locked input desktop produced a successful SendInput claim.'
	}
	[void]$inputTestProcess.CloseMainWindow()
	if (-not $inputTestProcess.WaitForExit(5000)) {
		Assert-TestCondition -Condition ([bool]$inputTestJob.Terminate(0)) -Message 'SendInput focus-target process could not be terminated.'
		[void]$inputTestProcess.WaitForExit(5000)
	}
	$inputTestJob.Dispose()
	$inputTestJob = $null
	$inputTestProcess.Dispose()
	$inputTestProcess = $null

	$validateNativeMutationRejected = {
		param($Records, [string]$ExpectedPattern, [string]$FailureMessage)
		$lines = @($Records | ForEach-Object { $_ | ConvertTo-Json -Depth 30 -Compress })
		[System.IO.File]::WriteAllLines($nativeEvidencePath, $lines, [System.Text.UTF8Encoding]::new($false))
		$rejected = $false
		try {
			[void](Read-AndValidateManualEvidence -Scenario $validatedScenario -ClickPath $clickPath -NativeEvidencePath $nativeEvidencePath -CompletionPath $completionPath -RunRoot $temporaryRoot -EditorProcess $fakeEditor -ManualStageStartedAtUtc $manualStageStartedAtUtc)
		}
		catch { $rejected = $_.Exception.Message -match $ExpectedPattern }
		Assert-TestCondition -Condition $rejected -Message $FailureMessage
	}

	$wrongPidRecords = $nativeRecords | ConvertTo-Json -Depth 30 | ConvertFrom-Json
	$wrongPidRecords[0].evidence[0].nativeWindow.processId = $PID + 1
	& $validateNativeMutationRejected $wrongPidRecords 'live HWND owner validation' 'Native capture metadata for a different PID was accepted.'

	$wrongHandleRecords = $nativeRecords | ConvertTo-Json -Depth 30 | ConvertFrom-Json
	$wrongHandleRecords[0].evidence[0].nativeWindow.windowHandle = '0x1'
	& $validateNativeMutationRejected $wrongHandleRecords 'windowHandle does not match' 'Native capture metadata for a different HWND was accepted.'

	foreach ($invalidPrintWindowCase in @(
		[ordered]@{ name = 'blank-title'; property = 'windowTitle'; value = ''; message = 'A native capture with no title was accepted.' },
		[ordered]@{ name = 'wrong-method'; property = 'captureMethod'; value = 'BitBlt'; message = 'A non-PrintWindow capture was accepted.' },
		[ordered]@{ name = 'dpi-unaware'; property = 'dpiAware'; value = $false; message = 'A DPI-unaware native capture was accepted.' },
		[ordered]@{ name = 'wrong-dpi-context'; property = 'dpiAwarenessContext'; value = 'SYSTEM_AWARE'; message = 'A non-Per-Monitor-V2 native capture context was accepted.' }
	)) {
		$invalidRecords = $nativeRecords | ConvertTo-Json -Depth 30 | ConvertFrom-Json
		$invalidRecords[0].evidence[0].nativeWindow.($invalidPrintWindowCase.property) = $invalidPrintWindowCase.value
		& $validateNativeMutationRejected $invalidRecords 'DPI-aware PrintWindow metadata' $invalidPrintWindowCase.message
	}

	$wrongBoundsRecords = $nativeRecords | ConvertTo-Json -Depth 30 | ConvertFrom-Json
	$wrongBoundsRecords[0].evidence[0].nativeWindow.pixelWidth = [int]$wrongBoundsRecords[0].evidence[0].nativeWindow.pixelWidth - 1
	& $validateNativeMutationRejected $wrongBoundsRecords 'dimensions do not match sensible live HWND bounds' 'Native capture bounds that differed from its PNG were accepted.'

	$staleActionRecords = $nativeRecords | ConvertTo-Json -Depth 30 | ConvertFrom-Json
	$staleActionRecords[0].actionObservedAtUtc = $manualStageStartedAtUtc.AddMilliseconds(-1).ToString('o')
	& $validateNativeMutationRejected $staleActionRecords 'predates the manual stage' 'Native evidence predating the manual stage was accepted.'

	$unboundCaptureRecords = $nativeRecords | ConvertTo-Json -Depth 30 | ConvertFrom-Json
	$unboundCaptureRecords[0].evidence[0].nativeWindow.capturedAtUtc = ([DateTimeOffset]::Parse([string]$unboundCaptureRecords[0].inputReceipts[0].startedAtUtc).AddMilliseconds(-1)).ToString('o')
	& $validateNativeMutationRejected $unboundCaptureRecords 'capture timestamp is not fresh and action-bound' 'A capture timestamp before its input receipt was accepted.'

	$blackCapturePath = Join-Path $manualDirectory 'black-native-window.png'
	$blackWidth = [int]$nativeRecords[0].evidence[0].nativeWindow.pixelWidth
	$blackHeight = [int]$nativeRecords[0].evidence[0].nativeWindow.pixelHeight
	$blackBytes = New-TestPng -Path $blackCapturePath -Background ([System.Drawing.Color]::Black) -Width $blackWidth -Height $blackHeight
	$blackStatistics = Get-PngVisualStatistics -Bytes $blackBytes
	$blackRecords = $nativeRecords | ConvertTo-Json -Depth 30 | ConvertFrom-Json
	$blackRecords[0].evidence[0].path = 'editor/manual/black-native-window.png'
	$blackRecords[0].evidence[0].sha256 = Get-Sha256 -Path $blackCapturePath
	$blackRecords[0].evidence[0].pixelSha256 = [string]$blackStatistics.pixelSha256
	$blackRecords[0].evidence[0].fileLastWriteTimeUtc = (Get-Item -LiteralPath $blackCapturePath).LastWriteTimeUtc.ToString('o')
	& $validateNativeMutationRejected $blackRecords 'black, uniform' 'A black or uniform full-size native capture was accepted.'

	# Appending bytes after the PNG IEND changes the file digest without changing
	# decoded pixels. This proves uniqueness is enforced at pixel level.
	$reencodedPath = Join-Path $manualDirectory '02-keyboard-reencoded.png'
	$mouseCapturePath = Join-Path $temporaryRoot ([string]$nativeRecords[0].evidence[0].path)
	$reencodedBytes = [byte[]]([System.IO.File]::ReadAllBytes($mouseCapturePath) + [byte[]](0x54, 0x45, 0x53, 0x54))
	[System.IO.File]::WriteAllBytes($reencodedPath, $reencodedBytes)
	$reencodedStatistics = Get-PngVisualStatistics -Bytes $reencodedBytes
	Assert-TestCondition -Condition ((Get-Sha256 -Path $mouseCapturePath) -cne (Get-Sha256 -Path $reencodedPath) -and [string]$nativeRecords[0].evidence[0].pixelSha256 -ceq [string]$reencodedStatistics.pixelSha256) -Message 'Re-encoded PNG fixture did not retain pixels while changing file bytes.'
	$duplicatePixelRecords = $nativeRecords | ConvertTo-Json -Depth 30 | ConvertFrom-Json
	$duplicatePixelRecords[1].evidence[0].path = 'editor/manual/02-keyboard-reencoded.png'
	$duplicatePixelRecords[1].evidence[0].sha256 = Get-Sha256 -Path $reencodedPath
	$duplicatePixelRecords[1].evidence[0].pixelSha256 = [string]$reencodedStatistics.pixelSha256
	$duplicatePixelRecords[1].evidence[0].fileLastWriteTimeUtc = (Get-Item -LiteralPath $reencodedPath).LastWriteTimeUtc.ToString('o')
	$duplicatePixelRecords[1].evidence[0].nativeWindow.pixelWidth = $duplicatePixelRecords[0].evidence[0].nativeWindow.pixelWidth
	$duplicatePixelRecords[1].evidence[0].nativeWindow.pixelHeight = $duplicatePixelRecords[0].evidence[0].nativeWindow.pixelHeight
	& $validateNativeMutationRejected $duplicatePixelRecords 'reuses identical screenshot pixels' 'Byte-distinct PNGs with identical decoded pixels satisfied two actions.'

	$callerEvidenceRecords = $clickRecords | ConvertTo-Json -Depth 20 | ConvertFrom-Json
	$callerEvidenceRecords[0] | Add-Member -NotePropertyName evidence -NotePropertyValue @([pscustomobject]@{ path = 'caller.png' })
	[System.IO.File]::WriteAllLines($clickPath, @($callerEvidenceRecords | ForEach-Object { $_ | ConvertTo-Json -Depth 20 -Compress }), [System.Text.UTF8Encoding]::new($false))
	$callerEvidenceRejected = $false
	try { [void](Read-ManualActionRecords -Scenario $validatedScenario -ClickPath $clickPath -RequireComplete) } catch { $callerEvidenceRejected = $_.Exception.Message -match 'may not provide screenshot evidence' }
	Assert-TestCondition -Condition $callerEvidenceRejected -Message 'Caller-authored screenshot metadata was accepted as gate-owned evidence.'

	$insufficientEvents = $clickRecords | ConvertTo-Json -Depth 20 | ConvertFrom-Json
	$insufficientEvents[0].inputRequests = @($insufficientEvents[0].inputRequests | Select-Object -First 1)
	[System.IO.File]::WriteAllLines($clickPath, @($insufficientEvents | ForEach-Object { $_ | ConvertTo-Json -Depth 20 -Compress }), [System.Text.UTF8Encoding]::new($false))
	$multiActionUnderReceiptRejected = $false
	try { [void](Read-ManualActionRecords -Scenario $validatedScenario -ClickPath $clickPath -RequireComplete) } catch { $multiActionUnderReceiptRejected = $_.Exception.Message -match 'one event cannot prove a multi-action narrative' }
	Assert-TestCondition -Condition $multiActionUnderReceiptRejected -Message 'One receipted event satisfied a multi-action checklist narrative.'

	[System.IO.File]::WriteAllLines($clickPath, @($clickLines[1], $clickLines[0]), [System.Text.UTF8Encoding]::new($false))
	$swappedActionRejected = $false
	try { [void](Read-ManualActionRecords -Scenario $validatedScenario -ClickPath $clickPath -RequireComplete) } catch { $swappedActionRejected = $_.Exception.Message -match 'checklist order remains exact' }
	Assert-TestCondition -Condition $swappedActionRejected -Message 'Out-of-order manual action requests satisfied the exact checklist.'
	[System.IO.File]::WriteAllLines($clickPath, $clickLines, [System.Text.UTF8Encoding]::new($false))
	[System.IO.File]::WriteAllLines($nativeEvidencePath, $nativeLines, [System.Text.UTF8Encoding]::new($false))

	[System.IO.File]::WriteAllText($completionPath, '{"completed":"true","defects":[]}', [System.Text.UTF8Encoding]::new($false))
	$stringBooleanRejected = $false
	try { [void](Read-AndValidateManualEvidence -Scenario $validatedScenario -ClickPath $clickPath -NativeEvidencePath $nativeEvidencePath -CompletionPath $completionPath -RunRoot $temporaryRoot -EditorProcess $fakeEditor -ManualStageStartedAtUtc $manualStageStartedAtUtc) } catch { $stringBooleanRejected = $_.Exception.Message -match 'JSON Boolean true' }
	Assert-TestCondition -Condition $stringBooleanRejected -Message 'String-coerced manual completion was accepted as a JSON Boolean.'
	[System.IO.File]::WriteAllText($completionPath, (([ordered]@{ completed = $true; defects = @() }) | ConvertTo-Json -Depth 10), [System.Text.UTF8Encoding]::new($false))

	$duplicateManualIds = $scenario | ConvertTo-Json -Depth 20 | ConvertFrom-Json
	$duplicateManualIds.manualChecklist[1].id = 'mouse'
	[System.IO.File]::WriteAllText($scenarioPath, ($duplicateManualIds | ConvertTo-Json -Depth 20), [System.Text.UTF8Encoding]::new($false))
	$duplicateManualIdRejected = $false
	try { [void](Read-ScenarioManifest -Path $scenarioPath -ExpectedPhase 'self-test') } catch { $duplicateManualIdRejected = $_.Exception.Message -match 'unique path-safe names' }
	Assert-TestCondition -Condition $duplicateManualIdRejected -Message 'Duplicate manual checklist IDs were accepted.'
	[System.IO.File]::WriteAllText($scenarioPath, ($scenario | ConvertTo-Json -Depth 20), [System.Text.UTF8Encoding]::new($false))

	$evidenceForm.Close()
	$evidenceForm.Dispose()
	$evidenceForm = $null

	# The same native helper used by the live gate must own and terminate an
	# entire bounded process lifetime without relying on Process.Kill alone.
	$jobStartInfo = [System.Diagnostics.ProcessStartInfo]::new()
	$jobStartInfo.FileName = (Get-Process -Id $PID).Path
	$jobStartInfo.Arguments = '-NoProfile -NonInteractive -Command "Start-Sleep -Seconds 30"'
	$jobStartInfo.UseShellExecute = $false
	$jobStartInfo.CreateNoWindow = $true
	$jobTestProcess = [System.Diagnostics.Process]::Start($jobStartInfo)
	$jobTestObject = [TerraEngine.Animation.Validation.NativeWindowEvidence]::CreateKillOnCloseProcessJob([int]$jobTestProcess.Id)
	Assert-TestCondition -Condition ([bool]$jobTestObject.Terminate(77)) -Message 'TerminateJobObject failed for the disposable cleanup fixture.'
	Assert-TestCondition -Condition ([bool]$jobTestProcess.WaitForExit(5000)) -Message 'Job Object termination did not stop its assigned process within five seconds.'
	$jobTestObject.Dispose()
	$jobTestObject = $null
	$jobTestProcess.Dispose()
	$jobTestProcess = $null

	$reconnectMarkers = @(
		'Invoke-McpRpc -Method ''initialize'' -Params ([ordered]@{ protocolVersion = ''2025-03-26''; clientInfo = [ordered]@{ name = ''animation-phase-gate-close-boundary''',
		'-DisableKeepAlive',
		'$postReconnectTools = Invoke-McpRpc -Method ''tools/list''',
		'$postReconnectPlay = Invoke-McpTool -Name ''play.state''',
		'editor/mcp-reconnect.json'
	)
	$reconnectMarkerIndex = -1
	foreach ($reconnectMarker in $reconnectMarkers) {
		$nextReconnectIndex = $runnerSource.IndexOf($reconnectMarker, $reconnectMarkerIndex + 1, [System.StringComparison]::Ordinal)
		Assert-TestCondition -Condition ($nextReconnectIndex -gt $reconnectMarkerIndex) -Message "Reconnect proof is missing or misorders '$reconnectMarker'."
		$reconnectMarkerIndex = $nextReconnectIndex
	}
	foreach ($cleanupMarker in @('CreateKillOnCloseProcessJob', '$editorJob.Dispose()', '$streamCapture.task.Wait(10000)', 'MCP loopback port $McpPort remained occupied')) {
		Assert-TestCondition -Condition ($runnerSource.Contains($cleanupMarker)) -Message "Bounded process-tree cleanup marker '$cleanupMarker' is missing."
	}

	# Acceptance updates are prepared without mutation, then committed with a
	# content-hash compare-and-swap only after the caller has locked evidence.
	$Phase = 'self-test'
	$RunId = 'self-test-run'
	$matrixPath = Join-Path $temporaryRoot 'acceptance-matrix.md'
	$utf8EmDash = [string][char]0x2014
	$matrixText = @(
		'| ID | Requirement | Tests | Live proof | Evidence | Status |',
		'|---|---|---|---|---|---|',
		("| animation-contract-represent-skeletons-animation-clips-animation-controllers-masks-retarget-mappings-motion-databases-ragdoll | requirement $utf8EmDash preserves UTF-8 | tests | live | initial $utf8EmDash evidence | Evidence incomplete |")
	) -join "`r`n"
	[System.IO.File]::WriteAllText($matrixPath, ($matrixText + "`r`n"), [System.Text.UTF8Encoding]::new($false))
	$acceptanceManifestPath = Join-Path $temporaryRoot 'acceptance-update.json'
	$acceptanceRunRoot = Join-Path $temporaryRoot 'run'
	[System.IO.Directory]::CreateDirectory((Join-Path $acceptanceRunRoot 'editor')) | Out-Null
	[System.IO.Directory]::CreateDirectory((Join-Path $acceptanceRunRoot 'tests')) | Out-Null
	foreach ($artifact in @('editor/mcp-operations.json', 'editor/manual-evidence.json', 'editor/camera-evidence.json', 'tests/focused-tests.command.json')) {
		$artifactPath = Join-Path $acceptanceRunRoot $artifact
		[System.IO.File]::WriteAllText($artifactPath, '{"passed":true}', [System.Text.UTF8Encoding]::new($false))
	}
	$acceptanceCameraInventory = @(
		[pscustomobject]@{ id = 'scene'; visualAssessment = [pscustomobject]@{ passed = $true } },
		[pscustomobject]@{ id = 'game'; visualAssessment = [pscustomobject]@{ passed = $true } },
		[pscustomobject]@{ id = 'ingame'; visualAssessment = [pscustomobject]@{ passed = $true } }
	)
	$acceptedEvidence = [ordered]@{
		statement = 'The reviewed phase operations, native camera captures, manual mouse and keyboard actions, and focused test artifact jointly support every accepted row.'
		requirementIds = @('animation-contract-represent-skeletons-animation-clips-animation-controllers-masks-retarget-mappings-motion-databases-ragdoll')
		operationIds = @('inspect')
		captureIds = @('scene', 'game', 'ingame')
		manualChecklistIds = @('mouse', 'keyboard')
		requiredArtifacts = @('editor/mcp-operations.json', 'editor/manual-evidence.json', 'editor/camera-evidence.json', 'tests/focused-tests.command.json')
	}
	[System.IO.File]::WriteAllText($acceptanceManifestPath,
		(([ordered]@{ schemaVersion = 1; phase = 'self-test'; acceptedEvidence = $acceptedEvidence; updates = @([ordered]@{ id = 'animation-contract-represent-skeletons-animation-clips-animation-controllers-masks-retarget-mappings-motion-databases-ragdoll'; status = 'Accepted' }) }) | ConvertTo-Json -Depth 10),
		[System.Text.UTF8Encoding]::new($false))
	$matrixBefore = Get-Sha256 -Path $matrixPath
	$acceptancePlan = New-AcceptanceMatrixUpdate -MatrixPath $matrixPath -UpdateManifestPath $acceptanceManifestPath -RepoRoot $temporaryRoot -RunRoot $acceptanceRunRoot -Scenario $validatedScenario -OperationInventory $operationEvidenceRecords -CameraInventory $acceptanceCameraInventory -ManualEvidence $manualEvidence -PerformanceDeferred $false
	Assert-TestCondition -Condition ((Get-Sha256 -Path $matrixPath) -ceq $matrixBefore) -Message 'Preparing an acceptance update mutated the matrix before evidence lock.'
	Assert-TestCondition -Condition ([string]$acceptancePlan.updatedText).Contains("requirement $utf8EmDash preserves UTF-8") -Message 'Acceptance planning corrupted BOM-less UTF-8 requirement text.'
	Assert-TestCondition -Condition ([string]$acceptancePlan.updatedText).Contains("initial $utf8EmDash evidence") -Message 'Acceptance planning corrupted BOM-less UTF-8 evidence text.'

	[System.IO.File]::WriteAllText($acceptanceManifestPath,
		(([ordered]@{ schemaVersion = 1; phase = 'self-test'; updates = @() }) | ConvertTo-Json -Depth 10),
		[System.Text.UTF8Encoding]::new($false))
	$emptyAcceptanceRejected = $false
	try {
		[void](New-AcceptanceMatrixUpdate -MatrixPath $matrixPath -UpdateManifestPath $acceptanceManifestPath -RepoRoot $temporaryRoot -RunRoot $acceptanceRunRoot -Scenario $validatedScenario -OperationInventory $operationEvidenceRecords -CameraInventory $acceptanceCameraInventory -ManualEvidence $manualEvidence -PerformanceDeferred $false)
	}
	catch { $emptyAcceptanceRejected = $_.Exception.Message -match 'non-empty JSON array' }
	Assert-TestCondition -Condition $emptyAcceptanceRejected -Message 'Empty acceptance updates produced an unblocking transaction.'
	[System.IO.File]::WriteAllText($acceptanceManifestPath,
		(([ordered]@{ schemaVersion = 1; phase = 'self-test'; updates = @([ordered]@{ id = 'animation-contract-represent-skeletons-animation-clips-animation-controllers-masks-retarget-mappings-motion-databases-ragdoll'; status = 'Evidence incomplete' }) }) | ConvertTo-Json -Depth 10),
		[System.Text.UTF8Encoding]::new($false))
	$noAcceptedRowsRejected = $false
	try {
		[void](New-AcceptanceMatrixUpdate -MatrixPath $matrixPath -UpdateManifestPath $acceptanceManifestPath -RepoRoot $temporaryRoot -RunRoot $acceptanceRunRoot -Scenario $validatedScenario -OperationInventory $operationEvidenceRecords -CameraInventory $acceptanceCameraInventory -ManualEvidence $manualEvidence -PerformanceDeferred $false)
	}
	catch { $noAcceptedRowsRejected = $_.Exception.Message -match 'no Accepted rows' }
	Assert-TestCondition -Condition $noAcceptedRowsRejected -Message 'A non-empty acceptance update with no accepted rows unblocked the next phase.'
	[System.IO.File]::WriteAllText($acceptanceManifestPath,
		(([ordered]@{ schemaVersion = 1; phase = 'self-test'; acceptedEvidence = $acceptedEvidence; updates = @([ordered]@{ id = 'animation-contract-represent-skeletons-animation-clips-animation-controllers-masks-retarget-mappings-motion-databases-ragdoll'; status = 'Accepted' }) }) | ConvertTo-Json -Depth 10),
		[System.Text.UTF8Encoding]::new($false))

	$unsupportedAcceptanceRejected = $false
	try {
		[void](New-AcceptanceMatrixUpdate -MatrixPath $matrixPath -UpdateManifestPath $acceptanceManifestPath -RepoRoot $temporaryRoot -RunRoot $acceptanceRunRoot -Scenario $validatedScenario -OperationInventory @() -CameraInventory $acceptanceCameraInventory -ManualEvidence $manualEvidence -PerformanceDeferred $false)
	}
	catch { $unsupportedAcceptanceRejected = $_.Exception.Message -match 'not backed by every declared passing operationIds' }
	Assert-TestCondition -Condition $unsupportedAcceptanceRejected -Message 'Accepted row was not bound to its executed passing operation evidence.'

	[System.IO.File]::AppendAllText($matrixPath, "concurrent change`r`n", [System.Text.UTF8Encoding]::new($false))
	$staleAcceptanceRejected = $false
	try { [void](Apply-AcceptanceMatrixUpdate -Plan $acceptancePlan) } catch { $staleAcceptanceRejected = $_.Exception.Message -match 'changed concurrently' }
	Assert-TestCondition -Condition $staleAcceptanceRejected -Message 'Acceptance compare-and-swap accepted a concurrently changed matrix.'
	[System.IO.File]::WriteAllBytes($matrixPath, [byte[]]$acceptancePlan.originalBytes)
	$committedAcceptance = Apply-AcceptanceMatrixUpdate -Plan $acceptancePlan
	Assert-TestCondition -Condition ([bool]$committedAcceptance.updateApplied -and (Get-Sha256 -Path $matrixPath) -ceq [string]$acceptancePlan.proposedAfterSha256) -Message 'Prepared acceptance update did not commit its exact content hash.'
	Restore-AcceptanceMatrixUpdate -Plan $acceptancePlan
	Assert-TestCondition -Condition ((Get-Sha256 -Path $matrixPath) -ceq $matrixBefore) -Message 'Acceptance rollback did not restore the original matrix bytes.'

	$performanceEvidencePath = Join-Path $temporaryRoot 'performance-evidence.json'
	$bareCriterionEvidence = [ordered]@{
		feature = $script:FeatureName
		task = 'AnimationValidationRunsEveryAccumulatedFeatureWorkflowBenchmarkTRunsEveryUnrelated'
		passed = $true
		status = 'passed'
		deferred = $false
		scope = 'final-animation-performance'
		successCriteria = @('animation-outcome-creator-import-representative-animated-gltf-fbx-package-obtain-correctly-referenced-playable-prefab')
	}
	[System.IO.File]::WriteAllText($performanceEvidencePath, ($bareCriterionEvidence | ConvertTo-Json -Depth 10), [System.Text.UTF8Encoding]::new($false))
	$bareCriterionRejected = $false
	try { [void](Read-FinalAnimationPerformanceEvidence -Path $performanceEvidencePath -OutRoot $temporaryRoot) } catch { $bareCriterionRejected = $_.Exception.Message -match 'bare ID without passing measurement evidence' }
	Assert-TestCondition -Condition $bareCriterionRejected -Message 'Final AnimationValidationRunsEveryAccumulatedFeatureWorkflowBenchmarkTRunsEveryUnrelated evidence accepted a bare success-criterion ID without measurements.'

	foreach ($receiptMarker in @(
		"receiptRole = 'terminal-acceptance-transaction-result'",
		"preCommitResultState = 'validation-passed-acceptance-pending'",
		'nextPhaseUnblocked = $true'
	)) {
		Assert-TestCondition -Condition ($runnerSource.Contains($receiptMarker)) -Message "Acceptance receipt/result provenance marker '$receiptMarker' is missing."
	}

	$installedScenarioCount = 0
	$emptyAllowedIds = [System.Collections.Generic.List[string]]::new()
	foreach ($installedManifest in @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'Tools/Animation') -Filter '*PhaseScenario.json' -File)) {
		$installedJson = Get-Content -Raw -LiteralPath $installedManifest.FullName | ConvertFrom-Json
		$installedValidated = Read-ScenarioManifest -Path $installedManifest.FullName -ExpectedPhase ([string]$installedJson.phase)
		foreach ($expectation in @($installedValidated.visualExpectations | Where-Object { [string]$_.contentPolicy -eq 'empty-scene-allowed' })) {
			$emptyAllowedIds.Add([string]$expectation.captureId)
		}
		$installedScenarioCount++
	}
	Assert-TestCondition -Condition ($installedScenarioCount -gt 0) -Message 'No installed phase scenario manifests were validated.'
	# Empty-scene evidence is fail-closed except for the two declared camera
	# negative controls: the GPU Animation Validation culling check and the final Animation Validation scale check.
	$emptyAllowedIdSummary = @($emptyAllowedIds | Sort-Object) -join ','
	Assert-TestCondition -Condition (
		$emptyAllowedIds.Count -eq 2 -and
		$emptyAllowedIdSummary -ceq 'animation-scale-validation-scene-away,gpu-AnimationValidationSceneOutOfView'
	) -Message 'Only the deliberate GPU Animation Validation culling and Animation Validation scale out-of-view captures may permit empty visual content.'

	$missingExpectation = $scenario | ConvertTo-Json -Depth 20 | ConvertFrom-Json
	$missingExpectation.visualExpectations = @($missingExpectation.visualExpectations | Select-Object -First 2)
	[System.IO.File]::WriteAllText($scenarioPath, ($missingExpectation | ConvertTo-Json -Depth 20), [System.Text.UTF8Encoding]::new($false))
	$missingRejected = $false
	try { [void](Read-ScenarioManifest -Path $scenarioPath -ExpectedPhase 'self-test') } catch { $missingRejected = $_.Exception.Message -match 'Every camera capture' }
	Assert-TestCondition -Condition $missingRejected -Message 'Scenario without an explicit expectation for every capture did not fail closed.'

	$cameraInventory = @(
		[pscustomobject]@{ id = 'scene'; view = 'SceneView'; sha256 = 'scene-file'; visualExpectation = $validatedScenario.captures[0].visualExpectation; visualStatistics = [pscustomobject]@{ pixelSha256 = 'scene-pixels' } },
		[pscustomobject]@{ id = 'game'; view = 'GameView'; sha256 = 'shared-file'; visualExpectation = $validatedScenario.captures[1].visualExpectation; visualStatistics = [pscustomobject]@{ pixelSha256 = 'shared-pixels' } },
		[pscustomobject]@{ id = 'ingame'; view = 'InGameCamera'; sha256 = 'shared-file'; visualExpectation = $validatedScenario.captures[2].visualExpectation; visualStatistics = [pscustomobject]@{ pixelSha256 = 'shared-pixels' } }
	)
	$identityValidation = Get-CameraEvidenceValidation -Scenario $validatedScenario -CameraInventory $cameraInventory
	Assert-TestCondition -Condition ([bool]$identityValidation.passed) -Message 'Defensibly declared Game/in-game parity exception was rejected.'
	$cameraInventory[0].sha256 = 'shared-file'
	$cameraInventory[0].visualStatistics.pixelSha256 = 'shared-pixels'
	$undeclaredIdentityRejected = $false
	try { [void](Get-CameraEvidenceValidation -Scenario $validatedScenario -CameraInventory $cameraInventory) } catch { $undeclaredIdentityRejected = $_.Exception.Message -match 'identical required visual evidence' }
	Assert-TestCondition -Condition $undeclaredIdentityRejected -Message 'Undeclared identical evidence across distinct view types did not fail closed.'

	[ordered]@{
		schemaVersion = 1
		passed = $true
		fixtures = @('solid-black-rejected', 'uniform-nonblack-rejected', 'nonuniform-content-accepted', 'explicit-empty-allowed', 'required-empty-array-preserved', 'expectation-without-required-fields', 'derived-multi-event-minimum', 'explicit-event-minimum-preserved', 'zero-event-minimum-rejected', 'fractional-event-minimum-rejected', 'oversized-event-minimum-rejected', 'missing-expectation-rejected', 'required-evidence-empty-rejected', 'required-evidence-traversal-rejected', 'duplicate-json-key-rejected', 'argument-binding-primitives-resolved', 'argument-binding-runtime-types-rejected', 'argument-binding-missing-success-registry-rejected', 'argument-binding-strict-schema-limits', 'argument-binding-path-source-destination-contracts', 'argument-binding-schema2-evidence-accepted', 'argument-binding-evidence-order-rejected', 'argument-binding-transcript-order-rejected', 'argument-binding-provenance-tamper-rejected', 'argument-binding-artifact-provenance-tamper-rejected', 'argument-binding-resolved-argument-tamper-rejected', 'argument-binding-transcript-request-tamper-rejected', 'argument-binding-source-envelope-tamper-rejected', 'approval-static-contracts-rejected', 'approval-schema2-evidence-accepted', 'approval-uint64-generation-accepted', 'approval-wrong-initial-rejected', 'approval-pending-tamper-rejected', 'approval-dispatch-tamper-rejected', 'approval-denial-rejected', 'approval-replay-rejected', 'recording-output-paired-sidecar-accepted', 'play-mode-recording-evidence-accepted', 'play-mode-before-after-summary-tamper-rejected', 'post-lock-output-tamper-rejected', 'authored-state-stability-bounded', 'fixture-overlay-copy-hash-exact', 'fixture-overlay-late-failure-rollback', 'fixture-overlay-empty-array-accepted', 'fixture-overlay-nonarray-rejected', 'fixture-overlay-count-limit-rejected', 'fixture-overlay-exact-keys-enforced', 'fixture-overlay-unsafe-paths-rejected', 'fixture-overlay-missing-empty-rejected', 'fixture-overlay-overlap-rejected', 'fixture-overlay-size-limits-rejected', 'fixture-overlay-source-reparse-rejected', 'fixture-overlay-destination-reparse-rejected', 'fixture-overlay-base-conflict-atomic', 'typed-operation-evidence-accepted', 'empty-operation-evidence-rejected', 'false-reconnect-boundary-rejected', 'verified-layout-chain-accepted', 'installed-layout-mismatch-rejected', 'missing-installed-layout-rejected', 'real-printwindow-evidence-accepted', 'pmv2-capture-context-runtime-verified', 'pmv2-context-restored-success-failure', 'pmv2-physical-client-click-oracle', 'pmv2-exclusive-client-bound-rejected', 'pmv2-receipt-provenance-enforced', 'non-pmv2-capture-metadata-rejected', 'input-desktop-classified', 'real-sendinput-or-explicit-lock-rejection', 'thread-input-attachment-balanced', 'wrong-owner-pid-rejected', 'wrong-window-handle-rejected', 'blank-title-rejected', 'non-printwindow-rejected', 'dpi-unaware-rejected', 'bounds-mismatch-rejected', 'stale-action-rejected', 'unbound-capture-time-rejected', 'full-size-black-capture-rejected', 'pixel-identical-reencoding-rejected', 'caller-evidence-rejected', 'multi-action-under-receipt-rejected', 'swapped-action-order-rejected', 'string-boolean-rejected', 'duplicate-manual-id-rejected', 'job-object-termination-verified', 'forced-http-reconnect-sequence-verified', 'bounded-cleanup-markers-verified', 'empty-acceptance-update-rejected', 'no-accepted-rows-rejected', 'unsupported-acceptance-row-rejected', 'acceptance-prepare-is-nonmutating', 'acceptance-stale-cas-rejected', 'acceptance-commit-and-rollback-hash-exact', 'acceptance-receipt-provenance-explicit', 'bare-AnimationValidationRunsEveryAccumulatedFeatureWorkflowBenchmarkTRunsEveryUnrelatedCriterionRejected', 'declared-parity-accepted', 'undeclared-cross-view-identity-rejected', 'preplay-before-manual-evidence', 'all-scenario-contracts-validated')
		installedScenarioCount = $installedScenarioCount
		nativeInput = [ordered]@{
			inputAvailable = [bool]$inputCapability.inputAvailable
			initialThreadDpiAwarenessContext = [string]$initialThreadDpiContext
			finalThreadDpiAwarenessContext = [TerraEngine.Animation.Validation.NativeWindowEvidence]::GetCurrentThreadDpiAwarenessContextName()
			clientGeometry = [ordered]@{
				dpiAware = [bool]$inputClientSnapshot.DpiAwareGeometry
				dpiAwarenessContext = [string]$inputClientSnapshot.DpiAwarenessContext
				dpi = [int]$inputClientSnapshot.Dpi
				pixelWidth = [int]$inputClientSnapshot.PixelWidth
				pixelHeight = [int]$inputClientSnapshot.PixelHeight
				nonDefaultDpi = [int]$inputClientSnapshot.Dpi -ne 96
				physicalScaleX = [Math]::Round($inputPhysicalScaleX, 4)
				physicalScaleY = [Math]::Round($inputPhysicalScaleY, 4)
				nonDefaultPhysicalScale = [bool]$inputUsesNonDefaultPhysicalScale
				clickEffectVerified = [bool]$dpiClickEffectVerified
			}
			mouse = [ordered]@{
				succeeded = [bool]$nativeMouseInputReceipt.Succeeded
				failureReason = [string]$nativeMouseInputReceipt.FailureReason
				dpiAwareGeometry = [bool]$nativeMouseInputReceipt.DpiAwareGeometry
				dpiAwarenessContext = [string]$nativeMouseInputReceipt.DpiAwarenessContext
				dpi = [int]$nativeMouseInputReceipt.Dpi
				requestedClientX = [int]$nativeMouseInputReceipt.RequestedClientX
				requestedClientY = [int]$nativeMouseInputReceipt.RequestedClientY
				screenX = [int]$nativeMouseInputReceipt.ScreenX
				screenY = [int]$nativeMouseInputReceipt.ScreenY
				physicalCursorPositionVerified = [bool]$nativeMouseInputReceipt.PhysicalCursorPositionVerified
				foregroundAcquired = [bool]$nativeMouseInputReceipt.ForegroundAcquired
				foregroundAcquiredBeforeInput = [bool]$nativeMouseInputReceipt.ForegroundAcquiredBeforeInput
				foregroundAcquiredByDeclaredMouseInput = [bool]$nativeMouseInputReceipt.ForegroundAcquiredByDeclaredMouseInput
				foregroundAttempts = [int]$nativeMouseInputReceipt.ForegroundAttempts
				postInputForegroundChecks = [int]$nativeMouseInputReceipt.PostInputForegroundChecks
				foregroundWindow = ('0x{0:x}' -f [UInt64]$nativeMouseInputReceipt.ForegroundWindow)
				threadInputAttachCount = [int]$nativeMouseInputReceipt.ThreadInputAttachCount
				threadInputDetachCount = [int]$nativeMouseInputReceipt.ThreadInputDetachCount
				threadInputDetached = [bool]$nativeMouseInputReceipt.ThreadInputDetached
				eventsRequested = [int]$nativeMouseInputReceipt.EventCountRequested
				eventsSubmitted = [int]$nativeMouseInputReceipt.EventCountSubmitted
				windowsError = [int]$nativeMouseInputReceipt.WindowsError
			}
			keyboard = if ($null -eq $nativeKeyboardInputReceipt) { $null } else { [ordered]@{
				succeeded = [bool]$nativeKeyboardInputReceipt.Succeeded
				failureReason = [string]$nativeKeyboardInputReceipt.FailureReason
				foregroundAcquiredBeforeInput = [bool]$nativeKeyboardInputReceipt.ForegroundAcquiredBeforeInput
				foregroundAcquiredByDeclaredMouseInput = [bool]$nativeKeyboardInputReceipt.ForegroundAcquiredByDeclaredMouseInput
				threadInputAttachCount = [int]$nativeKeyboardInputReceipt.ThreadInputAttachCount
				threadInputDetachCount = [int]$nativeKeyboardInputReceipt.ThreadInputDetachCount
				threadInputDetached = [bool]$nativeKeyboardInputReceipt.ThreadInputDetached
				eventsRequested = [int]$nativeKeyboardInputReceipt.EventCountRequested
				eventsSubmitted = [int]$nativeKeyboardInputReceipt.EventCountSubmitted
				windowsError = [int]$nativeKeyboardInputReceipt.WindowsError
			} }
		}
		productionFunctionsTested = $requiredFunctions
	} | ConvertTo-Json -Depth 10 -Compress | Write-Output
}
finally {
	if ($null -ne $inputTestJob) { $inputTestJob.Dispose() }
	if ($null -ne $inputTestProcess) {
		try {
			if (-not $inputTestProcess.HasExited) { $inputTestProcess.Kill() }
			[void]$inputTestProcess.WaitForExit(5000)
		}
		catch { }
		$inputTestProcess.Dispose()
	}
	if ($null -ne $jobTestObject) { $jobTestObject.Dispose() }
	if ($null -ne $jobTestProcess) {
		try {
			if (-not $jobTestProcess.HasExited) { $jobTestProcess.Kill() }
			[void]$jobTestProcess.WaitForExit(5000)
		}
		catch { }
		$jobTestProcess.Dispose()
	}
	if ($null -ne $stabilityTestProcess) {
		try {
			if (-not $stabilityTestProcess.HasExited) { $stabilityTestProcess.Kill() }
			[void]$stabilityTestProcess.WaitForExit(5000)
		}
		catch { }
		$stabilityTestProcess.Dispose()
	}
	if ($null -ne $evidenceForm) {
		$evidenceForm.Close()
		$evidenceForm.Dispose()
	}
	# Remove an explicitly created junction before recursively deleting its parent
	# test tree, so cleanup can never follow a reparse target.
	if ($null -ne $overlayJunctionPath -and (Test-Path -LiteralPath $overlayJunctionPath)) {
		[System.IO.Directory]::Delete($overlayJunctionPath)
	}
	# The exact GUID directory was validated above; delete only that isolated
	# self-test tree and never recurse over a caller-provided or broad path.
	if (Test-Path -LiteralPath $temporaryRoot) { Remove-Item -LiteralPath $temporaryRoot -Recurse -Force }
}
