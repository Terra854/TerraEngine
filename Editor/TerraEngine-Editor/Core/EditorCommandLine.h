// EditorCommandLine.h - Command-line parsing for the native editor executable.
//
// The parser is intentionally small and side-effect free so startup behavior can
// be tested without constructing the full engine/editor runtime.

#pragma once

#include <filesystem>
#include <optional>

struct EditorCommandLineOptions
{
	struct NavigationAgentValidationRequest
	{
		std::filesystem::path optionsPath;
		std::filesystem::path reportPath;
	};

	std::optional<std::filesystem::path> startupProjectPath;
	// Present only for the installed CapabilityValidation acceptance command. Normal editor
	// launches never generate or overwrite invariant evidence.
	std::optional<std::filesystem::path> physicsInvariantOutputPath;
	// Terrain live validation launches the normal editor and writes one bounded
	// result before requesting the engine's orderly shutdown path.
	std::optional<std::filesystem::path> terrainInvariantOutputPath;
	// The navigation agent acceptance lane needs a fixture/options document and a
	// distinct report destination. Keeping both paths parsed before engine setup
	// lets EditorMain configure deterministic worker capacity before Initialize.
	std::optional<NavigationAgentValidationRequest> navigationAgentValidation;
	// Keep malformed acceptance invocations fail-closed instead of silently
	// starting an interactive editor that produces no report.
	bool navigationAgentValidationArgumentInvalid = false;
};

EditorCommandLineOptions ParseEditorCommandLine(int argc, char** argv);
