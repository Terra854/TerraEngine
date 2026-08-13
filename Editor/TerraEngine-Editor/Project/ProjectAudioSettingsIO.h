// ProjectAudioSettingsIO.h - validated project-owned audio mixer persistence.

#pragma once

#include "Audio/AudioMixConfiguration.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace TerraEngine::Editor::ProjectSettings {
	inline constexpr std::string_view kAudioSettingsRelativePath = "ProjectSettings/AudioSettings.json";

	inline nlohmann::json BuildAudioMixSettingsJson(const Audio::AudioMixSnapshot& snapshot) {
		nlohmann::json groups = nlohmann::json::array();
		for(const auto& group : snapshot.groups) {
			groups.push_back({
				{"id", TerraEngine::ToHexString(group.id)},
				{"name", group.name},
				{"volume", group.volume},
				{"muted", group.muted}
			});
		}
		return {
			{"schemaVersion", 1U},
			{"master", {{"volume", snapshot.master.volume}, {"muted", snapshot.master.muted}}},
			{"groups", std::move(groups)}
		};
	}

	inline bool ReadProjectAudioMixSettings(
		const std::filesystem::path& projectRoot,
		Audio::AudioMixSnapshot& output,
		std::string& error) {
		const auto path = projectRoot / kAudioSettingsRelativePath;
		if(!std::filesystem::exists(path)) {
			error.clear();
			return true;
		}
		try {
			std::ifstream input(path);
			const auto document = nlohmann::json::parse(input);
			if(document.value("schemaVersion", 0U) != 1U)
				throw std::runtime_error("unsupported schema version");
			Audio::AudioMixSnapshot candidate;
			candidate.groups.clear();
			candidate.master.volume = document.at("master").at("volume").get<float>();
			candidate.master.muted = document.at("master").at("muted").get<bool>();
			for(const auto& groupJson : document.at("groups")) {
				const auto id = TerraEngine::ParseUUID(groupJson.at("id").get<std::string>());
				if(!id)
					throw std::runtime_error("invalid group identity");
				candidate.groups.push_back({*id, groupJson.at("name").get<std::string>(),
					groupJson.at("volume").get<float>(), groupJson.at("muted").get<bool>(), 1U});
			}
			if(!Audio::AudioMixConfiguration::Validate(candidate, error))
				return false;
			output = std::move(candidate);
			error.clear();
			return true;
		} catch(const std::exception& exception) {
			error = "Audio Settings could not be read: " + std::string(exception.what());
			return false;
		}
	}

	inline bool PersistProjectAudioMixSettings(
		const std::filesystem::path& projectRoot,
		const Audio::AudioMixSnapshot& snapshot,
		std::string& error) {
		if(!Audio::AudioMixConfiguration::Validate(snapshot, error))
			return false;
		const auto path = projectRoot / kAudioSettingsRelativePath;
		std::error_code filesystemError;
		std::filesystem::create_directories(path.parent_path(), filesystemError);
		if(filesystemError) {
			error = "Audio Settings directory could not be created.";
			return false;
		}
		const auto temporaryPath = std::filesystem::path(path.string() + ".tmp");
		{
			std::ofstream output(temporaryPath, std::ios::trunc);
			if(!output) {
				error = "Audio Settings temporary file could not be written.";
				return false;
			}
			output << BuildAudioMixSettingsJson(snapshot).dump(2);
		}
		std::filesystem::rename(temporaryPath, path, filesystemError);
		if(filesystemError) {
			std::filesystem::remove(path, filesystemError);
			filesystemError.clear();
			std::filesystem::rename(temporaryPath, path, filesystemError);
		}
		if(filesystemError) {
			std::filesystem::remove(temporaryPath);
			error = "Audio Settings could not be atomically published.";
			return false;
		}
		error.clear();
		return true;
	}
}
