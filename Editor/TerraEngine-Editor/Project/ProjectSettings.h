#pragma once

#include <nlohmann/json.hpp>

struct ProjectSettings {
    int _placeholder = 0; // Empty structs can be weird in some contexts, keeping dummy.

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ProjectSettings, _placeholder)
};