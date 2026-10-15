# Compile the production implementation in this process so executable-local new
# interception actually covers publication scratch and receipt construction.
add_executable(RenderPublicationAllocationFixture RenderPublicationAllocationFixture.cpp
	${PROJECT_SOURCE_DIR}/Engine/Renderer/Graph/RenderFramePublication.cpp)
target_compile_features(RenderPublicationAllocationFixture PRIVATE cxx_std_20)
target_compile_definitions(RenderPublicationAllocationFixture PRIVATE NOMINMAX TERRAENGINE_ENGINE_BUILD
	$<TARGET_PROPERTY:TerraEngine-Engine,COMPILE_DEFINITIONS>)
target_compile_options(RenderPublicationAllocationFixture PRIVATE
	$<TARGET_PROPERTY:TerraEngine-Engine,COMPILE_OPTIONS>)
target_include_directories(RenderPublicationAllocationFixture PRIVATE
	${PROJECT_SOURCE_DIR}/Engine ${PROJECT_SOURCE_DIR}/External/json/include
	${PROJECT_SOURCE_DIR}/External/glfw/include)
target_link_libraries(RenderPublicationAllocationFixture PRIVATE TerraEngine-Engine)
if(WIN32)
	add_custom_command(TARGET RenderPublicationAllocationFixture POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_RUNTIME_DLLS:RenderPublicationAllocationFixture>
			$<TARGET_FILE_DIR:RenderPublicationAllocationFixture>
		COMMAND_EXPAND_LISTS)
endif()
set(audio_renderer_safety_tools "${ENGINE_INSTALL_DESTINATION}/Tests/Tools/AudioRendererSafety")
install(TARGETS RenderPublicationAllocationFixture RUNTIME DESTINATION "${audio_renderer_safety_tools}")
install(FILES $<TARGET_RUNTIME_DLLS:RenderPublicationAllocationFixture>
	DESTINATION "${audio_renderer_safety_tools}")
install(FILES RunRenderPublicationAllocationSafety.ps1 DESTINATION "${audio_renderer_safety_tools}")
