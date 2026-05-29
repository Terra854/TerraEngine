# Editor unit tests only compile deterministic non-UI editor support code.
# They deliberately avoid the editor executable, GLFW, Vulkan, and launcher.
add_executable(TerraEngine-EditorTests
	EditorTests.cpp
	Export/ArchiveSerializationTests.cpp
	Export/BuildProfileCollectionTests.cpp
	Export/BuildProfileEdgeTests.cpp
	Export/BuildProfileRoundTripTests.cpp
	Export/ExportPathNormalizerTests.cpp
	Export/ExportValidatorManagedSourceTests.cpp
	Export/ExportValidatorTests.cpp
	Export/PackRoundTripTests.cpp
	Export/PackagingRequestValidationTests.cpp
	Profiling/ProfilerCaptureStoreTests.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/ArchiveSerialization.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/ExtentReader.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/PackagingRequest.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/PackPlanner.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/PackReader.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/PackWriter.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/BuildProfile.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/codec/CodecProvider.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/ExportValidator.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/io/PathNormalizer.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/util/WorkerPool.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/validation/PackValidator.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Profiling/ProfilerCaptureStore.cpp
	${CMAKE_SOURCE_DIR}/Engine/Profiling/ProfilerCapture.cpp
)

target_include_directories(TerraEngine-EditorTests
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_SOURCE_DIR}
		${CMAKE_SOURCE_DIR}/Engine
		${CMAKE_SOURCE_DIR}/Editor/Export
		${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Profiling
		${CMAKE_SOURCE_DIR}/External/json/single_include
)

set_target_properties(TerraEngine-EditorTests PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(MSVC)
	target_compile_options(TerraEngine-EditorTests PRIVATE /FS)
endif()

target_compile_definitions(TerraEngine-EditorTests
	PRIVATE
		XXH_STATIC_LINKING_ONLY
		ZSTD_STATIC_LINKING_ONLY
)

target_link_libraries(TerraEngine-EditorTests
	PRIVATE
		Threads::Threads
		libzstd_static
		xxhash
)

add_test(NAME Editor.ExportUnitTests COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.ExportUnitTests PROPERTIES LABELS "Editor")
