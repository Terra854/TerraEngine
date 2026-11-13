// PrefabInstanceReset.cpp -- Production prefab reset entry point shared with native regressions.
// Detached preparation feeds one atomic protected transaction before selection changes.
#include "MainEditorSubsystem.h"
#include "Engine/Components/PrefabLinkComponent.h"
#include "Engine/Components/Transform.h"
#include "Engine/Core/Logger/Logger.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Scene/Serializer.h"
#include "Engine/Scene/TransactionalMutation.h"
#include "Engine/ThreadPool/ThreadPool.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace
{
	constexpr std::size_t kMaximumResetNodes = 4096u;
	constexpr std::uintmax_t kMaximumPrefabBytes = 8u * 1024u * 1024u;

	// Worker-only value preparation: bounded I/O and component decode never
	// borrow live scene storage. Concrete UUID collision checks remain protected.
	nlohmann::json ReadResetCandidate(const std::filesystem::path& path) {
		const auto bytes = std::filesystem::file_size(path);
		if(bytes == 0 || bytes > kMaximumPrefabBytes) throw std::runtime_error("Prefab exceeds the reset byte budget.");
		std::ifstream input(path, std::ios::binary);
		std::string text(static_cast<std::size_t>(bytes), '\0');
		if(!input.read(text.data(), static_cast<std::streamsize>(text.size())) || input.peek() != std::char_traits<char>::eof())
			throw std::runtime_error("Prefab read failed or its size changed during preparation.");
		auto document = nlohmann::json::parse(text);
		if(!document.contains("objects") || !document["objects"].is_array() || document["objects"].empty())
			throw std::runtime_error("Prefab has no reset root.");
		auto candidate = std::move(document["objects"].front());
		if(!Serializer::PrepareObjectRestore(candidate, kMaximumResetNodes).accepted)
			throw std::runtime_error("Prefab concrete values failed detached preparation.");
		return candidate;
	}

	// Only exact source-object IDs are rewritten inside authored component values.
	// Iteration avoids recursion on a hostile component payload; prefab link values
	// are source provenance and are deliberately excluded by the caller.
	void RemapResetComponentReferences(nlohmann::json& component,
		const std::unordered_map<TerraEngine::UUID, TerraEngine::UUID>& remap)
	{
		std::vector<nlohmann::json*> pending{&component};
		while(!pending.empty())
		{
			auto& value = *pending.back();
			pending.pop_back();
			if(value.is_string())
			{
				const auto source = TerraEngine::ParseUUID(value.get<std::string>());
				if(source)
					if(const auto found = remap.find(*source); found != remap.end()) value = TerraEngine::ToHexString(found->second);
			}
			else if(value.is_structured())
				for(auto& child : value) pending.push_back(&child);
		}
	}
}

bool MainEditorSubsystem::RequestPrefabInstanceReset(entt::entity entity) {
	// PostFrame editor interaction reads branch revisions; only the later
	// CommitTransaction writes SceneObjects/Hierarchy/Components. Worker work is
	// file/JSON/value preparation, with no new scheduler phase or flush.
	if(m_PrefabResetRequest || ThreadPool::instance().getThreadCount() == 0u) return false;
	auto& objects = ObjectManager::instance();
	const auto root = objects.getObject(entity);
	const auto* link = root ? objects.GetComponent<PrefabLinkComponent>(*root) : nullptr;
	if(!link || link->prefabPath.empty()) return false;
	PrefabResetRequest request;
	request.objectId = objects.getUUID(*root);
	request.sceneRoot = SceneManager::Instance().GetSceneRoot();
	TerraEngine::Scene::TransactionalMutationBuilder witness("prefab-reset-observation-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID()),
		TerraEngine::Scene::TransactionOrigin::Editor, "Observe pending prefab reset");
	witness.ObserveScene(objects);
	std::vector<Object> branch{*root};
	for(std::size_t index = 0; index < branch.size(); ++index) {
		witness.ObserveObject(objects, objects.getUUID(branch[index]));
		const auto children = objects.getChildren(branch[index]);
		if(children.size() > kMaximumResetNodes - branch.size()) return false;
		branch.insert(branch.end(), children.begin(), children.end());
	}
	if(const auto parent = objects.getParent(*root)) witness.ObserveObject(objects, objects.getUUID(*parent));
	request.witness = witness.Build();
	auto path = std::filesystem::path(link->prefabPath);
	if(!path.is_absolute()) path = request.sceneRoot / path;
	try {
		auto task = ThreadPool::instance().createTask([path = std::move(path)] { return ReadResetCandidate(path); });
		request.future = task->getFuture();
		ThreadPool::instance().enqueue(task, 1u);
		m_PrefabResetRequest = std::move(request);
		m_LastPrefabResetResult.reset();
		return true;
	} catch(const std::exception& error) {
		TERRAENGINE_LOG(LogLevel::Error, "Cannot enqueue prefab reset: " << error.what());
		return false;
	}
}

