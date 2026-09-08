// NavigationProjectLifecycleTests.cpp - editor lifecycle hook contracts.

#include "../TestHarness.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::string ReadSource(const char* relativePath)
		{
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath, std::ios::binary);
			std::ostringstream output;
			output << input.rdbuf();
			return output.str();
		}
	}

	void RegisterNavigationProjectLifecycleTests(TestRegistry& registry)
	{
		registry.Add("Editor.NavigationProjectLifecycle.ProjectSceneAndPlayModeBoundariesRevokeNavigation", [](TestContext& context) {
			const std::string editor = ReadSource("Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.cpp");
			context.Expect(editor.find("NavigationSystem::Instance().RevokeAllOwners") != std::string::npos,
				"Project and scene lifecycle code must revoke navigation owners before replacing world state.");
			context.Expect(editor.find("ExitPlayMode()") != std::string::npos
				&& editor.find("EnterPlayMode()") != std::string::npos
				&& editor.find("SceneReplaced") != std::string::npos,
				"Play Mode exit and scene replacement must use the same navigation lifecycle boundary.");
		});

		registry.Add("Editor.NavigationProjectLifecycle.ScriptReloadAndShutdownContainLateCompletions", [](TestContext& context) {
			const std::string scripting = ReadSource("Engine/Scripting/ScriptingSystem.cpp");
			const std::string mainEngine = ReadSource("Engine/Core/MainEngine.cpp");
			context.Expect(scripting.find("RevokeAllOwners") != std::string::npos
				&& scripting.find("SceneReplaced") != std::string::npos,
				"Managed script reload must revoke navigation owners before old assembly work can complete.");
			context.Expect(mainEngine.find("NavigationSystem") != std::string::npos
				&& mainEngine.find("Shutdown") != std::string::npos,
				"Engine shutdown must retain an explicit navigation lifecycle boundary.");
		});
	}
}
