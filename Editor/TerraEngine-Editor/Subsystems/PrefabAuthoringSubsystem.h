// PrefabAuthoringSubsystem.h - editor-facing prefab workflow API.

#pragma once

#include "IEditorSubsystem.h"
#include "Scene/PrefabComposition.h"

#include <filesystem>
#include <optional>
#include <unordered_map>

class PrefabAuthoringSubsystem : public IEditorSubsystem {
public:
	void Initialize(EditorContext& context) override;
	void Update(float deltaTime) override;
	void DrawUI() override;
	void Shutdown() override;
	std::string GetName() const override;

	TerraEngine::Scene::PrefabAuthoringSession CreatePrefabFromSelection(TerraEngine::Scene::PrefabSource source);
	std::optional<TerraEngine::Scene::PrefabAuthoringSession> OpenPrefab(TerraEngine::UUID sourceId);
	bool SavePrefab(TerraEngine::UUID sourceId);
	std::vector<TerraEngine::Scene::SceneDiagnostic> DeletePrefab(TerraEngine::UUID sourceId, bool convertInstances);
	void ApplyInstanceOverridesToDefaults(TerraEngine::UUID sourceId, const TerraEngine::Scene::PrefabInstance& instance, const std::vector<TerraEngine::Scene::PrefabOverride>& selectedOverrides);
	void ResetInstanceOverrides(TerraEngine::Scene::PrefabInstance& instance, const std::optional<TerraEngine::UUID>& targetObjectId = std::nullopt);

	const std::unordered_map<TerraEngine::UUID, TerraEngine::Scene::PrefabSource>& GetSources() const;
	std::vector<TerraEngine::Scene::SceneDiagnostic> GetDiagnostics() const;

private:
	EditorContext* context = nullptr;
	std::unordered_map<TerraEngine::UUID, TerraEngine::Scene::PrefabSource> sources;
	std::vector<TerraEngine::Scene::PrefabInstance> instances;
	std::vector<TerraEngine::Scene::SceneDiagnostic> diagnostics;
};
