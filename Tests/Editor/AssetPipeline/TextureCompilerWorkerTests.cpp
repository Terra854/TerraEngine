#include "TestHarness.h"

#include "TextureCompiler.h"

namespace TerraEngine::EditorTests {
	void RegisterTextureCompilerWorkerTests(TestRegistry& registry) {
		registry.Add("Editor.AssetPipeline.TextureCompiler.CapsBasisuWorkers", [](TestContext& context) {
			context.ExpectEqual(1u, ResolveTextureCompilerWorkerCountForTests(0u, nullptr), "Worker policy should stay valid when hardware concurrency is unavailable.");
			context.ExpectEqual(1u, ResolveTextureCompilerWorkerCountForTests(1u, nullptr), "Single-core systems should not request extra compression workers.");
			context.ExpectEqual(2u, ResolveTextureCompilerWorkerCountForTests(16u, nullptr), "Default texture import should avoid all-core BasisU pools.");
			context.ExpectEqual(3u, ResolveTextureCompilerWorkerCountForTests(16u, "3"), "Explicit worker overrides should be honored when inside the safety cap.");
			context.ExpectEqual(4u, ResolveTextureCompilerWorkerCountForTests(16u, "64"), "Explicit worker overrides should clamp to the safety cap.");
			context.ExpectEqual(2u, ResolveTextureCompilerWorkerCountForTests(2u, "64"), "Worker overrides should not exceed available hardware threads.");
			context.ExpectEqual(2u, ResolveTextureCompilerWorkerCountForTests(16u, "not-a-number"), "Invalid overrides should fall back to the conservative default.");
		});
	}
}
