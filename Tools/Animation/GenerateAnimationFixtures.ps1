# Generates TerraEngine's small, source-independent animation fixture corpus.
# The generator is intentionally offline and uses only repository-authored data.
[CmdletBinding()]
param(
	# An explicit root prevents a typo from silently writing fixtures beside source
	# files. The directory must be new or empty and must remain inside this repo.
	[Parameter(Mandatory = $true)]
	[ValidateNotNullOrEmpty()]
	[string]$OutputRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.."))
$script:GeneratorVersion = 1
$script:MaximumFileCount = 128
$script:MaximumFileBytes = 256 * 1024
$script:MaximumTotalBytes = 2 * 1024 * 1024
$script:Utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)
$script:GeneratedFiles = [System.Collections.Generic.List[object]]::new()

function Get-AbsolutePath {
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[Parameter(Mandatory = $true)][string]$BasePath
	)

	if ([System.IO.Path]::IsPathRooted($Path)) {
		return [System.IO.Path]::GetFullPath($Path)
	}
	return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function Assert-StrictChildPath {
	param(
		[Parameter(Mandatory = $true)][string]$Parent,
		[Parameter(Mandatory = $true)][string]$Child,
		[Parameter(Mandatory = $true)][string]$Description
	)

	$parentPath = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
	$childPath = [System.IO.Path]::GetFullPath($Child)
	$prefix = $parentPath + [System.IO.Path]::DirectorySeparatorChar
	if (-not $childPath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
		throw "$Description '$childPath' must be strictly below '$parentPath'."
	}
}

function ConvertTo-StableJson {
	param([Parameter(Mandatory = $true)]$InputObject)

	# Ordered dictionaries and explicit arrays make property and item order stable.
	# Normalizing newlines keeps bytes identical between Windows PowerShell and
	# PowerShell on other supported hosts.
	$json = ConvertTo-Json -InputObject $InputObject -Depth 100
	$normalized = $json.Replace("`r`n", "`n").Replace("`r", "`n").TrimEnd("`n")
	return $normalized + "`n"
}

function Get-Sha256Text {
	param([Parameter(Mandatory = $true)][string]$Text)

	$algorithm = [System.Security.Cryptography.SHA256]::Create()
	try {
		$bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
		$hash = $algorithm.ComputeHash($bytes)
		return ([System.BitConverter]::ToString($hash) -replace '-', '').ToLowerInvariant()
	}
	finally {
		$algorithm.Dispose()
	}
}

function Resolve-GeneratedPath {
	param(
		[Parameter(Mandatory = $true)][string]$StagingRoot,
		[Parameter(Mandatory = $true)][string]$RelativePath
	)

	if ([System.IO.Path]::IsPathRooted($RelativePath)) {
		throw "Generated fixture paths must be relative: '$RelativePath'."
	}
	$segments = @($RelativePath -split '[\\/]')
	if ($segments.Count -eq 0 -or $segments -contains ".." -or $segments -contains "") {
		throw "Generated fixture path is not normalized: '$RelativePath'."
	}
	$path = [System.IO.Path]::GetFullPath((Join-Path $StagingRoot $RelativePath))
	Assert-StrictChildPath -Parent $StagingRoot -Child $path -Description "Generated fixture"
	return $path
}

function Write-FixtureDocument {
	param(
		[Parameter(Mandatory = $true)][string]$StagingRoot,
		[Parameter(Mandatory = $true)][string]$RelativePath,
		[Parameter(Mandatory = $true)]$Document,
		[Parameter(Mandatory = $true)][string]$FixtureId,
		[Parameter(Mandatory = $true)][string]$Role,
		[Parameter(Mandatory = $true)][string[]]$Requirements
	)

	$normalizedRelativePath = $RelativePath -replace '\\', '/'
	$path = Resolve-GeneratedPath -StagingRoot $StagingRoot -RelativePath $normalizedRelativePath
	$directory = Split-Path -Parent $path
	New-Item -ItemType Directory -Force -Path $directory | Out-Null
	$content = ConvertTo-StableJson -InputObject $Document
	[System.IO.File]::WriteAllText($path, $content, $script:Utf8WithoutBom)
	$file = Get-Item -LiteralPath $path
	if ($file.Length -gt $script:MaximumFileBytes) {
		throw "Fixture '$normalizedRelativePath' is $($file.Length) bytes; the per-file bound is $script:MaximumFileBytes bytes."
	}
	$script:GeneratedFiles.Add([pscustomobject]@{
		Path = $normalizedRelativePath
		FixtureId = $FixtureId
		Role = $Role
		Requirements = @($Requirements)
	})
}

function New-Dependency {
	param(
		[Parameter(Mandatory = $true)][string]$AssetId,
		[Parameter(Mandatory = $true)][string]$AssetType,
		[Parameter(Mandatory = $true)][string]$CompatibilityFingerprint
	)

	return [ordered]@{
		assetId = $AssetId
		assetType = $AssetType
		generation = 1
		compatibilityFingerprint = $CompatibilityFingerprint
	}
}

function New-AssetFixture {
	param(
		[Parameter(Mandatory = $true)][string]$FixtureId,
		[Parameter(Mandatory = $true)][string]$AssetType,
		[Parameter(Mandatory = $true)][string]$IntendedExtension,
		[Parameter(Mandatory = $true)][string]$AssetId,
		[Parameter(Mandatory = $true)][string]$CompatibilityFingerprint,
		[object[]]$Dependencies = @(),
		[Parameter(Mandatory = $true)]$Payload
	)

	return [ordered]@{
		schema = "terra.animation.source-independent-asset-fixture/v1"
		fixtureId = $FixtureId
		expectedValidity = "valid"
		sourceIndependent = $true
		formatBoundary = [ordered]@{
			intendedExtension = $IntendedExtension
			representation = "reviewed-semantic-json"
			binaryLayoutAssumed = $false
			conversionRule = "Pass this semantic payload through the production asset writer after its envelope and chunk layout are frozen."
		}
		identity = [ordered]@{
			assetId = $AssetId
			assetType = $AssetType
			generation = 1
			compatibilityFingerprint = $CompatibilityFingerprint
		}
		dependencies = @($Dependencies)
		payload = $Payload
	}
}

function New-ExpectedRejection {
	param(
		[Parameter(Mandatory = $true)][string]$DiagnosticCode,
		[string]$SafeFallback = "last-known-good-or-bind-pose"
	)

	return [ordered]@{
		status = "Rejected"
		diagnosticCode = $DiagnosticCode
		atomic = $true
		partialPublication = $false
		priorGenerationPreserved = $true
		safeFallback = $SafeFallback
	}
}

function New-SeedFixture {
	param(
		[Parameter(Mandatory = $true)][string]$FixtureId,
		[Parameter(Mandatory = $true)][string]$Category,
		[Parameter(Mandatory = $true)][string]$BaseFixture,
		[Parameter(Mandatory = $true)][string]$ApplicationStage,
		[Parameter(Mandatory = $true)][object[]]$Operations,
		[Parameter(Mandatory = $true)]$Expected,
		[string]$OperationMode = "compose-in-order"
	)

	return [ordered]@{
		schema = "terra.animation.corruption-seed/v1"
		fixtureId = $FixtureId
		category = $Category
		baseFixture = $BaseFixture
		applicationStage = $ApplicationStage
		operationMode = $OperationMode
		operations = @($Operations)
		expected = $Expected
		conversionBoundary = [ordered]@{
			layoutIndependent = $true
			instruction = "Resolve symbolic operations through the production serializer/test mutator; this seed never defines byte offsets or production magic values."
		}
	}
}

