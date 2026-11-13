# Windows script regressions use the installed watchdog and explicit source root,
# after the corresponding native producer targets have been declared.
# Each CTest is one maintained script contract, never one CTest per finding.
# These bounded CPU/parser/provenance protocols are not GPU/performance acceptance.
find_program(TERRAENGINE_REVIEW_PWSH NAMES pwsh
	HINTS "$ENV{ProgramFiles}/PowerShell/7" REQUIRED)
# The including CodeQuality CMakeLists validates this interpreter and overrides
# before registering any scripts; do not repeat unversioned discovery here.
set(_review_tools "${ENGINE_INSTALL_DESTINATION}/Tests/Tools")
set(_review_watchdog "${ENGINE_INSTALL_DESTINATION}/Tests/Tools/Correctness/RunWithWatchdog.ps1")
# Tiny build protocols select Ninja themselves, independently of the outer
# project generator. Never pass a Visual Studio/MSBuild executable as Ninja.
get_filename_component(_review_make_directory "${CMAKE_MAKE_PROGRAM}" DIRECTORY)
find_program(TERRAENGINE_REVIEW_NINJA NAMES ninja HINTS "${_review_make_directory}" REQUIRED)

# Installation preserves each script's repository-relative path. Producer source
# is intentionally read from the explicit checkout, never inferred from install.
set(_review_contract_scripts
	Physics/TestProfilerSummaryProvenance.ps1
	Animation/TestSoakResourceBaseline.ps1
	FramePublication/TestCaptureMetricSemantics.ps1
	Navigation/TestWorkerIdentityEvidence.ps1
	RuntimeMemorySafety/Parity/TestRollingFrameCandidates.py
	Terrain/TestBenchmarkMeasurementProvenance.ps1
	Terrain/TestTerrainSoakFailFast.ps1
	VirtualGeometry/TestDisposableSceneContract.ps1
	Audio/TestFixtureReproducibility.ps1
	Physics/TestResolvedWarmup.ps1
	Profiling/TestWindowGateNormalization.ps1
	RuntimeMemorySafety/Parity/TestComparatorFailureCategories.py
	RendererStability/TestEventBaselineClassification.ps1
	RendererStability/RunLiveSmoke.ps1
	RuntimeMemorySafety/TestAcquireConsumptionEvidence.ps1
	Terrain/TestArchitectureGuardDiagnostics.ps1
	CodeQuality/TestBuildInstallContracts.ps1
	CodeQuality/TestToolchainPaths.ps1
	CodeQuality/TestOwnedJobMembershipRetry.py
	Audio/TestProviderLoaderEnvironment.ps1
	Navigation/TestSoakArgumentPaths.ps1
	VirtualGeometry/TestInstalledRunnerDiscovery.ps1
	VirtualGeometry/TestRuntimeAdapterPaths.ps1
	Correctness/TestFixtureSceneIdentity.ps1)
foreach(_relative IN LISTS _review_contract_scripts)
	get_filename_component(_directory "${_relative}" DIRECTORY)
	install(FILES "${PROJECT_SOURCE_DIR}/Tests/Tools/${_relative}"
		DESTINATION "${_review_tools}/${_directory}")
endforeach()
# Tiny CPU drivers include these actual producer-used observation helpers.
install(FILES "${PROJECT_SOURCE_DIR}/Tests/Tools/RuntimeMemorySafety/Parity/RuntimeMemorySafetyFrameCandidates.h"
	DESTINATION "${_review_tools}/RuntimeMemorySafety/Parity")
install(FILES "${PROJECT_SOURCE_DIR}/Tests/Tools/RuntimeMemorySafety/RuntimeMemorySafetyAcquireEvidence.h"
	DESTINATION "${_review_tools}/RuntimeMemorySafety")

