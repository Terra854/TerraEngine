// BuildProfile.h — Data model for exported build configurations.
//
// A BuildProfile defines all the settings needed to produce one exported build:
// target platform, NativeAOT publish parameters, TEPAK packaging options, and
// output directory. Profiles are serialised as JSON files so they survive across
// editor sessions and can be committed to version control alongside the project.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

/// Available target platforms for export.
enum class ExportPlatform : uint32_t {
	Windows = 0,
	Linux   = 1,
};

/// Compression preset for TEPAK packaging.
enum class CompressionPreset : uint32_t {
	None   = 0,  // No compression, fastest export
	Fast   = 1,  // zstd level 3
	Normal = 2,  // zstd level 9
	Best   = 3,  // zstd level 19
};

/// One build profile that the user can create, edit, and export from the editor.
struct BuildProfile {
	/// Human-readable name shown in the BuildProfilesWindow and CLI.
	std::string name = "Default";

	/// Unique identifier generated on creation; used for file naming.
	std::string id;

	/// Target platform for the export.
	ExportPlatform platform = ExportPlatform::Windows;

	/// Root output directory where the export tree will be created.
	/// Defaults to <ProjectRoot>/Builds/<ProfileName>/
	std::filesystem::path outputDirectory;

	/// The startup scene to load when the standalone game is launched.
	std::string startupScene;

	/// TEPAK compression preset that maps to zstd compression levels.
	CompressionPreset compressionPreset = CompressionPreset::Normal;

	/// Number of worker threads for TEPAK compression (0 = auto-detect).
	uint32_t tepakWorkerCount = 0;

	/// Whether to enable NativeAOT compilation. Setting this to false skips
	/// source-generator validation and C# compilation entirely, useful for
	/// content-only iterations.
	bool enableManagedCompilation = true;

	/// Whether to include debug symbols in the export output.
	bool includeDebugSymbols = false;

	/// Whether to strip the output NativeAOT library to minimize size.
	bool stripNativeAotOutput = false;

	/// Additional NativeAOT publish arguments passed verbatim to dotnet publish.
	std::string additionalPublishArgs;

	/// Additional scenes to include in the export (beyond the active scene).
	std::vector<std::string> includedScenes;

	/// Path to the profile's JSON file on disk (set during load/save).
	std::filesystem::path filePath;

	/// Whether this profile has unsaved modifications.
	bool isDirty = false;
};

/// Generates a short unique ID for a new build profile.
std::string GenerateBuildProfileId();

/// Serialise a BuildProfile to a JSON file at the given path.
bool SaveBuildProfile(const BuildProfile& profile, const std::filesystem::path& path);

/// Deserialise a BuildProfile from a JSON file. Returns empty profile on failure.
BuildProfile LoadBuildProfile(const std::filesystem::path& path);

/// Load all profiles from a directory (e.g. <ProjectRoot>/BuildProfiles/).
std::vector<BuildProfile> LoadAllBuildProfiles(const std::filesystem::path& directory);
