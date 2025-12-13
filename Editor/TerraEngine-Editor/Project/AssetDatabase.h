#pragma once

#include "Utilities/UUID.h"
#include <filesystem>
#include <unordered_map>

class AssetDatabase {
public:
    void Initialize(const std::filesystem::path& projectRoot);
    void Refresh();
    
    // Placeholder API
    std::filesystem::path GetAssetPath(TerraEngine::UUID guid);
    TerraEngine::UUID GetAssetGUID(const std::filesystem::path& path);

private:
    std::filesystem::path rootPath;
};
