# Editor unit tests only compile deterministic non-UI editor support code.
# They deliberately avoid the editor executable, GLFW, Vulkan, and launcher.
add_executable(TerraEngine-EditorTests
	EditorTests.cpp
	Export/ArchiveSerializationTests.cpp
	Export/BuildProfileCollectionTests.cpp
	Export/BuildProfileEdgeTests.cpp
	Export/BuildProfileRoundTripTests.cpp
	Export/ExportPathNormalizerTests.cpp
	Export/ExportPackageValidatorTests.cpp
	Export/ExportRuntimeBaselineSampleTests.cpp
	Export/ExportScriptBindingRewriterTests.cpp
	Export/ExportValidatorManagedSourceTests.cpp
	Export/ExportValidatorTests.cpp
	Export/LinuxPlatformExportTests.cpp
	Export/MaterialExportValidationTests.cpp
	Export/MaterialPreviewContractTests.cpp
	Export/PackRoundTripTests.cpp
	Export/PackagingRequestValidationTests.cpp
	Export/ResidencyPolicyExportTests.cpp
	Profiling/ProfilerCaptureStoreTests.cpp
	Project/LinuxScriptCompilationTests.cpp
	Project/LinuxScriptEditorLaunchTests.cpp
	Project/LinuxScriptWorkspaceTests.cpp
	Scene/PrefabAuthoringWorkflowTests.cpp
	Scene/SchedulerPlayModeGraphTests.cpp
	Scene/SceneRuntimeParityTests.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Subsystems/PrefabAuthoringSubsystem.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Project/ScriptWorkspaceService.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/ArchiveSerialization.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/ExtentReader.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/PackagingRequest.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/PackPlanner.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/PackReader.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/PackWriter.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/BuildProfile.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/codec/CodecProvider.cpp
	${CMAKE_SOURCE_DIR}/Engine/Resource/MaterialRuntimeContract.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/ExportPackageValidator.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/ExportScriptBindingRewriter.cpp
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
		${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Core
		${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Project
		${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Subsystems
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
		TerraEngine-Engine
		Threads::Threads
		libzstd_static
		xxhash
)

if(WIN32)
	add_custom_command(TARGET TerraEngine-EditorTests POST_BUILD
		# Editor tests now exercise shared engine scene code, so copy dependent
		# runtime DLLs beside the executable just like the engine test target does.
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_RUNTIME_DLLS:TerraEngine-EditorTests>
			$<TARGET_FILE_DIR:TerraEngine-EditorTests>
		COMMAND_EXPAND_LISTS
	)
endif()

add_test(NAME Editor.ExportUnitTests COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.ExportUnitTests PROPERTIES LABELS "Editor")
