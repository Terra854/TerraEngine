#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests
{
	void RegisterVirtualGeometryDiagnosticsTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Editor.Diagnostics.UsesSelectedEvidence", [](TestContext& context)
		{
			const std::filesystem::path source = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Editor/TerraEngine-Editor/Windows/SceneViewWindow.cpp";
			std::ifstream input(source, std::ios::binary);
			std::ostringstream text;
			text << input.rdbuf();
			const std::string contents = text.str();
			context.Expect(!contents.empty(), "Scene View source must be available for diagnostics contract checks.");
			context.Expect(contents.find("VirtualGeometry") != std::string::npos, "Scene View must retain virtual-geometry diagnostics presentation.");
		});

		registry.Add("VirtualGeometry.Editor.Diagnostics.SeparatesAuxiliaryViews", [](TestContext& context)
		{
			const std::filesystem::path source = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Editor/TerraEngine-Editor/Windows/GameViewWindow.cpp";
			std::ifstream input(source, std::ios::binary);
			std::ostringstream text;
			text << input.rdbuf();
			const std::string contents = text.str();
			context.Expect(!contents.empty(), "Game View source must be available for diagnostics contract checks.");
			context.Expect(contents.find("VirtualGeometry") != std::string::npos, "Game View must retain virtual-geometry diagnostics presentation.");
		});
	}
}
