// ProjectSettings.h — Project-specific configuration settings
//
// Defines the data structure for project-specific settings that are persisted within the project directory. it currently serves as a placeholder for extensible project configurations and utilizes nlohmann/json for automated serialization.

#pragma once

#include <nlohmann/json.hpp>

struct ProjectSettings {
	int _placeholder = 0; // Empty structs can be weird in some contexts, keeping dummy.

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(ProjectSettings, _placeholder)
};
