# Build the installed fail-closed unbounded-scene validation entry point.
# The executable writes evidence only; production scene state remains owned by
# the engine scenarios invoked by the PowerShell orchestrator.
add_executable(TerraEngine-UnboundedSceneValidation
	UnboundedSceneValidation.cpp)
target_compile_features(TerraEngine-UnboundedSceneValidation PRIVATE cxx_std_20)
target_include_directories(TerraEngine-UnboundedSceneValidation PRIVATE
	${CMAKE_SOURCE_DIR}/External/json/single_include)
target_link_libraries(TerraEngine-UnboundedSceneValidation PRIVATE TerraEngine-Engine)

# Keep the executable beside the installed manifests so the wrapper can locate
# one self-contained tool bundle from any install prefix.
set(TERRAENGINE_UNBOUNDED_SCENE_TOOL_INSTALL_DIR
	"${CMAKE_BINARY_DIR}/Build/Tests/Tools/UnboundedScenes")
install(TARGETS TerraEngine-UnboundedSceneValidation
	RUNTIME DESTINATION "${TERRAENGINE_UNBOUNDED_SCENE_TOOL_INSTALL_DIR}")
if(WIN32)
	# The installed validator is launched directly by PowerShell. Keep its engine
	# runtime beside it so validation never depends on a developer PATH setting.
	install(FILES $<TARGET_FILE:TerraEngine-Engine>
		DESTINATION "${TERRAENGINE_UNBOUNDED_SCENE_TOOL_INSTALL_DIR}")
	install(FILES $<TARGET_RUNTIME_DLLS:TerraEngine-Engine>
		DESTINATION "${TERRAENGINE_UNBOUNDED_SCENE_TOOL_INSTALL_DIR}"
		OPTIONAL)
endif()
