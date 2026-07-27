// BuildProfile.cpp — Build profile configuration implementation
//
// Implements the serialization and utility functions for build profiles, using nlohmann/json for data persistence. It includes helpers for platform mapping, unique ID generation, and loading multiple profiles from the project directory.

#include "BuildProfile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

namespace {
	bool IsBlank(std::string_view value) {
		return std::all_of(value.begin(), value.end(), [](unsigned char character) {
			return std::isspace(character) != 0;
		});
	}

	bool IsSafeProfileId(std::string_view value) {
		return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char character) {
			return std::isalnum(character) != 0 || character == '-' || character == '_';
		});
	}
}

namespace {
	std::string PathToUtf8(const std::filesystem::path& path) {
		const std::u8string encoded = path.generic_u8string();
		return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
	}

	std::string QuoteShellArgument(std::string_view value) {
#ifdef _WIN32
		// Export currently runs through cmd.exe. Percent expansion still occurs
		// inside double quotes, while a caret inside those quotes is preserved.
		// Close the quoted segment around each percent, emit the caret outside,
		// and reopen it. cmd consumes that caret and the child process receives
		// one literal percent while all spaces remain in one logical argument.
		std::string quoted = "\"";
		for(const char character : value) {
			if(character == '%') quoted += "\"^%\"";
			else quoted += character;
		}
		quoted += '"';
		return quoted;
#else
		// POSIX single quotes suppress $, backtick, backslash, and whitespace
		// interpretation. A literal quote is represented by closing the quoted
		// region, emitting an escaped quote, and reopening it.
		std::string quoted = "'";
		for(const char character : value) {
			if(character == '\'') quoted += "'\\''";
			else quoted += character;
		}
		quoted += '\'';
		return quoted;
#endif
	}

	std::string QuoteShellPath(const std::filesystem::path& path) {
		return QuoteShellArgument(PathToUtf8(path));
	}

	std::string QuoteShellDirectoryProperty(
		const std::filesystem::path& directory)
	{
		// MSBuild concatenates several of these properties with filenames. A
		// generic trailing slash keeps the value unambiguous on every host and
		// avoids a backslash escaping the closing quote in downstream tools.
		std::string value = PathToUtf8(directory.lexically_normal());
		if(value.empty() || value.back() != '/') value += '/';
		return QuoteShellArgument(value);
	}

	std::string GeneratedTreeExcludesShellValue()
	{
		// A command-line `%3B` survives as an escaped literal when the value is
		// later consumed by an item Exclude. Preserve it through shell/MSBuild
		// command parsing, then deliberately unescape during project evaluation
		// so DefaultItemExcludes becomes five real patterns rather than one path.
		constexpr std::string_view kDelayedMsBuildExcludes =
			"$([MSBuild]::Unescape('obj/**%3Bbin/**%3BBuild/**%3BLibrary/**%3BTemp/**'))";
		return QuoteShellArgument(kDelayedMsBuildExcludes);
	}
}

// --- ID generation ---

std::string GenerateBuildProfileId() {
	// Generate an 8-character hex ID using a random engine seeded by the
	// high-resolution clock. Collision risk is acceptably low for the small
	// number of profiles a project typically has.
	auto seed = static_cast<uint64_t>(
		std::chrono::high_resolution_clock::now().time_since_epoch().count());
	std::mt19937_64 rng(seed);
	std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFFU);
	std::ostringstream oss;
	oss << std::hex << std::setfill('0') << std::setw(8) << dist(rng);
	return oss.str();
}

// --- Platform helpers ---

std::string ExportPlatformToString(ExportPlatform platform) {
	switch (platform) {
	case ExportPlatform::Windows: return "windows";
	case ExportPlatform::Linux:   return "linux";
	default:                       return "windows";
	}
}

ExportPlatform ExportPlatformFromString(const std::string& value) {
	if (value == "linux") return ExportPlatform::Linux;
	return ExportPlatform::Windows;
}

std::string GetExportRuntimeIdentifier(ExportPlatform platform) {
	return platform == ExportPlatform::Linux ? "linux-x64" : "win-x64";
}

ExportPlatform GetHostExportPlatform() {
#ifdef _WIN32
	return ExportPlatform::Windows;
#elif defined(__linux__)
	return ExportPlatform::Linux;
#else
	return ExportPlatform::Windows;
#endif
}

