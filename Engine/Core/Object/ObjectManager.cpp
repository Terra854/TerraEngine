// ObjectManager.cpp — Implementation of the engine's central entity and hierarchy manager.
//
// This file implements the lifecycle management for all scene objects, including
// creation, destruction, and complex hierarchical parenting. It maintains the
// primary registry of entities, handles UUID mapping for persistence, and
// provides sophisticated query functions for locating objects by name or tag.

#include "ObjectManager.h"

#include <glm/gtc/quaternion.hpp>

ObjectManager& ObjectManager::instance() {
	static ObjectManager instance;
	return instance;
}

ObjectManager::ObjectManager()
{
	// Initialize default tags and layers that the editor and gameplay systems
	// expect to be present in any valid project.
	availableTags = { "Untagged", "MainCamera", "Player", "GameController" };
	availableLayers = { "Default", "TransparentFX", "Ignore Raycast", "Water", "UI" };
}

ObjectManager::~ObjectManager() = default;

// ObjectManager::registerTag is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ObjectManager::registerTag(const std::string& tag)
{
	if (std::find(availableTags.begin(), availableTags.end(), tag) == availableTags.end())
	{
		availableTags.push_back(tag);
	}
}

// ObjectManager::registerLayer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ObjectManager::registerLayer(const std::string& layer)
{
	if (std::find(availableLayers.begin(), availableLayers.end(), layer) == availableLayers.end())
	{
		availableLayers.push_back(layer);
	}
}

// ObjectManager::makeHandle is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
Object ObjectManager::makeHandle(entt::entity entityId)
{
	// Build a lightweight handle that references an existing entity without
	// constructing a new one. The registry pointer is injected so the caller
	// can safely access components on the returned object.
	return Object(registry, entityId);
}

// ObjectManager::notifySpatialObjectDirty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ObjectManager::notifySpatialObjectDirty(entt::entity entityId)
{
	// Spatial indexes consume this monotonically increasing revision to decide
	// when their cached world bounds may be stale. The callback carries the
	// specific entity for systems that want incremental refreshes.
	++spatialRevision;
	if (onSpatialObjectDirty)
	{
		onSpatialObjectDirty(entityId);
	}
}

// Create a new Object
// ObjectManager::createObject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
Object ObjectManager::createObject(const std::string& name) {
	// Every entity begins life with a Transform so systems can safely assume
	// spatial data exists without guarding for missing components.
	auto object = Object(registry);
	object.addComponent<Transform>();

	// Assign a stable identity immediately upon creation so the entity can be
	// tracked across serialization and undo sessions.
	TerraEngine::UUID uuid = TerraEngine::GenerateUUID();
	entityToUuid.emplace(object.EntityId(), uuid);
	uuidToEntity.emplace(uuid, object.EntityId());
	
	ObjectMetadata metadata;
	metadata.name = name;
	metadataMap.insert({ object, metadata });
	parentMap.emplace(object.EntityId(), entt::null);
	childrenMap.emplace(object.EntityId(), std::vector<entt::entity>{});
	
	// Structured modifications trigger the observer callback to sync dirty state.
	notifySpatialObjectDirty(object.EntityId());
	if (onModified) onModified();
	
	return object;
}

// Remove an Object
// ObjectManager::removeObject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ObjectManager::removeObject(Object obj) {
	notifySpatialObjectDirty(obj.id);
	// Destroying the entity purges every component attached to it (Transform and any others),
	// ensuring no dangling component data survives past object lifetime.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (registry.valid(obj.id))
	{
		// Purge the UUID mapping before the component is destroyed to maintain
		// consistency in the lookup table.
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto it = entityToUuid.find(obj.id); it != entityToUuid.end())
		{
			uuidToEntity.erase(it->second);
			entityToUuid.erase(it);
		}
		registry.destroy(obj.id);
	}

	// Recursively delete children so the entire branch is removed.
	// We copy the child list first because removeObject will mutate the map.
	std::vector<entt::entity> childrenToDelete;
	if (auto childIter = childrenMap.find(obj.id); childIter != childrenMap.end())
	{
		childrenToDelete = childIter->second;
		childrenMap.erase(childIter);
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (entt::entity childId : childrenToDelete)
	{
		// removeObject expects an Object handle.
		// Since we are inside ObjectManager, we can construct one efficiently.
		// Note: The child entity might have been destroyed if it was somehow 
		// processed already (cycle?), but our hierarchy should be a tree.
		if (registry.valid(childId)) {
			removeObject(makeHandle(childId));
		}
	}

	// Remove this object from its parent's child list so no stale references remain.
	detachFromParent(obj.id);
	parentMap.erase(obj.id);

	// Erase the object from the map
	metadataMap.erase(obj);

	if (onModified) onModified();
}

