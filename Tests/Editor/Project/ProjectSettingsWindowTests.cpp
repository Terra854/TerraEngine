#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Project/ProjectSettingsMetadata.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::string ReadProjectSourceFile(const char* relativePath)
		{
			const std::filesystem::path sourcePath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath;
			std::ifstream file(sourcePath, std::ios::binary);
			if (!file)
			{
				return {};
			}

			std::ostringstream buffer;
			buffer << file.rdbuf();
			return buffer.str();
		}
	}

	void RegisterProjectSettingsWindowTests(TestRegistry& registry)
	{
		registry.Add("Editor.ProjectSettings.EditMenuContract", [](TestContext& context) {
			context.ExpectEqual(
				std::string("Edit"),
				std::string(TerraEngine::Editor::ProjectSettings::kProjectSettingsMenuName),
				"Project Settings should belong to the Edit menu.");
			context.ExpectEqual(
				std::string("Project Settings..."),
				std::string(TerraEngine::Editor::ProjectSettings::kProjectSettingsMenuLabel),
				"Project Settings should use the expected menu label.");
			context.ExpectEqual(
				std::string("Project Settings"),
				std::string(TerraEngine::Editor::ProjectSettings::kProjectSettingsWindowTitle),
				"Project Settings should open the expected window title.");
		});

		registry.Add("Editor.ProjectSettings.CategorySearchContract", [](TestContext& context) {
			const auto allCategories = TerraEngine::Editor::ProjectSettings::FilterProjectSettingsCategories("");
			context.ExpectEqual(static_cast<std::size_t>(3), allCategories.size(), "Empty search should show the current Project Settings categories.");

			const auto shadowMatches = TerraEngine::Editor::ProjectSettings::FilterProjectSettingsCategories("shadow");
			context.ExpectEqual(static_cast<std::size_t>(1), shadowMatches.size(), "Shadow search should find the graphics category.");
			context.ExpectEqual(
				std::string("graphics"),
				std::string(shadowMatches.front().stableId),
				"Shadow search should route to the graphics category.");

			const auto lodMatches = TerraEngine::Editor::ProjectSettings::FilterProjectSettingsCategories("lod");
			context.ExpectEqual(static_cast<std::size_t>(1), lodMatches.size(), "LOD search should find the graphics category for virtual geometry runtime controls.");
			context.ExpectEqual(
				std::string("graphics"),
				std::string(lodMatches.front().stableId),
				"LOD search should route to the graphics category.");

			const auto resourceMatches = TerraEngine::Editor::ProjectSettings::FilterProjectSettingsCategories("residency");
			context.ExpectEqual(static_cast<std::size_t>(1), resourceMatches.size(), "Residency search should find the resource policy category.");
			context.ExpectEqual(
				std::string("resources"),
				std::string(resourceMatches.front().stableId),
				"Residency search should route to the resources category.");

			const auto mipMatches = TerraEngine::Editor::ProjectSettings::FilterProjectSettingsCategories("mip");
			context.ExpectEqual(static_cast<std::size_t>(1), mipMatches.size(), "Texture mip search should find the resources category.");
			context.ExpectEqual(
				std::string("resources"),
				std::string(mipMatches.front().stableId),
				"Texture mip search should route to the resources category.");

			const auto skyMatches = TerraEngine::Editor::ProjectSettings::FilterProjectSettingsCategories("directional");
			context.ExpectEqual(static_cast<std::size_t>(0), skyMatches.size(), "Directional sky search should not expose removed scene-level sky settings.");
		});

		registry.Add("Editor.ProjectSettings.ResourcePoliciesMoveOutOfDiagnosticsWindow", [](TestContext& context) {
			const std::string projectSettingsSource =
				ReadProjectSourceFile("Editor/TerraEngine-Editor/Windows/ProjectSettingsWindow.cpp");
			const std::string residencySource =
				ReadProjectSourceFile("Editor/TerraEngine-Editor/Windows/ResourceResidencyWindow.cpp");
			context.Expect(!projectSettingsSource.empty(), "Project Settings source should be readable for resource policy ownership checks.");
			context.Expect(!residencySource.empty(), "Resource Residency source should be readable for diagnostic ownership checks.");

			context.Expect(projectSettingsSource.find("DrawResourcesCategory") != std::string::npos, "Project Settings should own the Resources category UI.");
			context.Expect(projectSettingsSource.find("Enable Loaded Resource Cleanup") != std::string::npos, "Loaded resource cleanup should move to Project Settings.");
			context.Expect(projectSettingsSource.find("Texture Mip Streaming") != std::string::npos, "Texture mip streaming should move to Project Settings.");
			context.Expect(projectSettingsSource.find("Virtual Geometry Page Budgets") != std::string::npos, "Virtual geometry page budgets should move to Project Settings.");
			context.Expect(projectSettingsSource.find("PersistResourcePoliciesToProjectManifest") != std::string::npos, "Resource policy edits should still persist to the project manifest.");

			context.Expect(residencySource.find("DrawPolicyControls") == std::string::npos, "Resource Residency should not keep the old policy editor.");
			context.Expect(residencySource.find("Enable Loaded Resource Cleanup") == std::string::npos, "Resource Residency should remain focused on diagnostics after policy controls move.");
			context.Expect(residencySource.find("Diagnostics Filters") != std::string::npos, "Window-local diagnostic filters should stay in Resource Residency.");
		});
	}
}
