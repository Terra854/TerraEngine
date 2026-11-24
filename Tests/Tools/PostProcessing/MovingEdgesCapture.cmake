# Included by the parent post-processing test-tools CMakeLists after its current
# install gate. This is a validation executable, never an engine/runtime target.
add_executable(PostProcessMovingEdgesCapture PostProcessMovingEdgesCapture.cpp
	${PROJECT_SOURCE_DIR}/cmake/TerraEngine.Windows.manifest)
target_compile_features(PostProcessMovingEdgesCapture PRIVATE cxx_std_20)
target_compile_definitions(PostProcessMovingEdgesCapture PRIVATE
	NOMINMAX TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION=1)
target_include_directories(PostProcessMovingEdgesCapture PRIVATE
	"${PROJECT_SOURCE_DIR}/Engine" "${PROJECT_SOURCE_DIR}/Tests/Support"
	"${PROJECT_SOURCE_DIR}/External/json/include" "${PROJECT_SOURCE_DIR}/External/glfw/include")
target_link_libraries(PostProcessMovingEdgesCapture PRIVATE
	TerraEngine-Engine TerraEngineCaptureDecodeAssertions bcrypt)
install(TARGETS PostProcessMovingEdgesCapture RUNTIME DESTINATION "${post_process_test_tools}")
install(FILES $<TARGET_RUNTIME_DLLS:PostProcessMovingEdgesCapture> DESTINATION "${post_process_test_tools}")
install(FILES MovingEdgesCaptureHost.md DESTINATION "${post_process_test_tools}")
