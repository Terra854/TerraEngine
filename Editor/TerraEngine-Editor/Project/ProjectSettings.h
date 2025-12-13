#pragma once

#include <string>
#include <vector>
#include "Utilities/UUID.h"
#include <nlohmann/json.hpp>

struct ProjectSettings {
    std::string projectName = "New Project";
    std::string version = "0.0.1";
    std::string renderPipeline = "Forward";
    TerraEngine::UUID startupSceneGuid = TerraEngine::EMPTY_UUID;
    std::string assetsPath = "Assets";
    std::string libraryPath = "Library";

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ProjectSettings, projectName, version, renderPipeline, startupSceneGuid, assetsPath, libraryPath)
};
