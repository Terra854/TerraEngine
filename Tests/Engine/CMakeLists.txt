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
	Runtime/ArchitectureMigrationSummaryTests.cpp
	Runtime/RenderProfileBootSummaryTests.cpp
	Runtime/SceneRuntimeParityTests.cpp
	Runtime/ThreadPoolSingletonTests.cpp
	Runtime/WorkerPoolTests.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/PackagingRequest.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/PackPlanner.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/archive/PackWriter.cpp
	Core/ActiveMembershipTests.cpp
	Core/RuntimeActiveMembershipTests.cpp
	Core/ObjectIdentityTests.cpp
	Core/ComponentAccessGrantTests.cpp
	Core/ComponentAccessTests.cpp
	Core/EcsQueryTaskGraphTests.cpp
	Core/FrameWorkloadExecutionTests.cpp
	Core/FullParallelFrameTestFixtures.cpp
	Core/FullParallelFrameExecutionTests.cpp
	Core/FullParallelFrameStressTests.cpp
	Core/WorkerConversionBaselineTests.cpp
	Core/WorkerConversionScenarioTests.cpp
	Core/WorkerCoverageReportTests.cpp
	Core/WorkerHardBoundaryTests.cpp
	Core/ObjectFacadeRemovalValidationTests.cpp
	Core/ObjectManagerTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Core/JobGraphTests.cpp
	Core/SystemSchedulerTests.cpp
	Core/TransformTaskGraphTests.cpp
	Input/InputSystemTests.cpp
	Input/WindowInputWorkerBoundaryTests.cpp
	Platform/PlatformUtilsTests.cpp
	Profiling/ProfilerCaptureTests.cpp
	Resource/ResourceResidencyBudgetTests.cpp
	Resource/ResourceResidencyDiagnosticsTests.cpp
	Resource/ResourceResidencyPolicyTests.cpp
	Resource/ResourceResidencyUsageTests.cpp
	Resource/ResourceManagerMeshTests.cpp
		Resource/ResourceStreamingDiagnosticsTests.cpp
		Resource/ResourceStreamingPolicyTests.cpp
		Resource/ResourceStreamingPreloadTests.cpp
		Resource/ResourceStreamingTests.cpp
		Resource/ResourceStreamingResidencyIntegrationTests.cpp
	Resource/VirtualGeometryPageResidencyTests.cpp
	Resource/VirtualGeometryPageStreamingHardeningTests.cpp
	Resource/VirtualGeometryResourceTests.cpp
	Resource/VirtualGeometryResidencyTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/MockRenderBackendTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RendererThreadingTestFixtures.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderBackendCompatibilityTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderBackendDiagnosticsOverheadTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderBackendStageTimingTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderBackendStallDiagnosticsTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderBudgetDiagnosticsTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderCaptureDiagnosticsTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderCaptureEncodingTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderCaptureQueueTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderDeferredRetirementTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderDiagnosticsTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderRuntimeLayoutTests.cpp
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
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderStreamingNeedTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderShadowParallelParityTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderSmallSceneOverheadTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderSnapshotContractTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderSubmissionOwnershipTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryBatchingTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryCompactionTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryCullingTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryDiagnosticsTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryHardeningTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryHiZTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryMultiViewTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryPageStreamingTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryPerformanceGateTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryQualityTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryRenderTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryShaderLayoutTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryShadowTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryStressTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderWorkerTaskGroupTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderSubmissionPlanTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/LiveGpuSubmissionBoundaryTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderTestRegistration.cpp
	# Virtual geometry renderer tests will be registered here as each
	# implementation story adds real test files.
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
	Scripting/ScriptingWorkerBoundaryTests.cpp
	Scene/PrefabCompositionTests.cpp
	Scene/HierarchySnapshotTests.cpp
	Scene/RuntimeHierarchyLayoutTests.cpp
	Scene/RuntimeLayoutCompatibilityTests.cpp
	Scene/SceneSerializationTests.cpp
	Scene/SceneGraphRuntimeTests.cpp
	Scene/SceneMutationQueueTests.cpp
	Scene/TransactionalMutationTestFixtures.cpp
	Scene/TransactionalMutationTests.cpp
	Scene/TransactionalMutationConflictTests.cpp
	Scene/TransactionalMutationStressTests.cpp
	Scene/TransactionalMutationDiagnosticsTests.cpp
	Scene/TransactionalMutationPolicyTests.cpp
	Scene/TransactionalMutationSerializationTests.cpp
	Scene/ParallelMutationBufferTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Scene/SkyComponentSerializationTests.cpp
	Scene/SceneStreamingDeferredLoadTests.cpp
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
		TERRAENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
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

add_test(NAME Engine.VirtualGeometryFoundation COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.VirtualGeometryFoundation PROPERTIES
	LABELS "Engine;VirtualGeometry"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometry,RenderTypes,Renderer.Diagnostics,Resource.Residency"
)

add_test(NAME Engine.VirtualGeometryHardening COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.VirtualGeometryHardening PROPERTIES
	LABELS "Engine;VirtualGeometry;Hardening"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometry.Hardening"
)

add_test(NAME Engine.VirtualGeometryRenderer COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.VirtualGeometryRenderer PROPERTIES
	LABELS "Engine;VirtualGeometry;Renderer;Stress"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometry.Render,VirtualGeometry.Diagnostics,VirtualGeometry.HiZ,VirtualGeometry.ShaderLayout,VirtualGeometry.Quality,VirtualGeometry.Stress"
)

add_test(NAME Engine.VirtualGeometryBatching COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.VirtualGeometryBatching PROPERTIES
	LABELS "Engine;VirtualGeometry;Batching"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometry.Batching"
)

add_test(NAME Engine.VirtualGeometryCompaction COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.VirtualGeometryCompaction PROPERTIES
	LABELS "Engine;VirtualGeometry;Compaction"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometry.Compaction"
)

add_test(NAME Engine.VirtualGeometryCulling COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.VirtualGeometryCulling PROPERTIES
	LABELS "Engine;VirtualGeometry;Culling"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometry.Culling"
)

add_test(NAME Engine.VirtualGeometryPerformanceGate COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.VirtualGeometryPerformanceGate PROPERTIES
	LABELS "Engine;VirtualGeometry;Performance"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometry.PerformanceGate"
)

add_test(NAME Engine.VirtualGeometryShadow COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.VirtualGeometryShadow PROPERTIES
	LABELS "Engine;VirtualGeometry;Shadow"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometry.Render.Shadow,VirtualGeometry.Diagnostics.US5"
)

add_test(NAME Engine.VirtualGeometryResource COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.VirtualGeometryResource PROPERTIES
	LABELS "Engine;VirtualGeometry;Resource;Streaming"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometry.Resource,Resource.Residency,Resource.Streaming"
)

add_test(NAME Engine.VirtualGeometryPageStreaming COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.VirtualGeometryPageStreaming PROPERTIES
	LABELS "Engine;VirtualGeometry;Streaming"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometryPageStreaming"
)

add_test(NAME Engine.VirtualGeometryPageResidency COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.VirtualGeometryPageResidency PROPERTIES
	LABELS "Engine;VirtualGeometry;Resource;Streaming"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometryPageResidency"
)

add_test(NAME Engine.AdaptiveVirtualGeometry COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.AdaptiveVirtualGeometry PROPERTIES
	LABELS "Engine;VirtualGeometry;AdaptiveVirtualGeometry"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=AdaptiveVirtualGeometry;VirtualGeometry.Render;VirtualGeometry.Resource;VirtualGeometry.Diagnostics;VirtualGeometry.HiZ;VirtualGeometry.Stress"
)
