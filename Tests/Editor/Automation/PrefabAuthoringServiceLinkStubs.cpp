#include "Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.h"
#include "Editor/TerraEngine-Editor/Subsystems/UndoRedoSystem.h"
#include "Engine/Components/PrefabLinkComponent.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Scene/Serializer.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
	std::optional<TerraEngine::UUID> ReadStubPrefabUuid(const nlohmann::json& objectJson)
	{
		const auto uuid = objectJson.find("uuid");
		return uuid != objectJson.end() && uuid->is_string()
			? TerraEngine::ParseUUID(uuid->get<std::string>())
			: std::nullopt;
	}

	bool PrepareStubPrefabIds(
		nlohmann::json& objectJson,
		std::unordered_map<TerraEngine::UUID, TerraEngine::UUID>& remap,
		std::vector<TerraEngine::UUID>& generated)
	{
		const auto source = ReadStubPrefabUuid(objectJson);
		TerraEngine::UUID instanceId = TerraEngine::GenerateUUID();
		while (ObjectManager::instance().getObject(instanceId).has_value()
			|| std::find(generated.begin(), generated.end(), instanceId) != generated.end())
		{
			instanceId = TerraEngine::GenerateUUID();
		}
		if (source && !remap.emplace(*source, instanceId).second)
		{
			return false;
		}
		generated.push_back(instanceId);
		objectJson["uuid"] = TerraEngine::ToHexString(instanceId);
		if (auto children = objectJson.find("children"); children != objectJson.end() && children->is_array())
		{
			for (nlohmann::json& child : *children)
			{
				if (!PrepareStubPrefabIds(child, remap, generated)) return false;
			}
		}
		return true;
	}

	void RewriteStubPrefabIds(nlohmann::json& value, const std::unordered_map<TerraEngine::UUID, TerraEngine::UUID>& remap)
	{
		if (value.is_string())
		{
			if (const auto parsed = TerraEngine::ParseUUID(value.get<std::string>()); parsed)
			{
				if (const auto replacement = remap.find(*parsed); replacement != remap.end())
				{
					value = TerraEngine::ToHexString(replacement->second);
				}
			}
			return;
		}
		if (value.is_array())
		{
			for (nlohmann::json& child : value) RewriteStubPrefabIds(child, remap);
			return;
		}
		if (value.is_object())
		{
			for (auto& [name, child] : value.items())
			{
				(void)name;
				RewriteStubPrefabIds(child, remap);
			}
		}
	}
}

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
	prefabsById[prefabId] = {prefabId, relative, sourceId, sourceName, 1u, {}, TerraEngine::EMPTY_UUID, {}, false};
	prefabIdByPath[relative] = prefabId;
	return {true, {}, "Created prefab.", {{"prefabId", prefabId}, {"sourceObjectId", sourceId}, {"path", relative}}};
}

