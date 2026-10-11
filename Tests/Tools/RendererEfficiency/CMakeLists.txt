# This bounded bootstrap utility runs outside frame execution. It depends only
# on native authored-policy declarations, committed JSON/GLM headers and the
# Windows SHA256 API. Reading the durable enum needs no engine/editor DLL.
add_executable(TerraEngine-RendererEfficiencyFixtureMaterializer
	RendererEfficiencyFixtureMaterializer.cpp
	ShadowInvalidationFixture.cpp
	LightingDirtyFixture.cpp
	LightingDepthFixture.cpp
	ShadowBatchingFixture.cpp
	StaticShadowFixture.cpp
	${PROJECT_SOURCE_DIR}/Engine/Renderer/LightingDepthReduction.cpp
	${PROJECT_SOURCE_DIR}/cmake/TerraEngine.Windows.manifest)
target_compile_features(TerraEngine-RendererEfficiencyFixtureMaterializer PRIVATE cxx_std_20)
target_compile_definitions(TerraEngine-RendererEfficiencyFixtureMaterializer PRIVATE NOMINMAX TERRAENGINE_ENGINE_BUILD)
target_include_directories(TerraEngine-RendererEfficiencyFixtureMaterializer PRIVATE
	${PROJECT_SOURCE_DIR}/Engine
	${PROJECT_SOURCE_DIR}/External/json/include ${PROJECT_SOURCE_DIR}/External/glm)
target_link_libraries(TerraEngine-RendererEfficiencyFixtureMaterializer PRIVATE bcrypt)

# The fixture CPU check shares the production numerical policy and creates no
# Vulkan device. Bundle that small source so the installed utility stays free
# of a runtime engine DLL dependency, just like the materializer above.
add_executable(TerraEngine-RendererEfficiencyDepthFixtureTests
	LightingDepthFixtureTests.cpp LightingDepthFixture.cpp
	${PROJECT_SOURCE_DIR}/Engine/Renderer/LightingDepthReduction.cpp)
target_compile_features(TerraEngine-RendererEfficiencyDepthFixtureTests PRIVATE cxx_std_20)
target_compile_definitions(TerraEngine-RendererEfficiencyDepthFixtureTests PRIVATE NOMINMAX TERRAENGINE_ENGINE_BUILD)
target_include_directories(TerraEngine-RendererEfficiencyDepthFixtureTests PRIVATE
	${PROJECT_SOURCE_DIR}/Engine
	${PROJECT_SOURCE_DIR}/External/json/include ${PROJECT_SOURCE_DIR}/External/glm)

set(renderer_efficiency_tools "${ENGINE_INSTALL_DESTINATION}/Tests/Tools/RendererEfficiency")
# Decode original binary profiler journals with their authoritative codec and
# committed Zstd dependency. These CPU utilities never create an engine frame.
add_library(TerraEngineRendererEfficiencyProfilerCodec STATIC
	${PROJECT_SOURCE_DIR}/Engine/Profiling/ProfilerJournalV1.cpp
	${PROJECT_SOURCE_DIR}/Engine/Profiling/ProfilerCapture.cpp
	${PROJECT_SOURCE_DIR}/Engine/Profiling/PerformanceTelemetry.cpp)
target_compile_features(TerraEngineRendererEfficiencyProfilerCodec PUBLIC cxx_std_20)
target_compile_definitions(TerraEngineRendererEfficiencyProfilerCodec PRIVATE NOMINMAX)
target_include_directories(TerraEngineRendererEfficiencyProfilerCodec PUBLIC
	${PROJECT_SOURCE_DIR}/Engine ${PROJECT_SOURCE_DIR}/External/json/include
	${PROJECT_SOURCE_DIR}/External/zstd/lib)
target_link_libraries(TerraEngineRendererEfficiencyProfilerCodec PUBLIC libzstd_static)
add_executable(TerraEngine-RendererEfficiencyProfilerJournalInspector
	RendererEfficiencyProfilerJournalInspector.cpp)
target_link_libraries(TerraEngine-RendererEfficiencyProfilerJournalInspector PRIVATE
	TerraEngineRendererEfficiencyProfilerCodec)
add_executable(TerraEngine-RendererEfficiencyProfilerJournalFixture
	RendererEfficiencyProfilerJournalFixture.cpp)
target_link_libraries(TerraEngine-RendererEfficiencyProfilerJournalFixture PRIVATE
	TerraEngineRendererEfficiencyProfilerCodec)
install(TARGETS TerraEngine-RendererEfficiencyProfilerJournalInspector
	TerraEngine-RendererEfficiencyProfilerJournalFixture RUNTIME DESTINATION "${renderer_efficiency_tools}")
