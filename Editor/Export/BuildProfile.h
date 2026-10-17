// BuildProfile.h — Build profile configuration data
//
// Defines the data structures and enumerations for managing exported build configurations. It handles the serialization and deserialization of build profiles, including target platform settings, compression presets, and NativeAOT publish parameters.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
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

	/// Whether the exported runtime should request Vulkan validation layers on
	/// startup. This is separate from the editor render setting so developers can
	/// keep editor validation on without shipping exports that request layers.
	bool enableVulkanValidationLayers = false;

	/// Whether the exported runtime should collect renderer debug diagnostics.
	/// This is runtime diagnostic data, not managed/native debug symbol output.
	bool enableRenderDebugDiagnostics = false;

	/// Whether exported builds should keep renderer recovery diagnostics in
	/// logs and manifest metadata. This is CPU-only data, so it remains useful
	/// even when the renderer cannot draw a diagnostics overlay.
	bool rendererRecoveryDiagnosticsEnabled = true;

	/// User-facing fatal reason shown when graphics device loss stops an export.
	std::string rendererRecoveryFatalMessage = "Graphics device lost. Restart the application to restore rendering.";

	/// Additional NativeAOT publish arguments passed verbatim to dotnet publish.
	std::string additionalPublishArgs;

	/// Coarse export quality rank: 0 = fallback only, 1 = low, 2 = medium, 3 = high.
	uint32_t virtualGeometryQualityLevel = 2;

	/// Whether exported builds keep virtual geometry diagnostics available for runtime tooling.
	bool virtualGeometryDiagnosticsEnabled = false;

	/// Optional exported runtime geometry page budget. Zero keeps the runtime default.
	std::uint64_t virtualGeometryBudgetBytes = 0;

	/// Runtime LOD error scale applied by exported virtual geometry selection.
	float virtualGeometryLodErrorScale = 1.0f;

	/// Maximum number of detail levels an exported build may drop from desired detail.
	uint32_t virtualGeometryMaxLodDrop = 0;

	/// Whether exported builds may trust validated Hi-Z history for VG occlusion.
	/// The runtime still fails open when history, view, or guard metadata is unsafe.
	bool virtualGeometryHiZOcclusionEnabled = true;

	/// Runtime page preload bias used when exported builds request VG pages.
	float virtualGeometryPreloadBias = 1.0f;

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

/// Returns the export platform that can be NativeAOT-published on the current host.
ExportPlatform GetHostExportPlatform();

// Build-profile collection operations live beside profile persistence so the
// editor window, MCP bridge, and headless tools all apply the same identity and
// validation rules. Draft creation does not write files; Save and Remove are
// the explicit persistence boundaries.
struct BuildProfileCollectionResult {
	bool succeeded = false;
	std::string error;
	std::optional<BuildProfile> profile;
};

BuildProfileCollectionResult CreateBuildProfileDraft(
	std::string name,
	ExportPlatform platform = GetHostExportPlatform());
BuildProfileCollectionResult DuplicateBuildProfileDraft(
	const BuildProfile& source,
	std::string name = {});
BuildProfileCollectionResult FindBuildProfileInCollection(
	const std::filesystem::path& directory,
	std::string_view selector);
BuildProfileCollectionResult SaveBuildProfileToCollection(
	const std::filesystem::path& directory,
	BuildProfile profile);
BuildProfileCollectionResult RemoveBuildProfileFromCollection(
	const std::filesystem::path& directory,
	std::string_view profileId);

/// Converts an export platform to the stable lowercase string stored in JSON.
std::string ExportPlatformToString(ExportPlatform platform);

/// Parses a lowercase export platform string, defaulting to Windows for legacy profiles.
ExportPlatform ExportPlatformFromString(const std::string& value);

/// Returns the .NET runtime identifier used when publishing managed export payloads.
std::string GetExportRuntimeIdentifier(ExportPlatform platform);

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

/// Describes the installed, executable-relative NativeAOT toolchain used by
/// export. Every path is explicit so a gameplay project cannot inherit a user
/// NuGet cache, an ambient package source, or another concurrent build's
/// intermediate/output tree.
struct NativeAotPublishContext {
	/// Absolute executable from the SDK installed beside the editor.
	std::filesystem::path dotnetExecutable;
	/// Installed directory used for global.json SDK selection.
	std::filesystem::path workingDirectory;
	/// Installed offline NuGet configuration and bundled package feed.
	std::filesystem::path nugetConfig;
	std::filesystem::path nugetFeed;
	/// Private per-operation package cache and dotnet first-use state.
	std::filesystem::path packageCache;
	std::filesystem::path dotnetCliHome;
	/// Private per-operation MSBuild obj and bin directories.
	std::filesystem::path intermediateOutputDirectory;
	std::filesystem::path binaryOutputDirectory;
};

/// Builds the offline restore that materializes the NativeAOT package graph.
/// The publish command deliberately runs with --no-restore after this succeeds.
std::string BuildNativeAotRestoreCommand(
	const BuildProfile& profile,
	const std::filesystem::path& managedProject,
	const NativeAotPublishContext& context);

/// Builds the dotnet publish command used for NativeAOT gameplay compilation.
/// @param profile Build profile whose toggles become publish properties.
/// @param managedProject Full path to Assembly-CSharp.csproj.
/// @param managedOutputDir Full path where dotnet should place NativeAOT output.
/// @param context Installed fail-closed feed and project-local cache paths.
std::string BuildNativeAotPublishCommand(
	const BuildProfile& profile,
	const std::filesystem::path& managedProject,
	const std::filesystem::path& managedOutputDir,
	const NativeAotPublishContext& context);

/// Returns the engine shared library filename installed beside exported launchers.
std::string GetExportEngineLibraryName(ExportPlatform platform);

/// Returns the GLFW runtime library filename installed beside native consumers.
std::string GetExportGlfwLibraryName(ExportPlatform platform);