PrefabAuthoringService::Result PrefabAuthoringService::RegisterExistingAsset(
	const std::filesystem::path& projectRelativePath,
	const std::filesystem::path& absolutePath,
	const TerraEngine::UUID& assetGuid,
	std::uint64_t publishedGeneration)
{
	const std::filesystem::path relativePath = projectRelativePath.lexically_normal();
	const auto firstSegment = relativePath.begin();
	if (assetGuid == TerraEngine::EMPTY_UUID || relativePath.empty() || relativePath.is_absolute()
		|| relativePath.extension() != ".teprefab" || firstSegment == relativePath.end()
		|| firstSegment->generic_string() != "Assets"
		|| std::find(relativePath.begin(), relativePath.end(), std::filesystem::path("..")) != relativePath.end()
		|| !absolutePath.is_absolute() || absolutePath.extension() != ".teprefab")
		return {false, "validation", "Registered prefab identity or path is invalid."};
	std::ifstream input(absolutePath, std::ios::binary);
	if (!input) return {false, "missing_target", "Registered prefab file was not found."};
	nlohmann::json document;
	try { input >> document; }
	catch (const nlohmann::json::exception&) { return {false, "validation", "Registered prefab document is malformed."}; }
	if (!document.is_object()) return {false, "validation", "Registered prefab document must be a JSON object."};
	const bool hasSerializedGraph = document.contains("objects");
	if (hasSerializedGraph && (!document["objects"].is_array() || document["objects"].empty()))
		return {false, "validation", "Registered prefab object graph must contain at least one root."};
	std::string sourceName;
	std::string sourceObjectId;
	if (const auto name = document.find("name"); name != document.end())
	{
		if (!name->is_string()) return {false, "validation", "Registered prefab name must be a string."};
		sourceName = name->get<std::string>();
	}
	if (const auto sourceId = document.find("sourceObjectId"); sourceId != document.end())
	{
		if (!sourceId->is_string()) return {false, "validation", "Registered prefab sourceObjectId must be a string."};
		sourceObjectId = sourceId->get<std::string>();
	}
	if (document.contains("objects") && document["objects"].is_array() && !document["objects"].empty()
		&& document["objects"].front().is_object())
	{
		const nlohmann::json& firstRoot = document["objects"].front();
		if (sourceName.empty())
		{
			if (const auto name = firstRoot.find("name"); name != firstRoot.end())
			{
				if (!name->is_string()) return {false, "validation", "Registered prefab root name must be a string."};
				sourceName = name->get<std::string>();
			}
		}
		if (sourceObjectId.empty())
		{
			if (const auto sourceId = firstRoot.find("uuid"); sourceId != firstRoot.end())
			{
				if (!sourceId->is_string()) return {false, "validation", "Registered prefab root UUID must be a string."};
				sourceObjectId = sourceId->get<std::string>();
			}
		}
	}
	if (sourceName.empty()) sourceName = absolutePath.stem().string();
	const std::string prefabId = TerraEngine::ToHexString(assetGuid);
	const std::string relative = relativePath.generic_string();
	const std::uint64_t generation = std::max<std::uint64_t>(1u, publishedGeneration);
	if (const auto existing = prefabsById.find(prefabId); existing != prefabsById.end())
		prefabIdByPath.erase(existing->second.relativePath);
	prefabsById[prefabId] = {
		prefabId,
		relative,
		sourceObjectId,
		sourceName,
		generation,
		std::filesystem::absolute(absolutePath).lexically_normal(),
		assetGuid,
		std::move(document),
		hasSerializedGraph
	};
	prefabIdByPath[relative] = prefabId;
	return {true, {}, "Registered existing prefab asset.", {{"prefabId", prefabId}, {"sourceObjectId", sourceObjectId}, {"path", relative}, {"prefabGeneration", generation}}};
}

