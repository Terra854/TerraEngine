# Terrain foundation build registration.
#
# Keep this list explicit and existence-guarded while the foundation lands in
# stages. Later authoring, rendering, and editor-story files must opt in from
# their own implementation task instead of being pulled into every build by a
# broad directory glob.
set(TERRAENGINE_TERRAIN_ENGINE_SOURCES)
set(TERRAENGINE_TERRAIN_ENGINE_HEADERS)
set(TERRAENGINE_TERRAIN_ENGINE_TEST_SOURCES)
set(TERRAENGINE_TERRAIN_EDITOR_TEST_SOURCES)
set(TERRAENGINE_TERRAIN_EDITOR_SOURCES)
set(TERRAENGINE_TERRAIN_RUNTIME_SHADER_SOURCES)
set(TERRAENGINE_TERRAIN_EDITOR_SHADER_SOURCES)
set(TERRAENGINE_TERRAIN_COMPILER_SOURCES)
set(TERRAENGINE_TERRAIN_STRESS_SOURCE Tests/Tools/Terrain/TerrainTransactionStress.cpp)
set(TERRAENGINE_TERRAIN_FIXTURE_SOURCE Tests/Tools/Terrain/TerrainLargeWorldFixture.cpp)
set(TERRAENGINE_TERRAIN_BENCHMARK_SOURCE Tests/Tools/Terrain/TerraEngineTerrainBenchmark.cpp)
set(TERRAENGINE_TERRAIN_AUTOMATION_RUNNER Tests/Tools/Terrain/RunTerrainValidation.ps1)
set(TERRAENGINE_TERRAIN_PHYSICS_SOURCES)
set(TERRAENGINE_TERRAIN_PHYSICS_HEADERS)

set(_TERRAENGINE_TERRAIN_ENGINE_FILES
	Engine/Terrain/TerrainTypes.cpp
	Engine/Terrain/TerrainTypes.h
	Engine/Terrain/TerrainAreaCodec.cpp
	Engine/Terrain/TerrainAreaCodec.h
	Engine/Terrain/TerrainAssetStore.cpp
	Engine/Terrain/TerrainAssetStore.h
	Engine/Terrain/TerrainEditTransaction.cpp
	Engine/Terrain/TerrainEditTransaction.h
	Engine/Terrain/TerrainMutationQueue.cpp
	Engine/Terrain/TerrainMutationQueue.h
	Engine/Terrain/TerrainPublication.cpp
	Engine/Terrain/TerrainPublication.h
	Engine/Terrain/TerrainStreamingSystem.cpp
	Engine/Terrain/TerrainStreamingSystem.h
	Engine/Terrain/TerrainDerivedSystem.cpp
	Engine/Terrain/TerrainDerivedSystem.h
	Engine/Terrain/TerrainDiagnostics.cpp
	Engine/Terrain/TerrainDiagnostics.h
	Engine/Terrain/TerrainGarbageCollector.cpp
	Engine/Terrain/TerrainGarbageCollector.h
	Engine/Terrain/TerrainSpatialProxy.cpp
	Engine/Terrain/TerrainSpatialProxy.h
	Engine/Components/TerrainComponent.h
	Engine/File Formats/TerraEngineTerrainFile.h
	Engine/Runtime/ContentMount/TerrainContentResolver.cpp
	Engine/Runtime/ContentMount/TerrainContentResolver.h
	Engine/Renderer/Terrain/TerrainPageCooker.cpp
	Engine/Renderer/Terrain/TerrainPageCooker.h
	Engine/Renderer/Terrain/TerrainRenderPackets.cpp
	Engine/Renderer/Terrain/TerrainRenderPackets.h
	Engine/Terrain/TerrainLayerStack.cpp
	Engine/Terrain/TerrainLayerStack.h
	Engine/Renderer/Terrain/TerrainSurfaceCache.cpp
	Engine/Renderer/Terrain/TerrainSurfaceCache.h
	Engine/Renderer/Terrain/TerrainDistantProxy.cpp
	Engine/Renderer/Terrain/TerrainDistantProxy.h
	Engine/Terrain/TerrainSelectionQuery.cpp
	Engine/Terrain/TerrainSelectionQuery.h
)
foreach(_terrain_file IN LISTS _TERRAENGINE_TERRAIN_ENGINE_FILES)
	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_terrain_file}")
		if(_terrain_file MATCHES "\\.(cpp|cc|cxx)$")
			list(APPEND TERRAENGINE_TERRAIN_ENGINE_SOURCES "${_terrain_file}")
		else()
			list(APPEND TERRAENGINE_TERRAIN_ENGINE_HEADERS "${_terrain_file}")
		endif()
	endif()
endforeach()