install(TARGETS TerraEngine-RendererEfficiencyFixtureMaterializer
	RUNTIME DESTINATION "${renderer_efficiency_tools}")
install(TARGETS TerraEngine-RendererEfficiencyDepthFixtureTests
	RUNTIME DESTINATION "${renderer_efficiency_tools}")
add_test(NAME Renderer.Efficiency.DepthFixtureContracts
	COMMAND "${renderer_efficiency_tools}/TerraEngine-RendererEfficiencyDepthFixtureTests.exe"
	"${ENGINE_INSTALL_DESTINATION}/Tests/Fixtures/RendererEfficiency/workloads.json")
set_tests_properties(Renderer.Efficiency.DepthFixtureContracts PROPERTIES
	LABELS "Renderer;Efficiency;Installed;Contract" TIMEOUT 120)
install(FILES Invoke-RendererEfficiencyValidation.ps1 Compare-RendererEfficiencyEvidence.ps1 Test-RendererEfficiencyFoundation.ps1 RendererEfficiencyStageSteps.ps1 Test-RendererEfficiencyNativeEvidence.ps1 Test-RendererEfficiencyLightingEvidence.ps1 Test-RendererEfficiencyDepthEvidence.ps1 Test-ShadowBatchingEvidence.ps1 Test-RendererEfficiencyEvidenceCollection.ps1 RendererEfficiencyRecoveredSessions.ps1 RendererEfficiencyEvidenceStream.ps1 Test-RendererEfficiencyRecoveredSessions.ps1
	DESTINATION "${renderer_efficiency_tools}")
install(DIRECTORY Schemas/ DESTINATION "${renderer_efficiency_tools}/Schemas")
install(FILES StaticShadowEvidence.Tests.ps1 DESTINATION "${renderer_efficiency_tools}")
install(FILES Compare-RendererEfficiencyBaselineTiming.ps1 Test-RendererEfficiencyBaselineTiming.ps1
	Test-RendererEfficiencyProfilerJournalInspector.ps1
	RendererEfficiencyBaselineTimingCapture.ps1 Test-RendererEfficiencyBaselineTimingCapture.ps1
	DESTINATION "${renderer_efficiency_tools}")
add_test(NAME Renderer.Efficiency.ProfilerJournalInspectorContracts COMMAND ${TERRAENGINE_CORRECTNESS_POWERSHELL}
	-NoProfile -ExecutionPolicy Bypass -Command
	"& '${renderer_efficiency_tools}/Test-RendererEfficiencyProfilerJournalInspector.ps1' -InstallRoot '${ENGINE_INSTALL_DESTINATION}' -WorkRoot ('${CMAKE_BINARY_DIR}/renderer-efficiency-journal-' + [Guid]::NewGuid().ToString('N'))")
