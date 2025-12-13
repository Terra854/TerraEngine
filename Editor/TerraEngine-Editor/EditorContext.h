#pragma once

#include <memory>
// Forward declarations
class Engine;
class ProjectManager;
class AssetDatabase;

struct EditorContext {
    Engine* engine = nullptr;
    std::shared_ptr<ProjectManager> projectManager;
    std::shared_ptr<AssetDatabase> assetDatabase;
};
