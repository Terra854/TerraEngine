// ExportRuntimeLayout.h - explicit paths consumed by non-interactive export.
//
// Export must behave identically no matter which working directory launched the
// process. This value object therefore records every installed-tree location
// that the shared pipeline is allowed to read.

#pragma once

#include "BuildProfile.h"

#include <filesystem>
#include <string>

struct ExportRuntimeLayout {
	std::filesystem::path executablePath;
	std::filesystem::path installRoot;
	std::filesystem::path editorDirectory;
	std::filesystem::path engineDirectory;
	std::filesystem::path launcherDirectory;
	std::filesystem::path shaderDirectory;
	std::filesystem::path fontDirectory;
	std::filesystem::path managedDirectory;
	std::filesystem::path modelPackageCompiler;
};

/// Resolves an installed layout from an editor-side executable. The executable
/// is expected below <install-root>/Editor, which is also where export tools are
/// installed by CMake.
bool DiscoverExportRuntimeLayout(
	const std::filesystem::path& executablePath,
	ExportRuntimeLayout& outLayout,
	std::string& outError);

/// Verifies the files required to stage an export for the selected platform.
/// Tests may construct a synthetic layout and call this directly.
bool ValidateExportRuntimeLayout(
	const ExportRuntimeLayout& layout,
	ExportPlatform platform,
	std::string& outError);
