# Build the installed fail-closed unbounded-scene validation entry point.
# The executable writes evidence only; production scene state remains owned by
# the engine scenarios invoked by the PowerShell orchestrator.
add_executable(TerraEngine-UnboundedSceneValidation
	UnboundedSceneValidation.cpp)
target_compile_features(TerraEngine-UnboundedSceneValidation PRIVATE cxx_std_20)
target_include_directories(TerraEngine-UnboundedSceneValidation PRIVATE
	${CMAKE_SOURCE_DIR}/External/json/single_include)
target_link_libraries(TerraEngine-UnboundedSceneValidation PRIVATE TerraEngine-Engine)
if(BUILD_TESTING)
	# Exercise the actual producer with both supported soak durations. A missing
	# native collector must finalize unavailable evidence before any long wait.
	add_test(NAME Tools.UnboundedScenes.UnsupportedSoakFailsImmediately
		COMMAND "${CMAKE_COMMAND}"
			"-DVALIDATOR=${CMAKE_BINARY_DIR}/Build/Tests/Tools/UnboundedScenes/TerraEngine-UnboundedSceneValidation${CMAKE_EXECUTABLE_SUFFIX}"
			"-DPROOF_ROOT=${CMAKE_BINARY_DIR}/Testing/UnboundedScenes"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/TestUnsupportedSoak.cmake")
	set_tests_properties(Tools.UnboundedScenes.UnsupportedSoakFailsImmediately
		PROPERTIES TIMEOUT 30 LABELS "Tools;UnboundedScenes;Regression")
endif()
if(UNIX AND NOT APPLE)
	# Resolve the real bundled engine relative to this installed tool, rather
	# than relying on a developer's build tree or inherited library search path.
	set_target_properties(TerraEngine-UnboundedSceneValidation PROPERTIES
		BUILD_RPATH "${TERRAENGINE_LOCAL_RPATH}"
		INSTALL_RPATH "${TERRAENGINE_LOCAL_RPATH}/../../../Engine")
endif()

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