bool IsExportPlatformNativeToHost(ExportPlatform platform) {
	// .NET NativeAOT does not support cross-OS publishing, so managed export
	// compilation can target only the current host OS.
	return platform == GetHostExportPlatform();
}

std::string GetExportExecutableSuffix(ExportPlatform platform) {
	return platform == ExportPlatform::Windows ? ".exe" : "";
}

std::string GetExportSharedLibrarySuffix(ExportPlatform platform) {
	return platform == ExportPlatform::Windows ? ".dll" : ".so";
}

std::string GetExportLauncherFileName(const std::string& projectName, ExportPlatform platform) {
	return projectName + GetExportExecutableSuffix(platform);
}

std::string GetExportManagedRuntimeLibraryName(ExportPlatform platform) {
	return "Assembly-CSharp" + GetExportSharedLibrarySuffix(platform);
}

namespace {
	void AppendEngineOwnedWorkspaceProperties(
		std::ostringstream& command,
		const NativeAotPublishContext& context)
	{
		// Restore and publish must consume exactly the same assets file and build
		// tree. These values are appended after profile arguments so a saved
		// profile cannot redirect generated files into the authored project.
		command
			<< " -p:BaseIntermediateOutputPath="
			<< QuoteShellDirectoryProperty(context.intermediateOutputDirectory)
			<< " -p:MSBuildProjectExtensionsPath="
			<< QuoteShellDirectoryProperty(context.intermediateOutputDirectory)
			<< " -p:BaseOutputPath="
			<< QuoteShellDirectoryProperty(context.binaryOutputDirectory)
			<< " -p:OutputPath="
			<< QuoteShellDirectoryProperty(context.binaryOutputDirectory)
			<< " -p:RestorePackagesPath="
			<< QuoteShellDirectoryProperty(context.packageCache)
			// Moving BaseIntermediateOutputPath also moves the SDK's implicit
			// obj exclusion. Reassert every TerraEngine-generated project tree
			// so stale editor outputs cannot be compiled as authored source.
			// MSBuild parses raw semicolons as command-switch separators before
			// evaluating the quoted property, so encode each list separator with
			// its documented property escape. A delayed MSBuild Unescape turns
			// those protected separators into a true Exclude list only when the
			// project evaluates DefaultItemExcludes.
			<< " -p:DefaultItemExcludes="
			<< GeneratedTreeExcludesShellValue();
	}

	void AppendOfflineNuGetProperties(
		std::ostringstream& command,
		const NativeAotPublishContext& context)
	{
		// These options appear after profile-provided arguments. MSBuild uses the
		// final property value, which prevents a saved profile from re-enabling an
		// ambient source, fallback folder, or online vulnerability endpoint.
		command << " -p:RestoreSources="
			<< QuoteShellDirectoryProperty(context.nugetFeed)
			<< " -p:RestoreFallbackFolders="
			<< " -p:RestoreAdditionalProjectSources="
			<< " -p:RestoreIgnoreFailedSources=false"
			<< " -p:NuGetAudit=false";
	}

	void AppendOfflineNuGetRestoreArguments(
		std::ostringstream& command,
		const NativeAotPublishContext& context)
	{
		// These switches belong to `dotnet restore`; `dotnet publish` does not
		// accept them even when --no-restore is present. The following MSBuild
		// properties are shared with publish because both commands understand them.
		command << " --configfile " << QuoteShellPath(context.nugetConfig)
			<< " --packages "
			<< QuoteShellDirectoryProperty(context.packageCache)
			<< " --no-http-cache"
			<< " --disable-parallel";
	}
}

std::string BuildNativeAotRestoreCommand(
	const BuildProfile& profile,
	const std::filesystem::path& managedProject,
	const NativeAotPublishContext& context)
{
	// NativeAOT packages are selected by the SDK during restore, not by a
	// PackageReference in the gameplay project. Make the property explicit so
	// a missing vendored compiler fails before publish starts writing output.
	std::ostringstream command;
	command << "dotnet restore"
		<< " " << QuoteShellPath(managedProject)
		<< " -r " << GetExportRuntimeIdentifier(profile.platform)
		<< " -p:PublishAot=true";
	AppendOfflineNuGetRestoreArguments(command, context);
	AppendEngineOwnedWorkspaceProperties(command, context);
	AppendOfflineNuGetProperties(command, context);
	return command.str();
}

