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
			// The shell passes quoted paths as a single next argument, so the editor
			// only needs to consume the next argv slot when present.
			if (index + 1 < argc && argv[index + 1] != nullptr)
			{
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
