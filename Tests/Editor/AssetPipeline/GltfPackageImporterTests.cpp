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
			const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "TerraEngineGltfImporterTests" / fixtureName;

			TerraEngine::AssetPipeline::GltfPackageImportOptions options{};
			options.sourcePath = fixturePath;
			options.assetRoot = tempRoot / "Assets";
			options.buildRoot = tempRoot / "Build";
			options.sourceRelativePath = std::filesystem::path("Models") / fixtureName;
			options.packageGuid = TerraEngine::GenerateUUID();
			options.forceReimport = true;
			return TerraEngine::AssetPipeline::ImportGltfPackage(options);
		}
	}

	void RegisterGltfPackageImporterTests(TestRegistry& registry) {
		registry.Add("Editor.AssetPipeline.GltfPackageImporter.PreservesRequiredEmissiveStrengthAndLegacyDefault", [](TestContext& context) {
			// Require the extension at package level so both real fastgltf parsing
			// paths must support it; merely inspecting input JSON would miss either
			// an ignored strength or a failure in the later animation parse.
			const auto parent = std::filesystem::temp_directory_path() / "TerraEngineGltfEmissiveTests";
			const auto token = TerraEngine::ToHexString(TerraEngine::GenerateUUID());
			const auto root = parent / token;
			struct Cleanup {
				std::filesystem::path root, expectedParent;
				std::string expectedName;
				~Cleanup() noexcept {
					try {
						// Never recursively remove a computed path without proving it
						// is the exclusively named test child of the intended parent.
						const auto resolved = std::filesystem::absolute(root).lexically_normal();
						if(resolved.parent_path() == std::filesystem::absolute(expectedParent).lexically_normal()
							&& resolved.filename() == expectedName) {
							std::error_code ignored; std::filesystem::remove_all(resolved, ignored);
						}
					} catch(...) { /* Best-effort cleanup cannot obscure the regression result. */ }
				}
			} cleanup{root, parent, token};
			std::filesystem::create_directories(root);
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Tests/Editor/AssetPipeline/Fixtures/Gltf/one-material.gltf");
			auto document = nlohmann::json::parse(input);
			auto boosted = document.at("materials").at(0);
			boosted["name"] = "ExplicitStrengthEight";
			boosted["emissiveFactor"] = {0.25f, 0.5f, 1.0f};
			boosted["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"] = 8.0f;
			auto legacy = document.at("materials").at(0);
			legacy["name"] = "ImplicitStrengthOne";
			legacy["emissiveFactor"] = {0.25f, 0.5f, 1.0f};
			document["materials"] = nlohmann::json::array({boosted, legacy});
			document["extensionsUsed"] = {"KHR_materials_emissive_strength"};
			document["extensionsRequired"] = {"KHR_materials_emissive_strength"};
			const auto source = root / "emissive-strength.gltf";
			{ std::ofstream output(source); output << document.dump(); }
			TerraEngine::AssetPipeline::GltfPackageImportOptions options{};
			options.sourcePath = source; options.assetRoot = root / "Assets"; options.buildRoot = root / "Build";
			options.sourceRelativePath = "Models/emissive-strength.gltf";
			options.packageGuid = TerraEngine::GenerateUUID(); options.forceReimport = true;
			const auto imported = TerraEngine::AssetPipeline::ImportGltfPackageWithAnimation(options);
			context.Expect(imported.package.status != TerraEngine::AssetPipeline::GltfImportStatus::Failed,
				"A required emissive-strength extension must survive static and animation parsing.");
			context.ExpectEqual(std::size_t{2}, imported.package.generatedMaterials.size(), "Both HDR and legacy materials must be emitted.");
			const std::array expected{std::array{2.0f, 4.0f, 8.0f}, std::array{0.25f, 0.5f, 1.0f}};
			for(std::size_t index = 0; index < expected.size(); ++index) {
				const auto& material = imported.package.generatedMaterials[index];
				const auto metadata = TerraEngine::AssetPipeline::ToJson(material);
				context.ExpectEqual(expected[index], material.emissiveFactor, "Imported material values must preserve HDR strength or the implicit one default.");
				for(std::size_t channel = 0; channel < expected[index].size(); ++channel)
					context.ExpectEqual(expected[index][channel], metadata.at("emissiveFactor").at(channel).get<float>(), "Emitted metadata must preserve the lowered emissive factor.");
				for(const auto& path : {options.assetRoot / material.sourcePath, options.buildRoot / material.compiledPath}) {
					MaterialHeader header{};
					std::ifstream native(path, std::ios::binary);
					native.read(reinterpret_cast<char*>(&header), sizeof(header));
					context.Expect(native.good() && header.headerSize == sizeof(header), "Source and compiled material must contain the full native header.");
					for(std::size_t channel = 0; channel < expected[index].size(); ++channel)
						context.ExpectEqual(expected[index][channel], header.emissiveFactor[channel], "Native material must retain HDR emissive values without clamping or double scaling.");
				}
			}
		});
		registry.Add("Editor.AssetPipeline.GltfPackageImporter.ImportsOneMaterialFixture", [](TestContext& context) {
			const auto result = ImportFixture("one-material.gltf");
			context.Expect(result.status != TerraEngine::AssetPipeline::GltfImportStatus::Failed, "One-material glTF should import successfully.");
			context.ExpectEqual(static_cast<std::size_t>(1), result.generatedMeshes.size(), "Importer should report one generated mesh.");
			context.ExpectEqual(static_cast<std::size_t>(1), result.generatedMaterials.size(), "Importer should report one generated material.");
			context.ExpectEqual(static_cast<std::size_t>(1), result.materialAssignments.size(), "Importer should preserve the primitive material slot.");
		});
	}
}
