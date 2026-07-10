// HeadlessExportCommandLine.h - strict parsing for the non-interactive exporter.

#pragma once

#include "ExportTypes.h"

#include <filesystem>
#include <string>
#include <vector>

struct HeadlessExportParseResult {
	bool success = false;
	bool showHelp = false;
	ExportRequest request;
	ExportFailureCategory failureCategory = ExportFailureCategory::InvalidArguments;
	std::string message;
};

/// Parses arguments excluding argv[0]. Relative paths are resolved against the
/// supplied launch directory before any later working-directory changes.
HeadlessExportParseResult ParseHeadlessExportCommandLine(
	const std::vector<std::string>& arguments,
	const std::filesystem::path& executablePath,
	const std::filesystem::path& launchDirectory);

std::string GetHeadlessExportUsage();

/// Returns the actual executable path where the host platform exposes it, with
/// argv[0] used only as a portable fallback.
std::filesystem::path ResolveHeadlessExportExecutablePath(const char* argvZero);
