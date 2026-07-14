#include "../TestHarness.h"
#include "EditorCommandLine.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace TerraEngine::EditorTests
{
	namespace
	{
		EditorCommandLineOptions Parse(std::initializer_list<const char*> arguments)
		{
			std::array<char*, 8> argv {};
			int argc = 0;
			for (const char* argument : arguments)
			{
				argv[argc++] = const_cast<char*>(argument);
			}
			return ParseEditorCommandLine(argc, argv.data());
		}
	}

	void RegisterEditorCommandLineTests(TestRegistry& registry)
	{
		registry.Add("Editor.CommandLine.ProjectFlagConsumesNextPath", [](TestContext& context) {
			const EditorCommandLineOptions options = Parse({"TerraEngine-Editor.exe", "--project", "C:/Projects/MCP Test Project"});
			context.Expect(options.startupProjectPath.has_value(), "--project should capture the next argument as the startup project.");
			context.ExpectEqual(std::string("C:/Projects/MCP Test Project"), options.startupProjectPath->generic_string(), "Startup project path should preserve spaces after shell parsing.");
		});

		registry.Add("Editor.CommandLine.ProjectEqualsCapturesPath", [](TestContext& context) {
			const EditorCommandLineOptions options = Parse({"TerraEngine-Editor.exe", "--project=C:/Projects/Sample"});
			context.Expect(options.startupProjectPath.has_value(), "--project=<path> should capture the inline path.");
			context.ExpectEqual(std::string("C:/Projects/Sample"), options.startupProjectPath->generic_string(), "Inline startup project path should round-trip.");
		});

		registry.Add("Editor.CommandLine.MissingProjectValueFallsBackToHub", [](TestContext& context) {
			const EditorCommandLineOptions options = Parse({"TerraEngine-Editor.exe", "--project"});
			context.Expect(!options.startupProjectPath.has_value(), "Missing --project value should leave the startup project unset.");
		});

		registry.Add("Editor.CommandLine.PhysicsInvariantOutputIsOptInAndPathSafe", [](TestContext& context) {
			const EditorCommandLineOptions separate = Parse({"TerraEngine-Editor.exe", "--project", "C:/Fixture Project/project.json",
				"--validate-editor-physics-invariants", "C:/Evidence Root/editor-physics.json"});
			context.Expect(separate.startupProjectPath == std::filesystem::path("C:/Fixture Project/project.json")
				&& separate.physicsInvariantOutputPath == std::filesystem::path("C:/Evidence Root/editor-physics.json"),
				"The parity command must preserve quoted project and evidence paths without changing normal startup parsing.");

			const EditorCommandLineOptions equals = Parse({"TerraEngine-Editor.exe",
				"--validate-editor-physics-invariants=C:/Evidence/editor-physics.json"});
			context.Expect(equals.physicsInvariantOutputPath == std::filesystem::path("C:/Evidence/editor-physics.json"),
				"The equals form must support process launchers that cannot safely split option values.");

			const EditorCommandLineOptions missing = Parse({"TerraEngine-Editor.exe", "--validate-editor-physics-invariants"});
			context.Expect(!missing.physicsInvariantOutputPath.has_value(),
				"A missing output path must not enable evidence generation or overwrite an implicit location.");
		});

		registry.Add("Editor.CommandLine.RendererBootstrapPrecedesEngineRun", [](TestContext& context) {
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/TerraEngine-Editor/Core/EditorMain.cpp");
			std::ostringstream source;
			source << input.rdbuf();
			const std::string text = source.str();
			const auto resolve = text.find("ResolveEditorStartupRendererConfiguration");
			const auto configure = text.find("ConfigureEditorStartupProfile");
			const auto run = text.find("engine.Run()");
			context.Expect(resolve < configure && configure < run, "Direct-project settings must be applied before Engine::Run initializes ResourceManager, RenderSystem, or Vulkan.");
		});
	}
}
