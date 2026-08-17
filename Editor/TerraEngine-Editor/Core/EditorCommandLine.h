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
	// Present only for the installed CapabilityValidation acceptance command. Normal editor
	// launches never generate or overwrite invariant evidence.
	std::optional<std::filesystem::path> physicsInvariantOutputPath;
	// Terrain live validation launches the normal editor and writes one bounded
	// result before requesting the engine's orderly shutdown path.
	std::optional<std::filesystem::path> terrainInvariantOutputPath;
};

EditorCommandLineOptions ParseEditorCommandLine(int argc, char** argv);
