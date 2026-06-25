// EditorCommandLine.h - Command-line parsing for the native editor executable.
//
// The parser is intentionally small and side-effect free so startup behavior can
// be tested without constructing the full engine/editor runtime.

#pragma once

#include <filesystem>
#include <optional>

struct EditorCommandLineOptions
{
	std::optional<std::filesystem::path> startupProjectPath;
};

EditorCommandLineOptions ParseEditorCommandLine(int argc, char** argv);
