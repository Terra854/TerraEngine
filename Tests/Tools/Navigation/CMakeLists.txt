# Real-engine navigation evidence runner used only by installed acceptance.
# Capture compiler provenance at configure time; the installed runner cannot
# infer a compiler version from whatever happens to be on the user's PATH.
# Developer shells may omit Git from PATH. Resolve its executable once rather
# than silently losing source provenance in an otherwise reproducible build.
find_package(Git QUIET)
if(GIT_FOUND)
	execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
		WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
		OUTPUT_VARIABLE TERRAENGINE_NAVIGATION_SOURCE_REVISION
		OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT TERRAENGINE_NAVIGATION_SOURCE_REVISION)
	set(TERRAENGINE_NAVIGATION_SOURCE_REVISION "unavailable")
endif()
configure_file(NavigationBuildMetadata.json.in navigation-build-metadata.json @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/navigation-build-metadata.json"
	DESTINATION Tests/Tools/Navigation)
# The workload uses a genuine flat terrain root produced by the editor, not a
# navigation-only mesh pretending to be physically published terrain.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/Tests/Fixtures/Navigation/AgentWorkload/TerrainSeed/"
	DESTINATION Tests/Fixtures/Navigation/AgentWorkload/TerrainSeed)

add_executable(TerraEngineNavigationValidation TerraEngineNavigationValidation.cpp)
target_compile_features(TerraEngineNavigationValidation PRIVATE cxx_std_20)
target_link_libraries(TerraEngineNavigationValidation PRIVATE TerraEngine-Engine)
set_target_properties(TerraEngineNavigationValidation PROPERTIES
	OUTPUT_NAME TerraEngineNavigationAcceptance
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Build/Tests/Tools/Navigation"
)

# The installed acceptance wrapper invokes the real engine runner by this
# stable name; keep it out of the normal SDK and package it beside the tool.
install(TARGETS TerraEngineNavigationValidation
	RUNTIME DESTINATION Tests/Tools/Navigation
	LIBRARY DESTINATION Tests/Tools/Navigation
	ARCHIVE DESTINATION Tests/Tools/Navigation
)

# Keep the installed wrapper and its evidence contract beside the executable so
# acceptance can run from a clean directory without source-tree fallbacks.
install(FILES
	RunInstalledNavigationAcceptance.ps1
	RunInstalledNavigationAgentAcceptance.ps1
	NewNavigationAgentWorkloadProject.ps1
	VerifyNavigationWorkloadScene.ps1
	CompareNavigationEvidence.ps1
	VerifyNavigationArchitecture.ps1
	navigation-evidence.schema.json
	DESTINATION Tests/Tools/Navigation
)

if(WIN32)
	# The acceptance executable is launched directly from the installed tool
	# directory, so keep its engine DLL dependencies beside it.
	install(FILES $<TARGET_FILE:TerraEngine-Engine>
		DESTINATION Tests/Tools/Navigation)
	install(FILES $<TARGET_RUNTIME_DLLS:TerraEngine-Engine>
		DESTINATION Tests/Tools/Navigation OPTIONAL)
endif()
