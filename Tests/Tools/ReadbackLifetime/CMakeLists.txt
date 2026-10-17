# Native validation follows the installed engine ABI and independently decodes
# transmitted PNG bytes through the existing private bundled decoder target.
set(readback_lifetime_tools "${ENGINE_INSTALL_DESTINATION}/Tests/Tools/ReadbackLifetime")
add_executable(ReadbackLifetimeFixture ReadbackLifetimeFixture.cpp
	${PROJECT_SOURCE_DIR}/cmake/TerraEngine.Windows.manifest)
target_compile_features(ReadbackLifetimeFixture PRIVATE cxx_std_20)
target_compile_definitions(ReadbackLifetimeFixture PRIVATE
	NOMINMAX TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION=1)
target_include_directories(ReadbackLifetimeFixture PRIVATE
	"${PROJECT_SOURCE_DIR}/Engine" "${PROJECT_SOURCE_DIR}/Tests/Support"
	"${PROJECT_SOURCE_DIR}/External/json/include" "${PROJECT_SOURCE_DIR}/External/glfw/include")
target_link_libraries(ReadbackLifetimeFixture PRIVATE
	TerraEngine-Engine TerraEngineCaptureDecodeAssertions bcrypt)
install(TARGETS ReadbackLifetimeFixture RUNTIME DESTINATION "${readback_lifetime_tools}")
install(FILES $<TARGET_RUNTIME_DLLS:ReadbackLifetimeFixture> DESTINATION "${readback_lifetime_tools}")
install(FILES RunInstalledReadbackLifetime.ps1 InvokeReadbackEditorAcceptance.ps1
	ReadbackLifetimeProtocol.psm1 TestReadbackLifetimeRunner.ps1
	TestReadbackLifetimeSensitivity.ps1 README.md DESTINATION "${readback_lifetime_tools}")
install(DIRECTORY Fixtures/ DESTINATION "${readback_lifetime_tools}/Fixtures")

# Parser fixtures never establish GPU acceptance. Protect the current complete
# selection rather than accepting a missing/empty script as a successful test.
add_test(NAME Runtime.ReadbackLifetime.ProtocolContracts
	COMMAND ${TERRAENGINE_CORRECTNESS_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
	-File "${readback_lifetime_tools}/TestReadbackLifetimeRunner.ps1")
set_tests_properties(Runtime.ReadbackLifetime.ProtocolContracts PROPERTIES
	PASS_REGULAR_EXPRESSION "152 readback protocol cases passed; synthetic parser evidence only"
	LABELS "ReadbackLifetime;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 180)

# Install-time sealing runs after the shared editor/materializer/watchdog
# closure. Hash actual installed bytes and compare directly built native targets.
configure_file(WriteReadbackLifetimeArtifacts.cmake.in
	"${CMAKE_CURRENT_BINARY_DIR}/WriteReadbackLifetimeArtifacts.configured.cmake" @ONLY)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/WriteReadbackLifetimeArtifacts-$<CONFIG>.cmake"
	INPUT "${CMAKE_CURRENT_BINARY_DIR}/WriteReadbackLifetimeArtifacts.configured.cmake")
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/WriteReadbackLifetimeArtifacts-$<CONFIG>.cmake")
