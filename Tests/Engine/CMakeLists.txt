# Engine unit tests stay headless and deterministic so contributors can run
# them before the editor or renderer is available.
add_executable(TerraEngine-EngineTests
	EngineTests.cpp
	Runtime/ArchiveSerializationTests.cpp
	Runtime/BinaryIOTests.cpp
	Runtime/BootSummaryValidationTests.cpp
	Runtime/CodecProviderTests.cpp
	Runtime/ContentMountPathNormalizerTests.cpp
	Runtime/ContentMountPublicApiTests.cpp
	Runtime/ExportRuntimeBaselineLaunchTests.cpp
	Runtime/ExportRuntimeSmokeTests.cpp
	Runtime/ExportScriptingRuntimeTests.cpp
	Runtime/ExportStartupFlowTests.cpp
	Runtime/LinuxStartupSmokeTests.cpp
	Runtime/MaterialRuntimeContractTests.cpp
	Runtime/RenderProfileBootSummaryTests.cpp
	Runtime/SceneRuntimeParityTests.cpp
	Runtime/ThreadPoolSingletonTests.cpp
	Runtime/WorkerPoolTests.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/PackagingRequest.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/PackPlanner.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/PackWriter.cpp
	Core/ComponentAccessTests.cpp
	Core/EcsQueryTaskGraphTests.cpp
	Core/ObjectManagerTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Core/JobGraphTests.cpp
	Core/SystemSchedulerTests.cpp
	Core/TransformTaskGraphTests.cpp
	Input/InputSystemTests.cpp
	Platform/PlatformUtilsTests.cpp
	Profiling/ProfilerCaptureTests.cpp
	Resource/ResourceResidencyBudgetTests.cpp
	Resource/ResourceResidencyDiagnosticsTests.cpp
	Resource/ResourceResidencyPolicyTests.cpp
	Resource/ResourceResidencyUsageTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/MockRenderBackendTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RendererThreadingTestFixtures.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderBackendCompatibilityTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderBackendDiagnosticsOverheadTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderBackendStageTimingTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderBackendStallDiagnosticsTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderBudgetDiagnosticsTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderDeferredRetirementTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderDiagnosticsTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderFailureContainmentTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderFrameBreakdownTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderFrameSnapshotTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderGraphContractTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderGraphThreadingContractTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderPassValidationStressTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderPerformanceGateTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderPreparationCacheTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderQueueReadinessTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderRandomizedSchedulingTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderResourceCancellationTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderResourceReadinessTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderShadowParallelParityTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderSmallSceneOverheadTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderSnapshotContractTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderSubmissionOwnershipTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderWorkerTaskGroupTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderSubmissionPlanTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderTestRegistration.cpp
	Renderer/RenderTypesTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderUiPreparationTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderUploadBudgetTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderViewSubmitReuseTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderWorkerSafetyTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderWorkSchedulingTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VulkanFrameDiagnosticsTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VulkanRecordingSchedulingTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VulkanThreadLocalContextTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VulkanStaleBatchTests.cpp
	Scripting/ScriptingComponentAccessTests.cpp
	Scripting/ScriptingDiagnosticsTests.cpp
	Scripting/ScriptingDispatchTests.cpp
	Scripting/ScriptingMutationTests.cpp
	Scripting/ScriptingRuntimeTestFixtures.cpp
	Scripting/ScriptingWorkloadTests.cpp
	Scene/PrefabCompositionTests.cpp
	Scene/SceneGraphRuntimeTests.cpp
	Scene/SceneMutationQueueTests.cpp
	Scene/SpatialBoundsTests.cpp
	Scene/SpatialIndexEdgeTests.cpp
	Scene/SpatialIndexQueryTests.cpp
	Scene/SpatialIndexRefreshTests.cpp
	UI/UIElementTests.cpp
	UI/UILayoutEngineTests.cpp
	UI/UIParserTests.cpp
	UI/UISystemPreparationTests.cpp
	UI/UIStyleTests.cpp
	${CMAKE_SOURCE_DIR}/Engine/Resource/MaterialRuntimeContract.cpp
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

if(WIN32)
	add_custom_command(TARGET TerraEngine-EngineTests POST_BUILD
		# Windows loads dependent DLLs from the executable directory first, so copy
		# the engine and vendor runtime DLLs beside the test executable for CTest.
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_RUNTIME_DLLS:TerraEngine-EngineTests>
			$<TARGET_FILE_DIR:TerraEngine-EngineTests>
		COMMAND_EXPAND_LISTS
	)
endif()

add_test(NAME Engine.SpatialUnitTests COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.SpatialUnitTests PROPERTIES LABELS "Engine")
