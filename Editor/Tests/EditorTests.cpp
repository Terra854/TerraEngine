#include "TestHarness.h"

#include <iostream>

namespace TerraEngine::EditorTests {
	void RegisterArchiveSerializationTests(TestRegistry& registry);
	void RegisterBuildProfileCollectionTests(TestRegistry& registry);
	void RegisterBuildProfileEdgeTests(TestRegistry& registry);
	void RegisterBuildProfileRoundTripTests(TestRegistry& registry);
	void RegisterExportPathNormalizerTests(TestRegistry& registry);
	void RegisterExportPackageValidatorTests(TestRegistry& registry);
	void RegisterExportScriptBindingRewriterTests(TestRegistry& registry);
	void RegisterExportValidatorManagedSourceTests(TestRegistry& registry);
	void RegisterExportValidatorTests(TestRegistry& registry);
	void RegisterPackRoundTripTests(TestRegistry& registry);
	void RegisterPackagingRequestValidationTests(TestRegistry& registry);
	void RegisterProfilerCaptureStoreTests(TestRegistry& registry);
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
	TerraEngine::EditorTests::RegisterExportScriptBindingRewriterTests(registry);
	TerraEngine::EditorTests::RegisterExportValidatorManagedSourceTests(registry);
	TerraEngine::EditorTests::RegisterExportValidatorTests(registry);
	TerraEngine::EditorTests::RegisterPackRoundTripTests(registry);
	TerraEngine::EditorTests::RegisterPackagingRequestValidationTests(registry);
	TerraEngine::EditorTests::RegisterProfilerCaptureStoreTests(registry);

	return registry.RunAll(std::cout);
}