std::string BuildNativeAotPublishCommand(
	const BuildProfile& profile,
	const std::filesystem::path& managedProject,
	const std::filesystem::path& managedOutputDir,
	const NativeAotPublishContext& context)
{
	// The export pipeline runs this exact command through the shell. Keeping the
	// string construction in one helper lets tests verify profile toggles and
	// the final fail-closed NativeAOT boundary without launching dotnet.
	std::ostringstream command;
	command << "dotnet publish"
		<< " " << QuoteShellPath(managedProject)
		<< " -c Release"
		<< " -r " << GetExportRuntimeIdentifier(profile.platform)
		<< " -p:PublishAot=true"
		<< " -p:DefineConstants=TERRAENGINE_EXPORT";

	if (profile.stripNativeAotOutput) {
		command << " -p:StripSymbols=true";
	}

	if (!profile.includeDebugSymbols) {
		command << " -p:DebugType=none";
	}

	if (!profile.additionalPublishArgs.empty()) {
		command << " " << profile.additionalPublishArgs;
	}

	AppendEngineOwnedWorkspaceProperties(command, context);
	// PublishDir is separate from the shared binary output. Appending both the
	// MSBuild property and dotnet's output switch after profile arguments makes
	// the private publish directory authoritative even for hostile overrides.
	command << " -p:PublishDir="
		<< QuoteShellDirectoryProperty(managedOutputDir)
		<< " -o " << QuoteShellDirectoryProperty(managedOutputDir);
	AppendOfflineNuGetProperties(command, context);
	// Keep this final so a saved profile cannot append a restore-enabling switch
	// after the engine's fail-closed decision.
	command << " --no-restore";
	return command.str();
}

std::string GetExportEngineLibraryName(ExportPlatform platform) {
	return platform == ExportPlatform::Windows ? "TerraEngine.dll" : "libTerraEngine.so";
}

std::string GetExportGlfwLibraryName(ExportPlatform platform) {
	return platform == ExportPlatform::Windows ? "glfw3.dll" : "libglfw.so";
}

// --- JSON helpers ---

// CompressionPresetToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static std::string CompressionPresetToString(CompressionPreset p) {
	switch (p) {
	case CompressionPreset::None:   return "none";
	case CompressionPreset::Fast:   return "fast";
	case CompressionPreset::Normal: return "normal";
	case CompressionPreset::Best:   return "best";
	default:                         return "normal";
	}
}

// StringToCompressionPreset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static CompressionPreset StringToCompressionPreset(const std::string& s) {
	if (s == "none") return CompressionPreset::None;
	if (s == "fast") return CompressionPreset::Fast;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (s == "best") return CompressionPreset::Best;
	return CompressionPreset::Normal;
}

// --- Serialisation ---

bool SaveBuildProfile(const BuildProfile& profile, const std::filesystem::path& path) {
	try {
		nlohmann::json j;
		j["name"] = profile.name;
		j["id"] = profile.id;
		j["platform"] = ExportPlatformToString(profile.platform);
		j["outputDirectory"] = profile.outputDirectory.string();
		j["startupScene"] = profile.startupScene;
		j["compressionPreset"] = CompressionPresetToString(profile.compressionPreset);
		j["tepakWorkerCount"] = profile.tepakWorkerCount;
		j["enableManagedCompilation"] = profile.enableManagedCompilation;
		j["includeDebugSymbols"] = profile.includeDebugSymbols;
		j["stripNativeAotOutput"] = profile.stripNativeAotOutput;
		j["enableVulkanValidationLayers"] = profile.enableVulkanValidationLayers;
		j["enableRenderDebugDiagnostics"] = profile.enableRenderDebugDiagnostics;
		j["rendererRecovery"] = {
			{ "diagnosticsEnabled", profile.rendererRecoveryDiagnosticsEnabled },
			{ "fatalMessage", profile.rendererRecoveryFatalMessage }
		};
		j["additionalPublishArgs"] = profile.additionalPublishArgs;
		j["virtualGeometry"] = {
			{ "qualityLevel", profile.virtualGeometryQualityLevel },
			{ "diagnosticsEnabled", profile.virtualGeometryDiagnosticsEnabled },
			{ "geometryBudgetBytes", profile.virtualGeometryBudgetBytes },
			{ "lodErrorScale", profile.virtualGeometryLodErrorScale },
			{ "maxLodDrop", profile.virtualGeometryMaxLodDrop },
			{ "hiZOcclusionEnabled", profile.virtualGeometryHiZOcclusionEnabled },
			{ "preloadBias", profile.virtualGeometryPreloadBias }
		};
		j["includedScenes"] = profile.includedScenes;

		// Ensure the parent directory exists before writing.
		std::filesystem::create_directories(path.parent_path());

		std::ofstream out(path);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!out) {
			return false;
		}
		out << j.dump(4);
		return out.good();
	}
	catch (...) {
		return false;
	}
}

