#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Project/ScriptWorkspaceService.h"

#include <filesystem>

namespace TerraEngine::EditorTests {
	void RegisterLinuxScriptWorkspaceTests(TestRegistry& registry) {
		registry.Add("Editor.LinuxScriptWorkspace.LiveAssemblyOutputUsesManagedDll", [](TestContext& context) {
			ScriptWorkspaceService service;
			const std::filesystem::path projectRoot = std::filesystem::path("/tmp") / "Terra Project";
			service.SetProjectRoot(projectRoot);

			context.ExpectEqual(projectRoot / "Build" / "Assembly-CSharp", service.GetOutputRoot(), "Script workspace output root should stay under the project Build directory.");
			context.ExpectEqual(projectRoot / "Build" / "Assembly-CSharp" / "Assembly-CSharp.dll", service.GetAssemblyPath(), "Live editor script reload should use the managed Assembly-CSharp DLL output on Linux.");
			context.ExpectEqual(projectRoot / "Build" / "Assembly-CSharp" / "References", service.GetReferenceRoot(), "Managed references should stay beside the generated Assembly-CSharp output.");
		});
	}
}
