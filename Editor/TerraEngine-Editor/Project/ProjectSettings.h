// ProjectSettings.h — Project-specific configuration settings
//
// Defines the data structure for project-specific settings that are persisted within the project directory. it currently serves as a placeholder for extensible project configurations and utilizes nlohmann/json for automated serialization.

#pragma once

#include <nlohmann/json.hpp>
#include <string>

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
