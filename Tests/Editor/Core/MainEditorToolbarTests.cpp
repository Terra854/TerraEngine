#include "../TestHarness.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::string ReadMainEditorSubsystemSource()
		{
			const std::filesystem::path sourcePath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.cpp";
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

	void RegisterMainEditorToolbarTests(TestRegistry& registry)
	{
		registry.Add("Editor.MainToolbar.PlayModeControlsUseIconsAndTooltips", [](TestContext& context) {
			const std::string source = ReadMainEditorSubsystemSource();
			context.Expect(!source.empty(), "Main editor subsystem source should be available for toolbar contract checks.");
			context.Expect(source.find("DrawPlayModeToolbarIconButton") != std::string::npos, "Play Mode toolbar should use icon-only buttons.");
			context.Expect(source.find("##play_mode_toolbar_icon") != std::string::npos, "Play Mode toolbar icon buttons should keep labels hidden from the button face.");
			context.Expect(source.find("DrawPlayModeToolbarTooltip") != std::string::npos, "Play Mode toolbar icons should expose readable command names through tooltips.");
			context.Expect(source.find("ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f)") != std::string::npos, "Play Mode tooltips should keep readable padding even while the toolbar uses compact window padding.");
			context.Expect(source.find("const char* playLabel") == std::string::npos, "Play Mode toolbar should not center itself around visible text labels.");
			context.Expect(source.find("ImGui::Button(playLabel)") == std::string::npos, "Play Mode toolbar should not render the Play/Stop command as text.");
			context.Expect(source.find("isRunning ? \"Stop\" : \"Play\"") != std::string::npos, "Play/Stop tooltip should preserve the visible command name.");
			context.Expect(source.find("isPaused ? \"Resume\" : \"Pause\"") != std::string::npos, "Pause tooltip should explain the toggle action.");
			context.Expect(source.find("PlayModeToolbarIcon::Next") != std::string::npos && source.find("\"Next\"") != std::string::npos, "Step command should keep its Next tooltip.");
		});
	}
}
