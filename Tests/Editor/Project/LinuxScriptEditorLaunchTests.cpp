#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Project/ScriptWorkspaceService.h"
#include "Scripting/ScriptingSystem.h"

#include <filesystem>
#include <string>

namespace TerraEngine::EditorTests {
	void RegisterLinuxScriptEditorLaunchTests(TestRegistry& registry) {
		registry.Add("Editor.LinuxScriptEditorLaunch.ExpandsConfiguredEditorCommand", [](TestContext& context) {
			const ScriptWorkspaceService::EditorCommandContext commandContext{
				std::filesystem::path("/tmp") / "Terra Project" / "Assembly-CSharp.sln",
				std::filesystem::path("/tmp") / "Terra Project" / "Assembly-CSharp.csproj",
				std::filesystem::path("/tmp") / "Terra Project" / "Assets" / "Player Controller.cs"
			};

			const std::string expanded = ScriptWorkspaceService::ExpandEditorCommandTemplate("code {solution} --goto {script}", commandContext);

			context.Expect(expanded.find("code ") == 0, "Configured Linux editor commands should keep the executable prefix.");
			context.Expect(expanded.find("Assembly-CSharp.sln") != std::string::npos, "Configured commands should expand the solution placeholder.");
			context.Expect(expanded.find("Player Controller.cs") != std::string::npos, "Configured commands should expand the script placeholder.");
			context.Expect(expanded.find("{solution}") == std::string::npos, "Configured commands should remove the solution placeholder.");
			context.Expect(expanded.find("{script}") == std::string::npos, "Configured commands should remove the script placeholder.");
		});

		registry.Add("Editor.LinuxScriptEditorLaunch.QuotesShellSensitiveScriptPaths", [](TestContext& context) {
			const std::string quoted = ScriptWorkspaceService::QuoteCommandArgument(std::filesystem::path("/tmp/Terra Project/Player's Script.cs"));

#if defined(_WIN32)
			context.Expect(quoted.front() == '"' && quoted.back() == '"', "Windows command arguments should be double quoted.");
#else
			context.Expect(quoted.front() == '\'' && quoted.back() == '\'', "Linux command arguments should be single quoted for shell launch.");
			context.Expect(quoted.find("'\\''") != std::string::npos, "Linux command arguments should escape embedded single quotes.");
#endif
		});

		registry.Add("Editor.LinuxScriptEditorLaunch.BuildsDefaultOpenerCommand", [](TestContext& context) {
			const std::filesystem::path targetPath = std::filesystem::path("/tmp") / "Terra Project" / "Assembly-CSharp.sln";
			const std::string command = ScriptWorkspaceService::BuildDefaultEditorCommand(targetPath);

#if defined(_WIN32)
			context.Expect(command.empty(), "Windows default editor launch should continue through ShellExecute instead of a shell command.");
#else
			context.Expect(command.find("xdg-open ") == 0, "Linux default editor launch should use the desktop opener.");
			context.Expect(command.find("Assembly-CSharp.sln") != std::string::npos, "Linux default opener command should include the target path.");
#endif
		});

		registry.Add("Editor.LinuxScriptEditorLaunch.DefaultOpenerCanTargetScriptFiles", [](TestContext& context) {
			const std::filesystem::path targetPath = std::filesystem::path("/tmp") / "Terra Project" / "Assets" / "Player Controller.cs";
			const std::string command = ScriptWorkspaceService::BuildDefaultEditorCommand(targetPath);

#if defined(_WIN32)
			context.Expect(command.empty(), "Windows default script launch should continue through ShellExecute instead of a shell command.");
#else
			context.Expect(command.find("xdg-open ") == 0, "Linux default script launch should use the desktop opener.");
			context.Expect(command.find("Player Controller.cs") != std::string::npos, "Linux default script launch should include the clicked script path.");
			context.Expect(command.find("Assembly-CSharp.sln") == std::string::npos, "Linux default script launch should not redirect script clicks to the solution file.");
#endif
		});

		registry.Add("Editor.LinuxScriptEditorLaunch.CSharpAuthoringNeedsNoSchedulerAnnotations", [](TestContext& context) {
			const ScriptingSystem& scripting = ScriptingSystem::Instance();

			context.Expect(!scripting.ManagedCallbacksRunProtected(), "Managed callbacks should be native-scheduled instead of forced through the protected boundary.");
			context.Expect(!scripting.ManagedCallbacksRequireSchedulerAnnotations(), "C# gameplay authors should not need scheduler annotations.");
			context.Expect(scripting.GetSchedulerBoundarySummary().find("normal API") != std::string::npos, "Compatibility summary should clearly state the unchanged C# workflow.");
		});
	}
}
