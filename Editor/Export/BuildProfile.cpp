// BuildProfile.cpp — Build profile configuration implementation
//
// Implements the serialization and utility functions for build profiles, using nlohmann/json for data persistence. It includes helpers for platform mapping, unique ID generation, and loading multiple profiles from the project directory.

#include "BuildProfile.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

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

// --- JSON helpers ---

static std::string PlatformToString(ExportPlatform p) {
	switch (p) {
	case ExportPlatform::Windows: return "windows";
	case ExportPlatform::Linux:   return "linux";
	default:                       return "windows";
	}
}

// StringToPlatform is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static ExportPlatform StringToPlatform(const std::string& s) {
	if (s == "linux") return ExportPlatform::Linux;
	return ExportPlatform::Windows;
}

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
		j["platform"] = PlatformToString(profile.platform);
		j["outputDirectory"] = profile.outputDirectory.string();
		j["startupScene"] = profile.startupScene;
		j["compressionPreset"] = CompressionPresetToString(profile.compressionPreset);
		j["tepakWorkerCount"] = profile.tepakWorkerCount;
		j["enableManagedCompilation"] = profile.enableManagedCompilation;
		j["includeDebugSymbols"] = profile.includeDebugSymbols;
		j["stripNativeAotOutput"] = profile.stripNativeAotOutput;
		j["additionalPublishArgs"] = profile.additionalPublishArgs;
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
		if (j.contains("platform"))                profile.platform = StringToPlatform(j["platform"].get<std::string>());
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
		if (j.contains("additionalPublishArgs"))   profile.additionalPublishArgs = j["additionalPublishArgs"].get<std::string>();
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
