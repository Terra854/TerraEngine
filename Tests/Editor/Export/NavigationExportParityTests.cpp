// NavigationExportParityTests.cpp - rendered/headless navigation parity contracts.

#include "TestHarness.h"

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
			if(!input) return {};
			std::ostringstream output;
			output << input.rdbuf();
			return output.str();
		}
	}

	void RegisterNavigationExportParityTests(TestRegistry& registry)
	{
		registry.Add("Editor.NavigationExportParity.RenderedAndHeadlessStartupOwnOneNavigationSystem", [](TestContext& context) {
			const std::string mainEngine = ReadSource("Engine/Core/MainEngine.cpp");
			const std::string navigation = ReadSource("Engine/Navigation/NavigationSystem.cpp");
			context.Expect(mainEngine.find("NavigationSystem::Instance()") != std::string::npos,
				"Rendered and headless startup must both initialize the engine-owned navigation system.");
			context.Expect(navigation.find("class NavigationSystem") == std::string::npos
				&& navigation.find("NavigationSystem::Initialize") != std::string::npos,
				"Navigation startup must remain one native system boundary rather than an editor-only implementation.");
			context.Expect(mainEngine.find("ContentMount") != std::string::npos,
				"Export startup must make mounted navigation companions available through the same runtime path.");
		});

		registry.Add("Editor.NavigationExportParity.PublicOutcomesUseSharedSemanticVocabulary", [](TestContext& context) {
			const std::string types = ReadSource("Engine/Navigation/NavigationTypes.cpp");
			const std::string managed = ReadSource("Engine/Managed/Source/Navigation/NavigationTypes.cs");
			context.Expect(types.find("case NavigationOutcome::Complete") != std::string::npos
				&& types.find("case NavigationReason::MissingNeighbor") != std::string::npos,
				"Native rendered and headless lanes must expose stable provider-neutral route outcome names.");
			context.Expect(managed.find("enum NavigationStatus : uint") != std::string::npos
				&& managed.find("enum NavigationReason : uint") != std::string::npos,
				"Managed consumers must use explicit status/reason values rather than provider codes.");
		});

		registry.Add("Editor.NavigationExportParity.HeadlessPathDoesNotDependOnEditorWindows", [](TestContext& context) {
			const std::string navigation = ReadSource("Engine/Navigation/NavigationSystem.cpp");
			context.Expect(navigation.find("Editor/TerraEngine-Editor") == std::string::npos
				&& navigation.find("imgui") == std::string::npos
				&& navigation.find("GLFW") == std::string::npos,
				"Headless navigation startup and query semantics must not depend on editor UI libraries.");
		});

		registry.Add("Editor.NavigationExportParity.RuntimePackageUsesOnlyAutomaticGeometryCompanions", [](TestContext& context) {
			const std::string dependencies = ReadSource("Editor/Export/NavigationExportDependencies.cpp");
			const std::string startup = ReadSource("Engine/Runtime/ExportStartupCoordinator.cpp");
			context.Expect(dependencies.find(".tenavsource") == std::string::npos
				&& dependencies.find(".tenavgeom") != std::string::npos
				&& startup.find("authored source data") != std::string::npos,
				"Export and runtime startup must consume automatic cooked geometry/cache data without authored source files.");
		});
	}
}
