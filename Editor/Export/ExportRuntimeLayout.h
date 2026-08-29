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
	// Only binaries copied into exported builds live below Editor/ExportRuntime.
	// Exporter and compiler tools remain in the editor tool directory.
	std::filesystem::path exportRuntimeDirectory;
	// These named paths remain separate so staging code communicates whether it
	// is looking for an engine library or the launcher executable.
	std::filesystem::path engineDirectory;
	std::filesystem::path launcherDirectory;
	std::filesystem::path shaderDirectory;
	std::filesystem::path fontDirectory;
	std::filesystem::path managedDirectory;
	std::filesystem::path modelPackageCompiler;
	// The editor ships the exact SDK used to build TerraEngine. Managed child
	// processes invoke this absolute path instead of resolving dotnet from PATH.
	std::filesystem::path dotnetSdkDirectory;
	std::filesystem::path dotnetExecutable;
	// NativeAOT compilation is an editor feature, so the installed feed and SDK
	// pin live beside the editor instead of depending on user NuGet settings.
	std::filesystem::path nativeAotToolchainDirectory;
	std::filesystem::path nativeAotNugetConfig;
	std::filesystem::path nativeAotNugetFeed;
	std::filesystem::path nativeAotGlobalJson;
};

/// Resolves an installed layout from an editor-side executable. The executable
/// is expected below <install-root>/Editor.
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
