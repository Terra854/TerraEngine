#include "TestHarness.h"

#include "ExportValidator.h"
#include "File Formats/TerraEngineMaterialFile.h"

#include <filesystem>
#include <fstream>

namespace TerraEngine::EditorTests {
	namespace {
		MaterialHeader MakeMinimalMaterialHeader() {
			MaterialHeader header{};
			header.magic[0] = 'T';
			header.magic[1] = 'M';
			header.magic[2] = 'A';
			header.magic[3] = 'T';
			header.version = kMaterialCurrentVersion;
			header.headerSize = sizeof(MaterialHeader);
			header.shadingModel = static_cast<std::uint32_t>(ShadingModel::MetallicRoughness);
			header.alphaMode = static_cast<std::uint32_t>(AlphaMode::Opaque);
			header.baseColorFactor[0] = 1.0f;
			header.baseColorFactor[1] = 1.0f;
			header.baseColorFactor[2] = 1.0f;
			header.baseColorFactor[3] = 1.0f;
			header.roughnessFactor = 0.5f;
			header.metallicFactor = 0.0f;
			header.normalScale = 1.0f;
			header.textureArrayOffset = sizeof(MaterialHeader);
			header.floatParameterOffset = sizeof(MaterialHeader);
			return header;
		}

		void WriteMaterial(const std::filesystem::path& path, const MaterialHeader& header) {
			std::filesystem::create_directories(path.parent_path());
			std::ofstream out(path, std::ios::binary);
			out.write(reinterpret_cast<const char*>(&header), sizeof(header));
		}
	}

	void RegisterMaterialExportValidationTests(TestRegistry& registry) {
		registry.Add("Editor.ExportValidator.MaterialFallbacksAreWarnings", [](TestContext& context) {
			const std::filesystem::path projectRoot = std::filesystem::temp_directory_path() / "TerraEngineMaterialExportValidation";
			std::filesystem::remove_all(projectRoot);
			WriteMaterial(projectRoot / "Assets" / "Materials" / "FallbackOnly.tmat", MakeMinimalMaterialHeader());

			BuildProfile profile{};
			profile.name = "Material Validation";
			profile.id = "material-validation";
			profile.outputDirectory = "Builds/MaterialValidation";
			profile.startupScene = "Scenes/Startup.tescene";
			profile.enableManagedCompilation = false;

			const ExportValidationResult result = ExportValidator{}.ValidateAll(profile, projectRoot);
			std::filesystem::remove_all(projectRoot);

			context.Expect(result.passed, "Material fallback diagnostics should warn without failing export validation.");
			context.Expect(result.warningCount >= 1u, "Missing runtime material slots should produce warnings.");
			context.Expect(!result.materialFallbackDiagnostics.empty(), "Material fallback diagnostics should be mirrored for export UI.");
		});
	}
}
