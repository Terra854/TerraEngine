#include "TestHarness.h"

#include <iostream>

namespace TerraEngine::EditorTests {
	void RegisterArchiveSerializationTests(TestRegistry& registry);
	void RegisterBuildProfileCollectionTests(TestRegistry& registry);
	void RegisterBuildProfileEdgeTests(TestRegistry& registry);
	void RegisterBuildProfileRoundTripTests(TestRegistry& registry);
	void RegisterExportPathNormalizerTests(TestRegistry& registry);
	void RegisterExportPackageValidatorTests(TestRegistry& registry);
	void RegisterExportRuntimeBaselineSampleTests(TestRegistry& registry);
	void RegisterExportScriptBindingRewriterTests(TestRegistry& registry);
	void RegisterExportValidatorManagedSourceTests(TestRegistry& registry);
	void RegisterExportValidatorTests(TestRegistry& registry);
	void RegisterLinuxPlatformExportTests(TestRegistry& registry);
	void RegisterMaterialExportValidationTests(TestRegistry& registry);
	void RegisterMaterialPreviewContractTests(TestRegistry& registry);
	void RegisterPackRoundTripTests(TestRegistry& registry);
	void RegisterPackagingRequestValidationTests(TestRegistry& registry);
	void RegisterPrefabAuthoringWorkflowTests(TestRegistry& registry);
	void RegisterProfilerCaptureStoreTests(TestRegistry& registry);
	void RegisterResidencyPolicyExportTests(TestRegistry& registry);
	void RegisterSchedulerPlayModeGraphTests(TestRegistry& registry);
	void RegisterLinuxScriptCompilationTests(TestRegistry& registry);
	void RegisterLinuxScriptEditorLaunchTests(TestRegistry& registry);
	void RegisterLinuxScriptWorkspaceTests(TestRegistry& registry);
	void RegisterSceneRuntimeParityTests(TestRegistry& registry);
}

int main() {
	TerraEngine::EditorTests::TestRegistry registry;

	registry.Add("Editor.Smoke.HarnessRuns", [](TerraEngine::EditorTests::TestContext& context) {
		context.Expect(true, "The editor test harness should execute registered tests.");
	});

	TerraEngine::EditorTests::RegisterArchiveSerializationTests(registry);
	TerraEngine::EditorTests::RegisterBuildProfileCollectionTests(registry);
	TerraEngine::EditorTests::RegisterBuildProfileEdgeTests(registry);
	TerraEngine::EditorTests::RegisterBuildProfileRoundTripTests(registry);
	TerraEngine::EditorTests::RegisterExportPathNormalizerTests(registry);
	TerraEngine::EditorTests::RegisterExportPackageValidatorTests(registry);
	TerraEngine::EditorTests::RegisterExportRuntimeBaselineSampleTests(registry);
	TerraEngine::EditorTests::RegisterExportScriptBindingRewriterTests(registry);
	TerraEngine::EditorTests::RegisterExportValidatorManagedSourceTests(registry);
	TerraEngine::EditorTests::RegisterExportValidatorTests(registry);
	TerraEngine::EditorTests::RegisterLinuxPlatformExportTests(registry);
	TerraEngine::EditorTests::RegisterMaterialExportValidationTests(registry);
	TerraEngine::EditorTests::RegisterMaterialPreviewContractTests(registry);
	TerraEngine::EditorTests::RegisterPackRoundTripTests(registry);
	TerraEngine::EditorTests::RegisterPackagingRequestValidationTests(registry);
	TerraEngine::EditorTests::RegisterPrefabAuthoringWorkflowTests(registry);
	TerraEngine::EditorTests::RegisterProfilerCaptureStoreTests(registry);
	TerraEngine::EditorTests::RegisterResidencyPolicyExportTests(registry);
	TerraEngine::EditorTests::RegisterSchedulerPlayModeGraphTests(registry);
	TerraEngine::EditorTests::RegisterLinuxScriptCompilationTests(registry);
	TerraEngine::EditorTests::RegisterLinuxScriptEditorLaunchTests(registry);
	TerraEngine::EditorTests::RegisterLinuxScriptWorkspaceTests(registry);
	TerraEngine::EditorTests::RegisterSceneRuntimeParityTests(registry);

	return registry.RunAll(std::cout);
}