PrefabAuthoringService::Result PrefabAuthoringService::Instantiate(
	std::string_view identity,
	std::string_view requestedName,
	std::optional<entt::entity> parent)
{
	const std::string prefabId = ResolvePrefabId(identity);
	if (prefabId.empty()) return {false, "missing_target", "Prefab was not found."};
	if (!activeContextPrefabId.empty() && activeContextPrefabId == prefabId)
		return {false, "conflict", "A prefab cannot contain itself.", {{"rolledBack", true}}};
	const PrefabState& prefab = prefabsById.at(prefabId);
	if (prefab.hasSerializedGraph)
	{
		nlohmann::json document = prefab.serializedDocument;
		const TerraEngine::Scene::PreparedSceneDecode prepared = Serializer::PrepareSceneDecode(document);
		if (!prepared.accepted || prepared.objectCount == 0U)
			return {false, "validation", prepared.reason.empty() ? "Registered prefab has no root objects." : prepared.reason, {{"rolledBack", true}}};

		ObjectManager& objects = ObjectManager::instance();
		if (objects.IsBulkPublicationActive())
			return {false, "conflict", "Another protected scene publication is already active.", {{"rolledBack", true}}};
		std::optional<TerraEngine::UUID> parentUuid;
		if (parent)
		{
			const std::optional<Object> parentObject = objects.getObject(*parent);
			if (!parentObject) return {false, "validation", "Prefab parent no longer exists.", {{"rolledBack", true}}};
			parentUuid = objects.getUUID(*parentObject);
		}

		std::vector<std::optional<TerraEngine::UUID>> sourceRootIds;
		std::unordered_map<TerraEngine::UUID, TerraEngine::UUID> remap;
		std::vector<TerraEngine::UUID> generated;
		for (std::size_t index = 0U; index < document["objects"].size(); ++index)
		{
			nlohmann::json& root = document["objects"][index];
			sourceRootIds.push_back(ReadStubPrefabUuid(root));
			if (index == 0U && !requestedName.empty()) root["name"] = std::string(requestedName);
			if (!PrepareStubPrefabIds(root, remap, generated))
				return {false, "validation", "Prefab graph contains duplicate object UUIDs.", {{"rolledBack", true}}};
		}
		for (nlohmann::json& root : document["objects"]) RewriteStubPrefabIds(root, remap);

		const bool sceneWasDirty = SceneManager::Instance().IsActiveSceneDirty();
		std::vector<entt::entity> roots;
		std::vector<std::unique_ptr<ICommand>> commands;
		objects.BeginBulkPublication({prepared.objectCount, prepared.componentCount});
		bool publicationActive = true;
		auto rollback = [&]() {
			for (const TerraEngine::UUID& objectId : generated)
			{
				if (std::optional<Object> object = objects.getObject(objectId)) objects.removeObject(*object);
			}
			if (publicationActive) objects.FinishBulkPublication(false);
			if (!sceneWasDirty) SceneManager::Instance().ClearActiveSceneDirty();
		};
		try
		{
			for (std::size_t index = 0U; index < document["objects"].size(); ++index)
			{
				const entt::entity root = Serializer::DeserializeObjectEntity(document["objects"][index], parent);
				const std::optional<Object> object = objects.getObject(root);
				if (!object) throw std::runtime_error("Prefab root was not created.");
				PrefabLinkComponent* link = objects.GetComponent<PrefabLinkComponent>(*object);
				if (link == nullptr) link = objects.AddComponent<PrefabLinkComponent>(*object);
				if (link == nullptr) throw std::runtime_error("Prefab link could not be attached.");
				link->prefabSourceId = prefab.assetGuid;
				link->sourceObjectId = sourceRootIds[index].value_or(objects.getUUID(*object));
				link->prefabName = prefab.absolutePath.stem().string();
				link->prefabPath = prefab.relativePath;
				roots.push_back(root);
				commands.push_back(std::make_unique<CreateObjectCommand>(root));
				if (parentUuid) commands.push_back(std::make_unique<SetParentCommand>(root, std::nullopt, parentUuid));
			}
			for (const TerraEngine::UUID& objectId : generated)
				if (!objects.getObject(objectId)) throw std::runtime_error("Prefab graph omitted an authored object.");
		}
		catch (const std::exception&)
		{
			rollback();
			return {false, "validation", "Registered prefab graph was rejected.", {{"rolledBack", true}}};
		}
		objects.FinishBulkPublication(true);
		publicationActive = false;
		UndoRedoSystem::Instance().PushTransactionGroup("Instantiate Prefab", std::move(commands));
		SceneManager::Instance().ForceMarkActiveSceneDirty();
		const Object instance = objects.makeObjectHandle(roots.front());
		const std::string instanceId = TerraEngine::ToHexString(objects.getUUID(instance));
		instances[instanceId] = {prefabId, prefab.sourceName};
		return {true, {}, "Instantiated prefab graph.", {
			{"prefabId", prefabId}, {"instanceId", instanceId}, {"path", prefab.relativePath},
			{"prefabGeneration", prefab.generation}, {"rootCount", roots.size()}, {"objectCount", prepared.objectCount}}};
	}

	Object instance = ObjectManager::instance().createObject(requestedName.empty() ? prefab.sourceName : std::string(requestedName));
	const std::string instanceId = TerraEngine::ToHexString(ObjectManager::instance().getUUID(instance));
	instances[instanceId] = {prefabId, prefab.sourceName};
	UndoRedoSystem::Instance().PushCommand(std::make_unique<CreateObjectCommand>(instance.EntityId()));
	return {true, {}, "Instantiated prefab.", {{"prefabId", prefabId}, {"instanceId", instanceId}, {"path", prefab.relativePath}, {"prefabGeneration", prefab.generation}}};
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