// ObjectManager::setParent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ObjectManager::setParent(Object child, std::optional<Object> parent)
{
	if (!registry.valid(child.id))
	{
		return false;
	}

	const entt::entity childId = child.id;
	const entt::entity parentId = parent ? parent->id : entt::null;

	// Prevent self-parenting and cycle creation by walking ancestors.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (parentId == childId || isDescendant(childId, parentId))
	{
		return false;
	}

	// Ensure bookkeeping entries exist so child lists stay stable.
	childrenMap.try_emplace(childId, std::vector<entt::entity>{});
	parentMap.try_emplace(childId, entt::null);
	if (parentId != entt::null)
	{
		childrenMap.try_emplace(parentId, std::vector<entt::entity>{});
		parentMap.try_emplace(parentId, entt::null);
	}

	auto readTransform = [&](entt::entity entityId) -> Transform {
		Transform result{};
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (Transform* t = registry.try_get<Transform>(entityId))
		{
			result.position = GetComponentProperty<Transform, glm::vec3>(*t, "Position");
			result.rotation = GetComponentProperty<Transform, glm::quat>(*t, "Rotation");
			result.scale = GetComponentProperty<Transform, glm::vec3>(*t, "Scale");
		}
		return result;
	};

	// Compute the child's world transform using the current hierarchy before reparenting.
	std::unordered_map<entt::entity, Transform> worldCacheBefore;
	std::function<Transform(entt::entity, std::unordered_map<entt::entity, Transform>&, std::unordered_set<entt::entity>&)> computeWorld =
		[&](entt::entity entityId, std::unordered_map<entt::entity, Transform>& cache, std::unordered_set<entt::entity>& visiting) -> Transform {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (auto cached = cache.find(entityId); cached != cache.end())
			{
				return cached->second;
			}
			Transform local = readTransform(entityId);
			entt::entity p = getParentEntity(entityId);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (p != entt::null && visiting.insert(entityId).second)
			{
				Transform parentWorld = computeWorld(p, cache, visiting);
				local.position = parentWorld.position + parentWorld.rotation * (parentWorld.scale * local.position);
				local.rotation = parentWorld.rotation * local.rotation;
				local.scale = parentWorld.scale * local.scale;
				visiting.erase(entityId);
			}
			cache[entityId] = local;
			return local;
		};
	std::unordered_set<entt::entity> visitingBefore;
	const Transform childWorld = computeWorld(childId, worldCacheBefore, visitingBefore);

	detachFromParent(childId);

	parentMap[childId] = parentId;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (parentId != entt::null)
	{
		childrenMap[parentId].push_back(childId);
	}

	// Preserve world-space placement by converting the saved world transform
	// into a new local transform relative to the target parent.
	Transform* childTransform = registry.try_get<Transform>(childId);
	if (childTransform)
	{
		std::unordered_map<entt::entity, Transform> worldCacheAfter;
		std::unordered_set<entt::entity> visitingAfter;
		Transform parentWorld{};
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (parentId != entt::null)
		{
			parentWorld = computeWorld(parentId, worldCacheAfter, visitingAfter);
		}
		const glm::quat invParentRot = glm::inverse(parentWorld.rotation);
		glm::vec3 invParentScale;
		const float kEpsilon = 1e-6f;
		invParentScale.x = std::abs(parentWorld.scale.x) < kEpsilon ? 0.0f : 1.0f / parentWorld.scale.x;
		invParentScale.y = std::abs(parentWorld.scale.y) < kEpsilon ? 0.0f : 1.0f / parentWorld.scale.y;
		invParentScale.z = std::abs(parentWorld.scale.z) < kEpsilon ? 0.0f : 1.0f / parentWorld.scale.z;

		glm::vec3 localPos = invParentRot * (childWorld.position - parentWorld.position);
		localPos *= invParentScale;
		glm::quat localRot = invParentRot * childWorld.rotation;
		glm::vec3 localScale = childWorld.scale * invParentScale;

		SetComponentProperty(*childTransform, "Position", localPos);
		SetComponentProperty(*childTransform, "Rotation", localRot);
		SetComponentProperty(*childTransform, "Scale", localScale);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (onModified) onModified();
	notifySpatialObjectDirty(childId);

	return true;
}

// ObjectManager::getChildren is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<Object> ObjectManager::getChildren(const Object& parent)
{
	std::vector<Object> children;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!registry.valid(parent.id))
	{
		return children;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto iter = childrenMap.find(parent.id); iter != childrenMap.end())
	{
		for (entt::entity childId : iter->second)
		{
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (registry.valid(childId))
			{
				children.push_back(makeHandle(childId));
			}
		}
	}
	// Stable hierarchy queries are important for editor trees, saved scenes,
	// and runtime summaries. Sort by UUID instead of transient EnTT ids so
	// reloads and undo sessions keep the same parent-before-child ordering.
	std::sort(children.begin(), children.end(), [this](const Object& lhs, const Object& rhs) {
		return TerraEngine::ToHexString(getUUID(lhs.EntityId())) < TerraEngine::ToHexString(getUUID(rhs.EntityId()));
	});
	return children;
}

