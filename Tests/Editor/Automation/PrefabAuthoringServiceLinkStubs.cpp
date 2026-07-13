#include "Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.h"
#include "Editor/TerraEngine-Editor/Subsystems/UndoRedoSystem.h"
#include "Engine/Core/Object/ObjectManager.h"

#include <fstream>

// The automation service now delegates Play Mode lifecycle to the native main
// editor. Unit tests intentionally do not link that ImGui/Vulkan workspace, so
// these unavailable-state doubles satisfy linkage without simulating a world.
bool MainEditorSubsystem::StartPlayMode()
{
	return false;
}

bool MainEditorSubsystem::SetPlayModePaused(bool)
{
	return false;
}

bool MainEditorSubsystem::StepPlayMode()
{
	return false;
}

void MainEditorSubsystem::StopPlayMode()
{
}

// The editor-test target links automation services without the complete main
// workspace/UI graph. This test-side implementation mirrors the owning prefab
// service contract; the installed editor links the production implementation
// in MainEditorSubsystem.cpp.
std::string PrefabAuthoringService::ResolvePrefabId(std::string_view identity) const
{
	const std::string value(identity);
	if (prefabsById.contains(value)) return value;
	const auto found = prefabIdByPath.find(std::filesystem::path(value).lexically_normal().generic_string());
	return found == prefabIdByPath.end() ? std::string{} : found->second;
}

PrefabAuthoringService::Result PrefabAuthoringService::Create(entt::entity sourceEntity, const std::filesystem::path& relativePath, const std::filesystem::path& absolutePath)
{
	if (sourceEntity == entt::null || absolutePath.extension() != ".teprefab" || std::filesystem::exists(absolutePath))
		return {false, "validation", "Prefab source or destination is invalid."};
	const Object object = ObjectManager::instance().makeObjectHandle(sourceEntity);
	const std::string sourceId = TerraEngine::ToHexString(ObjectManager::instance().getUUID(object));
	const std::string sourceName = ObjectManager::instance().getObjectName(object);
	const std::string prefabId = "prefab-" + std::to_string(nextPrefabId++);
	std::error_code error;
	std::filesystem::create_directories(absolutePath.parent_path(), error);
	if (error) return {false, "internal", "Could not create the prefab directory: " + error.message()};
	std::ofstream output(absolutePath, std::ios::binary | std::ios::trunc);
	output << nlohmann::json{{"prefabId", prefabId}, {"sourceObjectId", sourceId}, {"name", sourceName}}.dump(2);
	if (!output.good()) return {false, "internal", "Could not publish the prefab asset."};
	const std::string relative = relativePath.lexically_normal().generic_string();
	prefabsById[prefabId] = {prefabId, relative, sourceId, sourceName, 1u};
	prefabIdByPath[relative] = prefabId;
	return {true, {}, "Created prefab.", {{"prefabId", prefabId}, {"sourceObjectId", sourceId}, {"path", relative}}};
}

PrefabAuthoringService::Result PrefabAuthoringService::Instantiate(std::string_view identity, std::string_view requestedName)
{
	const std::string prefabId = ResolvePrefabId(identity);
	if (prefabId.empty()) return {false, "missing_target", "Prefab was not found."};
	if (!activeContextPrefabId.empty() && activeContextPrefabId == prefabId)
		return {false, "conflict", "A prefab cannot contain itself.", {{"rolledBack", true}}};
	const PrefabState& prefab = prefabsById.at(prefabId);
	Object instance = ObjectManager::instance().createObject(requestedName.empty() ? prefab.sourceName : std::string(requestedName));
	const std::string instanceId = TerraEngine::ToHexString(ObjectManager::instance().getUUID(instance));
	instances[instanceId] = {prefabId, prefab.sourceName};
	UndoRedoSystem::Instance().PushCommand(std::make_unique<CreateObjectCommand>(instance.EntityId()));
	return {true, {}, "Instantiated prefab.", {{"prefabId", prefabId}, {"instanceId", instanceId}}};
}

PrefabAuthoringService::Result PrefabAuthoringService::InspectOverrides(std::string_view identity) const
{
	const std::string instanceId(identity);
	const auto state = instances.find(instanceId);
	const auto uuid = TerraEngine::ParseUUID(instanceId);
	const auto instance = uuid ? ObjectManager::instance().getObject(*uuid) : std::nullopt;
	if (state == instances.end() || !instance) return {false, "missing_target", "Prefab instance was not found."};
	const PrefabState& prefab = prefabsById.at(state->second.prefabId);
	const std::string name = ObjectManager::instance().getObjectName(*instance);
	nlohmann::json overrides = nlohmann::json::array();
	if (name != prefab.sourceName) overrides.push_back({{"property", "name"}, {"value", name}, {"sourceValue", prefab.sourceName}});
	return {true, {}, "Inspected prefab overrides.", {{"overrides", overrides}, {"prefabGeneration", prefab.generation}}};
}

PrefabAuthoringService::Result PrefabAuthoringService::ApplyOverrides(std::string_view identity, std::optional<std::uint64_t> expectedGeneration)
{
	const std::string instanceId(identity);
	const auto state = instances.find(instanceId);
	const auto uuid = TerraEngine::ParseUUID(instanceId);
	const auto instance = uuid ? ObjectManager::instance().getObject(*uuid) : std::nullopt;
	if (state == instances.end() || !instance) return {false, "missing_target", "Prefab instance was not found."};
	PrefabState& prefab = prefabsById.at(state->second.prefabId);
	if (expectedGeneration && *expectedGeneration != prefab.generation) return {false, "stale_target", "Prefab source generation changed."};
	prefab.sourceName = ObjectManager::instance().getObjectName(*instance);
	++prefab.generation;
	return {true, {}, "Applied prefab overrides.", {{"historyId", "history-prefab-" + std::to_string(prefab.generation)}, {"prefabGeneration", prefab.generation}}};
}

PrefabAuthoringService::Result PrefabAuthoringService::RevertOverrides(std::string_view identity)
{
	const std::string instanceId(identity);
	const auto state = instances.find(instanceId);
	const auto uuid = TerraEngine::ParseUUID(instanceId);
	const auto instance = uuid ? ObjectManager::instance().getObject(*uuid) : std::nullopt;
	if (state == instances.end() || !instance) return {false, "missing_target", "Prefab instance was not found."};
	ObjectManager::instance().setObjectName(*instance, prefabsById.at(state->second.prefabId).sourceName);
	return {true, {}, "Reverted prefab overrides.", {{"remainingOverrideCount", 0u}}};
}

PrefabAuthoringService::Result PrefabAuthoringService::OpenEditContext(std::string_view identity)
{
	const std::string prefabId = ResolvePrefabId(identity);
	if (prefabId.empty()) return {false, "missing_target", "Prefab was not found."};
	if (!activeContextId.empty()) return {false, "conflict", "Another prefab editing context is already active."};
	activeContextId = "prefab-context-" + std::to_string(nextPrefabId++);
	activeContextPrefabId = prefabId;
	return {true, {}, "Opened prefab editing context.", {{"contextId", activeContextId}, {"prefabId", prefabId}}};
}

PrefabAuthoringService::Result PrefabAuthoringService::CloseEditContext(std::string_view identity)
{
	if (identity != activeContextId) return {false, "stale_target", "Prefab editing context identity is stale."};
	activeContextId.clear();
	activeContextPrefabId.clear();
	return {true, {}, "Closed prefab editing context."};
}