set_tests_properties(Renderer.Efficiency.ProfilerJournalInspectorContracts PROPERTIES
	PASS_REGULAR_EXPRESSION "21 profiler journal inspector cases passed; real CPU journal fixtures only"
	LABELS "Renderer;Efficiency;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 180)
# Synthetic receipts exercise the strict grader; only the separate native
# inspector can establish membership in real persisted profiler journals.
add_test(NAME Renderer.Efficiency.BaselineTimingContracts COMMAND ${TERRAENGINE_CORRECTNESS_POWERSHELL}
	-NoProfile -ExecutionPolicy Bypass -Command
	"& '${renderer_efficiency_tools}/Test-RendererEfficiencyBaselineTiming.ps1' -WorkRoot ('${CMAKE_BINARY_DIR}/renderer-efficiency-frame-timing-' + [Guid]::NewGuid().ToString('N'))")
set_tests_properties(Renderer.Efficiency.BaselineTimingContracts PROPERTIES
	PASS_REGULAR_EXPRESSION "31 baseline timing contracts passed[.]"
	LABELS "Renderer;Efficiency;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 1200)
# Real binary journals exercise transport ownership and frozen-play rejection
# with mocked MCP responses; native capture provenance remains a separate gate.
add_test(NAME Renderer.Efficiency.BaselineTimingCaptureContracts COMMAND ${TERRAENGINE_CORRECTNESS_POWERSHELL}
	-NoProfile -ExecutionPolicy Bypass -Command
	"& '${renderer_efficiency_tools}/Test-RendererEfficiencyBaselineTimingCapture.ps1' -InstallRoot '${ENGINE_INSTALL_DESTINATION}' -WorkRoot ('${CMAKE_BINARY_DIR}/renderer-efficiency-capture-' + [Guid]::NewGuid().ToString('N'))")
set_tests_properties(Renderer.Efficiency.BaselineTimingCaptureContracts PROPERTIES
	PASS_REGULAR_EXPRESSION "16 baseline timing capture cases passed; real CPU journals and mocked MCP only"
	LABELS "Renderer;Efficiency;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 180)
# Keep actual tool usage beside the sealed installed runner and verifier.
install(FILES README.md DESTINATION "${renderer_efficiency_tools}")
install(FILES ${PROJECT_SOURCE_DIR}/Tests/Fixtures/RendererEfficiency/workloads.json
	DESTINATION "${ENGINE_INSTALL_DESTINATION}/Tests/Fixtures/RendererEfficiency")
# Preserve the precompiled animation closure and authored importer identities;
# never copy local SDK build outputs or malformed gate inputs into a fresh scene.
set(renderer_efficiency_animation "${PROJECT_SOURCE_DIR}/Tests/Fixtures/Animation/ImportValidation")
set(renderer_efficiency_inputs "${ENGINE_INSTALL_DESTINATION}/Tests/Fixtures/RendererEfficiency/Inputs/AnimationImportValidation")
install(DIRECTORY "${renderer_efficiency_animation}/Assets/" DESTINATION "${renderer_efficiency_inputs}/Assets")
install(DIRECTORY "${renderer_efficiency_animation}/Build/Assets/" DESTINATION "${renderer_efficiency_inputs}/Build/Assets")

configure_file(WriteRendererEfficiencyArtifactManifest.cmake.in
	${CMAKE_CURRENT_BINARY_DIR}/WriteRendererEfficiencyArtifactManifest.configured.cmake @ONLY)
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/WriteRendererEfficiencyArtifactManifest-$<CONFIG>.cmake
	INPUT ${CMAKE_CURRENT_BINARY_DIR}/WriteRendererEfficiencyArtifactManifest.configured.cmake)
install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/WriteRendererEfficiencyArtifactManifest-$<CONFIG>.cmake)

# Each run owns a fresh directory and preserves its diagnostics for inspection.
# These cases exercise installed script rejection paths without launching a GPU.
add_test(NAME Renderer.Efficiency.ToolContracts COMMAND ${TERRAENGINE_CORRECTNESS_POWERSHELL}
	-NoProfile -ExecutionPolicy Bypass -Command
	"& '${renderer_efficiency_tools}/Test-RendererEfficiencyFoundation.ps1' -InstallRoot '${ENGINE_INSTALL_DESTINATION}' -WorkRoot ('${CMAKE_BINARY_DIR}/renderer-efficiency-contract-' + [Guid]::NewGuid().ToString('N'))")
set_tests_properties(Renderer.Efficiency.ToolContracts PROPERTIES
	LABELS "Renderer;Efficiency;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 180)

# Exercise the installed collector helpers on real owned files. Atomic write
# failures and unchanged nested artifact paths are collection correctness gates.
add_test(NAME Renderer.Efficiency.EvidenceCollectionContracts COMMAND ${TERRAENGINE_CORRECTNESS_POWERSHELL}
	-NoProfile -ExecutionPolicy Bypass -Command
	"& '${renderer_efficiency_tools}/Test-RendererEfficiencyEvidenceCollection.ps1' -InstallRoot '${ENGINE_INSTALL_DESTINATION}' -WorkRoot ('${CMAKE_BINARY_DIR}/renderer-efficiency-collection-' + [Guid]::NewGuid().ToString('N'))")
set_tests_properties(Renderer.Efficiency.EvidenceCollectionContracts PROPERTIES
	# Require all ten cases, including transient and permanent sharing failures.
	PASS_REGULAR_EXPRESSION "10 evidence collection unit cases passed; synthetic CPU bytes only"
	LABELS "Renderer;Efficiency;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 180)

# Recovery may skip a planned editor launch only after authentic closed inputs,
# byte-preserving transformation, and copied closure admission are verified.
add_test(NAME Renderer.Efficiency.RecoveredSessionContracts COMMAND ${TERRAENGINE_CORRECTNESS_POWERSHELL}
	-NoProfile -ExecutionPolicy Bypass -Command
	"& '${renderer_efficiency_tools}/Test-RendererEfficiencyRecoveredSessions.ps1' -InstallRoot '${ENGINE_INSTALL_DESTINATION}' -WorkRoot ('${CMAKE_BINARY_DIR}/renderer-efficiency-recovered-' + [Guid]::NewGuid().ToString('N'))")
set_tests_properties(Renderer.Efficiency.RecoveredSessionContracts PROPERTIES
	PASS_REGULAR_EXPRESSION "26 recovered session admission cases passed; synthetic CPU data only"
	LABELS "Renderer;Efficiency;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 900)

# Native-proof verifier tests run against the installed comparer and retain their
# own files. Synthetic CPU inputs validate rejection, never GPU acceptance.
add_test(NAME Renderer.Efficiency.NativeEvidenceContracts COMMAND ${TERRAENGINE_CORRECTNESS_POWERSHELL}
	-NoProfile -ExecutionPolicy Bypass -Command
	"& '${renderer_efficiency_tools}/Test-RendererEfficiencyNativeEvidence.ps1' -WorkRoot ('${CMAKE_BINARY_DIR}/renderer-efficiency-native-proof-' + [Guid]::NewGuid().ToString('N'))")
set_tests_properties(Renderer.Efficiency.NativeEvidenceContracts PROPERTIES
	# The completion line proves every currently registered contract case ran.
	PASS_REGULAR_EXPRESSION "194 native verifier unit cases passed; synthetic CPU bytes only"
	# Full native artifact checks include both residency cohorts. Allow CPU-heavy
	# validation on busy build hosts without changing GPU acceptance thresholds.
	LABELS "Renderer;Efficiency;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 1200)

# Lighting contracts use synthetic CPU files to check exact owner, payload and
# receipt rejection. They neither run the renderer nor prove GPU acceptance.
add_test(NAME Renderer.Efficiency.LightingEvidenceContracts COMMAND ${TERRAENGINE_CORRECTNESS_POWERSHELL}
	-NoProfile -ExecutionPolicy Bypass -Command
	"& '${renderer_efficiency_tools}/Test-RendererEfficiencyLightingEvidence.ps1' -WorkRoot ('${CMAKE_BINARY_DIR}/renderer-efficiency-lighting-proof-' + [Guid]::NewGuid().ToString('N'))")
set_tests_properties(Renderer.Efficiency.LightingEvidenceContracts PROPERTIES
	PASS_REGULAR_EXPRESSION "Renderer efficiency lighting evidence contracts completed: 66 cases, 0 failures."
	# The Windows PowerShell host traverses full baseline schemas more slowly
	# than pwsh; retain every negative case and allow that measured CPU workload.
	LABELS "Renderer;Efficiency;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 900)

# These copied CPU receipts exercise the installed depth verifier. The exact
# completion count prevents a partial suite from looking like shader acceptance.
add_test(NAME Renderer.Efficiency.DepthEvidenceContracts COMMAND ${TERRAENGINE_CORRECTNESS_POWERSHELL}
	-NoProfile -ExecutionPolicy Bypass -Command
	"& '${renderer_efficiency_tools}/Test-RendererEfficiencyDepthEvidence.ps1' -WorkRoot ('${CMAKE_BINARY_DIR}/renderer-efficiency-depth-proof-' + [Guid]::NewGuid().ToString('N'))")
set_tests_properties(Renderer.Efficiency.DepthEvidenceContracts PROPERTIES
	PASS_REGULAR_EXPRESSION "Renderer efficiency depth evidence contracts completed: 55 cases, 0 failures."
	LABELS "Renderer;Efficiency;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 900)

# Exact native receipt partitions are checked using synthetic CPU data. This
# test opens no editor or Vulkan device and supplies no GPU acceptance evidence.
add_test(NAME Renderer.Efficiency.ShadowBatchEvidenceContracts COMMAND ${TERRAENGINE_CORRECTNESS_POWERSHELL}
	-NoProfile -ExecutionPolicy Bypass -Command
	"& '${renderer_efficiency_tools}/Test-ShadowBatchingEvidence.ps1' -WorkRoot ('${CMAKE_BINARY_DIR}/renderer-efficiency-shadow-batch-proof-' + [Guid]::NewGuid().ToString('N'))")
set_tests_properties(Renderer.Efficiency.ShadowBatchEvidenceContracts PROPERTIES
	PASS_REGULAR_EXPRESSION "Renderer efficiency shadow batching evidence contracts completed: 74 cases, 0 failures."
	LABELS "Renderer;Efficiency;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 900)

# Copied CPU data checks static ownership, real receipt joins and strict budget
# transitions. This verifier opens no editor and supplies no GPU acceptance.
add_test(NAME Renderer.Efficiency.StaticShadowEvidenceContracts COMMAND ${TERRAENGINE_CORRECTNESS_POWERSHELL}
	-NoProfile -ExecutionPolicy Bypass -Command
	"& '${renderer_efficiency_tools}/StaticShadowEvidence.Tests.ps1' -WorkRoot ('${CMAKE_BINARY_DIR}/renderer-efficiency-static-proof-' + [Guid]::NewGuid().ToString('N'))")
set_tests_properties(Renderer.Efficiency.StaticShadowEvidenceContracts PROPERTIES
	PASS_REGULAR_EXPRESSION "Renderer efficiency static shadow evidence contracts completed: 88 cases, 0 failures."
	LABELS "Renderer;Efficiency;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 900)
