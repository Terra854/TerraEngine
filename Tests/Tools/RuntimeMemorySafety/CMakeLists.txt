# These installed validation utilities are separate from production execution.
# Journal readers share the authoritative codec and never load archived DLLs.
set(runtime_memory_safety_tools "${ENGINE_INSTALL_DESTINATION}/Tests/Tools/RuntimeMemorySafety")
add_executable(TerraEngine-RuntimeMemorySafetyTimingInspector RuntimeMemorySafetyTimingInspector.cpp)
add_executable(TerraEngine-RuntimeMemorySafetyTimingJournalFixture RuntimeMemorySafetyTimingJournalFixture.cpp)
foreach(runtime_memory_safety_target IN ITEMS
	TerraEngine-RuntimeMemorySafetyTimingInspector TerraEngine-RuntimeMemorySafetyTimingJournalFixture)
	target_compile_features(${runtime_memory_safety_target} PRIVATE cxx_std_20)
	target_link_libraries(${runtime_memory_safety_target} PRIVATE TerraEngineRendererEfficiencyProfilerCodec)
	install(TARGETS ${runtime_memory_safety_target} RUNTIME DESTINATION "${runtime_memory_safety_tools}")
endforeach()

add_executable(RuntimeMemorySafetyFixture RuntimeMemorySafetyFixture.cpp
	# Real renderer lifecycle probes share the normal engine entry points.
	RuntimeMemorySafetyRendererFixture.cpp
	${PROJECT_SOURCE_DIR}/cmake/TerraEngine.Windows.manifest)
target_compile_features(RuntimeMemorySafetyFixture PRIVATE cxx_std_20)
target_compile_definitions(RuntimeMemorySafetyFixture PRIVATE
	NOMINMAX TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION=1)
target_include_directories(RuntimeMemorySafetyFixture PRIVATE
	"${PROJECT_SOURCE_DIR}/Engine" "${PROJECT_SOURCE_DIR}/External/json/include"
	"${PROJECT_SOURCE_DIR}/External/glfw/include")
target_link_libraries(RuntimeMemorySafetyFixture PRIVATE TerraEngine-Engine)
install(TARGETS RuntimeMemorySafetyFixture RUNTIME DESTINATION "${runtime_memory_safety_tools}")
# Directly launched native fixtures require the same local DLL closure as the
# installed test executables; a developer PATH must never supply these files.
install(FILES $<TARGET_RUNTIME_DLLS:RuntimeMemorySafetyFixture> DESTINATION "${runtime_memory_safety_tools}")
install(FILES
	CaptureRuntimeMemorySafetyTiming.ps1 CompareRuntimeMemorySafetyTiming.ps1
	InvokeRuntimeMemorySafetyTimingMatrix.ps1
	Test-RuntimeMemorySafetyTiming.ps1 Test-RuntimeMemorySafetyTimingInspector.ps1
	MaterializeRuntimeMemorySafetyFixture.ps1 ValidationCases.json README.md
	# The final runner and its editor loopback share strict installed provenance.
	RunInstalledRuntimeMemorySafety.ps1 InvokeRuntimeMemorySafetyEditorDiagnostics.ps1
	Test-RuntimeMemorySafetyRunner.ps1 Test-RuntimeMemorySafetyRendererProtocol.ps1
	Test-RuntimeMemorySafetyEditorProtocol.ps1 Test-RuntimeMemorySafetyRecordedEvidence.ps1
	DESTINATION "${runtime_memory_safety_tools}")
install(DIRECTORY Fixtures/ DESTINATION "${runtime_memory_safety_tools}/Fixtures")
# Each parity run compiles these sources against its selected matching old/new
# ABI. Ordinary engine configuration must not require a historical archive.
install(DIRECTORY Parity/ DESTINATION "${runtime_memory_safety_tools}/Parity"
	FILES_MATCHING PATTERN "CMakeLists.txt" PATTERN "*.cpp" PATTERN "*.h"
	PATTERN "*.ps1" PATTERN "*.py" PATTERN "*.json" PATTERN "*.md")

add_test(NAME Runtime.MemorySafety.TimingInspectorContracts
	COMMAND ${TERRAENGINE_CORRECTNESS_POWERSHELL} -NoProfile -ExecutionPolicy Bypass -Command
	"& '${runtime_memory_safety_tools}/Test-RuntimeMemorySafetyTimingInspector.ps1' -InstallRoot '${ENGINE_INSTALL_DESTINATION}' -WorkRoot ('${CMAKE_BINARY_DIR}/runtime-memory-timing-decoder-' + [Guid]::NewGuid().ToString('N'))")
