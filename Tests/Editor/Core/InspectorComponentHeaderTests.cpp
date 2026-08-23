// InspectorComponentHeaderTests.cpp - regression coverage for one consistent component-header menu.

#include "../TestHarness.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::string ReadInspectorSource()
		{
			// Read the production Inspector source so this lightweight test can verify
			// the ImGui item ownership without linking the Vulkan-backed editor window.
			const std::filesystem::path sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/TerraEngine-Editor/Windows/InspectorWindow.cpp";
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

	void RegisterInspectorComponentHeaderTests(TestRegistry& registry)
	{
		registry.Add("Editor.Inspector.ComponentHeaderUsesOneContextMenu", [](TestContext& context) {
			const std::string source = ReadInspectorSource();
			context.Expect(!source.empty(), "InspectorWindow.cpp should be readable for header menu checks.");
			if (source.empty())
			{
				return;
			}

			// The native component popup is the last ComponentSettings block because
			// script bindings use an earlier, independent header implementation.
			const std::size_t popupStart = source.rfind("ImGui::BeginPopupContextItem(\"ComponentSettings\")");
			const std::size_t popupEnd = popupStart == std::string::npos
				? std::string::npos
				: source.find("ImGui::EndPopup();", popupStart);
			context.Expect(popupStart != std::string::npos && popupEnd != std::string::npos,
				"Native component headers should expose one full-width context popup.");
			if (popupStart == std::string::npos || popupEnd == std::string::npos)
			{
				return;
			}

			const std::string popupBody = source.substr(popupStart, popupEnd - popupStart);
			context.Expect(popupBody.find("Remove Component") != std::string::npos,
				"The unified component popup should retain component removal.");
			context.Expect(popupBody.find("Reset Component To Prefab Defaults") != std::string::npos,
				"The unified component popup should retain prefab reset.");
			context.Expect(source.find("PrefabComponentReset_") == std::string::npos,
				"The component label should not own a second context popup.");
		});
	}
}
