#include "TestHarness.h"

#include <iostream>

namespace TerraEngine::Tests {
	void RegisterArchiveSerializationTests(TestRegistry& registry);
	void RegisterBinaryIOTests(TestRegistry& registry);
	void RegisterBootSummaryValidationTests(TestRegistry& registry);
	void RegisterCodecProviderTests(TestRegistry& registry);
	void RegisterContentMountPathNormalizerTests(TestRegistry& registry);
	void RegisterContentMountPublicApiTests(TestRegistry& registry);
	void RegisterExportRuntimeSmokeTests(TestRegistry& registry);
	void RegisterExportRuntimeBaselineLaunchTests(TestRegistry& registry);
	void RegisterExportStartupFlowTests(TestRegistry& registry);
	void RegisterExportScriptingRuntimeTests(TestRegistry& registry);
	void RegisterMaterialRuntimeContractTests(TestRegistry& registry);
	void RegisterObjectManagerTests(TestRegistry& registry);
	void RegisterPrefabCompositionTests(TestRegistry& registry);
	void RegisterProfilerCaptureTests(TestRegistry& registry);
	void RegisterRenderTypesTests(TestRegistry& registry);
	void RegisterRenderProfileBootSummaryTests(TestRegistry& registry);
	void RegisterSceneRuntimeParityTests(TestRegistry& registry);
	void RegisterSceneGraphRuntimeTests(TestRegistry& registry);
	void RegisterSceneMutationQueueTests(TestRegistry& registry);
	void RegisterSpatialBoundsTests(TestRegistry& registry);
	void RegisterSpatialIndexEdgeTests(TestRegistry& registry);
	void RegisterSpatialIndexQueryTests(TestRegistry& registry);
	void RegisterSpatialIndexRefreshTests(TestRegistry& registry);
	void RegisterUIElementTests(TestRegistry& registry);
	void RegisterUILayoutEngineTests(TestRegistry& registry);
	void RegisterUIParserTests(TestRegistry& registry);
	void RegisterUIStyleTests(TestRegistry& registry);
	void RegisterThreadPoolSingletonTests(TestRegistry& registry);
	void RegisterWorkerPoolTests(TestRegistry& registry);
}

int main() {
	TerraEngine::Tests::TestRegistry registry;

	registry.Add("Engine.Smoke.HarnessRuns", [](TerraEngine::Tests::TestContext& context) {
		context.Expect(true, "The engine test harness should execute registered tests.");
	});

	TerraEngine::Tests::RegisterArchiveSerializationTests(registry);
	TerraEngine::Tests::RegisterBinaryIOTests(registry);
	TerraEngine::Tests::RegisterBootSummaryValidationTests(registry);
	TerraEngine::Tests::RegisterCodecProviderTests(registry);
	TerraEngine::Tests::RegisterContentMountPathNormalizerTests(registry);
	TerraEngine::Tests::RegisterContentMountPublicApiTests(registry);
	TerraEngine::Tests::RegisterExportRuntimeSmokeTests(registry);
	TerraEngine::Tests::RegisterExportRuntimeBaselineLaunchTests(registry);
	TerraEngine::Tests::RegisterExportStartupFlowTests(registry);
	TerraEngine::Tests::RegisterExportScriptingRuntimeTests(registry);
	TerraEngine::Tests::RegisterMaterialRuntimeContractTests(registry);
	TerraEngine::Tests::RegisterWorkerPoolTests(registry);
	TerraEngine::Tests::RegisterThreadPoolSingletonTests(registry);
	TerraEngine::Tests::RegisterObjectManagerTests(registry);
	TerraEngine::Tests::RegisterPrefabCompositionTests(registry);
	TerraEngine::Tests::RegisterProfilerCaptureTests(registry);
	TerraEngine::Tests::RegisterRenderTypesTests(registry);
	TerraEngine::Tests::RegisterRenderProfileBootSummaryTests(registry);
	TerraEngine::Tests::RegisterSceneRuntimeParityTests(registry);
	TerraEngine::Tests::RegisterSceneGraphRuntimeTests(registry);
	TerraEngine::Tests::RegisterSceneMutationQueueTests(registry);
	TerraEngine::Tests::RegisterSpatialBoundsTests(registry);
	TerraEngine::Tests::RegisterSpatialIndexEdgeTests(registry);
	TerraEngine::Tests::RegisterSpatialIndexQueryTests(registry);
	TerraEngine::Tests::RegisterSpatialIndexRefreshTests(registry);
	TerraEngine::Tests::RegisterUIElementTests(registry);
	TerraEngine::Tests::RegisterUILayoutEngineTests(registry);
	TerraEngine::Tests::RegisterUIParserTests(registry);
	TerraEngine::Tests::RegisterUIStyleTests(registry);

	return registry.RunAll(std::cout);
}