void MainEditorSubsystem::PollPrefabInstanceReset() {
	if(!m_PrefabResetRequest || !m_PrefabResetRequest->future.valid()
		|| m_PrefabResetRequest->future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
	bool accepted = false;
	try {
		m_PrefabResetRequest->prepared = m_PrefabResetRequest->future.get();
		const auto root = ObjectManager::instance().getObject(m_PrefabResetRequest->objectId);
		if(root && m_PrefabResetRequest->sceneRoot == SceneManager::Instance().GetSceneRoot())
			accepted = ResetPrefabInstanceToDefaults(root->EntityId());
	} catch(const std::exception& error) {
		TERRAENGINE_LOG(LogLevel::Error, "Pending prefab reset rejected: " << error.what());
	}
	// This protected owner publishes a terminal result even for normal transaction
	// conflicts, missing roots and project switches; none of these must look done.
	m_LastPrefabResetResult = accepted;
	if(!accepted) TERRAENGINE_LOG(LogLevel::Warning, "Pending prefab reset did not complete successfully; inspect reset diagnostics.");
	m_PrefabResetRequest.reset();
}

bool MainEditorSubsystem::ResetPrefabInstanceToDefaults(entt::entity object)
{
	ObjectManager& objManager = ObjectManager::instance();
	std::optional<Object> objectHandle = objManager.getObject(object);
	PrefabLinkComponent* link = objectHandle ? objManager.GetComponent<PrefabLinkComponent>(*objectHandle) : nullptr;
	if (link == nullptr || link->prefabPath.empty())
	{
		return false;
	}

	std::filesystem::path prefabPath = link->prefabPath;
	if (!prefabPath.is_absolute())
	{
		prefabPath = SceneManager::Instance().GetSceneRoot() / prefabPath;
	}

	// Editor interaction phase: read the selected branch, source file and external
	// parent; write the replacement branch/selection only at the explicit protected
	// CommitTransaction boundary below. File/JSON/concrete-value preparation is
	// detached; live UUID collision checks and publication remain on the UI owner.
	using namespace TerraEngine::Scene;
	const auto originalId = objManager.getUUID(*objectHandle);
	const auto parent = objManager.getParent(*objectHandle);
	TransactionalMutationBuilder builder("prefab-reset-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID()),
		TransactionOrigin::Editor, "Reset prefab instance");
	builder.SetBudget({kMaximumResetNodes + 1u, 128u});
	builder.ObserveScene(objManager).ObserveObject(objManager, originalId);
	if(parent) builder.ObserveObject(objManager, objManager.getUUID(*parent));
	// Capture descendants too: a changed authored component must invalidate the
	// prepared reset even if it did not change the hierarchy revision.
	std::vector<Object> observed{*objectHandle};
	for(std::size_t index = 0; index < observed.size(); ++index)
	{
		const auto children = objManager.getChildren(observed[index]);
		if(children.size() > kMaximumResetNodes - observed.size()) return false;
		for(const auto& child : children)
		{
			builder.ObserveObject(objManager, objManager.getUUID(child));
			observed.push_back(child);
		}
	}

	Transaction transaction;
	TerraEngine::UUID replacementId = TerraEngine::EMPTY_UUID;
	try
	{
		auto candidate = m_PrefabResetRequest && m_PrefabResetRequest->prepared
			&& m_PrefabResetRequest->objectId == originalId ? *m_PrefabResetRequest->prepared : ReadResetCandidate(prefabPath);
		// Decode every concrete component before touching the original. This also
		// rejects duplicate IDs, bad metadata/children and oversized restore graphs.
		const auto source = Serializer::PrepareObjectRestore(candidate, kMaximumResetNodes);
		if(!source.accepted || source.nodes.empty()) return false;
		const auto sourceRootId = source.nodes.front().id;
		std::unordered_map<TerraEngine::UUID, TerraEngine::UUID> remap;
		std::unordered_set<TerraEngine::UUID> reserved;
		// Stable instance IDs keep references held by objects outside this branch valid.
		// Source provenance, rather than child order or names, identifies descendants.
		std::unordered_map<TerraEngine::UUID, TerraEngine::UUID> existingBySource;
		std::unordered_set<TerraEngine::UUID> observedIds;
		for(const auto& existing : observed) observedIds.insert(objManager.getUUID(existing));
		if(!link->sourceToInstanceIds.empty()) {
			// The map is authored input: bound it and reject foreign/stale identities
			// before it can influence the protected replacement transaction.
			if(link->sourceToInstanceIds.size() > kMaximumPrefabBytes) return false;
			const auto ids = nlohmann::json::parse(link->sourceToInstanceIds);
			if(!ids.is_object() || ids.size() > kMaximumResetNodes) return false;
			for(const auto& [sourceText, instance] : ids.items()) {
				const auto sourceId = TerraEngine::ParseUUID(sourceText);
				const auto instanceId = instance.is_string() ? TerraEngine::ParseUUID(instance.get<std::string>()) : std::nullopt;
				if(!sourceId || !instanceId) return false;
				// Deleted descendants are allowed to disappear; never reuse their IDs.
				if(observedIds.contains(*instanceId)) existingBySource.emplace(*sourceId, *instanceId);
			}
		}
		for(const auto& existing : observed) {
			const auto* provenance = objManager.GetComponent<PrefabLinkComponent>(existing);
			if(provenance && provenance->prefabSourceId == link->prefabSourceId
				&& provenance->sourceObjectId != TerraEngine::EMPTY_UUID
				) {
				const auto [found, inserted] = existingBySource.emplace(provenance->sourceObjectId, objManager.getUUID(existing));
				if(!inserted && found->second != objManager.getUUID(existing)) return false;
			}
		}
		for(const auto& node : source.nodes)
		{
			auto instanceId = node.id == sourceRootId ? originalId : TerraEngine::EMPTY_UUID;
			if(instanceId == TerraEngine::EMPTY_UUID)
				if(const auto found = existingBySource.find(node.id); found != existingBySource.end()) instanceId = found->second;
			if(instanceId != TerraEngine::EMPTY_UUID) { if(!reserved.insert(instanceId).second) return false; }
			else {
				instanceId = TerraEngine::GenerateUUID();
				while(instanceId == TerraEngine::EMPTY_UUID || objManager.getObject(instanceId) || !reserved.insert(instanceId).second)
					instanceId = TerraEngine::GenerateUUID();
			}
			remap.emplace(node.id, instanceId);
		}
		// Legacy children may lack provenance, or the source may remove a child.
		// Reject before publication if replacing such a child would strand an external
		// authored UUID reference. Never guess identity from mutable names or order.
		std::unordered_set<TerraEngine::UUID> branchIds, removedIds;
		for(const auto& existing : observed) {
			const auto id = objManager.getUUID(existing); branchIds.insert(id);
			if(!reserved.contains(id)) removedIds.insert(id);
		}
		// Hierarchy enumeration covers every live authored object, including inactive
		// and Transform-less objects. Visit each once without component filtering.
		auto sceneObjects = removedIds.empty() ? std::vector<Object>{} : objManager.getRootObjects();
		for(std::size_t index = 0; !removedIds.empty() && index < sceneObjects.size(); ++index) {
			const auto external = sceneObjects[index];
			const auto children = objManager.getChildren(external);
			sceneObjects.insert(sceneObjects.end(), children.begin(), children.end());
			if(branchIds.contains(objManager.getUUID(external))) continue;
			const auto snapshot = Serializer::SerializeObjectShallow(external.EntityId());
			std::vector<const nlohmann::json*> values;
			if(auto components = snapshot.find("components"); components != snapshot.end())
				for(const auto& [name, value] : components->items()) if(name != PrefabLinkComponent::kComponentName) values.push_back(&value);
			while(!values.empty()) {
				const auto& value = *values.back(); values.pop_back();
				if(value.is_string()) { const auto id = TerraEngine::ParseUUID(value.get<std::string>()); if(id && removedIds.contains(*id)) return false; }
				else if(value.is_structured()) for(const auto& child : value) values.push_back(&child);
			}
		}
		replacementId = remap.at(sourceRootId);
		std::vector<nlohmann::json*> pending{&candidate};
		while(!pending.empty())
		{
			auto& node = *pending.back();
			pending.pop_back();
			const auto sourceId = TerraEngine::ParseUUID(node["uuid"].get<std::string>());
			node["uuid"] = TerraEngine::ToHexString(remap.at(*sourceId));
			if(auto components = node.find("components"); components != node.end())
				for(auto& [name, value] : components->items())
					if(name != PrefabLinkComponent::kComponentName) RemapResetComponentReferences(value, remap);
			if(auto children = node.find("children"); children != node.end())
				for(auto& child : *children) pending.push_back(&child);
		}
		// Build link metadata inside the detached candidate too. A failed link
		// decode/allocation must never occur after the original branch was deleted.
		std::error_code relativeError;
		const auto relative = std::filesystem::relative(prefabPath, SceneManager::Instance().GetSceneRoot(), relativeError);
		nlohmann::json instanceIds = nlohmann::json::object();
		for(const auto& [sourceId, instanceId] : remap)
			instanceIds[TerraEngine::ToHexString(sourceId)] = TerraEngine::ToHexString(instanceId);
		candidate["components"]["PrefabLinkComponent"] = {
			{"prefabSourceId", TerraEngine::ToHexString(link->prefabSourceId)},
			{"sourceObjectId", TerraEngine::ToHexString(sourceRootId)},
			{"prefabName", link->prefabName.empty() ? prefabPath.stem().string() : link->prefabName},
			{"prefabPath", relativeError ? prefabPath.generic_string() : relative.generic_string()},
			{"overriddenProperties", ""}, {"sourceToInstanceIds", instanceIds.dump()}};
		if(!Serializer::PrepareObjectRestore(candidate, kMaximumResetNodes).accepted) return false;
		builder.DeleteObject(originalId);
		transaction = builder.Build();
		// A user edit while detached preparation was pending must reject the old
		// request, rather than replacing its original observations with fresh ones.
		if(m_PrefabResetRequest && m_PrefabResetRequest->prepared)
			transaction.readSet = m_PrefabResetRequest->witness.readSet;
		TransactionWriteOperation restore;
		restore.kind = TransactionWriteKind::RestoreObjectSnapshot;
		restore.target = ExistingObject(replacementId);
		if(parent) restore.parent = ExistingObject(objManager.getUUID(*parent));
		restore.serializedObject = std::move(candidate);
		transaction.writeSet.operations.push_back(std::move(restore));
	}
	catch(const std::exception& error)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Cannot prepare prefab reset: " << error.what());
		return false;
	}

	// RestoreObjectSnapshot preflight rejects UUID collisions and decodes all
	// concrete values again. Commit retains deleted storage in its protected
	// journal; any failed restore/reparent rolls back the same objects/lifetimes.
	// Publication failures after commit deliberately propagate as committed errors.
	const auto result = CommitTransaction(objManager, transaction);
	if(result.status != TransactionStatus::Committed) return false;
	const auto restored = objManager.getObject(replacementId);
	if(!restored) throw std::logic_error("Committed prefab reset has no replacement root.");
	SetSelectedEntity(restored->EntityId());
	SceneManager::Instance().ForceMarkActiveSceneDirty();
	TERRAENGINE_LOG(LogLevel::Info, "Reset prefab instance to defaults from " << prefabPath);
	return true;
}