// ObjectManager::getRootObjects is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<Object> ObjectManager::getRootObjects()
{
	std::vector<Object> roots;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& [objectHandle, metadata] : metadataMap)
	{
		(void)metadata;
		const entt::entity entityId = objectHandle.EntityId();
		auto parentIter = parentMap.find(entityId);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (parentIter == parentMap.end() || parentIter->second == entt::null)
		{
			roots.push_back(makeHandle(entityId));
		}
	}
	// Root order is part of the authored scene surface, so make it independent
	// from unordered_map bucket order and stable across editor/runtime/export.
	std::sort(roots.begin(), roots.end(), [this](const Object& lhs, const Object& rhs) {
		return TerraEngine::ToHexString(getUUID(lhs.EntityId())) < TerraEngine::ToHexString(getUUID(rhs.EntityId()));
	});
	return roots;
}

// ObjectManager::getObjectName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ObjectManager::getObjectName(const Object& obj) const
{
	static const std::string kUnnamed = "Unnamed";
	auto iter = metadataMap.find(obj);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (iter != metadataMap.end())
	{
		return iter->second.name;
	}
	return kUnnamed;
}

// ObjectManager::setObjectName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ObjectManager::setObjectName(const Object& obj, const std::string& name)
{
	if (auto it = metadataMap.find(obj); it != metadataMap.end())
	{
		it->second.name = name;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (onModified) onModified();
	}
}

// ObjectManager::getObjectTag is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ObjectManager::getObjectTag(const Object& obj) const
{
	static const std::string kUntagged = "Untagged";
	auto iter = metadataMap.find(obj);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (iter != metadataMap.end())
	{
		return iter->second.tag;
	}
	return kUntagged;
}

// ObjectManager::setObjectTag is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ObjectManager::setObjectTag(const Object& obj, const std::string& tag)
{
	if (auto it = metadataMap.find(obj); it != metadataMap.end())
	{
		it->second.tag = tag;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (onModified) onModified();
	}
}

// ObjectManager::getObjectLayer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ObjectManager::getObjectLayer(const Object& obj) const
{
	static const std::string kDefault = "Default";
	auto iter = metadataMap.find(obj);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (iter != metadataMap.end())
	{
		return iter->second.layer;
	}
	return kDefault;
}

// ObjectManager::setObjectLayer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ObjectManager::setObjectLayer(const Object& obj, const std::string& layer)
{
	if (auto it = metadataMap.find(obj); it != metadataMap.end())
	{
		it->second.layer = layer;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (onModified) onModified();
	}
}

// ObjectManager::isObjectActive is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ObjectManager::isObjectActive(const Object& obj) const
{
	auto iter = metadataMap.find(obj);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (iter != metadataMap.end())
	{
		return iter->second.active;
	}
	return true;
}

// ObjectManager::setObjectActive is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ObjectManager::setObjectActive(const Object& obj, bool active)
{
	if (auto it = metadataMap.find(obj); it != metadataMap.end())
	{
		it->second.active = active;
		notifySpatialObjectDirty(obj.EntityId());
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (onModified) onModified();
	}
}

// ObjectManager::isObjectStatic is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ObjectManager::isObjectStatic(const Object& obj) const
{
	auto iter = metadataMap.find(obj);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (iter != metadataMap.end())
	{
		return iter->second.isStatic;
	}
	return false;
}

