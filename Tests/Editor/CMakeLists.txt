# Editor unit tests only compile deterministic non-UI editor support code.
# They deliberately avoid the editor executable, GLFW, Vulkan, and launcher.
file(GLOB_RECURSE TERRAENGINE_EDITOR_ANIMATION_TEST_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/Animation/*.cpp"
)

add_executable(TerraEngine-EditorTests
	EditorTests.cpp
	Audio/AudioClipCompilerTests.cpp
	Audio/AudioImportPlaybackTests.cpp
	Audio/AudioAuthoringPersistenceTests.cpp
	Audio/AudioEditorControlTests.cpp
	Audio/AudioExportTests.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/AudioClipCompiler.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/NavigationGeometryCompiler.cpp
	${CMAKE_SOURCE_DIR}/Editor/Export/NavigationExportDependencies.cpp
	# Navigation editor contract tests link the same deterministic support code
	# used by the production editor; only the thin ImGui window remains UI-facing.
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Navigation/NavigationInspectorSupport.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Navigation/NavigationAuthoringTransactions.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/Providers/NavigationAutomationProvider.cpp
	${TERRAENGINE_EDITOR_ANIMATION_TEST_SOURCES}
	${TERRAENGINE_TERRAIN_EDITOR_TEST_SOURCES}
	# Terrain editor tests exercise the real provider-neutral creation/session
	# implementation instead of local mocks. UI windows remain on the production
	# editor target; only the small deterministic services are linked here.
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/TerrainCompiler.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Terrain/TerrainCreationService.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Terrain/TerrainAuthoringSession.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Terrain/TerrainPicking.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Terrain/TerrainBrushPresetStore.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Terrain/TerrainAuthoringSystem.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Terrain/TerrainHistoryCommand.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Terrain/TerrainDirtyState.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Terrain/TerrainSurfaceBrush.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Terrain/TerrainHoleBrush.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Terrain/TerrainProceduralOperators.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Terrain/TerrainHydraulicErosion.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Terrain/TerrainThermalErosion.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Terrain/TerrainProceduralJobs.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Terrain/TerrainResizeJobs.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Terrain/TerrainSplineGrader.cpp
	# The schema contract test links the same lowering implementation used by
	# the installed automation provider, avoiding a test-only validator fork.
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/TerrainAutomationSchema.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/Providers/TerrainAutomationProvider.cpp
	# Keep the generation-safe adapter available to editor contract tests. It
	# only queues typed transactions and never mutates live roots from JSON.
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Terrain/TerrainAutomationAdapter.cpp
	Export/ArchiveSerializationTests.cpp
	Export/BuildProfileCollectionTests.cpp
	Export/BuildProfileEdgeTests.cpp
	Export/BuildProfileRoundTripTests.cpp
	Export/ExportEvidenceTests.cpp
	Export/ExportOutputTransactionTests.cpp
	Export/ExportPathNormalizerTests.cpp
	Export/ExportPackageValidatorTests.cpp
	Export/ExportRuntimeBaselineSampleTests.cpp
	Export/ExportScriptBindingRewriterTests.cpp
	Export/ExportValidatorManagedSourceTests.cpp
	Export/ExportValidatorTests.cpp
	Export/ExportParityTests.cpp
	Export/PhysicsExportManifestTests.cpp
	Export/PhysicsExportValidationTests.cpp
	Export/PhysicsExportParityTests.cpp
	Export/NavigationExportParityTests.cpp
	Project/NavigationProjectLifecycleTests.cpp
	Export/NavigationExportTests.cpp
	Export/HeadlessExportBaselineTests.cpp
	Export/HeadlessExportContractTests.cpp
	Export/HeadlessExportDocumentationTests.cpp
	Export/HeadlessExportPipelineTests.cpp
	Export/LinuxPlatformExportTests.cpp
	Export/MaterialExportValidationTests.cpp
	Export/MaterialPreviewContractTests.cpp
	Export/NativeAotProjectWorkspaceTests.cpp
	Export/PackRoundTripTests.cpp
	Export/PackagingRequestValidationTests.cpp
	Export/ResidencyPolicyExportTests.cpp
	Export/StreamingPolicyExportTests.cpp
	Export/VirtualGeometryExportTests.cpp
	Export/VirtualGeometryScalingExportTests.cpp
	Profiling/ProfilerCaptureStoreTests.cpp
	Profiling/ProfilerWindowTests.cpp
	Profiling/ProfilerIdleOverheadTests.cpp
	Project/LinuxScriptCompilationTests.cpp
	Project/LinuxScriptEditorLaunchTests.cpp
	Project/LinuxScriptWorkspaceTests.cpp
	Project/PreferencesWindowTests.cpp
	Project/ProjectSettingsWindowTests.cpp
	Project/ProjectSettingsMenuInventoryTests.cpp
	Project/ProjectSettingsPersistenceTests.cpp
	Project/ManagedReadinessTests.cpp
	Project/ProjectReadinessPerformanceTests.cpp
	Core/EditorCommandLineTests.cpp
	Core/EditorRuntimeLayoutTests.cpp
	Core/EditorStartupRendererConfigurationTests.cpp
	Core/EditorPostFrameBoundaryTests.cpp
	Core/MainEditorToolbarTests.cpp
	Core/InspectorDisplayNameTests.cpp
	Core/InspectorComponentHeaderTests.cpp
	VirtualGeometry/VirtualGeometryProductionScaffoldingTests.cpp
	VirtualGeometry/VirtualGeometryDiagnosticsTests.cpp
	VirtualGeometry/DenseAnimatedVirtualGeometryFixtureTests.cpp
	Automation/EditorAutomationServiceTests.cpp
	Automation/EditorAutomationRegistryTests.cpp
	Automation/EditorAutomationJobTests.cpp
	Automation/EditorAutomationLogTests.cpp
	Automation/EditorAutomationIdempotencyTests.cpp
	Automation/EditorAutomationSchedulerTests.cpp
	Automation/EditorAutomationTransactionTests.cpp
	Automation/EditorMcpHistoryTests.cpp
	Automation/EditorReflectionWorkerBoundaryTests.cpp
	Automation/EditorMcpBridgeContractTests.cpp
	Automation/EditorMcpBridgeSafetyTests.cpp
	Automation/EditorMcpCameraImageContractTests.cpp
	Automation/EditorMcpCameraImageSafetyTests.cpp
	Automation/EditorMcpProjectLifecycleTests.cpp
	Automation/EditorMcpSceneLifecycleTests.cpp
	Automation/EditorMcpExportJobTests.cpp
	Automation/EditorMcpEndToEndTests.cpp
	Automation/EditorMcpPlayModeTests.cpp
	Automation/EditorMcpStructuredLogTests.cpp
	Automation/EditorMcpAssetOrganizationTests.cpp
	Automation/EditorMcpPrefabTests.cpp
	Automation/EditorMcpScriptTests.cpp
	Automation/EditorMcpBuildProfileTests.cpp
	Automation/EditorMcpSettingsTests.cpp
	Automation/EditorMcpSceneViewTests.cpp
	Automation/UnboundedWorldAutomationTests.cpp
	Automation/SceneViewAutomationLinkStubs.cpp
	Automation/PrefabAuthoringServiceLinkStubs.cpp
	Automation/EditorMcpProfilerTests.cpp
	Automation/EditorAutomationExtensibilityTests.cpp
	Automation/EditorMcpPhysicsContractTests.cpp
	Automation/EditorMcpPhysicsSafetyTests.cpp
	Automation/EditorMcpTransportTests.cpp
	Automation/CurrentProjectCompatibilityTests.cpp
	Automation/EditorMcpAssetImportTests.cpp
	Automation/EditorMcpAssetImportSafetyTests.cpp
	Automation/ProfilerDiagnosticsAutomationTests.cpp
	Automation/FramePublicationToolContractTests.cpp
	Automation/RendererRecoveryAutomationTests.cpp
	Automation/VirtualGeometryAutomationTests.cpp
	Automation/VirtualGeometryCapabilityAutomationTests.cpp
	Automation/VirtualGeometryScalingAutomationTests.cpp
	AssetPipeline/ModelPackageImportResultTests.cpp
	AssetPipeline/FbxPackageImporterTests.cpp
	AssetPipeline/FbxReimportTests.cpp
	AssetPipeline/FbxMaterialTextureTests.cpp
	AssetPipeline/FbxMaterialAssignmentTests.cpp
	AssetPipeline/FbxImportDiagnosticsTests.cpp
	AssetPipeline/FbxPackageBrowserContractTests.cpp
	AssetPipeline/FbxReadOnlySubAssetTests.cpp
	AssetPipeline/AssetDatabaseCancellationTests.cpp
	AssetPipeline/TextureCompilerWorkerTests.cpp
	AssetPipeline/NavigationGeometryCompilerTests.cpp
	Navigation/NavigationAuthoringTests.cpp
	Navigation/NavigationHistoryTests.cpp
	Navigation/NavigationDiagnosticsTests.cpp
	Navigation/NavigationAutomationTests.cpp
	AssetPipeline/GltfPackageImporterTests.cpp
	AssetPipeline/GltfMaterialTextureTests.cpp
	AssetPipeline/GltfMaterialAssignmentTests.cpp
	AssetPipeline/GltfImportDiagnosticsTests.cpp
	AssetPipeline/GltfUnsupportedFeatureTests.cpp
	AssetPipeline/GltfImportFailureTests.cpp
	AssetPipeline/GltfReimportTests.cpp
	AssetPipeline/VirtualGeometryInspectorTests.cpp
	AssetPipeline/VirtualGeometryAttributeSimplificationTests.cpp
	AssetPipeline/VirtualGeometryClusterDagTests.cpp
	AssetPipeline/VirtualGeometryCompactFormatTests.cpp
	AssetPipeline/VirtualGeometryDeterministicBuildTests.cpp
	AssetPipeline/VirtualGeometryScalingAssetPipelineTests.cpp
	AssetPipeline/VirtualGeometryMeshCompilerTests.cpp
	AssetPipeline/VirtualGeometryPageProfileTests.cpp
	AssetPipeline/VirtualGeometryStableIdentityTests.cpp
	AssetPipeline/VirtualGeometryQuantizationProfileTests.cpp
	AssetPipeline/VirtualGeometryQuantizationBoundaryTests.cpp
	AssetPipeline/VirtualGeometryProductionBlockTests.cpp
	AssetPipeline/VirtualGeometryRecookPublicationTests.cpp
	AssetPipeline/VirtualGeometryDirectRouteCookTests.cpp
	AssetPipeline/DeformableDemandedPageFormatTests.cpp
	AssetPipeline/VirtualGeometryPackageImportTests.cpp
	AssetPipeline/VirtualGeometryCanonicalRejectionTests.cpp
	Automation/VirtualGeometryProductionAutomationTests.cpp
	Export/VirtualGeometryBenchmarkExportTests.cpp
	Renderer/GameViewStatsOverlayTests.cpp
	Renderer/CheckedImGuiVulkanBridgeTests.cpp
	Renderer/ViewportTextureRetirementTests.cpp
	Renderer/RendererRecoveryEditorTests.cpp
	Renderer/RendererRecoveryWindowTests.cpp
	Renderer/ResourceResidencyWindowTests.cpp
	Renderer/SceneViewDisplayToggleTests.cpp
	Renderer/SceneViewMarkerOverlayTests.cpp
	Renderer/SceneViewMarkerPerformanceTests.cpp
	Renderer/SceneViewOrientationWidgetTests.cpp
	Renderer/SceneViewOverlayAccessibilityTests.cpp
	Renderer/SceneViewOverlayRendererUnavailableTests.cpp
	Renderer/SceneViewToolbarOverlayTests.cpp
	Renderer/SceneViewToolbarSnapTests.cpp
	Renderer/VirtualGeometryDebugViewTests.cpp
	Renderer/VirtualGeometryHardeningDebugViewTests.cpp
	Renderer/VirtualGeometryScalingDebugViewTests.cpp
	Renderer/UnboundedRenderViewTests.cpp
	# Virtual geometry asset-pipeline, automation, and export tests will be
	# registered here when their story-specific test files are added.
	Scene/ArchitectureHardeningPlayModeTests.cpp
	Scene/FullParallelFrameDiagnosticsTests.cpp
	Scene/RuntimeLayoutDiagnosticsTests.cpp
	Scene/SceneViewMarkerSelectionTests.cpp
	Scene/SceneViewOverlaySchedulerTests.cpp
	Scene/TransactionalMutationDiagnosticsWindowTests.cpp
	Scene/TransactionalMutationUndoTests.cpp
	Scene/PrefabAuthoringWorkflowTests.cpp
	Project/RendererRecoveryProjectSaveTests.cpp
	Project/SceneViewOverlayPreferenceTests.cpp
	Physics/PhysicsPlayModeTests.cpp
	Physics/PhysicsColliderCompilerTests.cpp
	Physics/PhysicsMaterialAssetTests.cpp
	Physics/PhysicsInspectorTests.cpp
	Physics/PhysicsPrefabUndoTests.cpp
	Physics/PhysicsProjectSettingsTests.cpp
	Physics/PhysicsSceneViewTests.cpp
	Physics/PhysicsPlayModeSoakTests.cpp
	Physics/PhysicsDiagnosticsWindowTests.cpp
	Physics/PhysicsEditorPerformanceTests.cpp
	Performance/PerformanceTestFixtures.cpp
	Profiling/RendererRecoveryProfilerTests.cpp
	Scene/SchedulerPlayModeGraphTests.cpp
	Scene/SceneRuntimeParityTests.cpp
	Scene/StreamingEditorWorkflowTests.cpp
	Scene/UnboundedSceneAuthoringTests.cpp
	Scene/UnboundedScenePersistenceTests.cpp
	Hierarchy/HierarchyVirtualizationTests.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Subsystems/PrefabAuthoringSubsystem.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Subsystems/ScriptCompilationSubsystem.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Subsystems/UndoRedoSystem.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Core/EditorCommandLine.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Core/EditorRuntimeLayout.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Core/EditorStartupRendererConfiguration.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Core/CheckedImGuiVulkanBridge.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Core/ViewportTextureRegistry.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Animation/AnimationPreviewController.cpp
	# Animation authoring and automation tests link the same UI-independent
	# production models used by the installed editor and MCP provider.
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Animation/AnimationAuthoringTransaction.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Animation/AnimationControllerAuthoringService.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Animation/AnimationOverlays.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Animation/AnimationPrefabIntegration.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Animation/AnimationRecorder.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Animation/AnimationSceneHandles.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Assets/AnimationAssetActions.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Project/EditorPreferencesStorage.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Project/ProjectManager.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Project/ManagedReadinessManifest.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Project/ScriptWorkspaceService.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Assets/AssetDatabase.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Assets/FileWatcher.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/EditorAutomationService.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/EditorAutomationRegistry.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/EditorAutomationTransaction.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/EditorAutomationJobService.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/EditorAutomationLogStore.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/EditorAutomationIdempotencyStore.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/EditorMcpBridge.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/EditorMcpTransport.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/Providers/AutomationProvider.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/Providers/ProjectSceneAutomationProvider.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/Providers/AssetPrefabAutomationProvider.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/Providers/ScriptBuildAutomationProvider.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/Providers/EditorDiagnosticsAutomationProvider.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/Providers/HistoryPlayModeAutomationProvider.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/Providers/AuthoringAutomationProvider.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/Providers/PhysicsAutomationProvider.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/Providers/AudioAutomationProvider.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/AnimationAutomationSchemas.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/AnimationAutomationTransactions.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/AnimationAutomationJobs.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/AnimationAssetOperations.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/AnimationControllerOperations.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/AnimationAdvancedOperations.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/AnimationRuntimeOperations.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation/Providers/AnimationAutomationProvider.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Windows/SceneViewOrientationWidget.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Windows/SceneViewCameraState.cpp
	Core/EditorWindowRttiStubs.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Windows/SceneViewMarkerPipeline.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Windows/SceneViewOverlayIcons.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Windows/SceneViewOverlayMarkers.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Windows/SceneViewOverlayTypes.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/MeshCompiler.cpp
	${TERRAENGINE_VIRTUAL_GEOMETRY_ASSET_COMPILER_SOURCES}
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/AnimationClipCompiler.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/MotionDatabaseCompiler.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Windows/MotionDatabaseAuthoringHost.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Windows/RetargetEditorWindow.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Windows/RagdollEditorWindow.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/TextureCompiler.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/ShaderCompiler.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/ModelPackageImporter.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/FbxPackageImporter.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/GltfImportTypes.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/GltfPackageImporter.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/MaterialAssetWriter.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/PhysicsColliderCompiler.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/PhysicsMaterialAssetWriter.cpp
	${CMAKE_SOURCE_DIR}/Engine/Resource/MaterialRuntimeContract.cpp
	${CMAKE_SOURCE_DIR}/Tools/HeadlessExport/HeadlessExportCommandLine.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Profiling/ProfilerCaptureStore.cpp
	${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Profiling/EditorProfilerService.cpp
	${CMAKE_SOURCE_DIR}/Engine/Profiling/ProfilerCapture.cpp
	${CMAKE_SOURCE_DIR}/Engine/Profiling/ProfilerJournalV1.cpp
)

add_test(NAME Editor.AudioClipCompiler COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.AudioClipCompiler PROPERTIES
	LABELS "Editor;Audio;AssetPipeline"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Editor.Audio.Compile")

# Audio editor tests are state based, so CI does not need an audible hardware device.
add_test(NAME Editor.AudioIntegration COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.AudioIntegration PROPERTIES
	LABELS "Editor;Audio;AssetPipeline;Export"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Editor.AudioImportPlayback,Editor.AudioAuthoring,Editor.AudioControl,Editor.AudioExport")

target_include_directories(TerraEngine-EditorTests
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_SOURCE_DIR}
		${CMAKE_SOURCE_DIR}/Engine
		${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Core
		${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Project
		${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Subsystems
		${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Assets
		${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation
		${CMAKE_SOURCE_DIR}/Editor/AssetCompilers
		${CMAKE_SOURCE_DIR}/Editor/Export
		${CMAKE_SOURCE_DIR}/Tools/HeadlessExport
		${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Profiling
		${CMAKE_SOURCE_DIR}/Editor/TerraEngine-Editor/Terrain
		${CMAKE_SOURCE_DIR}/External/basisu/encoder
		${CMAKE_SOURCE_DIR}/External/basisu/transcoder
		${CMAKE_SOURCE_DIR}/External/basisu/zstd
		${CMAKE_SOURCE_DIR}/External/cimgui/imgui
		${CMAKE_SOURCE_DIR}/External/json/single_include
)

set_target_properties(TerraEngine-EditorTests PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(MSVC)
	# The editor test binary intentionally compiles production translation units with
	# extensive reflection and automation coverage, which can exceed COFF's default
	# section count in optimized builds. /bigobj preserves that coverage.
	target_compile_options(TerraEngine-EditorTests PRIVATE /FS /bigobj)
endif()

target_compile_definitions(TerraEngine-EditorTests
	PRIVATE
		TERRAENGINE_EDITOR_TESTS
		XXH_STATIC_LINKING_ONLY
		ZSTD_STATIC_LINKING_ONLY
		TERRAENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(TerraEngine-EditorTests
	PRIVATE
		TerraEngine-Engine
		TerraEngine-ExportSupport
		Threads::Threads
		assimp::assimp
		fastgltf::fastgltf
		meshoptimizer
		mikktspace
		basisu
		libzstd_static
		xxhash
)

# Script-workspace tests must consume the managed bridge and source generator
# produced by this CMake build. Declaring the dependency prevents a developer's
# pre-existing Engine/Managed/bin output from masking a clean-checkout failure.
add_dependencies(TerraEngine-EditorTests
	TerraEngine
	TerraEngine.Export.Generator
)

if(WIN32)
	target_link_libraries(TerraEngine-EditorTests PRIVATE ws2_32)
endif()

if(WIN32)
	add_custom_command(TARGET TerraEngine-EditorTests POST_BUILD
		# Editor tests now exercise shared engine scene code, so copy dependent
		# runtime DLLs beside the executable just like the engine test target does.
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_RUNTIME_DLLS:TerraEngine-EditorTests>
			$<TARGET_FILE_DIR:TerraEngine-EditorTests>
		# ScriptWorkspaceService resolves managed assets relative to the running
		# executable. Stage the CMake-owned outputs there so synchronization tests
		# remain self-contained when no direct dotnet bin directory exists.
		COMMAND ${CMAKE_COMMAND} -E make_directory
			"$<TARGET_FILE_DIR:TerraEngine-EditorTests>/Managed/Analyzers"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${MANAGED_OUTPUT_DIR}/TerraEngine.CSharp.dll"
			"$<TARGET_FILE_DIR:TerraEngine-EditorTests>/Managed/TerraEngine.CSharp.dll"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${MANAGED_OUTPUT_DIR}/Analyzers/TerraEngine.Export.Generator.dll"
			"$<TARGET_FILE_DIR:TerraEngine-EditorTests>/Managed/Analyzers/TerraEngine.Export.Generator.dll"
		COMMAND_EXPAND_LISTS
	)
endif()

add_test(NAME Editor.ExportUnitTests COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.ExportUnitTests PROPERTIES LABELS "Editor")

add_test(NAME Editor.HeadlessExport COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.HeadlessExport PROPERTIES
	LABELS "Editor;Export;HeadlessExport"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=HeadlessExport,ExportEvidence,ExportOutputTransaction,ExportParity"
)

add_test(NAME Editor.ExportTransaction COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.ExportTransaction PROPERTIES
	LABELS "Editor;Export;HeadlessExport;Transaction"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=ExportOutputTransaction"
)

add_test(NAME Editor.ExportParity COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.ExportParity PROPERTIES
	LABELS "Editor;Export;HeadlessExport;Parity"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=ExportParity"
)

add_test(NAME Editor.ExportBaseline COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.ExportBaseline PROPERTIES
	LABELS "Editor;Export;HeadlessExport;Baseline"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=ExportRuntimeBaseline,HeadlessExport.Baseline"
)

add_test(NAME Editor.ExportDocumentation COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.ExportDocumentation PROPERTIES
	LABELS "Editor;Export;HeadlessExport;Documentation"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=HeadlessExport.Documentation"
)

add_test(NAME Editor.VirtualGeometryFoundation COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.VirtualGeometryFoundation PROPERTIES
	LABELS "Editor;VirtualGeometry"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometry,Editor.AssetPipeline.ModelPackage,Editor.Automation"
)

# Keep the installed/package contract selectable without running the complete
# editor registry. The filter also covers migration, benchmark-claim, and
# automation contract cases introduced for the production-readiness phase.
add_test(NAME Editor.VirtualGeometryProductionContracts COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.VirtualGeometryProductionContracts PROPERTIES
	LABELS "Editor;VirtualGeometry;Production;Contract"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometry.Editor.Production;VirtualGeometry.Editor.AssetPipeline.CanonicalProduction;VirtualGeometry.Editor.Export.Benchmark;VirtualGeometry.Editor.Automation"
)

add_test(NAME Editor.VirtualGeometryHardening COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.VirtualGeometryHardening PROPERTIES
	LABELS "Editor;VirtualGeometry;Hardening"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometry.Editor.Hardening"
)

add_test(NAME Editor.VirtualGeometryAssetPipeline COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.VirtualGeometryAssetPipeline PROPERTIES
	LABELS "Editor;VirtualGeometry;AssetPipeline"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Editor.AssetPipeline.VirtualGeometry,VirtualGeometry.Editor.PackageImport,VirtualGeometry.Editor.Inspector,VirtualGeometry.Editor.AssetDatabase"
)

add_test(NAME Editor.AssetDatabaseCancellation COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.AssetDatabaseCancellation PROPERTIES
	LABELS "Editor;AssetPipeline;Cancellation"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=AssetDatabaseCancellation"
)

add_test(NAME Editor.VirtualGeometryAutomation COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.VirtualGeometryAutomation PROPERTIES
	LABELS "Editor;VirtualGeometry;Automation"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometry.Editor.Automation"
)

add_test(NAME Editor.VirtualGeometryExport COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.VirtualGeometryExport PROPERTIES
	LABELS "Editor;VirtualGeometry;Export"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometry.Editor.Export,VirtualGeometry.Editor.DebugView"
)

add_test(NAME Editor.VirtualGeometryDiagnostics COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.VirtualGeometryDiagnostics PROPERTIES
	LABELS "Editor;VirtualGeometry;Diagnostics"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometry.Editor.Diagnostics"
)

add_test(NAME Editor.ProfilerIdleOverhead COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.ProfilerIdleOverhead PROPERTIES
	LABELS "Editor;Profiler;Performance"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Editor.ProfilerIdleOverhead"
)

add_test(NAME Editor.DenseAnimatedVirtualGeometryFixture COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.DenseAnimatedVirtualGeometryFixture PROPERTIES
	LABELS "Editor;VirtualGeometry;Animation;Fixture"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=VirtualGeometry.Editor.DenseAnimatedFixture"
)

add_test(NAME Editor.CurrentProjectCompatibility COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.CurrentProjectCompatibility PROPERTIES
	LABELS "Editor;Automation;Compatibility"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Editor.Automation.CurrentProjectCompatibility"
)

add_test(NAME Editor.HierarchyVirtualization COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.HierarchyVirtualization PROPERTIES
	LABELS "Editor;Hierarchy;Performance"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Editor.Hierarchy.Virtualization"
)

add_test(NAME Editor.RendererRecovery COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.RendererRecovery PROPERTIES
	LABELS "Editor;RendererRecovery;DeviceLoss"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=RendererRecovery,ExportPackageValidator,VirtualGeometry.Editor.Export,LinuxPlatformExport"
)

add_test(NAME Editor.AdaptiveVirtualGeometry COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.AdaptiveVirtualGeometry PROPERTIES
	LABELS "Editor;VirtualGeometry;AdaptiveVirtualGeometry"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=AdaptiveVirtualGeometry;Editor.AssetPipeline.VirtualGeometry;VirtualGeometry.Editor.Inspector;VirtualGeometry.Editor.Automation;VirtualGeometry.Editor.Export;VirtualGeometry.Editor.DebugView"
)

add_test(NAME Editor.PhysicsWorkflows COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.PhysicsWorkflows PROPERTIES
	LABELS "Editor;Physics;Workflows"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Physics.Editor"
)

add_test(NAME Editor.PhysicsAssetPipeline COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.PhysicsAssetPipeline PROPERTIES
	LABELS "Editor;Physics;AssetPipeline"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Physics.Editor.ColliderCompiler,Physics.Editor.Material"
)

add_test(NAME Editor.PhysicsDiagnostics COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.PhysicsDiagnostics PROPERTIES
	LABELS "Editor;Physics;Diagnostics"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Editor.PhysicsInspector,Editor.PhysicsPrefabUndo,Editor.PhysicsProjectSettings,Physics.SceneView,Physics.Diagnostics"
)

add_test(NAME Editor.PhysicsPerformance COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.PhysicsPerformance PROPERTIES
	LABELS "Editor;Physics;Performance"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Physics.Editor.Performance"
)

add_test(NAME Editor.PhysicsAutomation COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.PhysicsAutomation PROPERTIES
	LABELS "Editor;Physics;Automation"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Editor.MCP.Physics"
)

add_test(NAME Editor.PhysicsExport COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.PhysicsExport PROPERTIES
	LABELS "Editor;Physics;Export"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=PhysicsExport"
)

add_test(NAME Editor.PhysicsSoak COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.PhysicsSoak PROPERTIES
	LABELS "Editor;Physics;Stress;Soak"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Physics.Editor.Soak"
)

# Keep design-time navigation evidence split by authoring, saved-cache/export,
# and automation diagnostics so an installed-editor regression is actionable.
add_test(NAME Editor.Navigation.Authoring COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.Navigation.Authoring PROPERTIES
	LABELS "Editor;Navigation;Authoring;History"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Editor.NavigationAuthoring,Editor.NavigationHistory,Navigation.Editor.Compiler"
)
add_test(NAME Editor.Navigation.DiagnosticsAutomation COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.Navigation.DiagnosticsAutomation PROPERTIES
	LABELS "Editor;Navigation;Diagnostics;Automation"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Editor.Navigation.Diagnostics,Editor.Navigation.Automation,Editor.NavigationProjectLifecycle"
)
add_test(NAME Editor.Navigation.Export COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.Navigation.Export PROPERTIES
	LABELS "Editor;Navigation;Export;Headless"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Editor.NavigationExport,Editor.NavigationExportParity"
)

# Editor animation tests share one durable filter so later authoring and MCP
# stories can add files without reopening this central registration list.
add_test(NAME Editor.Animation COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.Animation PROPERTIES
	LABELS "Editor;Animation"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Animation"
)
if(EXISTS "${CMAKE_SOURCE_DIR}/Tests/Editor/Terrain/TerrainAutomationContractTests.cpp")
	target_compile_definitions(TerraEngine-EditorTests PRIVATE TERRAENGINE_HAS_TERRAIN_AUTOMATION_CONTRACT_TESTS=1)
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/Tests/Editor/Terrain/TerrainAutomationJobTests.cpp")
	target_compile_definitions(TerraEngine-EditorTests PRIVATE TERRAENGINE_HAS_TERRAIN_AUTOMATION_JOB_TESTS=1)
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/Tests/Editor/Terrain/TerrainAutomationParityTests.cpp")
	target_compile_definitions(TerraEngine-EditorTests PRIVATE TERRAENGINE_HAS_TERRAIN_AUTOMATION_PARITY_TESTS=1)
endif()

add_test(NAME Editor.Terrain.Foundation COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.Terrain.Foundation PROPERTIES
	LABELS "Editor;Terrain;Foundation"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Terrain."
)

add_test(NAME Editor.Terrain.CreateSculpt COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.Terrain.CreateSculpt PROPERTIES
	LABELS "Editor;Terrain;Create;Sculpt"
	# The executable uses behavior-specific names; select the real creation,
	# sculpt-session, and preset tests instead of an empty umbrella substring.
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Editor.TerrainCreation,Editor.TerrainSculpt,Terrain.CreateSculpt"
)

add_test(NAME Editor.Terrain.Layers COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.Terrain.Layers PROPERTIES
	LABELS "Editor;Terrain;Surface;Layers"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Editor.TerrainLayers")

add_test(NAME Editor.Terrain.MappingPreview COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.Terrain.MappingPreview PROPERTIES
	LABELS "Editor;Terrain;Mapping;Preview;Transactions"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Editor.TerrainLayers.Mapping,Editor.TerrainLifecycle.Mapping")

add_test(NAME Editor.Terrain.Compatibility COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.Terrain.Compatibility PROPERTIES
	LABELS "Editor;Terrain;Format;Compatibility;Recovery"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Editor.TerrainCreation.TERRAROOT1,Editor.TerrainSaveRecovery.TERRAROOT1")

add_test(NAME Editor.Terrain.CollisionAlignment COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.Terrain.CollisionAlignment PROPERTIES
	LABELS "Editor;Terrain;Physics;Collision"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=TerrainCollision")

add_test(NAME Editor.Terrain.Procedural COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.Terrain.Procedural PROPERTIES
	LABELS "Editor;Terrain;Procedural;Cancellation"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=TerrainProcedural,TerrainSpline")

if(EXISTS "${CMAKE_SOURCE_DIR}/Tests/Editor/Terrain/TerrainAutomationContractTests.cpp"
	OR EXISTS "${CMAKE_SOURCE_DIR}/Tests/Editor/Terrain/TerrainAutomationJobTests.cpp"
	OR EXISTS "${CMAKE_SOURCE_DIR}/Tests/Editor/Terrain/TerrainAutomationParityTests.cpp")
	add_test(NAME Editor.Terrain.Automation COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
	set_tests_properties(Editor.Terrain.Automation PROPERTIES
		LABELS "Editor;Terrain;Automation;Parity;Jobs"
		ENVIRONMENT "TERRAENGINE_TEST_FILTER=TerrainAutomation,Terrain.Automation")
endif()

add_test(NAME Editor.Terrain.History COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.Terrain.History PROPERTIES
	LABELS "Editor;Terrain;History"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Editor.TerrainHistory"
)

add_test(NAME Editor.Terrain.Resize COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.Terrain.Resize PROPERTIES
	LABELS "Editor;Terrain;Resize;Expansion;Crop;History"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Editor.TerrainResize"
)

add_test(NAME Editor.Terrain.SaveRecovery COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.Terrain.SaveRecovery PROPERTIES
	LABELS "Editor;Terrain;Save;Recovery;Lifecycle"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Editor.TerrainSaveRecovery,Editor.TerrainLifecycle"
)

add_test(NAME Editor.Terrain.LargeWorld COMMAND $<TARGET_FILE:TerraEngine-EditorTests>)
set_tests_properties(Editor.Terrain.LargeWorld PROPERTIES
	LABELS "Editor;Terrain;LargeWorld;Soak"
	ENVIRONMENT "TERRAENGINE_TEST_FILTER=Editor.TerrainLargeWorldSoak"
)
