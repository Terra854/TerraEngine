# Engine unit tests stay headless and deterministic so contributors can run
# them before the editor or renderer is available.
add_executable(TerraEngine-EngineTests
	EngineTests.cpp
	Runtime/ArchiveSerializationTests.cpp
	Runtime/BinaryIOTests.cpp
	Runtime/CodecProviderTests.cpp
	Runtime/ContentMountPathNormalizerTests.cpp
	Runtime/ContentMountPublicApiTests.cpp
	Runtime/ExportRuntimeSmokeTests.cpp
	Runtime/ExportScriptingRuntimeTests.cpp
	Runtime/ThreadPoolSingletonTests.cpp
	Runtime/WorkerPoolTests.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/PackagingRequest.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/PackPlanner.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/PackWriter.cpp
	Core/ObjectManagerTests.cpp
	Profiling/ProfilerCaptureTests.cpp
	Renderer/RenderTypesTests.cpp
	Scene/SpatialBoundsTests.cpp
	Scene/SpatialIndexEdgeTests.cpp
	Scene/SpatialIndexQueryTests.cpp
	Scene/SpatialIndexRefreshTests.cpp
	UI/UIElementTests.cpp
	UI/UILayoutEngineTests.cpp
	UI/UIParserTests.cpp
	UI/UIStyleTests.cpp
)

target_include_directories(TerraEngine-EngineTests
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_SOURCE_DIR}/Engine/Core/Object
		${CMAKE_SOURCE_DIR}/Engine/Runtime/ContentMount
		${CMAKE_SOURCE_DIR}/Editor/Export
		${CMAKE_SOURCE_DIR}/External/xxhash
		${CMAKE_SOURCE_DIR}/External/zstd/lib
)

target_link_libraries(TerraEngine-EngineTests
	PRIVATE
		TerraEngine-Engine
		libzstd_static
		xxhash
)

set_target_properties(TerraEngine-EngineTests PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(MSVC)
	target_compile_options(TerraEngine-EngineTests PRIVATE /FS)
endif()

target_compile_definitions(TerraEngine-EngineTests
	PRIVATE
		XXH_STATIC_LINKING_ONLY
		ZSTD_STATIC_LINKING_ONLY
)

add_custom_command(TARGET TerraEngine-EngineTests POST_BUILD
	# Windows loads dependent DLLs from the executable directory first, so copy
	# the engine and vendor runtime DLLs beside the test executable for CTest.
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		$<TARGET_RUNTIME_DLLS:TerraEngine-EngineTests>
		$<TARGET_FILE_DIR:TerraEngine-EngineTests>
	COMMAND_EXPAND_LISTS
)

add_test(NAME Engine.SpatialUnitTests COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.SpatialUnitTests PROPERTIES LABELS "Engine")
