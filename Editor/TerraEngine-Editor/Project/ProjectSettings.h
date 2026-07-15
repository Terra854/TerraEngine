// ProjectSettings.h — Project-specific configuration settings
//
// Defines the data structure for project-specific settings that are persisted within the project directory. it currently serves as a placeholder for extensible project configurations and utilizes nlohmann/json for automated serialization.

#pragma once

#include <nlohmann/json.hpp>
#include <array>
#include <string>
#include <string_view>

struct ProjectSettings {
	std::string projectDisplayName = "TerraEngine Project";

	friend void to_json(nlohmann::json& json, const ProjectSettings& settings)
	{
		json = nlohmann::json{
			{ "projectDisplayName", settings.projectDisplayName }
		};
	}

	friend void from_json(const nlohmann::json& json, ProjectSettings& settings)
	{
		// Keep old projects loadable even though early ProjectSettings files only
		// contained a placeholder field.
		settings.projectDisplayName = json.value("projectDisplayName", "TerraEngine Project");
	}
};

namespace TerraEngine::Editor::ProjectSettings
{
	// Physics policy has a dedicated project-owned document because exports and
	// the runtime consume it without loading editor automation preferences.
	inline constexpr std::string_view kPhysicsSettingsRelativePath =
		"ProjectSettings/PhysicsSettings.json";
}

namespace TerraEngine::Editor::Settings
{
	inline constexpr std::array<std::string_view, 1> kProjectAutomationSettingKeys{
		"projectDisplayName"
	};

	inline nlohmann::json ReadProjectAutomationSetting(const ::ProjectSettings& settings, std::string_view key)
	{
		if (key == "projectDisplayName")
			return settings.projectDisplayName;
		return nullptr;
	}

	inline bool WriteProjectAutomationSetting(
		::ProjectSettings& settings,
		std::string_view key,
		const nlohmann::json& value,
		std::string& error)
	{
		if (key != "projectDisplayName")
		{
			error = "Project setting key is not allowlisted.";
			return false;
		}
		if (!value.is_string() || value.get_ref<const std::string&>().find_first_not_of(" \t\r\n") == std::string::npos)
		{
			error = "projectDisplayName must be a non-blank string.";
			return false;
		}
		settings.projectDisplayName = value.get<std::string>();
		return true;
	}
}
