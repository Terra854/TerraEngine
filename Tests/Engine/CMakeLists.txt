# Engine unit tests stay headless and deterministic so contributors can run
# them before the editor or renderer is available.
file(GLOB_RECURSE TERRAENGINE_ENGINE_ANIMATION_TEST_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/Animation/*.cpp"
)

add_executable(TerraEngine-EngineTests
	EngineTests.cpp
	${TERRAENGINE_ENGINE_ANIMATION_TEST_SOURCES}
	# Codec round-trip tests use the production cooker encoders directly while
	# the runtime decoder remains linked from the TerraEngine library.
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/VirtualGeometryCodecs.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/VirtualGeometryMeshoptCodec.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/VirtualGeometryProductionBlocks.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/VirtualGeometryAssemblyBuilder.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/VirtualGeometryFoliageCurveBuilder.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/VirtualGeometryVoxelBuilder.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/VirtualGeometrySplineCurveBuilder.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/VirtualGeometryDisplacementBuilder.cpp
	${CMAKE_SOURCE_DIR}/Tools/VirtualGeometry/VirtualGeometryComparativeEvidence.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/AnimationClipCompiler.cpp
	# Motion database cook tests link the production deterministic compiler
	# directly, matching the existing clip compiler test boundary above.
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/MotionDatabaseCompiler.cpp
	Runtime/ArchiveSerializationTests.cpp
	Runtime/AnimationParityValidatorTests.cpp
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
	Runtime/WorkerPoolTelemetryTests.cpp
	Runtime/ExportRuntimePresentationTests.cpp
	Runtime/RendererStabilitySmokeToolTests.cpp
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
	Performance/PerformanceTestFixtures.cpp
	Performance/PerformanceBaselineTests.cpp
	Input/InputSystemTests.cpp
	Input/WindowInputWorkerBoundaryTests.cpp
	Platform/PlatformUtilsTests.cpp
	Platform/FatalErrorDialogTests.cpp
	Physics/PhysicsDependencyContractTests.cpp
	Physics/PhysicsHandleTests.cpp
	Physics/PhysicsSettingsTests.cpp
	Physics/PhysicsBackendBoundaryTests.cpp
	Physics/PhysicsJobAdapterTests.cpp
	Physics/PhysicsCommandQueueTests.cpp
	Physics/PhysicsGameplayCommandTests.cpp
	Physics/PhysicsQueryTests.cpp
	Physics/PhysicsEventTests.cpp
	Physics/PhysicsAuthoringIncrementalTests.cpp
	Physics/PhysicsPreparationPlanTests.cpp
	Physics/PhysicsBackendExtractionTests.cpp
	Physics/IncrementalBodyStateStoreTests.cpp
	Physics/PhysicsPublicationPacketTests.cpp
	Physics/PhysicsStalePublicationTests.cpp
	Physics/PhysicsProviderQueryEventTests.cpp
	Physics/PhysicsTransactionTests.cpp
	Physics/PhysicsComponentTests.cpp
	Physics/PhysicsConstraintComponentTests.cpp
	Physics/PhysicsCharacterComponentTests.cpp
	Physics/PhysicsConstraintSimulationTests.cpp
	Physics/PhysicsCharacterSimulationTests.cpp
	Physics/PhysicsConstraintWorldTests.cpp
	Physics/PhysicsCharacterContractTests.cpp
	Physics/PhysicsSerializationTests.cpp
	Physics/PhysicsMaterialAssetTests.cpp
	Physics/ColliderRecipeTests.cpp
	Physics/CookedColliderAssetTests.cpp
	Physics/PhysicsCookedShapeSimulationTests.cpp
	Physics/PhysicsFixedStepTests.cpp
	Physics/PhysicsRigidBodyTransactionTests.cpp
	Physics/PhysicsRigidBodySimulationTests.cpp
	Physics/PhysicsSchedulerTests.cpp
	Physics/PhysicsPerformanceTests.cpp
	Physics/PhysicsManagedAbiTests.cpp
	Physics/PhysicsManagedIntegrationTests.cpp
	Physics/PhysicsDeterminismStressTests.cpp
	Physics/PhysicsLifecycleStressTests.cpp
	Runtime/PhysicsExportRuntimeTests.cpp
	Profiling/ProfilerCaptureTests.cpp
	Profiling/ProfilerCaptureV2Tests.cpp
	Profiling/ProfilerOptimizationEvidenceTests.cpp
	Profiling/ProfilerCaptureLevelTests.cpp
	Profiling/ProfilerCapturePerformanceTests.cpp
	Profiling/ProfilerPerformanceSummaryTests.cpp
	Profiling/PerformanceTelemetryTests.cpp
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
	Resource/VirtualGeometryFeedbackStreamingTests.cpp
	Resource/VirtualGeometryPhysicalPageTests.cpp
	Resource/VirtualGeometryStreamingEndToEndTests.cpp
	Resource/VirtualGeometryPagePublicationTests.cpp
	Resource/VirtualGeometryResidencyLifecycleTests.cpp
	Resource/VirtualGeometryStreamingSoakTests.cpp
	Resource/VirtualGeometryPublicationContractTests.cpp
	Resource/VirtualGeometryCodecTests.cpp
	Resource/VirtualGeometryProductionFormatTests.cpp
	Resource/VirtualGeometryResourceTests.cpp
	Resource/VirtualGeometryResidencyTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/MockRenderBackendTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/AnimatedRenderViewTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/AnimatedVirtualGeometryAbiTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/AnimatedVirtualGeometryRuntimeTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/AnimatedVirtualGeometrySchedulerTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/DeformableVirtualGeometryTests.cpp
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
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RendererDeviceLossLatchTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RendererRecoveryDiagnosticsTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderFrameBreakdownTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderFrameSnapshotTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderGraphContractTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderGraphThreadingContractTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderPassValidationStressTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderPerformanceGateTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderPreparationCacheTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderInstanceAbiTests.cpp
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
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryAggregateContractTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryAggregateTransitionTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryBenchmarkTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryCapabilityContractTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryCapabilityTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryCapabilityTruthTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryCompactionTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryCullingTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryDiagnosticsTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryEvidenceSchemaTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryComparativeEvidenceTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryClaimClassificationTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryHardeningTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryHiZTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryGpuTimingTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryGpuDrivenPerformanceTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryInstanceTraversalTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryMultiViewTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryPageStreamingTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryPerformanceGateTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryQualityTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryRenderTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryShaderLayoutTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryShadowTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometrySchedulerContractTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryRoutePolicyTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryDirectRouteTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryClassPolicyTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/ProceduralGeometryPublicationTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryProductionClassTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryStressTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryVisibilityLayoutTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryVisibilityResolveTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryVisibilitySemanticsTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VulkanVirtualGeometryCompactionTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VulkanVirtualGeometryIndirectTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VulkanVirtualGeometryProductionDecodeTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryMultiPassTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryMaterialIntegrationTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryBackendContractTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryShaderProvenanceTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryTraversalOracleTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryTraversalOverflowTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryTwoPassOcclusionTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryViewOwnershipTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RenderRouteParityTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryViewEvidenceTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RendererViewLifecycleTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/PersistentRenderTopologyTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/SharedRenderSubmissionTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VulkanGroupedSubmissionTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/RendererScalePerformanceTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryProductionSoakTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryHardwareVisibilityTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryComputeRasterTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryHybridRasterTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryShadingBinTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VirtualGeometryBackendParityTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/AnimatedVirtualGeometryMorphParityTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/AnimatedVirtualGeometryReuseTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/AnimatedVirtualGeometryBoundsTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/AnimatedVirtualGeometryLodTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/AnimatedVirtualGeometryPerformanceTests.cpp
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
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VulkanVirtualGeometryTimingTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VulkanImageReadbackTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VulkanPresentationLifecycleTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VulkanUiSubmissionFailureTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/EditorFrameCompletionTrackerTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VulkanRecordingSchedulingTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VulkanThreadLocalContextTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Renderer/VulkanStaleBatchTests.cpp
	# Animation ABI and managed parity tests live with the scripting boundary
	# instead of the recursively discovered native Animation tests, so list them
	# explicitly.
	Scripting/AnimationAbiTests.cpp
	Scripting/AnimationManagedLifecycleTests.cpp
	Scripting/AnimationManagedParityTests.cpp
	Scripting/ScriptingComponentAccessTests.cpp
	Scripting/ScriptingDiagnosticsTests.cpp
	Scripting/ScriptingDispatchTests.cpp
	Scripting/ScriptingMutationTests.cpp
	Scripting/ScriptingRuntimeTestFixtures.cpp
	Scripting/ScriptingWorkloadTests.cpp
	Scripting/ScriptingWorkerBoundaryTests.cpp
	Scene/PrefabCompositionTests.cpp
	Scene/DerivedSceneCacheTests.cpp
	Scene/BulkScenePublicationTests.cpp
	Scene/HierarchySnapshotTests.cpp
	Scene/RuntimeHierarchyLayoutTests.cpp
	Scene/RuntimeLayoutCompatibilityTests.cpp
	Scene/SceneSerializationTests.cpp
	Scene/SceneGraphRuntimeTests.cpp
	Scene/SceneChangePacketTests.cpp
	Scene/SceneMutationQueueTests.cpp
	Scene/TransactionalMutationTestFixtures.cpp
	Scene/TransactionalMutationTests.cpp
	Scene/TransactionalMutationConflictTests.cpp
	Scene/TransactionalMutationStressTests.cpp
	Scene/TransactionalMutationDiagnosticsTests.cpp
	Scene/TransactionalMutationPolicyTests.cpp
	Scene/TransactionalMutationSerializationTests.cpp
	Scene/AutomationTransactionVisibilityTests.cpp
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
		# Some cooker-side codec headers intentionally include the public engine
		# format contract through its repository-qualified path.
		${CMAKE_SOURCE_DIR}
		${CMAKE_SOURCE_DIR}/Engine/Core/Object
		${CMAKE_SOURCE_DIR}/Engine/Runtime/ContentMount
		${CMAKE_SOURCE_DIR}/Editor/Export
		${CMAKE_SOURCE_DIR}/Editor/AssetCompilers
		${CMAKE_SOURCE_DIR}/External/xxhash
		${CMAKE_SOURCE_DIR}/External/zstd/lib
)

target_link_libraries(TerraEngine-EngineTests
	PRIVATE
		TerraEngine-Engine
		TerraEngineVirtualGeometrySchemas
		# PhysicsJobAdapterTests exercises the provider-private bridge directly.
		# Keep Jolt private to this test target so gameplay consumers still see no
		# provider headers or link requirements.
		Jolt
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

# This smoke process loads TerraEngine.CSharp.dll through the real .NET host and
# crosses the native dispatcher with public API calls. Running from the native
# output directory lets Windows resolve TerraEngine.dll and its bundled peers.
add_test(
	NAME Engine.ManagedRuntimeApiSmoke
	COMMAND dotnet run --project "${CMAKE_SOURCE_DIR}/Tests/Managed/RuntimeApiSmoke/RuntimeApiSmoke.csproj" --configuration Debug
)
set_tests_properties(Engine.ManagedRuntimeApiSmoke PROPERTIES
	LABELS "Engine;Managed;Scripting"
	WORKING_DIRECTORY "$<TARGET_FILE_DIR:TerraEngine-EngineTests>"
)

add_test(NAME Engine.PhysicsDependencyContract COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.PhysicsDependencyContract PROPERTIES
	LABELS "Engine;Physics;Dependency"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Physics.DependencyContract"
)

add_test(NAME Engine.PhysicsFoundation COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.PhysicsFoundation PROPERTIES
	LABELS "Engine;Physics;Foundation"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Physics.Handle,Physics.Settings,Physics.BackendBoundary,Physics.JobAdapter,Physics.CommandQueue,Physics.Transaction"
)

add_test(NAME Engine.PhysicsSimulationComponents COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.PhysicsSimulationComponents PROPERTIES
	LABELS "Engine;Physics;Simulation;Components"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Physics.Components,Physics.Serialization,Physics.Material,Physics.FixedStep,Physics.RigidBody,Physics.Scheduler"
)

add_test(NAME Engine.PhysicsCookedShapes COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.PhysicsCookedShapes PROPERTIES
	LABELS "Engine;Physics;Cooking"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Physics.ColliderRecipe,Physics.CookedColliderAsset,Physics.CookedShapeSimulation,Physics.CookedShapeCache"
)

add_test(NAME Engine.PhysicsQueriesAndEvents COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.PhysicsQueriesAndEvents PROPERTIES
	LABELS "Engine;Physics;Queries;Events"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Physics.GameplayCommand,Physics.Query,Physics.Event,Physics.StalePublication,Physics.Provider"
)

add_test(NAME Engine.PhysicsConstraintsAndCharacters COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.PhysicsConstraintsAndCharacters PROPERTIES
	LABELS "Engine;Physics;Constraints;Characters"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Physics.ConstraintComponent,Physics.CharacterComponent,Physics.Serialization.ConstraintAndCharacter,Physics.ConstraintSimulation,Physics.CharacterSimulation,Physics.ConstraintWorld,Physics.CharacterWorld"
)

add_test(NAME Engine.PhysicsDiagnostics COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.PhysicsDiagnostics PROPERTIES
	LABELS "Engine;Physics;Diagnostics;Profiler"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Physics.Scheduler,Engine.Profiler"
)

add_test(NAME Engine.PhysicsPerformance COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.PhysicsPerformance PROPERTIES
	LABELS "Engine;Physics;Performance"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Physics.Performance"
)

add_test(NAME Engine.PhysicsManaged COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.PhysicsManaged PROPERTIES
	LABELS "Engine;Physics;Managed"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=PhysicsManaged"
)

add_test(NAME Engine.PhysicsExport COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.PhysicsExport PROPERTIES
	LABELS "Engine;Physics;Export"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=PhysicsExportRuntime"
)

add_test(NAME Engine.PhysicsStress COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.PhysicsStress PROPERTIES
	LABELS "Engine;Physics;Stress;Determinism;Lifecycle"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Physics.DeterminismStress,Physics.LifecycleStress"
)

add_test(NAME Engine.VirtualGeometryFoundation COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.VirtualGeometryFoundation PROPERTIES
	LABELS "Engine;VirtualGeometry"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometry,RenderTypes,Renderer.Diagnostics,Resource.Residency"
)

# Keep a stable production contract slice independent of the broader renderer
# registry. It covers truth/evidence, scheduler/publication boundaries,
# portable visibility, backend contracts, and animated-page integration without
# pretending that a live GPU or soak ran inside CTest.
add_test(NAME Engine.VirtualGeometryProductionContracts COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.VirtualGeometryProductionContracts PROPERTIES
	LABELS "Engine;VirtualGeometry;Production;Contract"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometry.Capability;VirtualGeometry.GpuTiming;Renderer.VulkanVirtualGeometryTiming;VirtualGeometry.Evidence;VirtualGeometry.Scheduler;VirtualGeometry.Publication;VirtualGeometry.Visibility;VirtualGeometry.Backend;AnimatedVirtualGeometry"
)

add_test(NAME Engine.RenderRouteParity COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.RenderRouteParity PROPERTIES
	LABELS "Engine;Renderer;VirtualGeometry;Parity"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Renderer.RouteParity,VirtualGeometry.ViewEvidence,Renderer.ViewLifecycle"
)

add_test(NAME Engine.RendererScaleAndGrouping COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.RendererScaleAndGrouping PROPERTIES
	LABELS "Engine;Renderer;Performance;Grouping"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Renderer.PersistentTopology,Renderer.SharedSubmission,Renderer.VulkanGroupedSubmission,Renderer.ScalePerformance"
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
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometry.Render.Shadow,VirtualGeometry.Diagnostics.PublishesPerShadowViewCountersSeparately"
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

add_test(NAME Engine.RendererRecovery COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.RendererRecovery PROPERTIES
	LABELS "Engine;RendererRecovery;DeviceLoss"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=RendererRecovery,DeviceLoss,RenderFailureContainment,PlatformUtils,SystemScheduler,ExportRuntime"
)

add_test(NAME Engine.VulkanImageReadback COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.VulkanImageReadback PROPERTIES
	LABELS "Engine;Renderer;Vulkan;ImageReadback"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Renderer.VulkanImageReadback"
)

add_test(NAME Engine.AdaptiveVirtualGeometry COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.AdaptiveVirtualGeometry PROPERTIES
	LABELS "Engine;VirtualGeometry;AdaptiveVirtualGeometry"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=AdaptiveVirtualGeometry;VirtualGeometry.Render;VirtualGeometry.Resource;VirtualGeometry.Diagnostics;VirtualGeometry.HiZ;VirtualGeometry.Stress"
)

# The broad Animation filter is intentionally stable. Story-specific test names
# can be added under Tests/Engine/Animation without changing CTest wiring.
add_test(NAME Engine.Animation COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.Animation PROPERTIES
	LABELS "Engine;Animation"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Animation"
)

add_test(NAME Engine.AnimationParityValidator COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.AnimationParityValidator PROPERTIES
	LABELS "Engine;Animation;Parity"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Engine.AnimationParity"
)

# The cross-platform acceptance stream is semantic correctness work, not a
# benchmark. Keeping it as a separately named serial test makes the exact
# 100,000-tick evidence selectable on every installed toolchain without hiding
# the workload behind the shorter developer smoke profile.
add_test(NAME Engine.AnimationDeterminism100k COMMAND $<TARGET_FILE:TerraEngine-EngineTests>)
set_tests_properties(Engine.AnimationDeterminism100k PROPERTIES
	LABELS "Engine;Animation;Determinism;CrossPlatform"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Animation.CrossPlatformDeterminism;TERRAENGINE_ANIMATION_DETERMINISM_TICKS=100000"
	RUN_SERIAL TRUE
	TIMEOUT 1800
)
