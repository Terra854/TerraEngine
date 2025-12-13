#include "AssetDatabase.h"

void AssetDatabase::Initialize(const std::filesystem::path& projectRoot) {
    rootPath = projectRoot;
}

void AssetDatabase::Refresh() {
    // Scan logic
}

std::filesystem::path AssetDatabase::GetAssetPath(TerraEngine::UUID guid) {
    return "";
}

TerraEngine::UUID AssetDatabase::GetAssetGUID(const std::filesystem::path& path) {
    return TerraEngine::EMPTY_UUID;
}