// LoadBuildProfile is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
BuildProfile LoadBuildProfile(const std::filesystem::path& path) {
	BuildProfile profile;
	try {
		std::ifstream in(path);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!in) {
			return profile;
		}

		nlohmann::json j = nlohmann::json::parse(in);

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (j.contains("name"))                    profile.name = j["name"].get<std::string>();
		if (j.contains("id"))                      profile.id = j["id"].get<std::string>();
		if (j.contains("platform"))                profile.platform = ExportPlatformFromString(j["platform"].get<std::string>());
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (j.contains("outputDirectory"))         profile.outputDirectory = j["outputDirectory"].get<std::string>();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (j.contains("startupScene"))            profile.startupScene = j["startupScene"].get<std::string>();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (j.contains("compressionPreset"))       profile.compressionPreset = StringToCompressionPreset(j["compressionPreset"].get<std::string>());
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (j.contains("tepakWorkerCount"))        profile.tepakWorkerCount = j["tepakWorkerCount"].get<uint32_t>();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (j.contains("enableManagedCompilation"))profile.enableManagedCompilation = j["enableManagedCompilation"].get<bool>();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (j.contains("includeDebugSymbols"))     profile.includeDebugSymbols = j["includeDebugSymbols"].get<bool>();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (j.contains("stripNativeAotOutput"))    profile.stripNativeAotOutput = j["stripNativeAotOutput"].get<bool>();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (j.contains("enableVulkanValidationLayers")) profile.enableVulkanValidationLayers = j["enableVulkanValidationLayers"].get<bool>();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (j.contains("enableRenderDebugDiagnostics")) profile.enableRenderDebugDiagnostics = j["enableRenderDebugDiagnostics"].get<bool>();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto recoveryIt = j.find("rendererRecovery"); recoveryIt != j.end() && recoveryIt->is_object()) {
			profile.rendererRecoveryDiagnosticsEnabled = recoveryIt->value("diagnosticsEnabled", profile.rendererRecoveryDiagnosticsEnabled);
			profile.rendererRecoveryFatalMessage = recoveryIt->value("fatalMessage", profile.rendererRecoveryFatalMessage);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (j.contains("additionalPublishArgs"))   profile.additionalPublishArgs = j["additionalPublishArgs"].get<std::string>();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto virtualGeometryIt = j.find("virtualGeometry"); virtualGeometryIt != j.end() && virtualGeometryIt->is_object()) {
			profile.virtualGeometryQualityLevel = virtualGeometryIt->value("qualityLevel", profile.virtualGeometryQualityLevel);
			profile.virtualGeometryDiagnosticsEnabled = virtualGeometryIt->value("diagnosticsEnabled", profile.virtualGeometryDiagnosticsEnabled);
			profile.virtualGeometryBudgetBytes = virtualGeometryIt->value("geometryBudgetBytes", profile.virtualGeometryBudgetBytes);
			profile.virtualGeometryLodErrorScale = virtualGeometryIt->value("lodErrorScale", profile.virtualGeometryLodErrorScale);
			profile.virtualGeometryMaxLodDrop = virtualGeometryIt->value("maxLodDrop", profile.virtualGeometryMaxLodDrop);
			profile.virtualGeometryHiZOcclusionEnabled = virtualGeometryIt->value("hiZOcclusionEnabled", profile.virtualGeometryHiZOcclusionEnabled);
			profile.virtualGeometryPreloadBias = virtualGeometryIt->value("preloadBias", profile.virtualGeometryPreloadBias);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (j.contains("includedScenes")) {
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const auto& scene : j["includedScenes"]) {
				profile.includedScenes.push_back(scene.get<std::string>());
			}
		}
		profile.filePath = path;
		profile.isDirty = false;
	}
	catch (...) {
		// Return default profile on parse failure.
	}
	return profile;
}