set(_TERRAENGINE_TERRAIN_PHYSICS_FILES
	Engine/Physics/Terrain/TerrainCollisionSystem.cpp
	Engine/Physics/Terrain/TerrainCollisionSystem.h
)
foreach(_terrain_file IN LISTS _TERRAENGINE_TERRAIN_PHYSICS_FILES)
	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_terrain_file}")
		if(_terrain_file MATCHES "\\.(cpp|cc|cxx)$")
			list(APPEND TERRAENGINE_TERRAIN_PHYSICS_SOURCES "${_terrain_file}")
		else()
			list(APPEND TERRAENGINE_TERRAIN_PHYSICS_HEADERS "${_terrain_file}")
		endif()
	endif()
endforeach()

list(APPEND TERRAENGINE_TERRAIN_COMPILER_SOURCES
	Editor/TerraEngineTerrainCompiler.cpp
	Editor/AssetCompilers/TerrainCompiler.cpp)

set(_TERRAENGINE_TERRAIN_EDITOR_FILES
	Editor/AssetCompilers/TerrainCompiler.cpp
	Editor/AssetCompilers/TerrainCompiler.h
	Editor/TerraEngine-Editor/Terrain/TerrainBrushPresetStore.cpp
	Editor/TerraEngine-Editor/Terrain/TerrainBrushPresetStore.h
	Editor/TerraEngine-Editor/Terrain/TerrainBrushEngine.cpp
	Editor/TerraEngine-Editor/Terrain/TerrainBrushEngine.h
	Editor/TerraEngine-Editor/Terrain/TerrainAuthoringSession.cpp
	Editor/TerraEngine-Editor/Terrain/TerrainAuthoringSession.h
	Editor/TerraEngine-Editor/Terrain/TerrainPicking.cpp
	Editor/TerraEngine-Editor/Terrain/TerrainPicking.h
	Editor/TerraEngine-Editor/Terrain/TerrainCreationService.cpp
	Editor/TerraEngine-Editor/Terrain/TerrainCreationService.h
	Editor/TerraEngine-Editor/Terrain/TerrainAuthoringSystem.cpp
	Editor/TerraEngine-Editor/Terrain/TerrainAuthoringSystem.h
	Editor/TerraEngine-Editor/Terrain/TerrainHistoryCommand.cpp
	Editor/TerraEngine-Editor/Terrain/TerrainHistoryCommand.h
	Editor/TerraEngine-Editor/Terrain/TerrainDirtyState.cpp
	Editor/TerraEngine-Editor/Terrain/TerrainDirtyState.h
	Editor/TerraEngine-Editor/Windows/TerrainWindow.cpp
	Editor/TerraEngine-Editor/Windows/TerrainWindow.h
	Editor/TerraEngine-Editor/Terrain/TerrainSurfaceBrush.cpp
	Editor/TerraEngine-Editor/Terrain/TerrainSurfaceBrush.h
	Editor/TerraEngine-Editor/Terrain/TerrainHoleBrush.cpp
	Editor/TerraEngine-Editor/Terrain/TerrainHoleBrush.h
	Editor/TerraEngine-Editor/Terrain/TerrainProceduralOperators.cpp
	Editor/TerraEngine-Editor/Terrain/TerrainProceduralOperators.h
	Editor/TerraEngine-Editor/Terrain/TerrainHydraulicErosion.cpp
	Editor/TerraEngine-Editor/Terrain/TerrainHydraulicErosion.h
	Editor/TerraEngine-Editor/Terrain/TerrainThermalErosion.cpp
	Editor/TerraEngine-Editor/Terrain/TerrainThermalErosion.h
	Editor/TerraEngine-Editor/Terrain/TerrainSplineGrader.cpp
	Editor/TerraEngine-Editor/Terrain/TerrainSplineGrader.h
	Editor/TerraEngine-Editor/Terrain/TerrainProceduralJobs.cpp
	Editor/TerraEngine-Editor/Terrain/TerrainProceduralJobs.h
)
foreach(_terrain_file IN LISTS _TERRAENGINE_TERRAIN_EDITOR_FILES)
	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_terrain_file}")
		list(APPEND TERRAENGINE_TERRAIN_EDITOR_SOURCES "${_terrain_file}")
	endif()
endforeach()

# Phase 9 automation implementation files are deliberately opt-in. This keeps
# a partially landed provider from changing the editor target until its paired
# header/source is present.
set(_TERRAENGINE_TERRAIN_AUTOMATION_EDITOR_FILES
	Editor/TerraEngine-Editor/Automation/TerrainAutomationSchema.cpp
	Editor/TerraEngine-Editor/Automation/TerrainAutomationSchema.h
	Editor/TerraEngine-Editor/Automation/Providers/TerrainAutomationProvider.cpp
	Editor/TerraEngine-Editor/Automation/Providers/TerrainAutomationProvider.h
	Editor/TerraEngine-Editor/Terrain/TerrainAutomationAdapter.cpp
	Editor/TerraEngine-Editor/Terrain/TerrainAutomationAdapter.h
)
foreach(_terrain_file IN LISTS _TERRAENGINE_TERRAIN_AUTOMATION_EDITOR_FILES)
	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_terrain_file}")
		list(APPEND TERRAENGINE_TERRAIN_EDITOR_SOURCES "${_terrain_file}")
	endif()
endforeach()

