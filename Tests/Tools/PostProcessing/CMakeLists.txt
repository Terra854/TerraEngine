# The Windows Vulkan fixture runs against the installed editor shaders and
# decodes its native captures through the same bundled test decoder.
set(post_process_test_tools "${ENGINE_INSTALL_DESTINATION}/Tests/Tools/PostProcessing")
add_executable(PostProcessFoundationFixture PostProcessFoundationFixture.cpp
	PostProcessScreenLightingGeometry.cpp
	${PROJECT_SOURCE_DIR}/cmake/TerraEngine.Windows.manifest)
target_compile_features(PostProcessFoundationFixture PRIVATE cxx_std_20)
target_compile_definitions(PostProcessFoundationFixture PRIVATE
	NOMINMAX TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION=1)
target_include_directories(PostProcessFoundationFixture PRIVATE
	"${PROJECT_SOURCE_DIR}/Engine" "${PROJECT_SOURCE_DIR}/Tests/Support"
	"${PROJECT_SOURCE_DIR}/External/json/include" "${PROJECT_SOURCE_DIR}/External/glfw/include")
target_link_libraries(PostProcessFoundationFixture PRIVATE
	TerraEngine-Engine TerraEngineCaptureDecodeAssertions)
install(TARGETS PostProcessFoundationFixture RUNTIME DESTINATION "${post_process_test_tools}")
install(FILES $<TARGET_RUNTIME_DLLS:PostProcessFoundationFixture> DESTINATION "${post_process_test_tools}")
install(FILES TestInstalledGraphicsSettings.ps1 DESTINATION "${post_process_test_tools}")
install(FILES TestInstalledPostProcessColor.ps1 DESTINATION "${post_process_test_tools}")
install(FILES TestInstalledPostProcessAuthoring.ps1 DESTINATION "${post_process_test_tools}")
install(FILES "${PROJECT_SOURCE_DIR}/Engine/Shaders/PostProcess/Validation/PostProcessFixtureTint.frag.hlsl"
	DESTINATION "${ENGINE_INSTALL_DESTINATION}/Editor/Shaders/PostProcess/Validation")
# The installed image scorer uses the frozen generic fixture beside other tests.
install(FILES TestPostProcessImageQuality.ps1 DESTINATION "${post_process_test_tools}")

include("${CMAKE_CURRENT_LIST_DIR}/MovingEdgesCapture.cmake")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/Tests/Fixtures/PostProcessing/"
	DESTINATION "${ENGINE_INSTALL_DESTINATION}/Tests/Fixtures/PostProcessing"
	PATTERN "bin" EXCLUDE PATTERN "obj" EXCLUDE)
