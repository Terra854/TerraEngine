#include "../TestHarness.h"
#include "EditorCommandLine.h"

#include <array>

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
	}
}
