// BuildProfile.h — Build profile configuration data
//
// Defines the data structures and enumerations for managing exported build configurations. It handles the serialization and deserialization of build profiles, including target platform settings, compression presets, and NativeAOT publish parameters.

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
// CompressionPreset names the supported states explicitly so serialization and switches stay readable.
enum class CompressionPreset : uint32_t {
	None   = 0,  // No compression, fastest export
	Fast   = 1,  // zstd level 3
	Normal = 2,  // zstd level 9
	Best   = 3,  // zstd level 19
};

/// One build profile that the user can create, edit, and export from the editor.
// struct BuildProfile groups related state and operations so callers have one documented contract.
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

	/// Controls runtime RAM/VRAM cleanup for loaded resources in exported builds.
	/// These values do not affect persistent Vulkan or shader disk caches.
	bool residencyCleanupEnabled = true;
	double residencyIdleSecondsThreshold = 10.0;
	uint64_t residencyMemoryBudgetMb = 0;
	double residencyCleanupIntervalSeconds = 1.0;
	double residencyReloadGraceSeconds = 0.5;
	bool residencyStrictBudgetMode = false;

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

/// Converts an export platform to the stable lowercase string stored in JSON.
std::string ExportPlatformToString(ExportPlatform platform);

/// Parses a lowercase export platform string, defaulting to Windows for legacy profiles.
ExportPlatform ExportPlatformFromString(const std::string& value);

/// Returns the .NET runtime identifier used when publishing managed export payloads.
std::string GetExportRuntimeIdentifier(ExportPlatform platform);

/// Returns the export platform that can be NativeAOT-published on the current host.
ExportPlatform GetHostExportPlatform();

/// Returns true when the selected export platform can be NativeAOT-published on this host OS.
bool IsExportPlatformNativeToHost(ExportPlatform platform);

/// Returns the executable suffix used by the exported launcher on the target platform.
std::string GetExportExecutableSuffix(ExportPlatform platform);

/// Returns the native shared-library suffix used by the target platform.
std::string GetExportSharedLibrarySuffix(ExportPlatform platform);

/// Returns the launcher filename for the named project and target platform.
std::string GetExportLauncherFileName(const std::string& projectName, ExportPlatform platform);

/// Returns the NativeAOT gameplay library filename expected under Managed/.
std::string GetExportManagedRuntimeLibraryName(ExportPlatform platform);

/// Builds the dotnet publish command used for NativeAOT gameplay compilation.
/// @param profile Build profile whose toggles become publish properties.
/// @param managedProject Full path to Assembly-CSharp.csproj.
/// @param managedOutputDir Full path where dotnet should place NativeAOT output.
std::string BuildNativeAotPublishCommand(
	const BuildProfile& profile,
	const std::filesystem::path& managedProject,
	const std::filesystem::path& managedOutputDir);

/// Returns the engine shared library filename installed beside exported launchers.
std::string GetExportEngineLibraryName(ExportPlatform platform);

/// Returns the GLFW runtime library filename installed beside native consumers.
std::string GetExportGlfwLibraryName(ExportPlatform platform);
