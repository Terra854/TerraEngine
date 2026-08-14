# FramePublication tooling is source-distributed PowerShell/schema content.
# Keep it in TerraEngine's runnable Build tree with the other installed tests.
set(TERRAENGINE_FRAME_PUBLICATION_INSTALL_DIR
	"${CMAKE_BINARY_DIR}/Build/Tests/Tools/FramePublication")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/"
	DESTINATION "${TERRAENGINE_FRAME_PUBLICATION_INSTALL_DIR}")
# The installed capture orchestrator resolves its live adapter beside the
# installed Tools tree. Install that exact dependency so a clean install does
# not silently fall back to the source or build directories.
install(FILES
	"${CMAKE_SOURCE_DIR}/Tests/Tools/VirtualGeometry/CaptureVirtualGeometryLiveVisual.ps1"
	DESTINATION "${CMAKE_BINARY_DIR}/Build/Tests/Tools/VirtualGeometry")
add_executable(TerraEngine-FramePublicationContracts FramePublicationContracts.cpp)
target_compile_features(TerraEngine-FramePublicationContracts PRIVATE cxx_std_20)
target_link_libraries(TerraEngine-FramePublicationContracts PRIVATE TerraEngine-Engine)
install(TARGETS TerraEngine-FramePublicationContracts
	RUNTIME DESTINATION "${TERRAENGINE_FRAME_PUBLICATION_INSTALL_DIR}")
if(WIN32)
	# The native contract tool is installed outside the editor/engine runtime
	# folders. Keep its linked runtime beside the executable so invoking the
	# installed validation script never depends on a developer PATH setting.
	install(FILES $<TARGET_FILE:TerraEngine-Engine>
		DESTINATION "${TERRAENGINE_FRAME_PUBLICATION_INSTALL_DIR}")
	install(FILES $<TARGET_RUNTIME_DLLS:TerraEngine-Engine>
		DESTINATION "${TERRAENGINE_FRAME_PUBLICATION_INSTALL_DIR}"
		OPTIONAL)
endif()