// LoadAllBuildProfiles is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<BuildProfile> LoadAllBuildProfiles(const std::filesystem::path& directory) {
	std::vector<BuildProfile> profiles;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory)) {
		return profiles;
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& entry : std::filesystem::directory_iterator(directory)) {
		if (entry.is_regular_file() && entry.path().extension() == ".json") {
			auto profile = LoadBuildProfile(entry.path());
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!profile.id.empty()) {
				profiles.push_back(std::move(profile));
			}
		}
	}

	return profiles;
}

BuildProfileCollectionResult CreateBuildProfileDraft(std::string name, ExportPlatform platform) {
	if (name.empty() || IsBlank(name)) {
		return { false, "Build profile name must contain a visible character.", std::nullopt };
	}

	BuildProfile profile;
	profile.id = GenerateBuildProfileId();
	profile.name = std::move(name);
	profile.platform = platform;
	profile.isDirty = true;
	return { true, {}, std::move(profile) };
}

BuildProfileCollectionResult DuplicateBuildProfileDraft(const BuildProfile& source, std::string name) {
	if (source.id.empty()) {
		return { false, "A persisted profile identity is required before duplication.", std::nullopt };
	}
	if (name.empty()) {
		name = source.name + " (Copy)";
	}
	if (IsBlank(name)) {
		return { false, "Build profile name must contain a visible character.", std::nullopt };
	}

	BuildProfile duplicate = source;
	duplicate.id = GenerateBuildProfileId();
	duplicate.name = std::move(name);
	duplicate.filePath.clear();
	duplicate.isDirty = true;
	return { true, {}, std::move(duplicate) };
}

BuildProfileCollectionResult FindBuildProfileInCollection(
	const std::filesystem::path& directory,
	std::string_view selector) {
	if (selector.empty()) {
		return { false, "Build profile selector is required.", std::nullopt };
	}

	for (BuildProfile profile : LoadAllBuildProfiles(directory)) {
		if (profile.id == selector || profile.name == selector || profile.filePath.filename().string() == selector) {
			return { true, {}, std::move(profile) };
		}
	}
	return { false, "Build profile was not found.", std::nullopt };
}

BuildProfileCollectionResult SaveBuildProfileToCollection(
	const std::filesystem::path& directory,
	BuildProfile profile) {
	if (profile.name.empty() || IsBlank(profile.name)) {
		return { false, "Build profile name must contain a visible character.", std::nullopt };
	}
	if (profile.id.empty()) {
		profile.id = GenerateBuildProfileId();
	}
	if (!IsSafeProfileId(profile.id)) {
		return { false, "Build profile identity contains unsafe path characters.", std::nullopt };
	}

	const std::filesystem::path path = directory / (profile.id + ".json");
	if (!SaveBuildProfile(profile, path)) {
		return { false, "Build profile could not be persisted.", std::nullopt };
	}
	profile.filePath = path;
	profile.isDirty = false;
	return { true, {}, std::move(profile) };
}

BuildProfileCollectionResult RemoveBuildProfileFromCollection(
	const std::filesystem::path& directory,
	std::string_view profileId) {
	if (!IsSafeProfileId(profileId)) {
		return { false, "A safe build profile identity is required.", std::nullopt };
	}

	BuildProfileCollectionResult found = FindBuildProfileInCollection(directory, profileId);
	if (!found.succeeded || !found.profile.has_value()) {
		return found;
	}
	std::error_code error;
	// Legacy profiles may have a filename that differs from their stable ID, so
	// remove the path discovered by collection loading rather than guessing it.
	const bool removed = std::filesystem::remove(found.profile->filePath, error);
	if (error || !removed) {
		return { false, error ? error.message() : "Build profile file was not found.", std::nullopt };
	}
	return { true, {}, std::move(found.profile) };
}
