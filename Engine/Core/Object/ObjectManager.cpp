#include "ObjectManager.h"

#include "Components/TagComponent.h"

#include <glm/gtc/quaternion.hpp>

ObjectManager& ObjectManager::instance() {
	static ObjectManager instance;
	return instance;
}

Object ObjectManager::makeHandle(entt::entity entityId)
{
	// Build a lightweight handle that references an existing entity without
	// constructing a new one. The registry pointer is injected so the caller
	// can safely access components on the returned object.
	return Object(registry, entityId);
}

// Create a new Object
Object ObjectManager::createObject(const std::string& name) {
	// Every entity begins life with a Transform so systems can safely assume
	// spatial data exists without guarding for missing components. Objects also
	// get a TagComponent immediately so FindWithTag never depends on optional
	// component setup that gameplay authors could forget.
	auto object = Object(registry);
	object.addComponent<Transform>();
	object.addComponent<TagComponent>();
	// Assign a stable identity immediately upon creation so the entity can be
	// tracked across serialization and undo sessions.
	TerraEngine::UUID uuid = TerraEngine::GenerateUUID();
	entityToUuid.emplace(object.EntityId(), uuid);
	uuidToEntity.emplace(uuid, object.EntityId());
	
	objectMap.insert({ object, name });
	parentMap.emplace(object.EntityId(), entt::null);
	childrenMap.emplace(object.EntityId(), std::vector<entt::entity>{});
	
	// Structured modifications trigger the observer callback to sync dirty state.
	if (onModified) onModified();
	
	return object;
}

// Remove an Object
void ObjectManager::removeObject(Object obj) {
	// Destroying the entity purges every component attached to it (Transform and any others),
	// ensuring no dangling component data survives past object lifetime.
	if (registry.valid(obj.id))
	{
		// Purge the UUID mapping before the component is destroyed to maintain
		// consistency in the lookup table.
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
	objectMap.erase(obj);

	if (onModified) onModified();
}

bool ObjectManager::setParent(Object child, std::optional<Object> parent)
{
	if (!registry.valid(child.id))
	{
		return false;
	}

	const entt::entity childId = child.id;
	const entt::entity parentId = parent ? parent->id : entt::null;

	// Prevent self-parenting and cycle creation by walking ancestors.
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
			if (auto cached = cache.find(entityId); cached != cache.end())
			{
				return cached->second;
			}
			Transform local = readTransform(entityId);
			entt::entity p = getParentEntity(entityId);
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

	if (onModified) onModified();

	return true;
}

std::vector<Object> ObjectManager::getChildren(const Object& parent)
{
	std::vector<Object> children;
	if (!registry.valid(parent.id))
	{
		return children;
	}

	if (auto iter = childrenMap.find(parent.id); iter != childrenMap.end())
	{
		for (entt::entity childId : iter->second)
		{
			if (registry.valid(childId))
			{
				children.push_back(makeHandle(childId));
			}
		}
	}
	return children;
}

std::vector<Object> ObjectManager::getRootObjects()
{
	std::vector<Object> roots;
	for (const auto& [objectHandle, name] : objectMap)
	{
		(void)name;
		const entt::entity entityId = objectHandle.EntityId();
		auto parentIter = parentMap.find(entityId);
		if (parentIter == parentMap.end() || parentIter->second == entt::null)
		{
			roots.push_back(makeHandle(entityId));
		}
	}
	return roots;
}

std::string ObjectManager::getObjectName(const Object& obj) const
{
	static const std::string kUnnamed = "Unnamed";
	auto iter = objectMap.find(obj);
	if (iter != objectMap.end())
	{
		return iter->second;
	}
	return kUnnamed;
}

void ObjectManager::setObjectName(const Object& obj, const std::string& name)
{
	if (objectMap.find(obj) != objectMap.end())
	{
		objectMap[obj] = name;
		if (onModified) onModified();
	}
}

std::optional<Object> ObjectManager::FindObjectByName(std::string_view name) const
{
	// Names live in the side-table because they are editor-facing metadata
	// rather than ECS component state. Iterate that table so lookups stay in
	// sync with the hierarchy names authors actually see.
	for (const auto& [objectHandle, objectName] : objectMap)
	{
		if (!registry.valid(objectHandle.EntityId()))
		{
			continue;
		}

		if (objectName == name)
		{
			return const_cast<ObjectManager*>(this)->makeHandle(objectHandle.EntityId());
		}
	}

	return std::nullopt;
}

std::optional<Object> ObjectManager::FindObjectByTag(std::string_view tag) const
{
	// Tags live on a dedicated component so scripts can query them without
	// overloading the scene-hierarchy display name with gameplay meaning.
	auto view = registry.view<const TagComponent>();
	for (entt::entity entityId : view)
	{
		const TagComponent& tagComponent = view.get<const TagComponent>(entityId);
		auto guard = tagComponent.acquireReadLock();
		if (!tagComponent.active || tagComponent.tag != tag)
		{
			continue;
		}

		return const_cast<ObjectManager*>(this)->makeHandle(entityId);
	}

	return std::nullopt;
}

std::optional<Object> ObjectManager::getObject(entt::entity id)
{
	if (!registry.valid(id))
	{
		return std::nullopt;
	}
	return makeHandle(id);
}

std::optional<Object> ObjectManager::getObject(const TerraEngine::UUID& id)
{
	auto it = uuidToEntity.find(id);
	if (it != uuidToEntity.end())
	{
		return getObject(it->second);
	}
	return std::nullopt;
}

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
}

void ObjectManager::detachFromParent(entt::entity childId)
{
	if (auto parentIter = parentMap.find(childId); parentIter != parentMap.end())
	{
		const entt::entity parentId = parentIter->second;
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

bool ObjectManager::isDescendant(entt::entity ancestor, entt::entity potentialChild) const
{
	entt::entity cursor = potentialChild;
	while (cursor != entt::null)
	{
		if (cursor == ancestor)
		{
			return true;
		}

		auto parentIter = parentMap.find(cursor);
		if (parentIter == parentMap.end())
		{
			break;
		}
		cursor = parentIter->second;
	}
	return false;
}

entt::entity ObjectManager::getParentEntity(entt::entity child) const
{
	auto iter = parentMap.find(child);
	if (iter != parentMap.end())
	{
		return iter->second;
	}
	return entt::null;
}

TerraEngine::UUID ObjectManager::getUUID(entt::entity entity)
{
	if (auto it = entityToUuid.find(entity); it != entityToUuid.end())
	{
		return it->second;
	}
	return TerraEngine::UUID();
}

void ObjectManager::clear()
{
	registry.clear();
	objectMap.clear();
	parentMap.clear();
	childrenMap.clear();
	uuidToEntity.clear();
	entityToUuid.clear();
}
