#include "../TestHarness.h"

#include "Editor/TerraEngine-Editor/Windows/SceneViewOverlayTypes.h"

#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::string ReadEditorWindowSource()
		{
			const auto read = [](const std::string& candidate) {
				std::ifstream file(candidate, std::ios::binary);
				if (!file)
				{
					return std::string();
				}
				std::ostringstream buffer;
				buffer << file.rdbuf();
				return buffer.str();
			};

			const std::string path = "Editor/TerraEngine-Editor/Framework/EditorWindow.cpp";
			std::string source = read(path);
			if (!source.empty())
			{
				return source;
			}
			source = read(std::string("../../../") + path);
			if (!source.empty())
			{
				return source;
			}
			const std::string thisFile = __FILE__;
			for (const std::string marker : {
				std::string("Tests\\Editor\\Scene\\SceneViewOverlaySchedulerTests.cpp"),
				std::string("Tests/Editor/Scene/SceneViewOverlaySchedulerTests.cpp") })
			{
				const auto offset = thisFile.find(marker);
				if (offset != std::string::npos)
				{
					return read(thisFile.substr(0u, offset) + path);
				}
			}
			return {};
		}
	}

	void RegisterSceneViewOverlaySchedulerTests(TestRegistry& registry)
	{
		registry.Add("SceneViewOverlay.Scheduler.DeclaresResourcesAndBoundaries", [](TestContext& context) {
			const auto declaration = TerraEngine::Editor::SceneViewOverlay::BuildSceneViewOverlaySchedulerDeclaration();

			context.ExpectEqual(std::string("EditorUI"), declaration.schedulerPhase, "Scene View overlays should run in the editor UI phase.");
			context.Expect(!declaration.readResources.empty(), "Scene View overlay declaration should name read resources.");
			context.Expect(!declaration.writeResources.empty(), "Scene View overlay declaration should name write resources.");
			context.Expect(declaration.workerSafePreparation.find("immutable") != std::string::npos, "Worker-safe overlay preparation should use immutable snapshots.");
			context.Expect(declaration.protectedBoundary.find("ImGui") != std::string::npos, "Protected boundary should identify ImGui drawing/input ownership.");
			context.Expect(declaration.visibilityBoundary.find("transactional") != std::string::npos, "Visibility boundary should mention transactional/deferred mutation behavior.");
		});

		registry.Add("SceneViewOverlay.Scheduler.HiddenAndCollapsedWindowsOmitWindowWork", [](TestContext& context) {
			const std::string source = ReadEditorWindowSource();

			context.Expect(!source.empty(), "The shared EditorWindow source should be available to the scheduling regression test.");
			context.Expect(
				source.find("if (!m_IsVisible) return;") != std::string::npos,
				"Hidden editor windows should return before ImGui or window-specific preparation begins.");
			context.Expect(
				source.find("if (expanded && m_IsVisible)") != std::string::npos,
				"Collapsed editor windows should omit OnGui and all window-specific overlay work.");
		});
	}
}