set_tests_properties(Runtime.MemorySafety.TimingInspectorContracts PROPERTIES
	PASS_REGULAR_EXPRESSION "runtime memory safety timing inspector cases passed; real CPU journals only"
	LABELS "RuntimeMemorySafety;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 180)

add_test(NAME Runtime.MemorySafety.TimingProtocolContracts
	COMMAND ${TERRAENGINE_CORRECTNESS_POWERSHELL} -NoProfile -ExecutionPolicy Bypass -Command
	"& '${runtime_memory_safety_tools}/Test-RuntimeMemorySafetyTiming.ps1' -InstallRoot '${ENGINE_INSTALL_DESTINATION}' -WorkRoot ('${CMAKE_BINARY_DIR}/runtime-memory-timing-protocol-' + [Guid]::NewGuid().ToString('N'))")
set_tests_properties(Runtime.MemorySafety.TimingProtocolContracts PROPERTIES
	PASS_REGULAR_EXPRESSION "runtime memory safety timing protocol cases passed; no performance acceptance"
	LABELS "RuntimeMemorySafety;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 600)

# These protocol cases use synthetic evidence and reject malformed or incomplete
# receipts. Live GPU/editor acceptance remains an explicit installed runner job.
add_test(NAME Runtime.MemorySafety.RunnerProtocolContracts
	COMMAND ${TERRAENGINE_CORRECTNESS_POWERSHELL} -NoProfile -ExecutionPolicy Bypass -Command
	"& '${runtime_memory_safety_tools}/Test-RuntimeMemorySafetyRunner.ps1' -WorkRoot ('${CMAKE_BINARY_DIR}/runtime-memory-runner-protocol-' + [Guid]::NewGuid().ToString('N')) -MatrixPath '${runtime_memory_safety_tools}/InvokeRuntimeMemorySafetyTimingMatrix.ps1'")
set_tests_properties(Runtime.MemorySafety.RunnerProtocolContracts PROPERTIES
	PASS_REGULAR_EXPRESSION "runtime memory safety runner protocol cases passed; mocks only"
	LABELS "RuntimeMemorySafety;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 180)
foreach(runtime_memory_protocol IN ITEMS Renderer Editor)
	add_test(NAME Runtime.MemorySafety.${runtime_memory_protocol}ProtocolContracts
		COMMAND ${TERRAENGINE_CORRECTNESS_POWERSHELL} -NoProfile -ExecutionPolicy Bypass -Command
		"& '${runtime_memory_safety_tools}/Test-RuntimeMemorySafety${runtime_memory_protocol}Protocol.ps1' -WorkRoot ('${CMAKE_BINARY_DIR}/runtime-memory-${runtime_memory_protocol}-protocol-' + [Guid]::NewGuid().ToString('N'))")
	set_tests_properties(Runtime.MemorySafety.${runtime_memory_protocol}ProtocolContracts PROPERTIES
		PASS_REGULAR_EXPRESSION "protocol cases passed; synthetic"
		LABELS "RuntimeMemorySafety;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 180)
endforeach()

# Synthetic join tests check evidence admission without opening a graphics device.
add_test(NAME Runtime.MemorySafety.ParityJoinContracts
	COMMAND ${TERRAENGINE_CORRECTNESS_POWERSHELL} -NoProfile -ExecutionPolicy Bypass -Command
	"& '${runtime_memory_safety_tools}/Parity/Test-RuntimeMemorySafetyParityJoin.ps1' -WorkRoot ('${CMAKE_BINARY_DIR}/runtime-memory-parity-join-' + [Guid]::NewGuid().ToString('N'))")
set_tests_properties(Runtime.MemorySafety.ParityJoinContracts PROPERTIES
	PASS_REGULAR_EXPRESSION "parity join protocol checks passed; no actual parity acceptance"
	LABELS "RuntimeMemorySafety;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 180)

# Generate after all targets exist, so build-versus-install checks refer to the
# exact configuration's binaries. The script seals actual maintained inputs.
configure_file(WriteRuntimeMemorySafetyArtifactManifest.cmake.in
	"${CMAKE_CURRENT_BINARY_DIR}/WriteRuntimeMemorySafetyArtifactManifest.cmake.in" @ONLY)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/WriteRuntimeMemorySafetyArtifactManifest-$<CONFIG>.cmake"
	INPUT "${CMAKE_CURRENT_BINARY_DIR}/WriteRuntimeMemorySafetyArtifactManifest.cmake.in")
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/WriteRuntimeMemorySafetyArtifactManifest-$<CONFIG>.cmake")