function New-HardLimitOverflowOperations {
	param([Parameter(Mandatory = $true)][object[]]$Limits)

	foreach ($limit in $Limits) {
		[ordered]@{
			operator = "set-semantic-count"
			field = $limit.field
			unit = $limit.unit
			value = [long]$limit.hard + 1
			hardLimit = [long]$limit.hard
		}
	}
}

$resolvedOutputRoot = Get-AbsolutePath -Path $OutputRoot -BasePath $script:RepositoryRoot
Assert-StrictChildPath -Parent $script:RepositoryRoot -Child $resolvedOutputRoot -Description "Output root"
if ($resolvedOutputRoot -eq [System.IO.Path]::GetPathRoot($resolvedOutputRoot)) {
	throw "A drive or filesystem root cannot be used as the fixture output root."
}
if (Test-Path -LiteralPath $resolvedOutputRoot) {
	if (-not (Test-Path -LiteralPath $resolvedOutputRoot -PathType Container)) {
		throw "Output root exists but is not a directory: '$resolvedOutputRoot'."
	}
	if (@(Get-ChildItem -LiteralPath $resolvedOutputRoot -Force).Count -ne 0) {
		throw "Output root must be new or empty; existing content is never overwritten: '$resolvedOutputRoot'."
	}
}
else {
	New-Item -ItemType Directory -Path $resolvedOutputRoot | Out-Null
}

$stagingRoot = Join-Path $resolvedOutputRoot ".fixture-generation-staging"
Assert-StrictChildPath -Parent $resolvedOutputRoot -Child $stagingRoot -Description "Fixture staging root"
New-Item -ItemType Directory -Path $stagingRoot | Out-Null

# These fixed identities make every generated byte independent of the output
# path, clock, machine, process, locale, and source-control revision.
$sourceSkeletonId = "11111111-1111-4111-8111-111111111111"
$targetSkeletonId = "22222222-2222-4222-8222-222222222222"
$clipId = "33333333-3333-4333-8333-333333333333"
$controllerId = "44444444-4444-4444-8444-444444444444"
$maskId = "55555555-5555-4555-8555-555555555555"
$retargetId = "66666666-6666-4666-8666-666666666666"
$motionDatabaseId = "77777777-7777-4777-8777-777777777777"
$ragdollId = "88888888-8888-4888-8888-888888888888"
$recordingId = "99999999-9999-4999-8999-999999999999"
$meshId = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
$sourceSkeletonFingerprint = "1111111111111111111111111111111111111111111111111111111111111111"
$targetSkeletonFingerprint = "2222222222222222222222222222222222222222222222222222222222222222"
$runtimeSetFingerprint = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

$hardLimits = @(
	[ordered]@{ field = "jointsPerSkeleton"; unit = "count"; operational = 256; hard = 4096 },
	[ordered]@{ field = "jointsPerPalettePartition"; unit = "count"; operational = 256; hard = 256 },
	[ordered]@{ field = "retainedInfluencesPerCookedVertex"; unit = "count"; operational = 4; hard = 4 },
	[ordered]@{ field = "skinsOrSkeletonsPerSourcePackage"; unit = "count"; operational = 64; hard = 1024 },
	[ordered]@{ field = "clipsPerSourcePackage"; unit = "count"; operational = 256; hard = 4096 },
	[ordered]@{ field = "clipDuration"; unit = "seconds"; operational = 1800; hard = 86400 },
	[ordered]@{ field = "channelsPerClip"; unit = "count"; operational = 16384; hard = 65536 },
	[ordered]@{ field = "keysPerClip"; unit = "count"; operational = 4194304; hard = 16777216 },
	[ordered]@{ field = "morphTargetsPerMesh"; unit = "count"; operational = 256; hard = 4096 },
	[ordered]@{ field = "controllerNodes"; unit = "count"; operational = 8192; hard = 65536 },
	[ordered]@{ field = "stateMachineNestingDepth"; unit = "levels"; operational = 16; hard = 64 },
	[ordered]@{ field = "reusableSubgraphCallDepth"; unit = "levels"; operational = 8; hard = 32 },
	[ordered]@{ field = "orderedControllerLayers"; unit = "count"; operational = 16; hard = 32 },
	[ordered]@{ field = "controllerParameters"; unit = "count"; operational = 1024; hard = 4096 },
	[ordered]@{ field = "transitionsPerState"; unit = "count"; operational = 256; hard = 4096 },
	[ordered]@{ field = "eventsAuthoredPerClip"; unit = "count"; operational = 65536; hard = 1048576 },
	[ordered]@{ field = "eventsEmittedPerAnimatorUpdate"; unit = "count"; operational = 1024; hard = 4096 },
	[ordered]@{ field = "retainedEventRecordsPerAnimator"; unit = "count"; operational = 2048; hard = 8192 },
	[ordered]@{ field = "proceduralGoalsPerAnimator"; unit = "count"; operational = 64; hard = 256 },
	[ordered]@{ field = "chainIkIterations"; unit = "iterations"; operational = 32; hard = 128 },
	[ordered]@{ field = "motionDatabaseSamples"; unit = "count"; operational = 1000000; hard = 16777216 },
	[ordered]@{ field = "motionQueryTopKResults"; unit = "count"; operational = 32; hard = 256 },
	[ordered]@{ field = "recordingDuration"; unit = "seconds"; operational = 1800; hard = 14400 },
	[ordered]@{ field = "recordingSampleRate"; unit = "hertz"; operational = 60; hard = 240 },
	[ordered]@{ field = "rollbackHistory"; unit = "ticks"; operational = 240; hard = 4096 },
	[ordered]@{ field = "detailedDiagnosticHistory"; unit = "frames"; operational = 600; hard = 36000 },
	[ordered]@{ field = "concurrentDurableAutomationJobsPerSession"; unit = "count"; operational = 16; hard = 64 }
)

