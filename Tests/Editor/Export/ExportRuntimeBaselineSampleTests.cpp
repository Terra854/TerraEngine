#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests {
	namespace {
		std::filesystem::path SampleRoot() {
			return (std::filesystem::path(__FILE__).parent_path() / ".." / ".." / "Fixtures" / "ExportRuntimeBaseline").lexically_normal();
		}

		std::string ReadText(const std::filesystem::path& path) {
			std::ifstream stream(path, std::ios::binary);
			std::ostringstream buffer;
			buffer << stream.rdbuf();
			return buffer.str();
		}
	}

	void RegisterExportRuntimeBaselineSampleTests(TestRegistry& registry) {
		registry.Add("Editor.ExportRuntimeBaseline.ProjectManifestAndRenderSettingsExist", [](TestContext& context) {
			const std::filesystem::path projectPath = SampleRoot() / "project.json";
			context.Expect(std::filesystem::exists(projectPath), "Baseline sample should include project.json.");

			const nlohmann::json project = nlohmann::json::parse(ReadText(projectPath));
			context.ExpectEqual(std::string("Export Runtime Baseline"), project.value("name", ""), "Sample project should have stable product metadata.");
			context.ExpectEqual(std::string("Scenes/Startup.tescene"), project.value("startupScene", ""), "Sample project should identify its startup scene.");
			context.Expect(project.contains("buildProfiles"), "Sample project should carry a baseline build profile.");

			const std::filesystem::path renderSettingsPath = SampleRoot() / "ProjectSettings" / "RenderSettings.json";
			context.Expect(std::filesystem::exists(renderSettingsPath), "Baseline sample should keep render settings in ProjectSettings.");
			const nlohmann::json renderSettings = nlohmann::json::parse(ReadText(renderSettingsPath));
			context.Expect(renderSettings.contains("settings"), "Render settings should contain the renderer-owned settings object.");
			context.Expect(renderSettings.value("validationEnabled", false), "Sample render settings should enable validation.");
		});

		registry.Add("Editor.ExportRuntimeBaseline.SceneReferencesRequiredAssets", [](TestContext& context) {
			const std::filesystem::path root = SampleRoot();
			const nlohmann::json scene = nlohmann::json::parse(ReadText(root / "Scenes" / "Startup.tescene"));
			const std::string sceneText = scene.dump();

			context.Expect(sceneText.find("Assets/Meshes/Baseline.temf") != std::string::npos, "Startup scene should reference the sample mesh.");
			context.Expect(sceneText.find("Assets/Materials/Baseline.tmat") != std::string::npos, "Startup scene should reference the sample material.");
			context.Expect(sceneText.find("UI/Baseline.ui") != std::string::npos, "Startup scene should reference the sample UI document.");
			context.Expect(sceneText.find("BaselineScript") != std::string::npos, "Startup scene should reference the sample script.");
			context.Expect(std::filesystem::exists(root / "Assets" / "Textures" / "Baseline.tetf"), "Sample texture placeholder should exist.");
		});
	}
}
