#include "TestHarness.h"

#include "GltfPackageImporter.h"
#include "Engine/File Formats/TerraEngineMaterialFile.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests {
	namespace {
		TerraEngine::AssetPipeline::GltfImportResult ImportFixture(std::string_view fixtureName) {
			const std::filesystem::path sourceRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			const std::filesystem::path fixturePath = sourceRoot / "Tests/Editor/AssetPipeline/Fixtures/Gltf" / fixtureName;
			const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "TerraEngineGltfMaterialTests" / fixtureName;

			TerraEngine::AssetPipeline::GltfPackageImportOptions options{};
			options.sourcePath = fixturePath;
			options.assetRoot = tempRoot / "Assets";
			options.buildRoot = tempRoot / "Build";
			options.sourceRelativePath = std::filesystem::path("Models") / fixtureName;
			options.packageGuid = TerraEngine::GenerateUUID();
			return TerraEngine::AssetPipeline::ImportGltfPackage(options);
		}
	}

	void RegisterGltfMaterialTextureTests(TestRegistry& registry) {
		registry.Add("Editor.AssetPipeline.GltfMaterialTexture.PreservesAuthoredAlphaCoverage", [](TestContext& context) {
			// Import real glTF bytes, then inspect the cooked TMAT headers used by
			// camera and shadow shaders. A source-only check would miss lost settings.
			const auto root = std::filesystem::temp_directory_path() / ("TerraEngineAlphaImport-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID()));
			struct Cleanup {
				std::filesystem::path root;
				~Cleanup() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
			} cleanup{root};
			std::filesystem::create_directories(root);
			std::ifstream fixture(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Tests/Editor/AssetPipeline/Fixtures/Gltf/embedded-texture.gltf");
			auto document = nlohmann::json::parse(fixture);
			const auto base = document.at("materials").at(0);
			document["materials"] = nlohmann::json::array();
			const std::array modes{AlphaMode::Mask, AlphaMode::Mask, AlphaMode::Opaque, AlphaMode::Blend};
			const std::array cutoffs{0.25f, 0.75f, 0.5f, 0.5f};
			for(std::size_t index = 0; index < modes.size(); ++index) {
				auto material = base;
				material["name"] = "AlphaMaterial" + std::to_string(index);
				material["alphaMode"] = modes[index] == AlphaMode::Mask ? "MASK" : modes[index] == AlphaMode::Blend ? "BLEND" : "OPAQUE";
				material["alphaCutoff"] = cutoffs[index];
				document["materials"].push_back(std::move(material));
			}
			const auto source = root / "alpha.gltf";
			{ std::ofstream output(source); output << document.dump(); }
			TerraEngine::AssetPipeline::GltfPackageImportOptions options{};
			options.sourcePath = source;
			options.assetRoot = root / "Assets";
			options.buildRoot = root / "Build";
			options.sourceRelativePath = "Models/alpha.gltf";
			options.packageGuid = TerraEngine::GenerateUUID();
			const auto result = TerraEngine::AssetPipeline::ImportGltfPackage(options);
			context.Expect(result.status != TerraEngine::AssetPipeline::GltfImportStatus::Failed, "Alpha material fixture must complete real package import.");
			context.ExpectEqual(modes.size(), result.generatedMaterials.size(), "All authored opacity classes must produce native materials.");
			for(std::size_t index = 0; index < modes.size(); ++index) {
				MaterialHeader header{};
				std::ifstream cooked(options.buildRoot / result.generatedMaterials[index].compiledPath, std::ios::binary);
				cooked.read(reinterpret_cast<char*>(&header), sizeof(header));
				context.Expect(cooked.good(), "Cooked material must contain a complete native header.");
				context.ExpectEqual(static_cast<std::uint32_t>(modes[index]), header.alphaMode, "Cooked alpha mode must preserve authored OPAQUE/MASK/BLEND semantics.");
				context.ExpectEqual(cutoffs[index], header.alphaCutoff, "Cooked cutoff must preserve both initial and changed MASK thresholds.");
				context.Expect(header.textureCount != 0, "Alpha coverage texture binding must survive material cooking.");
			}
		});
		registry.Add("Editor.AssetPipeline.GltfMaterialTexture.RecordsEmbeddedBaseColorTexture", [](TestContext& context) {
			const auto result = ImportFixture("embedded-texture.gltf");
			context.Expect(result.status != TerraEngine::AssetPipeline::GltfImportStatus::Failed, "Embedded texture fixture should still import package geometry.");
			context.ExpectEqual(static_cast<std::size_t>(1), result.generatedTextures.size(), "Importer should report the embedded base-color texture.");
			context.Expect(result.generatedTextures[0].semantic == TerraEngine::AssetPipeline::GltfTextureSemantic::Color, "Base-color texture should use color semantics.");
			context.Expect(!result.generatedMaterials.empty() && !result.generatedMaterials[0].textureGuids.empty(), "Generated material should bind the generated texture id.");
		});
	}
}