// ObjectManager::setObjectStatic is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ObjectManager::setObjectStatic(const Object& obj, bool isStatic)
{
	if (auto it = metadataMap.find(obj); it != metadataMap.end())
	{
		it->second.isStatic = isStatic;
		notifySpatialObjectDirty(obj.EntityId());
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (onModified) onModified();
	}
}

// ObjectManager::FindObjectByName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<Object> ObjectManager::FindObjectByName(std::string_view name) const
{
	// Names live in the side-table because they are editor-facing metadata
	// rather than ECS component state. Iterate that table so lookups stay in
	// sync with the hierarchy names authors actually see.
	for (const auto& [objectHandle, metadata] : metadataMap)
	{
		if (!registry.valid(objectHandle.EntityId()))
		{
			continue;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (metadata.name == name)
		{
			return const_cast<ObjectManager*>(this)->makeHandle(objectHandle.EntityId());
		}
	}

	return std::nullopt;
}

// ObjectManager::FindObjectByTag is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<Object> ObjectManager::FindObjectByTag(std::string_view tag) const
{
	// Tags now live in the metadata side-table.
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& [objectHandle, metadata] : metadataMap)
	{
		if (!registry.valid(objectHandle.EntityId()))
		{
			continue;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (metadata.active && metadata.tag == tag)
		{
			return const_cast<ObjectManager*>(this)->makeHandle(objectHandle.EntityId());
		}
	}

	return std::nullopt;
}

// ObjectManager::getObject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<Object> ObjectManager::getObject(entt::entity id)
{
	if (!registry.valid(id))
	{
		return std::nullopt;
	}
	return makeHandle(id);
}

// ObjectManager::getObject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<Object> ObjectManager::getObject(const TerraEngine::UUID& id)
{
	auto it = uuidToEntity.find(id);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (it != uuidToEntity.end())
	{
		return getObject(it->second);
	}
	return std::nullopt;
}

// ObjectManager::UpdateEntityUUID is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ObjectManager::UpdateEntityUUID(entt::entity entity, const TerraEngine::UUID& newId)
{
	if (!registry.valid(entity)) return;

	// Allows the identity of an existing entity to be swapped. Primarily used
	// during scene deserialization to restore authored or undo-session IDs
	// while keeping the lookup table in sync.
	if (auto it = entityToUuid.find(entity); it != entityToUuid.end())
	{
		uuidToEntity.erase(it->second);
		it->second = newId;
		uuidToEntity[newId] = entity;
	}
	else
	{
		entityToUuid[entity] = newId;
		uuidToEntity[newId] = entity;
	}
	notifySpatialObjectDirty(entity);
}

// ObjectManager::detachFromParent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ObjectManager::detachFromParent(entt::entity childId)
{
	if (auto parentIter = parentMap.find(childId); parentIter != parentMap.end())
	{
		const entt::entity parentId = parentIter->second;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (parentId != entt::null)
		{
			if (auto childList = childrenMap.find(parentId); childList != childrenMap.end())
			{
				auto& siblings = childList->second;
				siblings.erase(std::remove(siblings.begin(), siblings.end(), childId), siblings.end());
			}
		}
	}
}

// ObjectManager::isDescendant is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ObjectManager::isDescendant(entt::entity ancestor, entt::entity potentialChild) const
{
	entt::entity cursor = potentialChild;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (cursor != entt::null)
	{
		if (cursor == ancestor)
		{
			return true;
		}

		auto parentIter = parentMap.find(cursor);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (parentIter == parentMap.end())
		{
			break;
		}
		cursor = parentIter->second;
	}
	return false;
}

// ObjectManager::getParentEntity is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
entt::entity ObjectManager::getParentEntity(entt::entity child) const
{
	auto iter = parentMap.find(child);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (iter != parentMap.end())
	{
		return iter->second;
	}
	return entt::null;
}

// ObjectManager::getUUID is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::UUID ObjectManager::getUUID(entt::entity entity)
{
	if (auto it = entityToUuid.find(entity); it != entityToUuid.end())
	{
		return it->second;
	}
	return TerraEngine::UUID();
}

// ObjectManager::clear is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ObjectManager::clear()
{
	registry.clear();
	metadataMap.clear();
	parentMap.clear();
	childrenMap.clear();
	uuidToEntity.clear();
	entityToUuid.clear();
}