# Generate only a tiny per-script launcher, using the EXISTING maintained Windows
# job watchdog. Every invocation gets fresh evidence AND log roots, so re-running
# CTest never overwrites archived evidence or violates fresh-root requirements.
function(terraengine_review_script scope category name relative minimum group seconds program)
	string(TOLOWER "${category}" _category_directory)
	set(_argument_text "")
	foreach(_argument IN LISTS ARGN)
		if(_argument STREQUAL "@PROOF@")
			string(APPEND _argument_text ",$proof")
		else()
			string(REPLACE "'" "''" _quoted "${_argument}")
			string(APPEND _argument_text ",'${_quoted}'")
		endif()
	endforeach()
	if(relative MATCHES "[.]ps1$")
		set(_prefix "'-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',")
	else()
		set(_prefix "")
	endif()
	set(_launcher "${CMAKE_CURRENT_BINARY_DIR}/review-${category}-${name}-$<CONFIG>.ps1")
	# Source/install roots with apostrophes must be escaped in PS single quotes.
	foreach(_field PROJECT_SOURCE_DIR _review_tools _review_watchdog program)
		string(REPLACE "'" "''" _ps_${_field} "${${_field}}")
	endforeach()
	file(GENERATE OUTPUT "${_launcher}" CONTENT "
$ErrorActionPreference='Stop'
$proof=Join-Path '${_ps_PROJECT_SOURCE_DIR}' ('out/validation/review-remediation/${_category_directory}-ctest/${name}-'+[guid]::NewGuid().ToString('N'))
$logs=$proof+'-watchdog'
$arguments=@(${_prefix}'${_ps__review_tools}/${relative}'${_argument_text})
& '${_ps__review_watchdog}' -Executable '${_ps_program}' -ArgumentList $arguments -WorkingDirectory '${_ps_PROJECT_SOURCE_DIR}' -LogDirectory $logs -TimeoutSeconds ${seconds}
$childExit=$LASTEXITCODE
$text=Get-Content -LiteralPath (Join-Path $logs 'stdout.log') -Raw
Write-Output $text
Get-Content -LiteralPath (Join-Path $logs 'stderr.log') | Write-Output
if($childExit -ne 0){throw \"Script contract failed/unavailable; child exit=$childExit; retained logs=$logs\"}
$receipt=Join-Path $proof 'result.json'
if(Test-Path -LiteralPath $receipt){
 $result=Get-Content -LiteralPath $receipt -Raw|ConvertFrom-Json
 if($null -ne $result.PSObject.Properties['protocolExecuted']){
  if(-not $result.protocolOnly){throw 'Native qualification cannot be counted as protocol-only success'}
  $count=[int]$result.protocolExecuted
  $passed=@($result.cases | Where-Object {$_.status -eq 'passed' -and $_.scope -ceq 'actual AST with explicit protocol spy'}).Count
  if($passed -lt $count){throw 'Protocol receipt does not contain the claimed passing cases'}
 }else{
  $count=if($null -ne $result.PSObject.Properties['selectedCases']){[int]$result.selectedCases}else{[int]$result.selected}
 }
 foreach($field in @('failedCases','failures','unverifiedCases')){if($null -ne $result.PSObject.Properties[$field] -and [int]$result.$field -ne 0){throw \"Nonzero $field in $receipt\"}}
}elseif(Test-Path -LiteralPath (Join-Path $proof 'evidence.json')){
 $receipt=Join-Path $proof 'evidence.json'
 $result=Get-Content -LiteralPath $receipt -Raw|ConvertFrom-Json
 if($result.qualification -cne 'ISOLATED PRODUCER PROTOCOLS'){throw 'Unsupported protocol evidence kind'}
 $rows=@($result.results)
 if(@($rows | Where-Object {$_.passed -isnot [bool] -or -not $_.passed}).Count){throw 'Failed/malformed producer protocol result'}
 $count=$rows.Count
}else{
 $matches=[regex]::Matches($text,'(?m)^(\\d+) selected tests, (\\d+) failures\\s*$')
 if($matches.Count -ne 1 -or [int]$matches[0].Groups[2].Value -ne 0){throw 'Missing/ambiguous/nonzero strict selected-test summary'}
 $count=[int]$matches[0].Groups[1].Value
}
if($count -lt ${minimum}){throw \"Empty/incomplete contract: selected=$count expected>=${minimum}\"}
Write-Output \"${category} contract selected=$count minimum=${minimum}; CPU/protocol evidence only\"
")
	add_test(NAME "${scope}.${name}" COMMAND "${TERRAENGINE_REVIEW_PWSH}"
		-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "${_launcher}")
	math(EXPR _outer_timeout "${seconds}+20")
	set_tests_properties("${scope}.${name}" PROPERTIES
		LABELS "ReviewRemediation;${category};ScriptContract;${group}" TIMEOUT ${_outer_timeout}
		RUN_SERIAL TRUE ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endfunction()

# Preserve every existing name, argument and floor through the scoped launcher.
function(terraengine_review_evidence name relative minimum group seconds program)
	terraengine_review_script(Repository.ReviewEvidence Evidence "${name}" "${relative}"
		"${minimum}" "${group}" "${seconds}" "${program}" ${ARGN})
endfunction()

# Evidence normalization: protocol fixtures, not measured frame/worker timings.
terraengine_review_evidence(ProfilerSummary Physics/TestProfilerSummaryProvenance.ps1 3 EvidenceNormalization 90 "${TERRAENGINE_REVIEW_PWSH}" -RepoRoot "${PROJECT_SOURCE_DIR}" -WorkRoot @PROOF@)
terraengine_review_evidence(ResolvedWarmup Physics/TestResolvedWarmup.ps1 25 EvidenceNormalization 90 "${TERRAENGINE_REVIEW_PWSH}" -RepoRoot "${PROJECT_SOURCE_DIR}" -WorkRoot @PROOF@)
terraengine_review_evidence(ToolchainPaths CodeQuality/TestToolchainPaths.ps1 7 EvidenceNormalization 60 "${TERRAENGINE_REVIEW_PWSH}" -RepoRoot "${PROJECT_SOURCE_DIR}" -WorkRoot @PROOF@)
terraengine_review_evidence(CaptureMetrics FramePublication/TestCaptureMetricSemantics.ps1 7 EvidenceNormalization 90 "${TERRAENGINE_REVIEW_PWSH}" -SourceRoot "${PROJECT_SOURCE_DIR}")
terraengine_review_evidence(WindowNormalization Profiling/TestWindowGateNormalization.ps1 4 EvidenceNormalization 60 "${TERRAENGINE_REVIEW_PWSH}" -SourceRoot "${PROJECT_SOURCE_DIR}")
terraengine_review_evidence(EventClassification RendererStability/TestEventBaselineClassification.ps1 8 EvidenceNormalization 90 "${TERRAENGINE_REVIEW_PWSH}" -SourceRoot "${PROJECT_SOURCE_DIR}")
terraengine_review_evidence(RendererSmokeHarness RendererStability/RunLiveSmoke.ps1
	6 EvidenceNormalization 90 "${TERRAENGINE_REVIEW_PWSH}" -RunHarnessTests)

# These native children supply deliberate unsupported/malformed protocol input.
# They never synthesize measured terrain performance or qualify a GPU route.
add_executable(TerraEngine-TerrainSoakProtocolChild
	"${PROJECT_SOURCE_DIR}/Tests/Tools/Terrain/Fixtures/TerrainSoakProtocolChild.cpp")
target_compile_features(TerraEngine-TerrainSoakProtocolChild PRIVATE cxx_std_20)
if(MSVC)
	set_property(TARGET TerraEngine-TerrainSoakProtocolChild PROPERTY
		MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
install(TARGETS TerraEngine-TerrainSoakProtocolChild
	RUNTIME DESTINATION "${_review_tools}/Terrain" COMPONENT SourceReview)
terraengine_review_evidence(TerrainSoakFailFast Terrain/TestTerrainSoakFailFast.ps1
	10 EvidenceNormalization 150 "${TERRAENGINE_REVIEW_PWSH}"
	-SourceRoot "${PROJECT_SOURCE_DIR}" -EvidenceDirectory @PROOF@
	-ProtocolChild "${_review_tools}/Terrain/TerraEngine-TerrainSoakProtocolChild${CMAKE_EXECUTABLE_SUFFIX}"
	-RealInstallRoot "${ENGINE_INSTALL_DESTINATION}")
# Resource/assets: no real soak, scene authoring, audio renderer or Event Log IO.
terraengine_review_evidence(SoakBaseline Animation/TestSoakResourceBaseline.ps1 7 ResourceAssetProtocol 90 "${TERRAENGINE_REVIEW_PWSH}" -RepoRoot "${PROJECT_SOURCE_DIR}" -WorkRoot @PROOF@)
terraengine_review_evidence(DisposableScene VirtualGeometry/TestDisposableSceneContract.ps1 11 ResourceAssetProtocol 90 "${TERRAENGINE_REVIEW_PWSH}" -RepoRoot "${PROJECT_SOURCE_DIR}" -WorkRoot @PROOF@)
terraengine_review_evidence(AudioFixture Audio/TestFixtureReproducibility.ps1 4 ResourceAssetProtocol 150 "${TERRAENGINE_REVIEW_PWSH}" -SourceRoot "${PROJECT_SOURCE_DIR}" -EvidenceDirectory @PROOF@)
terraengine_review_evidence(TerrainGuard Terrain/TestArchitectureGuardDiagnostics.ps1 3 ResourceAssetProtocol 120 "${TERRAENGINE_REVIEW_PWSH}" -SourceRoot "${PROJECT_SOURCE_DIR}" -EvidenceDirectory @PROOF@)
# CPU receipt helpers: current drivers use MSVC flags, not portable GCC flags.
# Run CTest in a VsDevCmd/vcvars64 environment; absolute cl alone does not supply
# INCLUDE/LIB. Missing compiler/helper is a failed/unverified prerequisite.
terraengine_review_evidence(RollingFrames RuntimeMemorySafety/Parity/TestRollingFrameCandidates.py 12 CpuReceipt 150 "${TERRAENGINE_REVIEW_PYTHON}" --source-root "${PROJECT_SOURCE_DIR}" --evidence-directory @PROOF@ --cxx "${CMAKE_CXX_COMPILER}")
terraengine_review_evidence(ComparatorCategories RuntimeMemorySafety/Parity/TestComparatorFailureCategories.py 12 CpuReceipt 150 "${TERRAENGINE_REVIEW_PYTHON}" --source-root "${PROJECT_SOURCE_DIR}" --evidence-directory @PROOF@ --pwsh "${TERRAENGINE_REVIEW_PWSH}")
terraengine_review_evidence(AcquireFormatter RuntimeMemorySafety/TestAcquireConsumptionEvidence.ps1 5 CpuReceipt 150 "${TERRAENGINE_REVIEW_PWSH}" -SourceRoot "${PROJECT_SOURCE_DIR}" -EvidenceDirectory @PROOF@ -Cxx "${CMAKE_CXX_COMPILER}")
if(WIN32)
	# Compile the real membership loop with only its kernel query substituted.
	# Real owned-process/native GPU lanes separately prove handle settlement.
	terraengine_review_evidence(OwnedJobMembership CodeQuality/TestOwnedJobMembershipRetry.py 6 CpuReceipt 90 "${TERRAENGINE_REVIEW_PYTHON}" --source-root "${PROJECT_SOURCE_DIR}" --evidence-directory @PROOF@ --cxx "${CMAKE_CXX_COMPILER}")
endif()
# Short producer invocations use the installed executable and adjacent DLLs.
# A build-tree executable can lack that closure even after a successful install.
# These checks do NOT establish live GPU or performance acceptance.
terraengine_review_evidence(WorkerIdentity Navigation/TestWorkerIdentityEvidence.ps1 6 NativeProducerProvenance 150 "${TERRAENGINE_REVIEW_PWSH}" -RepoRoot "${PROJECT_SOURCE_DIR}" -WorkRoot @PROOF@ -NavigationExecutable "${_review_tools}/Navigation/TerraEngineNavigationAcceptance${CMAKE_EXECUTABLE_SUFFIX}" -NavigationManifest "${PROJECT_SOURCE_DIR}/Tests/Fixtures/Navigation/manifest.json")
terraengine_review_evidence(BenchmarkProvenance Terrain/TestBenchmarkMeasurementProvenance.ps1 5 NativeProducerProvenance 150 "${TERRAENGINE_REVIEW_PWSH}" -SourceRoot "${PROJECT_SOURCE_DIR}" -EvidenceDirectory @PROOF@ -Lane All -TerrainBenchmarkExecutable "${_review_tools}/Terrain/TerraEngineTerrainBenchmark${CMAKE_EXECUTABLE_SUFFIX}" -UnboundedValidationExecutable "${_review_tools}/UnboundedScenes/TerraEngine-UnboundedSceneValidation${CMAKE_EXECUTABLE_SUFFIX}")

# Windows portability protocols are supporting evidence, never Linux/native acceptance.
# The provider protocol explicitly opts into protocol-only mode; the absent native Linux proof is
# retained in its receipt and reserved for actual Linux platform qualification.
terraengine_review_script(Repository.ReviewPortability Portability BuildInstall CodeQuality/TestBuildInstallContracts.ps1 6 BuildInstallProtocol 180 "${TERRAENGINE_REVIEW_PWSH}" -RepositoryRoot "${PROJECT_SOURCE_DIR}" -MakeProgram "${TERRAENGINE_REVIEW_NINJA}" -CMakeCommand "${CMAKE_COMMAND}" -WorkRoot @PROOF@)
terraengine_review_script(Repository.ReviewPortability Portability ProviderLoaderProtocol Audio/TestProviderLoaderEnvironment.ps1 4 ProviderProtocol 90 "${TERRAENGINE_REVIEW_PWSH}" -RepoRoot "${PROJECT_SOURCE_DIR}" -WorkRoot @PROOF@ -ProtocolOnly)
terraengine_review_script(Repository.ReviewPortability Portability SoakArgv Navigation/TestSoakArgumentPaths.ps1 4 ArgvProtocol 90 "${TERRAENGINE_REVIEW_PWSH}" -SourceRoot "${PROJECT_SOURCE_DIR}" -ArgumentRecorderExecutable "${ENGINE_INSTALL_DESTINATION}/Tests/Tools/CodeQuality/TerraEngine-NativeArgvReceipt${CMAKE_EXECUTABLE_SUFFIX}")
terraengine_review_script(Repository.ReviewPortability Portability RunnerDiscovery VirtualGeometry/TestInstalledRunnerDiscovery.ps1 7 DiscoveryProtocol 90 "${TERRAENGINE_REVIEW_PWSH}" -SourceRoot "${PROJECT_SOURCE_DIR}")
# Scene identity follows the owned source file even when its authored display
# name differs; this protocol never launches the interactive acceptance editor.
terraengine_review_script(Repository.ReviewPortability Portability FixtureSceneIdentity Correctness/TestFixtureSceneIdentity.ps1 7 SceneIdentityProtocol 90 "${TERRAENGINE_REVIEW_PWSH}" -SourceRoot "${PROJECT_SOURCE_DIR}" -EvidenceDirectory @PROOF@)
# The runtime adapter shares one repository/install layout and a narrowly owned
# pressure-scene spelling; filesystem protocols retain every refusal boundary.
terraengine_review_script(Repository.ReviewPortability Portability RuntimeAdapterPaths VirtualGeometry/TestRuntimeAdapterPaths.ps1 11 AdapterPathProtocol 90 "${TERRAENGINE_REVIEW_PWSH}" -RepoRoot "${PROJECT_SOURCE_DIR}" -WorkRoot @PROOF@)
