#include "TestHarness.h"

#include "GltfPackageImporter.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests {
	void RegisterGltfUnsupportedFeatureTests(TestRegistry& registry) {
		registry.Add("Editor.AssetPipeline.GltfUnsupportedFeature.WarnsForIgnoredCamera", [](TestContext& context) {
			const std::filesystem::path sourceRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			const std::filesystem::path baseFixture = sourceRoot / "Tests/Editor/AssetPipeline/Fixtures/Gltf/one-material.gltf";
			const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "TerraEngineGltfUnsupportedTests";
			const std::filesystem::path fixturePath = tempRoot / "camera.gltf";
			std::filesystem::create_directories(tempRoot);

			std::ifstream in(baseFixture);
			nlohmann::json document = nlohmann::json::parse(in);
			document["cameras"] = nlohmann::json::array({ {{"type", "perspective"}, {"perspective", {{"yfov", 0.7}, {"znear", 0.1}}}} });
			document["nodes"].push_back({{"camera", 0}});
			document["scenes"][0]["nodes"].push_back(document["nodes"].size() - 1u);
			std::ofstream out(fixturePath);
			out << document.dump(2);
			out.close();

			TerraEngine::AssetPipeline::GltfPackageImportOptions options{};
			options.sourcePath = fixturePath;
			options.assetRoot = tempRoot / "Assets";
			options.buildRoot = tempRoot / "Build";
			options.sourceRelativePath = "Models/camera.gltf";
			options.packageGuid = TerraEngine::GenerateUUID();

			const auto result = TerraEngine::AssetPipeline::ImportGltfPackage(options);
			bool sawCameraDiagnostic = false;
			for (const auto& diagnostic : result.diagnostics) {
				sawCameraDiagnostic = sawCameraDiagnostic || diagnostic.code == "UnsupportedCameraIgnored";
			}
			context.Expect(result.status == TerraEngine::AssetPipeline::GltfImportStatus::SucceededWithWarnings, "Unsupported camera should downgrade to a warning.");
			context.Expect(sawCameraDiagnostic, "Camera skip should be visible in diagnostics.");
		});
	}
}
