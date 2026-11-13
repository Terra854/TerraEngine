// The review harness uses production editor implementations and explicit,
// independently selectable regressions. Tests own any UI/runtime initialization.
#include "TestHarness.h"
#include "Core/Logger/Logger.h"
#include <iostream>

namespace TerraEngine::EditorTests {
	void RegisterProfilerOperationFailureStatusTests(TestRegistry&);
	void RegisterRawMeshStagingTests(TestRegistry&);
	void RegisterShaderCompilerArgumentTests(TestRegistry&);
	void RegisterCompilerPipeOwnershipTests(TestRegistry&);
	void RegisterAssemblyIdentityCollisionTests(TestRegistry&);
	void RegisterAssetOrganizationAtomicityTests(TestRegistry&);
	void RegisterAnimationJobRetryLifecycleTests(TestRegistry&);
	void RegisterProjectPathBoundaryTests(TestRegistry&);
	void RegisterCapabilityFailureRecoveryTests(TestRegistry&);
	void RegisterScriptWorkspaceConcurrencyTests(TestRegistry&);
	void RegisterLayoutPreferenceMigrationTests(TestRegistry&);
	void RegisterRendererSessionAdmissionTests(TestRegistry&);
	void RegisterAuthoringReplacementSafetyTests(TestRegistry&);
	void RegisterTerrainPersistenceRoundTripTests(TestRegistry&);
	void RegisterProceduralPreviewCancellationTests(TestRegistry&);
	void RegisterHierarchyEditInputTests(TestRegistry&);
	void RegisterBuildProfileCollectionIdentityTests(TestRegistry&);
	void RegisterExportCancellationPublicationTests(TestRegistry&);
	void RegisterExportPublicationRollbackTests(TestRegistry&);
	void RegisterAuthoringControlContinuityTests(TestRegistry&);
	void RegisterUIBuilderReferencePersistenceTests(TestRegistry&);
	void RegisterDeformationPageBoundaryTests(TestRegistry&);
	void RegisterSceneViewGenerationTests(TestRegistry&);
}

int main() {
	using namespace TerraEngine::EditorTests;
	TestRegistry registry;
	RegisterRawMeshStagingTests(registry);
	RegisterShaderCompilerArgumentTests(registry);
	RegisterCompilerPipeOwnershipTests(registry);
	RegisterAssemblyIdentityCollisionTests(registry);
	RegisterAssetOrganizationAtomicityTests(registry);
	RegisterAnimationJobRetryLifecycleTests(registry);
	RegisterProjectPathBoundaryTests(registry);
	RegisterCapabilityFailureRecoveryTests(registry);
	RegisterScriptWorkspaceConcurrencyTests(registry);
	RegisterLayoutPreferenceMigrationTests(registry);
	RegisterRendererSessionAdmissionTests(registry);
	RegisterAuthoringReplacementSafetyTests(registry);
	RegisterTerrainPersistenceRoundTripTests(registry);
	RegisterProceduralPreviewCancellationTests(registry);
	RegisterHierarchyEditInputTests(registry);
	RegisterBuildProfileCollectionIdentityTests(registry);
	RegisterExportCancellationPublicationTests(registry);
	RegisterExportPublicationRollbackTests(registry);
	RegisterAuthoringControlContinuityTests(registry);
	RegisterUIBuilderReferencePersistenceTests(registry);
	RegisterDeformationPageBoundaryTests(registry);
	RegisterSceneViewGenerationTests(registry);
	RegisterProfilerOperationFailureStatusTests(registry);
	const int result = registry.RunAll(std::cout);
	Logger::Instance().Shutdown();
	std::cout.flush();
	return result;
}
