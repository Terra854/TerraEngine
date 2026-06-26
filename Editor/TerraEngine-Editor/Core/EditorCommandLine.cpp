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

		constexpr std::string_view projectEqualsPrefix = "--project=";
		if (argument.rfind(projectEqualsPrefix, 0) == 0)
		{
			const std::string_view value = argument.substr(projectEqualsPrefix.size());
			if (!value.empty())
			{
				options.startupProjectPath = std::filesystem::path(value);
			}
		}
	}

	return options;
}