set(_TERRAENGINE_TERRAIN_ENGINE_TEST_FILES
	Tests/Engine/Terrain/TerrainCoordinateTests.cpp
	Tests/Engine/Terrain/TerrainFormatTests.cpp
	Tests/Engine/Terrain/TerrainAssetIntegrationTests.cpp
	Tests/Engine/Terrain/TerrainSchedulerContractTests.cpp
	Tests/Engine/Terrain/TerrainTransactionTests.cpp
	Tests/Engine/Terrain/TerrainBrushKernelTests.cpp
	Tests/Engine/Terrain/TerrainPageCookerTests.cpp
	Tests/Engine/Terrain/TerrainRenderSystemIntegrationTests.cpp
	Tests/Engine/Terrain/TerrainAtomicEditTests.cpp
	Tests/Engine/Terrain/TerrainLayerWeightTests.cpp
	Tests/Engine/Terrain/TerrainSurfaceRenderingTests.cpp
	Tests/Engine/Terrain/TerrainLargeWorldCoordinateTests.cpp
	Tests/Engine/Terrain/TerrainResidencyTests.cpp
	Tests/Engine/Terrain/TerrainLodSeamTests.cpp
	Tests/Engine/Terrain/TerrainHoleTests.cpp
	Tests/Engine/Terrain/TerrainHeightFieldRecipeTests.cpp
	Tests/Engine/Terrain/TerrainCollisionPublicationTests.cpp
	Tests/Engine/Terrain/TerrainProceduralOperationTests.cpp
	Tests/Engine/Terrain/TerrainDiagnosticsTests.cpp
	Tests/Engine/Terrain/TerrainAutomationDiagnosticTests.cpp
	Tests/Engine/Terrain/TerrainMultithreadedStressTests.cpp
	Tests/Engine/Terrain/TerrainExportRuntimeTests.cpp
)
foreach(_terrain_test IN LISTS _TERRAENGINE_TERRAIN_ENGINE_TEST_FILES)
	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_terrain_test}")
		# Test subdirectories consume this variable from a different source
		# directory, so keep each path absolute instead of resolving it twice.
		list(APPEND TERRAENGINE_TERRAIN_ENGINE_TEST_SOURCES
			"${CMAKE_CURRENT_SOURCE_DIR}/${_terrain_test}")
	endif()
endforeach()

set(_TERRAENGINE_TERRAIN_EDITOR_TEST_FILES
	Tests/Editor/Terrain/TerrainAssetDatabaseTests.cpp
	Tests/Editor/Terrain/TerrainBrushPresetStoreTests.cpp
	Tests/Editor/Terrain/TerrainCreationImportTests.cpp
	Tests/Editor/Terrain/TerrainSculptWorkflowTests.cpp
	Tests/Editor/Terrain/TerrainHistoryTests.cpp
	Tests/Editor/Terrain/TerrainSaveRecoveryTests.cpp
	Tests/Editor/Terrain/TerrainEditLifecycleTests.cpp
	Tests/Editor/Terrain/TerrainFailureContainmentTests.cpp
	Tests/Editor/Terrain/TerrainLayerWorkflowTests.cpp
	Tests/Editor/Terrain/TerrainLargeWorldSoakContractTests.cpp
	Tests/Editor/Terrain/TerrainCollisionAlignmentTests.cpp
	Tests/Editor/Terrain/TerrainProceduralJobTests.cpp
	Tests/Editor/Terrain/TerrainSplineGradingTests.cpp
	Tests/Editor/Terrain/TerrainAutomationContractTests.cpp
	Tests/Editor/Terrain/TerrainAutomationJobTests.cpp
	Tests/Editor/Terrain/TerrainAutomationParityTests.cpp
)
foreach(_terrain_test IN LISTS _TERRAENGINE_TERRAIN_EDITOR_TEST_FILES)
	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_terrain_test}")
		list(APPEND TERRAENGINE_TERRAIN_EDITOR_TEST_SOURCES
			"${CMAKE_CURRENT_SOURCE_DIR}/${_terrain_test}")
	endif()
endforeach()

set(TERRAENGINE_TERRAIN_CONTRACT_FILES)
file(GLOB _terrain_contract_files CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/specs/053-terrain-authoring-foundation/contracts/*")
if(_terrain_contract_files)
	list(APPEND TERRAENGINE_TERRAIN_CONTRACT_FILES ${_terrain_contract_files})
endif()

file(GLOB_RECURSE _terrain_runtime_shader_files CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/Engine/Shaders/Terrain/*")
if(_terrain_runtime_shader_files)
	list(APPEND TERRAENGINE_TERRAIN_RUNTIME_SHADER_SOURCES ${_terrain_runtime_shader_files})
endif()
file(GLOB_RECURSE _terrain_editor_shader_files CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/Editor/Shaders/Terrain/*")
if(_terrain_editor_shader_files)
	list(APPEND TERRAENGINE_TERRAIN_EDITOR_SHADER_SOURCES ${_terrain_editor_shader_files})
endif()
