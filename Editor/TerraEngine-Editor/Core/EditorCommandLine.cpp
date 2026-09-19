// EditorCommandLine.cpp - Native editor command-line parser.

#include "EditorCommandLine.h"

#include <string_view>

EditorCommandLineOptions ParseEditorCommandLine(int argc, char** argv)
{
	EditorCommandLineOptions options;
	for (int index = 1; index < argc; ++index)
	{
		if (argv[index] == nullptr)
		{
			continue;
		}

		const std::string_view argument(argv[index]);
		if (argument == "--project")
		{
			if (index + 1 < argc && argv[index + 1] != nullptr)
			{
				// Paths that contain spaces must be quoted by the caller so the
				// platform command-line parser delivers them as one argv item.
				options.startupProjectPath = std::filesystem::path(argv[++index]);
			}
			continue;
		}
		if (argument == "--validate-editor-physics-invariants")
		{
			if (index + 1 < argc && argv[index + 1] != nullptr)
			{
				options.physicsInvariantOutputPath = std::filesystem::path(argv[++index]);
			}
			continue;
		}
		if (argument == "--validate-editor-terrain-invariants")
		{
			if (index + 1 < argc && argv[index + 1] != nullptr)
				options.terrainInvariantOutputPath = std::filesystem::path(argv[++index]);
			continue;
		}
		if (argument == "--validate-editor-navigation-agents")
		{
			if (index + 2 < argc && argv[index + 1] != nullptr && argv[index + 2] != nullptr)
			{
				// The options document is intentionally separate from the output report:
				// installed acceptance fixtures remain immutable inputs across runs.
				options.navigationAgentValidation = EditorCommandLineOptions::NavigationAgentValidationRequest{
					std::filesystem::path(argv[++index]), std::filesystem::path(argv[++index])};
			}
			else
			{
				options.navigationAgentValidationArgumentInvalid = true;
			}
			continue;
		}

		constexpr std::string_view projectEqualsPrefix = "--project=";
		if (argument.rfind(projectEqualsPrefix, 0) == 0)
		{
			const std::string_view value = argument.substr(projectEqualsPrefix.size());
			if (!value.empty())
			{
				options.startupProjectPath = std::filesystem::path(value);
			}
			continue;
		}

		constexpr std::string_view invariantEqualsPrefix = "--validate-editor-physics-invariants=";
		if (argument.rfind(invariantEqualsPrefix, 0) == 0)
		{
			const std::string_view value = argument.substr(invariantEqualsPrefix.size());
			if (!value.empty())
			{
				options.physicsInvariantOutputPath = std::filesystem::path(value);
			}
		}
		constexpr std::string_view terrainInvariantEqualsPrefix = "--validate-editor-terrain-invariants=";
		if (argument.rfind(terrainInvariantEqualsPrefix, 0) == 0)
		{
			const std::string_view value = argument.substr(terrainInvariantEqualsPrefix.size());
			if (!value.empty()) options.terrainInvariantOutputPath = std::filesystem::path(value);
		}
	}

	return options;
}
