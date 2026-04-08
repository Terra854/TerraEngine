// BuildProfile.cpp — Serialisation and utilities for build profiles.

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

static ExportPlatform StringToPlatform(const std::string& s) {
	if (s == "linux") return ExportPlatform::Linux;
	return ExportPlatform::Windows;
}

static std::string CompressionPresetToString(CompressionPreset p) {
	switch (p) {
	case CompressionPreset::None:   return "none";
	case CompressionPreset::Fast:   return "fast";
	case CompressionPreset::Normal: return "normal";
	case CompressionPreset::Best:   return "best";
	default:                         return "normal";
	}
}

static CompressionPreset StringToCompressionPreset(const std::string& s) {
	if (s == "none") return CompressionPreset::None;
	if (s == "fast") return CompressionPreset::Fast;
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

BuildProfile LoadBuildProfile(const std::filesystem::path& path) {
	BuildProfile profile;
	try {
		std::ifstream in(path);
		if (!in) {
			return profile;
		}

		nlohmann::json j = nlohmann::json::parse(in);

		if (j.contains("name"))                    profile.name = j["name"].get<std::string>();
		if (j.contains("id"))                      profile.id = j["id"].get<std::string>();
		if (j.contains("platform"))                profile.platform = StringToPlatform(j["platform"].get<std::string>());
		if (j.contains("outputDirectory"))         profile.outputDirectory = j["outputDirectory"].get<std::string>();
		if (j.contains("startupScene"))            profile.startupScene = j["startupScene"].get<std::string>();
		if (j.contains("compressionPreset"))       profile.compressionPreset = StringToCompressionPreset(j["compressionPreset"].get<std::string>());
		if (j.contains("tepakWorkerCount"))        profile.tepakWorkerCount = j["tepakWorkerCount"].get<uint32_t>();
		if (j.contains("enableManagedCompilation"))profile.enableManagedCompilation = j["enableManagedCompilation"].get<bool>();
		if (j.contains("includeDebugSymbols"))     profile.includeDebugSymbols = j["includeDebugSymbols"].get<bool>();
		if (j.contains("stripNativeAotOutput"))    profile.stripNativeAotOutput = j["stripNativeAotOutput"].get<bool>();
		if (j.contains("additionalPublishArgs"))   profile.additionalPublishArgs = j["additionalPublishArgs"].get<std::string>();
		if (j.contains("includedScenes")) {
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

std::vector<BuildProfile> LoadAllBuildProfiles(const std::filesystem::path& directory) {
	std::vector<BuildProfile> profiles;

	if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory)) {
		return profiles;
	}

	for (const auto& entry : std::filesystem::directory_iterator(directory)) {
		if (entry.is_regular_file() && entry.path().extension() == ".json") {
			auto profile = LoadBuildProfile(entry.path());
			if (!profile.id.empty()) {
				profiles.push_back(std::move(profile));
			}
		}
	}

	return profiles;
}