try {
	$limitContract = [ordered]@{
		schema = "terra.animation.runtime-limits/v1"
		source = "Docs/Animation/README.md"
		arithmetic = [ordered]@{
			serializedRangeWidthBits = 64
			checkedBefore = @("allocation", "multiplication", "decompression", "gpu-upload", "graph-compilation", "publication")
		}
		limits = $hardLimits
		overflowPolicy = [ordered]@{
			status = "Rejected"
			partialPublication = $false
			priorGenerationPreserved = $true
		}
	}
	Write-FixtureDocument -StagingRoot $stagingRoot -RelativePath "contracts/runtime-limits-v1.json" -Document $limitContract -FixtureId "runtime-limits-v1" -Role "frozen-contract" -Requirements @("animation-contract-validate-skeleton-topology-transform-validity-bind-data-influence-data-channel-targets-timestamps", "animation-contract-reject-content-exceeds-hard-limits-safe-deterministic-reduction-unavailable", "animation-contract-streaming-residency-retained-histories-have-explicit-memory-time-bounds-pressure-behavior-diagnostics", "animation-contract-feature-include-reproducible-correctness-corruption-rollback-scheduler-transaction-multithreaded-stress-soak-performance")

	$toleranceContract = [ordered]@{
		schema = "terra.animation.validation-tolerances/v1"
		source = "Docs/Animation/README.md"
		exact = @("state", "transition-choice", "event-identity-and-order", "root-motion-interval-identity", "motion-match-selection", "command-status", "generation-identity", "deterministic-hash")
		maximumDifference = [ordered]@{
			gpuJointTranslationMillimetres = 0.5
			gpuJointRotationDegrees = 0.1
			gpuScaleComponentPercent = 0.05
			morphWeightAbsolute = 0.0005
			rootMotionTranslationMillimetres = 0.5
			rootMotionRotationDegrees = 0.1
			retargetContactMillimetres = 20
			reachableIkEndEffectorMillimetres = 10
			deformationBoundsRule = "never-underestimate"
		}
	}
	Write-FixtureDocument -StagingRoot $stagingRoot -RelativePath "contracts/validation-tolerances-v1.json" -Document $toleranceContract -FixtureId "validation-tolerances-v1" -Role "frozen-contract" -Requirements @("animation-outcome-retarget-fixtures-preserve-required-contacts-facing-timing-root-displacement-within-documented-per", "animation-outcome-reachable-ik-fixtures-meet-declared-end-effector-tolerance-unreachable-singular-fixtures-remain", "animation-outcome-installed-editor-exported-runtime-fixtures-produce-same-discrete-animation-states-events-root", "animation-outcome-gpu-visual-hybrid-authoritative-poses-match-cpu-reference-poses-within-declared-visual")

	$sourceSkeleton = New-AssetFixture -FixtureId "minimal-source-skeleton" -AssetType "Skeleton" -IntendedExtension ".teskel" -AssetId $sourceSkeletonId -CompatibilityFingerprint $sourceSkeletonFingerprint -Dependencies @() -Payload ([ordered]@{
		formatVersion = [ordered]@{ major = 1; minor = 0 }
		rootJoint = 0
		joints = @([ordered]@{
			stableJointId = "root"
			displayName = "Root"
			normalizedPath = "/root"
			parentIndex = -1
			localBindTransform = [ordered]@{ translation = @(0, 0, 0); rotation = @(0, 0, 0, 1); scale = @(1, 1, 1) }
			semantic = "Root"
			deformsMesh = $true
		})
		inverseBindMatrices = @(@(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1))
		semanticMap = [ordered]@{ Root = 0 }
		referencePoseHash = "0000000000000001"
		sourceMetadata = [ordered]@{ coordinateConvention = "TerraEngineProject"; units = "metres"; sourcePathRequired = $false }
	})
	Write-FixtureDocument -StagingRoot $stagingRoot -RelativePath "valid/minimal-source-skeleton.fixture.json" -Document $sourceSkeleton -FixtureId "minimal-source-skeleton" -Role "valid-asset" -Requirements @("animation-contract-represent-skeletons-animation-clips-animation-controllers-masks-retarget-mappings-motion-databases-ragdoll", "animation-contract-validate-skeleton-topology-transform-validity-bind-data-influence-data-channel-targets-timestamps", "animation-contract-generated-runtime-assets-require-source-gltf-fbx-dcc-editor-files-after-cooking", "animation-contract-distinguish-semantic-joint-identity-display-names-so-duplicate-renamed-display-names-do")

	$targetSkeleton = New-AssetFixture -FixtureId "minimal-target-skeleton" -AssetType "Skeleton" -IntendedExtension ".teskel" -AssetId $targetSkeletonId -CompatibilityFingerprint $targetSkeletonFingerprint -Dependencies @() -Payload ([ordered]@{
		formatVersion = [ordered]@{ major = 1; minor = 0 }
		rootJoint = 0
		joints = @([ordered]@{
			stableJointId = "target-root"
			displayName = "Target Root"
			normalizedPath = "/target-root"
			parentIndex = -1
			localBindTransform = [ordered]@{ translation = @(0, 0, 0); rotation = @(0, 0, 0, 1); scale = @(1, 1, 1) }
			semantic = "Root"
			deformsMesh = $true
		})
		inverseBindMatrices = @(@(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1))
		semanticMap = [ordered]@{ Root = 0 }
		referencePoseHash = "0000000000000002"
		sourceMetadata = [ordered]@{ coordinateConvention = "TerraEngineProject"; units = "metres"; sourcePathRequired = $false }
	})
	Write-FixtureDocument -StagingRoot $stagingRoot -RelativePath "valid/minimal-target-skeleton.fixture.json" -Document $targetSkeleton -FixtureId "minimal-target-skeleton" -Role "valid-asset" -Requirements @("animation-contract-represent-skeletons-animation-clips-animation-controllers-masks-retarget-mappings-motion-databases-ragdoll", "animation-contract-validate-skeleton-topology-transform-validity-bind-data-influence-data-channel-targets-timestamps", "animation-contract-generated-runtime-assets-require-source-gltf-fbx-dcc-editor-files-after-cooking", "animation-contract-distinguish-semantic-joint-identity-display-names-so-duplicate-renamed-display-names-do")

	$clip = New-AssetFixture -FixtureId "minimal-clip" -AssetType "AnimationClip" -IntendedExtension ".teanim" -AssetId $clipId -CompatibilityFingerprint $sourceSkeletonFingerprint -Dependencies @(
		(New-Dependency -AssetId $sourceSkeletonId -AssetType "Skeleton" -CompatibilityFingerprint $sourceSkeletonFingerprint)
	) -Payload ([ordered]@{
		formatVersion = [ordered]@{ major = 1; minor = 0 }
		durationTicks = 60
		sampleRate = 60
		segments = @([ordered]@{ segmentId = 0; firstTick = 0; lastTick = 60; independentlySeekable = $true })
		jointTracks = @([ordered]@{
			jointId = "root"
			translation = [ordered]@{ interpolation = "Linear"; keys = @([ordered]@{ tick = 0; value = @(0, 0, 0) }, [ordered]@{ tick = 60; value = @(1, 0, 0) }) }
			rotation = [ordered]@{ interpolation = "Constant"; value = @(0, 0, 0, 1) }
			scale = [ordered]@{ interpolation = "DefaultReferencePose" }
		})
		morphTracks = @()
		curveTracks = @([ordered]@{ name = "speed"; valueType = "Float"; keys = @([ordered]@{ tick = 0; value = 0.0 }, [ordered]@{ tick = 60; value = 1.0 }) })
		events = @([ordered]@{ eventId = "halfway"; tick = 30; payloadType = "None"; directionPolicy = "Both"; loopPolicy = "PerLoop" })
		rootMotion = [ordered]@{ jointId = "root"; extractTranslation = $true; extractYaw = $true; accumulatedCycleTranslation = @(1, 0, 0); accumulatedCycleYawDegrees = 0 }
		compression = [ordered]@{ representation = "semantic-source-for-production-codec"; decoderVersion = 1; measuredErrorsRequired = $true }
	})
	Write-FixtureDocument -StagingRoot $stagingRoot -RelativePath "valid/minimal-clip.fixture.json" -Document $clip -FixtureId "minimal-clip" -Role "valid-asset" -Requirements @("animation-contract-represent-skeletons-animation-clips-animation-controllers-masks-retarget-mappings-motion-databases-ragdoll", "animation-contract-generated-runtime-assets-require-source-gltf-fbx-dcc-editor-files-after-cooking", "animation-contract-evaluate-translation-rotation-scale-morph-weight-root-motion-event-custom-curve-channels", "animation-contract-support-step-linear-smooth-cubic-interpolation-finite-normalized-continuity-preserving-rotation-results", "animation-contract-support-deterministic-ordered-animation-events-stable-identifier-time-payload-source-state-layer")

	$controller = New-AssetFixture -FixtureId "minimal-controller" -AssetType "AnimationController" -IntendedExtension ".teanimctrl" -AssetId $controllerId -CompatibilityFingerprint $sourceSkeletonFingerprint -Dependencies @(
		(New-Dependency -AssetId $sourceSkeletonId -AssetType "Skeleton" -CompatibilityFingerprint $sourceSkeletonFingerprint),
		(New-Dependency -AssetId $clipId -AssetType "AnimationClip" -CompatibilityFingerprint $sourceSkeletonFingerprint)
	) -Payload ([ordered]@{
		formatVersion = [ordered]@{ major = 1; minor = 0 }
		parameters = @()
		states = @([ordered]@{ stateId = "idle"; displayName = "Idle"; clipAssetId = $clipId; isDefault = $true })
		transitions = @()
		layers = @([ordered]@{ layerId = "base"; weight = 1.0; blendMode = "Override"; defaultStateId = "idle" })
		syncGroups = @()
		compiledEvaluationProgram = [ordered]@{
			representation = "semantic-operations-v1"
			binaryOpcodesAssumed = $false
			operations = @([ordered]@{ operation = "SampleClip"; stateId = "idle" }, [ordered]@{ operation = "PublishPose" })
			programHash = "0000000000000001"
		}
	})
	Write-FixtureDocument -StagingRoot $stagingRoot -RelativePath "valid/minimal-controller.fixture.json" -Document $controller -FixtureId "minimal-controller" -Role "valid-asset" -Requirements @("animation-contract-represent-skeletons-animation-clips-animation-controllers-masks-retarget-mappings-motion-databases-ragdoll", "animation-contract-preserve-compatibility-identity-prevents-clip-controller-retarget-map-ragdoll-motion-database-binding", "animation-contract-support-typed-float-integer-boolean-trigger-vector-transform-tag-parameters-declared-defaults", "animation-contract-support-graph-variables-reusable-parameter-bindings-without-allowing-unbounded-recursive-evaluation")

	$mask = New-AssetFixture -FixtureId "minimal-mask" -AssetType "AnimationMask" -IntendedExtension ".teanimmask" -AssetId $maskId -CompatibilityFingerprint $sourceSkeletonFingerprint -Dependencies @(
		(New-Dependency -AssetId $sourceSkeletonId -AssetType "Skeleton" -CompatibilityFingerprint $sourceSkeletonFingerprint)
	) -Payload ([ordered]@{
		formatVersion = [ordered]@{ major = 1; minor = 0 }
		selectors = @([ordered]@{ selectorType = "StableJointId"; value = "root"; includeDescendants = $true; weight = 1.0 })
		curveNames = @("speed")
		morphTargetIds = @()
		compiledJointWeights = @(1.0)
	})
	Write-FixtureDocument -StagingRoot $stagingRoot -RelativePath "valid/minimal-mask.fixture.json" -Document $mask -FixtureId "minimal-mask" -Role "valid-asset" -Requirements @("animation-contract-represent-skeletons-animation-clips-animation-controllers-masks-retarget-mappings-motion-databases-ragdoll", "animation-contract-preserve-compatibility-identity-prevents-clip-controller-retarget-map-ragdoll-motion-database-binding", "animation-contract-support-multiple-ordered-layers-override-additive-composition", "animation-contract-layers-support-reusable-masks-over-joints-hierarchy-branches-morph-channels-curves-root")

	$retarget = New-AssetFixture -FixtureId "minimal-retarget-profile" -AssetType "RetargetProfile" -IntendedExtension ".teretarget" -AssetId $retargetId -CompatibilityFingerprint $runtimeSetFingerprint -Dependencies @(
		(New-Dependency -AssetId $sourceSkeletonId -AssetType "Skeleton" -CompatibilityFingerprint $sourceSkeletonFingerprint),
		(New-Dependency -AssetId $targetSkeletonId -AssetType "Skeleton" -CompatibilityFingerprint $targetSkeletonFingerprint)
	) -Payload ([ordered]@{
		formatVersion = [ordered]@{ major = 1; minor = 0 }
		sourceSkeletonFingerprint = $sourceSkeletonFingerprint
		targetSkeletonFingerprint = $targetSkeletonFingerprint
		mappings = @([ordered]@{ semantic = "Root"; sourceJointId = "root"; targetJointId = "target-root"; required = $true })
		translationScalePolicy = "UniformRootScale"
		rootTrajectoryPolicy = "PreserveDirectionAndNormalizedDisplacement"
		chains = @()
		validation = [ordered]@{ expectedContactToleranceMillimetres = 20; reducedFidelity = $false }
	})
	Write-FixtureDocument -StagingRoot $stagingRoot -RelativePath "valid/minimal-retarget-profile.fixture.json" -Document $retarget -FixtureId "minimal-retarget-profile" -Role "valid-asset" -Requirements @("animation-contract-represent-skeletons-animation-clips-animation-controllers-masks-retarget-mappings-motion-databases-ragdoll", "animation-contract-preserve-compatibility-identity-prevents-clip-controller-retarget-map-ragdoll-motion-database-binding", "animation-contract-support-explicit-source-target-retarget-mappings-reusable-semantic-rig-profiles", "animation-outcome-retarget-fixtures-preserve-required-contacts-facing-timing-root-displacement-within-documented-per")

	$motionDatabase = New-AssetFixture -FixtureId "minimal-motion-database" -AssetType "MotionDatabase" -IntendedExtension ".temotiondb" -AssetId $motionDatabaseId -CompatibilityFingerprint $sourceSkeletonFingerprint -Dependencies @(
		(New-Dependency -AssetId $sourceSkeletonId -AssetType "Skeleton" -CompatibilityFingerprint $sourceSkeletonFingerprint),
		(New-Dependency -AssetId $clipId -AssetType "AnimationClip" -CompatibilityFingerprint $sourceSkeletonFingerprint)
	) -Payload ([ordered]@{
		formatVersion = [ordered]@{ major = 1; minor = 0 }
		featureSchema = @([ordered]@{ featureId = "root-speed-x"; quantization = "Signed16"; scale = 0.001 })
		samples = @([ordered]@{ sampleId = 0; clipAssetId = $clipId; tick = 0; quantizedFeatures = @(0); contactTags = @(); excluded = $false })
		continuityEdges = @()
		gpuLayout = [ordered]@{ blockSize = 1; featureStrideBytes = 2 }
		cpuReferenceLayout = [ordered]@{ ordering = "StableSampleId" }
		sourceHashes = @("3333333333333333333333333333333333333333333333333333333333333333")
	})
	Write-FixtureDocument -StagingRoot $stagingRoot -RelativePath "valid/minimal-motion-database.fixture.json" -Document $motionDatabase -FixtureId "minimal-motion-database" -Role "valid-asset" -Requirements @("animation-contract-represent-skeletons-animation-clips-animation-controllers-masks-retarget-mappings-motion-databases-ragdoll", "animation-contract-preserve-compatibility-identity-prevents-clip-controller-retarget-map-ragdoll-motion-database-binding", "animation-contract-build-versioned-motion-databases-selected-clips-retargeted-clips-tags-contacts-trajectory-samples", "animation-contract-motion-matching-support-rollback-replay-network-authority-retargeting-root-motion-ik-post")

	$ragdoll = New-AssetFixture -FixtureId "minimal-ragdoll-profile" -AssetType "RagdollProfile" -IntendedExtension ".teragdoll" -AssetId $ragdollId -CompatibilityFingerprint $sourceSkeletonFingerprint -Dependencies @(
		(New-Dependency -AssetId $sourceSkeletonId -AssetType "Skeleton" -CompatibilityFingerprint $sourceSkeletonFingerprint)
	) -Payload ([ordered]@{
		formatVersion = [ordered]@{ major = 1; minor = 0 }
		joltCookFingerprint = [ordered]@{ provider = "Jolt"; version = "5.6.1"; revision = "2e28006e4a367624bb512bf2c37ca99a93d9d1ea" }
		bodies = @([ordered]@{ bodyId = "root-body"; jointId = "root"; shape = [ordered]@{ type = "Sphere"; radiusMetres = 0.1 }; massKilograms = 1.0 })
		constraints = @()
		blendMask = @([ordered]@{ jointId = "root"; weight = 1.0 })
		getUp = [ordered]@{ enabled = $false }
	})
	Write-FixtureDocument -StagingRoot $stagingRoot -RelativePath "valid/minimal-ragdoll-profile.fixture.json" -Document $ragdoll -FixtureId "minimal-ragdoll-profile" -Role "valid-asset" -Requirements @("animation-contract-represent-skeletons-animation-clips-animation-controllers-masks-retarget-mappings-motion-databases-ragdoll", "animation-contract-preserve-compatibility-identity-prevents-clip-controller-retarget-map-ragdoll-motion-database-binding", "animation-contract-represent-ragdoll-body-collider-mass-constraint-motor-limit-collision-skeleton-mapping-reusable", "animation-contract-support-full-body-partial-ragdoll-per-joint-per-branch-layered-masked-time")

	$recording = New-AssetFixture -FixtureId "minimal-recording" -AssetType "AnimationRecording" -IntendedExtension ".teanim" -AssetId $recordingId -CompatibilityFingerprint $sourceSkeletonFingerprint -Dependencies @(
		(New-Dependency -AssetId $sourceSkeletonId -AssetType "Skeleton" -CompatibilityFingerprint $sourceSkeletonFingerprint),
		(New-Dependency -AssetId $controllerId -AssetType "AnimationController" -CompatibilityFingerprint $sourceSkeletonFingerprint)
	) -Payload ([ordered]@{
		formatVersion = [ordered]@{ major = 1; minor = 0 }
		provenance = [ordered]@{ animatorFixtureId = "minimal-runtime-set"; controllerAssetId = $controllerId; firstTick = 0; lastTick = 1; captureMode = "AuthoritativeSubset"; sourceSceneIdentity = "fixture-scene"; sourceSessionIdentity = "fixture-session" }
		capturedChannels = @("root.translation")
		sampleRate = 60
		samples = @([ordered]@{ tick = 0; rootTranslation = @(0, 0, 0) }, [ordered]@{ tick = 1; rootTranslation = @(0.016666667, 0, 0) })
		reduction = [ordered]@{ requested = $true; measuredErrorsRequired = $true }
	})
	Write-FixtureDocument -StagingRoot $stagingRoot -RelativePath "valid/minimal-recording.fixture.json" -Document $recording -FixtureId "minimal-recording" -Role "valid-asset" -Requirements @("animation-contract-represent-skeletons-animation-clips-animation-controllers-masks-retarget-mappings-motion-databases-ragdoll", "animation-contract-editor-provide-controller-authoring-states-nested-machines-blend-trees-layers-masks-synchronization", "animation-contract-animation-assets-component-data-participate-prefab-overrides-apply-revert-connected-duplication-stable", "animation-outcome-invalid-cancelled-import-graph-publication-recording-motion-database-build-retarget-edit-ragdoll")

	$mesh = New-AssetFixture -FixtureId "minimal-deformable-mesh" -AssetType "DeformableMesh" -IntendedExtension ".temf" -AssetId $meshId -CompatibilityFingerprint $sourceSkeletonFingerprint -Dependencies @(
		(New-Dependency -AssetId $sourceSkeletonId -AssetType "Skeleton" -CompatibilityFingerprint $sourceSkeletonFingerprint)
	) -Payload ([ordered]@{
		formatVersion = [ordered]@{ major = 1; minor = 0 }
		vertices = @(
			[ordered]@{ position = @(0, 0, 0); paletteIndices = @(0, 0, 0, 0); normalizedWeightsUnorm8 = @(255, 0, 0, 0) },
			[ordered]@{ position = @(1, 0, 0); paletteIndices = @(0, 0, 0, 0); normalizedWeightsUnorm8 = @(255, 0, 0, 0) },
			[ordered]@{ position = @(0, 1, 0); paletteIndices = @(0, 0, 0, 0); normalizedWeightsUnorm8 = @(255, 0, 0, 0) }
		)
		indices = @(0, 1, 2)
		palettePartitions = @([ordered]@{ partitionId = 0; jointIndices = @(0); triangleIndices = @(0) })
		morphTargets = @()
		deformationEnvelope = [ordered]@{ minimum = @(-0.1, -0.1, -0.1); maximum = @(1.1, 1.1, 0.1); conservative = $true }
		virtualGeometry = [ordered]@{ deformable = $true; completeVisibleFallback = $true; unsafeConeCullingDisabled = $true }
	})
	Write-FixtureDocument -StagingRoot $stagingRoot -RelativePath "valid/minimal-deformable-mesh.fixture.json" -Document $mesh -FixtureId "minimal-deformable-mesh" -Role "valid-asset" -Requirements @("animation-contract-reduce-excessive-vertex-influences-deterministically-according-documented-policy-normalize-retained-weights-report", "animation-contract-generated-runtime-assets-require-source-gltf-fbx-dcc-editor-files-after-cooking", "animation-contract-render-skeletal-deformation-correctly-positions-normals-tangents-supported-render-pass", "animation-contract-deformable-virtual-geometry-page-loss-late-residency-retain-complete-conservative-fallback-coverage")

	$runtimeSet = [ordered]@{
		schema = "terra.animation.source-independent-runtime-set/v1"
		fixtureId = "minimal-runtime-set"
		sourceIndependent = $true
		compatibilityFingerprint = $runtimeSetFingerprint
		conversionOrder = @(
			"valid/minimal-source-skeleton.fixture.json",
			"valid/minimal-target-skeleton.fixture.json",
			"valid/minimal-clip.fixture.json",
			"valid/minimal-deformable-mesh.fixture.json",
			"valid/minimal-mask.fixture.json",
			"valid/minimal-retarget-profile.fixture.json",
			"valid/minimal-motion-database.fixture.json",
			"valid/minimal-ragdoll-profile.fixture.json",
			"valid/minimal-controller.fixture.json",
			"valid/minimal-recording.fixture.json"
		)
		publication = [ordered]@{ atomic = $true; preservePriorGenerationOnFailure = $true; mixedGenerationsAllowed = $false }
	}
	Write-FixtureDocument -StagingRoot $stagingRoot -RelativePath "valid/minimal-runtime-set.fixture.json" -Document $runtimeSet -FixtureId "minimal-runtime-set" -Role "valid-set" -Requirements @("animation-contract-represent-skeletons-animation-clips-animation-controllers-masks-retarget-mappings-motion-databases-ragdoll", "animation-contract-multi-asset-import-recook-publish-mutually-dependent-outputs-atomically-publish-none-them", "animation-contract-generated-runtime-assets-require-source-gltf-fbx-dcc-editor-files-after-cooking", "animation-contract-preserve-compatibility-identity-prevents-clip-controller-retarget-map-ragdoll-motion-database-binding")

	$seedDefinitions = @(
		[ordered]@{
			path = "seeds/truncation/truncated-header.seed.json"
			id = "truncated-header"
			requirements = @("animation-contract-validate-skeleton-topology-transform-validity-bind-data-influence-data-channel-targets-timestamps", "animation-contract-preserve-last-known-good-generated-asset-set-import-cooking-validation-publication-fails", "animation-contract-feature-include-reproducible-correctness-corruption-rollback-scheduler-transaction-multithreaded-stress-soak-performance")
			document = New-SeedFixture -FixtureId "truncated-header" -Category "truncation" -BaseFixture "valid/minimal-source-skeleton.fixture.json" -ApplicationStage "after-production-serialization" -Operations @([ordered]@{ operator = "truncate-before-envelope-header-completes"; boundary = "productionHeaderSize-1" }) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Asset.TruncatedHeader")
		},
		[ordered]@{
			path = "seeds/truncation/truncated-dependency-table.seed.json"
			id = "truncated-dependency-table"
			requirements = @("animation-contract-validate-skeleton-topology-transform-validity-bind-data-influence-data-channel-targets-timestamps", "animation-contract-preserve-last-known-good-generated-asset-set-import-cooking-validation-publication-fails", "animation-contract-export-dependency-discovery-include-transitive-skeleton-mesh-clip-controller-mask-retarget-map")
			document = New-SeedFixture -FixtureId "truncated-dependency-table" -Category "truncation" -BaseFixture "valid/minimal-controller.fixture.json" -ApplicationStage "after-production-serialization" -Operations @([ordered]@{ operator = "truncate-inside-dependency-table"; retainedEntries = 1; declaredEntries = 2 }) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Asset.TruncatedDependencyTable")
		},
		[ordered]@{
			path = "seeds/truncation/truncated-payload.seed.json"
			id = "truncated-payload"
			requirements = @("animation-contract-validate-skeleton-topology-transform-validity-bind-data-influence-data-channel-targets-timestamps", "animation-contract-preserve-last-known-good-generated-asset-set-import-cooking-validation-publication-fails", "animation-contract-feature-include-reproducible-correctness-corruption-rollback-scheduler-transaction-multithreaded-stress-soak-performance")
			document = New-SeedFixture -FixtureId "truncated-payload" -Category "truncation" -BaseFixture "valid/minimal-clip.fixture.json" -ApplicationStage "after-production-serialization" -Operations @([ordered]@{ operator = "truncate-before-declared-payload-end"; boundary = "declaredPayloadEnd-1" }) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Asset.TruncatedPayload")
		},
		[ordered]@{
			path = "seeds/range/payload-range-past-end.seed.json"
			id = "payload-range-past-end"
			requirements = @("animation-contract-validate-skeleton-topology-transform-validity-bind-data-influence-data-channel-targets-timestamps", "animation-contract-reject-content-exceeds-hard-limits-safe-deterministic-reduction-unavailable", "animation-contract-feature-include-reproducible-correctness-corruption-rollback-scheduler-transaction-multithreaded-stress-soak-performance")
			document = New-SeedFixture -FixtureId "payload-range-past-end" -Category "range" -BaseFixture "valid/minimal-clip.fixture.json" -ApplicationStage "after-production-serialization" -Operations @([ordered]@{ operator = "set-range-past-file-end"; range = "payload"; offset = "fileSize-1"; size = 2 }) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Asset.RangePastEnd")
		},
		[ordered]@{
			path = "seeds/range/checked-arithmetic-overflow.seed.json"
			id = "checked-arithmetic-overflow"
			requirements = @("animation-contract-validate-skeleton-topology-transform-validity-bind-data-influence-data-channel-targets-timestamps", "animation-contract-reject-content-exceeds-hard-limits-safe-deterministic-reduction-unavailable", "animation-contract-feature-include-reproducible-correctness-corruption-rollback-scheduler-transaction-multithreaded-stress-soak-performance")
			document = New-SeedFixture -FixtureId "checked-arithmetic-overflow" -Category "range" -BaseFixture "valid/minimal-motion-database.fixture.json" -ApplicationStage "after-production-serialization" -Operations @([ordered]@{ operator = "set-range-arithmetic-operands"; operation = "multiply-count-by-stride"; leftUnsigned64 = "18446744073709551615"; rightUnsigned64 = "2" }) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Asset.CheckedArithmeticOverflow")
		},
		[ordered]@{
			path = "seeds/range/every-hard-limit-overflow.seed.json"
			id = "every-hard-limit-overflow"
			requirements = @("animation-contract-reject-content-exceeds-hard-limits-safe-deterministic-reduction-unavailable", "animation-contract-streaming-residency-retained-histories-have-explicit-memory-time-bounds-pressure-behavior-diagnostics", "animation-contract-feature-include-reproducible-correctness-corruption-rollback-scheduler-transaction-multithreaded-stress-soak-performance")
			document = New-SeedFixture -FixtureId "every-hard-limit-overflow" -Category "range" -BaseFixture "valid/minimal-runtime-set.fixture.json" -ApplicationStage "semantic-validation-before-allocation" -Operations @(New-HardLimitOverflowOperations -Limits $hardLimits) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Asset.HardLimitExceeded") -OperationMode "each-operation-is-an-independent-case"
		},
		[ordered]@{
			path = "seeds/versioning/bad-magic.seed.json"
			id = "bad-magic"
			requirements = @("animation-contract-represent-skeletons-animation-clips-animation-controllers-masks-retarget-mappings-motion-databases-ragdoll", "animation-contract-validate-skeleton-topology-transform-validity-bind-data-influence-data-channel-targets-timestamps", "animation-contract-feature-include-reproducible-correctness-corruption-rollback-scheduler-transaction-multithreaded-stress-soak-performance")
			document = New-SeedFixture -FixtureId "bad-magic" -Category "versioning" -BaseFixture "valid/minimal-source-skeleton.fixture.json" -ApplicationStage "after-production-serialization" -Operations @([ordered]@{ operator = "replace-envelope-magic"; value = "not-the-production-magic" }) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Asset.BadMagic")
		},
		[ordered]@{
			path = "seeds/versioning/unsupported-major-version.seed.json"
			id = "unsupported-major-version"
			requirements = @("animation-contract-represent-skeletons-animation-clips-animation-controllers-masks-retarget-mappings-motion-databases-ragdoll", "animation-contract-validate-skeleton-topology-transform-validity-bind-data-influence-data-channel-targets-timestamps", "animation-contract-feature-include-reproducible-correctness-corruption-rollback-scheduler-transaction-multithreaded-stress-soak-performance")
			document = New-SeedFixture -FixtureId "unsupported-major-version" -Category "versioning" -BaseFixture "valid/minimal-clip.fixture.json" -ApplicationStage "after-production-serialization" -Operations @([ordered]@{ operator = "set-envelope-major-version"; value = "currentSupportedMajor+1" }) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Asset.UnsupportedMajorVersion")
		},
		[ordered]@{
			path = "seeds/versioning/unknown-required-chunk.seed.json"
			id = "unknown-required-chunk"
			requirements = @("animation-contract-represent-skeletons-animation-clips-animation-controllers-masks-retarget-mappings-motion-databases-ragdoll", "animation-contract-validate-skeleton-topology-transform-validity-bind-data-influence-data-channel-targets-timestamps", "animation-contract-feature-include-reproducible-correctness-corruption-rollback-scheduler-transaction-multithreaded-stress-soak-performance")
			document = New-SeedFixture -FixtureId "unknown-required-chunk" -Category "versioning" -BaseFixture "valid/minimal-clip.fixture.json" -ApplicationStage "after-production-serialization" -Operations @([ordered]@{ operator = "append-unknown-chunk"; required = $true; recordedSize = 4; payloadPattern = "00000000" }) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Asset.UnknownRequiredChunk")
		},
		[ordered]@{
			path = "seeds/versioning/unknown-optional-minor-chunk.seed.json"
			id = "unknown-optional-minor-chunk"
			requirements = @("animation-contract-represent-skeletons-animation-clips-animation-controllers-masks-retarget-mappings-motion-databases-ragdoll", "animation-contract-validate-skeleton-topology-transform-validity-bind-data-influence-data-channel-targets-timestamps")
			document = New-SeedFixture -FixtureId "unknown-optional-minor-chunk" -Category "versioning" -BaseFixture "valid/minimal-clip.fixture.json" -ApplicationStage "after-production-serialization" -Operations @([ordered]@{ operator = "append-unknown-chunk"; required = $false; recordedSize = 4; payloadPattern = "00000000" }) -Expected ([ordered]@{ status = "Ok"; diagnosticCode = "Animation.Asset.OptionalChunkSkipped"; optionalChunkSkipped = $true; publicationAllowed = $true })
		},
		[ordered]@{
			path = "seeds/dependency/missing-dependency.seed.json"
			id = "missing-dependency"
			requirements = @("animation-contract-preserve-last-known-good-generated-asset-set-import-cooking-validation-publication-fails", "animation-contract-preserve-compatibility-identity-prevents-clip-controller-retarget-map-ragdoll-motion-database-binding", "animation-contract-incompatible-unavailable-dependency-produce-documented-safe-pose-diagnostic-without-partially-mixing-generations", "animation-contract-export-dependency-discovery-include-transitive-skeleton-mesh-clip-controller-mask-retarget-map")
			document = New-SeedFixture -FixtureId "missing-dependency" -Category "dependency" -BaseFixture "valid/minimal-controller.fixture.json" -ApplicationStage "dependency-resolution" -Operations @([ordered]@{ operator = "remove-dependency"; assetId = $clipId }) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Dependency.Missing")
		},
		[ordered]@{
			path = "seeds/dependency/incompatible-fingerprint.seed.json"
			id = "incompatible-fingerprint"
			requirements = @("animation-contract-preserve-compatibility-identity-prevents-clip-controller-retarget-map-ragdoll-motion-database-binding", "animation-contract-incompatible-unavailable-dependency-produce-documented-safe-pose-diagnostic-without-partially-mixing-generations", "animation-outcome-hot-reload-switches-compatible-asset-sets-without-mixed-generations-retains-previous-valid")
			document = New-SeedFixture -FixtureId "incompatible-fingerprint" -Category "dependency" -BaseFixture "valid/minimal-clip.fixture.json" -ApplicationStage "dependency-resolution" -Operations @([ordered]@{ operator = "replace-dependency-compatibility-fingerprint"; assetId = $sourceSkeletonId; value = $targetSkeletonFingerprint }) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Dependency.IncompatibleFingerprint")
		},
		[ordered]@{
			path = "seeds/dependency/cyclic-dependency.seed.json"
			id = "cyclic-dependency"
			requirements = @("animation-contract-validate-skeleton-topology-transform-validity-bind-data-influence-data-channel-targets-timestamps", "animation-contract-multi-asset-import-recook-publish-mutually-dependent-outputs-atomically-publish-none-them", "animation-contract-export-dependency-discovery-include-transitive-skeleton-mesh-clip-controller-mask-retarget-map")
			document = New-SeedFixture -FixtureId "cyclic-dependency" -Category "dependency" -BaseFixture "valid/minimal-controller.fixture.json" -ApplicationStage "dependency-resolution" -Operations @([ordered]@{ operator = "add-dependency-cycle"; path = @($controllerId, $clipId, $controllerId) }) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Dependency.Cycle")
		},
		[ordered]@{
			path = "seeds/dependency/corrupt-transitive-export-dependency.seed.json"
			id = "corrupt-transitive-export-dependency"
			requirements = @("animation-contract-export-dependency-discovery-include-transitive-skeleton-mesh-clip-controller-mask-retarget-map", "animation-contract-export-validation-reject-unresolved-editor-source-corrupt-incompatible-stale-unsupported-animation-dependencies", "animation-outcome-export-validation-identifies-deliberately-removed-corrupted-transitive-animation-dependencies-before-publication")
			document = New-SeedFixture -FixtureId "corrupt-transitive-export-dependency" -Category "dependency" -BaseFixture "valid/minimal-runtime-set.fixture.json" -ApplicationStage "export-dependency-validation" -Operations @([ordered]@{ operator = "mark-transitive-dependency-corrupt"; assetId = $clipId; corruption = "content-hash-mismatch" }) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Export.CorruptDependency" -SafeFallback "no-export-publication")
		},
		[ordered]@{
			path = "seeds/corruption/content-hash-mismatch.seed.json"
			id = "content-hash-mismatch"
			requirements = @("animation-contract-validate-skeleton-topology-transform-validity-bind-data-influence-data-channel-targets-timestamps", "animation-contract-preserve-last-known-good-generated-asset-set-import-cooking-validation-publication-fails", "animation-contract-failed-incompatible-hot-reload-retain-previous-valid-generation-expose-reason", "animation-contract-feature-include-reproducible-correctness-corruption-rollback-scheduler-transaction-multithreaded-stress-soak-performance")
			document = New-SeedFixture -FixtureId "content-hash-mismatch" -Category "corruption" -BaseFixture "valid/minimal-clip.fixture.json" -ApplicationStage "after-production-serialization" -Operations @([ordered]@{ operator = "flip-one-payload-bit-without-updating-content-hash"; target = "first-required-payload-byte" }) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Asset.ContentHashMismatch")
		},
		[ordered]@{
			path = "seeds/corruption/non-finite-bind-transform.seed.json"
			id = "non-finite-bind-transform"
			requirements = @("animation-contract-validate-skeleton-topology-transform-validity-bind-data-influence-data-channel-targets-timestamps", "animation-contract-runtime-evaluation-never-publish-non-finite-pose-morph-bound-curve-event-root", "animation-contract-recoverable-allocation-import-resource-renderer-physics-automation-managed-export-validation-failures-preserve")
			document = New-SeedFixture -FixtureId "non-finite-bind-transform" -Category "corruption" -BaseFixture "valid/minimal-source-skeleton.fixture.json" -ApplicationStage "semantic-validation" -Operations @([ordered]@{ operator = "replace-semantic-number"; jsonPointer = "/payload/joints/0/localBindTransform/translation/0"; valueToken = "positive-infinity" }) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Skeleton.NonFiniteTransform")
		},
		[ordered]@{
			path = "seeds/corruption/hierarchy-cycle.seed.json"
			id = "hierarchy-cycle"
			requirements = @("animation-contract-validate-skeleton-topology-transform-validity-bind-data-influence-data-channel-targets-timestamps", "animation-contract-preserve-last-known-good-generated-asset-set-import-cooking-validation-publication-fails", "animation-contract-recoverable-allocation-import-resource-renderer-physics-automation-managed-export-validation-failures-preserve")
			document = New-SeedFixture -FixtureId "hierarchy-cycle" -Category "corruption" -BaseFixture "valid/minimal-source-skeleton.fixture.json" -ApplicationStage "semantic-validation" -Operations @([ordered]@{ operator = "replace-parent-index"; jointIndex = 0; parentIndex = 0 }) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Skeleton.HierarchyCycle")
		},
		[ordered]@{
			path = "seeds/corruption/descending-track-time.seed.json"
			id = "descending-track-time"
			requirements = @("animation-contract-validate-skeleton-topology-transform-validity-bind-data-influence-data-channel-targets-timestamps", "animation-contract-support-exact-clip-boundaries-zero-duration-clips-repeated-timestamps-reverse-traversal-multi", "animation-contract-recoverable-allocation-import-resource-renderer-physics-automation-managed-export-validation-failures-preserve")
			document = New-SeedFixture -FixtureId "descending-track-time" -Category "corruption" -BaseFixture "valid/minimal-clip.fixture.json" -ApplicationStage "semantic-validation" -Operations @([ordered]@{ operator = "replace-key-ticks"; track = "root.translation"; values = @(60, 0) }) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Clip.UnorderedKeyTimes")
		},
		[ordered]@{
			path = "seeds/corruption/invalid-weight-sum.seed.json"
			id = "invalid-weight-sum"
			requirements = @("animation-contract-validate-skeleton-topology-transform-validity-bind-data-influence-data-channel-targets-timestamps", "animation-contract-reduce-excessive-vertex-influences-deterministically-according-documented-policy-normalize-retained-weights-report", "animation-contract-reject-content-exceeds-hard-limits-safe-deterministic-reduction-unavailable")
			document = New-SeedFixture -FixtureId "invalid-weight-sum" -Category "corruption" -BaseFixture "valid/minimal-deformable-mesh.fixture.json" -ApplicationStage "semantic-validation" -Operations @([ordered]@{ operator = "replace-unorm8-weights"; vertexIndex = 0; values = @(200, 0, 0, 0); expectedSum = 255 }) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Mesh.InvalidWeightSum")
		},
		[ordered]@{
			path = "seeds/lifecycle/cancel-before-publication.seed.json"
			id = "cancel-before-publication"
			requirements = @("animation-contract-preserve-last-known-good-generated-asset-set-import-cooking-validation-publication-fails", "animation-contract-multi-asset-import-recook-publish-mutually-dependent-outputs-atomically-publish-none-them", "animation-contract-automation-rejection-cancellation-timeout-disconnect-validation-failure-leave-no-partial-asset-graph", "animation-contract-scene-unload-object-reuse-component-removal-asset-reload-script-reload-play-mode", "animation-outcome-invalid-cancelled-import-graph-publication-recording-motion-database-build-retarget-edit-ragdoll")
			document = New-SeedFixture -FixtureId "cancel-before-publication" -Category "lifecycle" -BaseFixture "valid/minimal-runtime-set.fixture.json" -ApplicationStage "staged-publication" -Operations @([ordered]@{ operator = "signal-cancellation"; checkpoint = "after-validation-before-protected-publish"; tokenGeneration = 1 }) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Publication.Cancelled")
		},
		[ordered]@{
			path = "seeds/lifecycle/stale-source-generation.seed.json"
			id = "stale-source-generation"
			requirements = @("animation-contract-preserve-last-known-good-generated-asset-set-import-cooking-validation-publication-fails", "animation-contract-runtime-evaluation-reject-stale-object-component-asset-scene-physics-world-command-generations", "animation-contract-failed-incompatible-hot-reload-retain-previous-valid-generation-expose-reason", "animation-contract-scene-unload-object-reuse-component-removal-asset-reload-script-reload-play-mode", "animation-outcome-stale-object-component-scene-script-asset-renderer-physics-automation-managed-rollback-identities")
			document = New-SeedFixture -FixtureId "stale-source-generation" -Category "lifecycle" -BaseFixture "valid/minimal-runtime-set.fixture.json" -ApplicationStage "staged-publication" -Operations @([ordered]@{ operator = "advance-source-generation-before-publish"; capturedGeneration = 1; liveGeneration = 2 }) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Publication.StaleSourceGeneration")
		},
		[ordered]@{
			path = "seeds/lifecycle/stale-work-completion.seed.json"
			id = "stale-work-completion"
			requirements = @("animation-contract-runtime-evaluation-reject-stale-object-component-asset-scene-physics-world-command-generations", "animation-contract-pose-morph-asset-page-view-generation-changes-invalidate-cached-visibility-submission-decision", "animation-contract-scene-unload-object-reuse-component-removal-asset-reload-script-reload-play-mode", "animation-contract-unsupported-capabilities-gpu-budget-exhaustion-stale-gpu-work-renderer-loss-gpu-evaluation", "animation-outcome-stale-object-component-scene-script-asset-renderer-physics-automation-managed-rollback-identities")
			document = New-SeedFixture -FixtureId "stale-work-completion" -Category "lifecycle" -BaseFixture "valid/minimal-runtime-set.fixture.json" -ApplicationStage "runtime-work-completion" -Operations @([ordered]@{ operator = "complete-work-with-stale-generations"; captured = [ordered]@{ instance = 1; asset = 1; renderer = 1 }; live = [ordered]@{ instance = 2; asset = 2; renderer = 2 } }) -Expected (New-ExpectedRejection -DiagnosticCode "Animation.Runtime.StaleWork")
		}
	)

	foreach ($seed in $seedDefinitions) {
		Write-FixtureDocument -StagingRoot $stagingRoot -RelativePath $seed.path -Document $seed.document -FixtureId $seed.id -Role "mutation-seed" -Requirements @($seed.requirements)
	}

	if ($script:GeneratedFiles.Count -gt $script:MaximumFileCount - 1) {
		throw "Generated payload count $($script:GeneratedFiles.Count) exceeds the bounded manifest capacity."
	}

	$manifestEntries = @($script:GeneratedFiles | Sort-Object Path | ForEach-Object {
		$filePath = Resolve-GeneratedPath -StagingRoot $stagingRoot -RelativePath $_.Path
		$file = Get-Item -LiteralPath $filePath
		[ordered]@{
			path = $_.Path
			fixtureId = $_.FixtureId
			role = $_.Role
			bytes = [long]$file.Length
			sha256 = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash.ToLowerInvariant()
			requirements = @($_.Requirements)
		}
	})
	$contentSetRecord = [ordered]@{
		schema = "terra.animation.fixture-content-set/v1"
		generatorVersion = $script:GeneratorVersion
		files = $manifestEntries
	}
	$contentSetSha256 = Get-Sha256Text -Text (ConvertTo-StableJson -InputObject $contentSetRecord)
	$manifest = [ordered]@{
		schema = "terra.animation.fixture-manifest/v1"
		generator = "Tools/Animation/GenerateAnimationFixtures.ps1"
		generatorVersion = $script:GeneratorVersion
		sourceIndependent = $true
		determinism = [ordered]@{
			clockDataIncluded = $false
			absolutePathsIncluded = $false
			localeDependentDataIncluded = $false
			orderedJson = $true
			utf8Bom = $false
			lineEnding = "LF"
		}
		bounds = [ordered]@{
			maximumFileCountIncludingManifest = $script:MaximumFileCount
			maximumFileBytes = $script:MaximumFileBytes
			maximumTotalBytesIncludingManifest = $script:MaximumTotalBytes
		}
		productionFormatBoundary = "Valid documents are semantic writer inputs; mutation seeds use symbolic operators resolved by production-aware test code. No fixture freezes an unimplemented binary offset, magic, enum value, or chunk layout."
		contentSetSha256 = $contentSetSha256
		files = $manifestEntries
	}
	$manifestPath = Resolve-GeneratedPath -StagingRoot $stagingRoot -RelativePath "manifest.json"
	[System.IO.File]::WriteAllText($manifestPath, (ConvertTo-StableJson -InputObject $manifest), $script:Utf8WithoutBom)

	$allFiles = @(Get-ChildItem -LiteralPath $stagingRoot -File -Recurse)
	$totalBytes = [long](($allFiles | Measure-Object -Property Length -Sum).Sum)
	if ($allFiles.Count -gt $script:MaximumFileCount) {
		throw "Generated file count $($allFiles.Count) exceeds the $script:MaximumFileCount-file safety bound."
	}
	if ($totalBytes -gt $script:MaximumTotalBytes) {
		throw "Generated corpus size $totalBytes exceeds the $script:MaximumTotalBytes-byte safety bound."
	}
	foreach ($file in $allFiles) {
		if ($file.Length -gt $script:MaximumFileBytes) {
			throw "Generated file '$($file.FullName)' exceeds the per-file safety bound."
		}
	}

	# Publication moves only verified children out of the staging directory. The
	# output root was proven empty, so no source or prior evidence can be replaced.
	foreach ($child in @(Get-ChildItem -LiteralPath $stagingRoot -Force | Sort-Object Name)) {
		Move-Item -LiteralPath $child.FullName -Destination $resolvedOutputRoot
	}
	Remove-Item -LiteralPath $stagingRoot

	Write-Host "Generated $($allFiles.Count) deterministic animation fixture files."
	Write-Host "Content-set SHA-256: $contentSetSha256"
	Write-Host "Output root: $resolvedOutputRoot"
}
catch {
	throw "Animation fixture generation failed. Verified output was not published; inspect '$stagingRoot'. $($_.Exception.Message)"
}
