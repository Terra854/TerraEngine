#include "TestHarness.h"

#include "GltfPackageImporter.h"

#include <filesystem>
#include <fstream>

namespace TerraEngine::EditorTests {
	void RegisterGltfImportFailureTests(TestRegistry& registry) {
		registry.Add("Editor.AssetPipeline.GltfImportFailure.FailsWithoutGeometry", [](TestContext& context) {
			const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "TerraEngineGltfFailureTests";
			const std::filesystem::path fixturePath = tempRoot / "no-geometry.gltf";
			std::filesystem::create_directories(tempRoot);
			std::ofstream out(fixturePath);
			out << R"({"asset":{"version":"2.0"},"nodes":[{}],"scenes":[{"nodes":[0]}],"scene":0})";
			out.close();

			TerraEngine::AssetPipeline::GltfPackageImportOptions options{};
			options.sourcePath = fixturePath;
			options.assetRoot = tempRoot / "Assets";
			options.buildRoot = tempRoot / "Build";
			options.sourceRelativePath = "Models/no-geometry.gltf";
			options.packageGuid = TerraEngine::GenerateUUID();

			const auto result = TerraEngine::AssetPipeline::ImportGltfPackage(options);
			context.Expect(result.status == TerraEngine::AssetPipeline::GltfImportStatus::Failed, "No-geometry package should fail import.");
			context.Expect(!result.diagnostics.empty(), "Failure should include diagnostics.");
		});
	}
}
