// ObjectManager.cpp — Implementation of the engine's central entity and hierarchy manager.
//
// This file implements the lifecycle management for all scene objects, including
// creation, destruction, and complex hierarchical parenting. It maintains the
// primary registry of entities, handles UUID mapping for persistence, and
// provides sophisticated query functions for locating objects by name or tag.

#include "ObjectManager.h"

#include "Core/Scheduling/WorkerSafety.h"

#include <glm/gtc/quaternion.hpp>

#include <stdexcept>

namespace
{
	bool ReportWorkerObjectMutation(const char* operationName, const char* message)
	{
		if (!TerraEngine::Scheduling::IsWorkerSafetyScopeActive())
		{
			return false;
		}
		TerraEngine::Scheduling::ReportWorkerSafetyViolation(
			TerraEngine::Scheduling::WorkerSafetySeverity::Error,
			operationName,
			message,
			"Capture a SceneMutationQueue request and apply it at a protected scheduler flush.");
		return true;
	}
}

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
	// Object is a compact identity value. The manager owns all mutable access
	// and fills the generation/hash fields used for stale-handle validation.
	return makeObjectHandle(entityId);
}

std::optional<entt::entity> ObjectManager::resolveEntity(const Object& reference) const
{
	const entt::entity entityId = static_cast<entt::entity>(reference.entityValue);
	if (reference.IsEmpty() || !registry.valid(entityId))
	{
		return std::nullopt;
	}
	const auto generation = generationByEntity.find(entityId);
	const auto uuid = entityToUuid.find(entityId);
	if (generation == generationByEntity.end() || uuid == entityToUuid.end())
	{
		return std::nullopt;
	}
	if (TerraEngine::CompressObjectGeneration(generation->second) != reference.generation)
	{
		return std::nullopt;
	}
	if (TerraEngine::MakeObjectStableHash(uuid->second) != reference.stableIdHash)
	{
		return std::nullopt;
	}
	return entityId;
}

bool ObjectManager::isEntityValid(entt::entity entityId) const
{
	return registry.valid(entityId);
}

void ObjectManager::addLookupIndex(entt::entity entityId, const ObjectMetadata& metadata)
{
	// Keep lookup buckets append-only for creation order and remove stale
	// entries on rename/delete before inserting the new metadata values.
	nameIndex[metadata.name].push_back(entityId);
	tagIndex[metadata.tag].push_back(entityId);
}

void ObjectManager::removeLookupIndex(entt::entity entityId, const ObjectMetadata& metadata)
{
	removeFromLookupBucket(nameIndex, metadata.name, entityId);
	removeFromLookupBucket(tagIndex, metadata.tag, entityId);
}

void ObjectManager::removeFromLookupBucket(std::unordered_map<std::string, std::vector<entt::entity>>& index, const std::string& key, entt::entity entityId)
{
	auto bucket = index.find(key);
	if (bucket == index.end())
	{
		return;
	}

	std::vector<entt::entity>& entities = bucket->second;
	entities.erase(std::remove(entities.begin(), entities.end(), entityId), entities.end());
	if (entities.empty())
	{
		index.erase(bucket);
	}
}

// ObjectManager::notifySpatialObjectDirty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ObjectManager::notifySpatialObjectDirty(entt::entity entityId)
{
	// Spatial indexes consume this monotonically increasing revision to decide
	// when their cached world bounds may be stale. The callback carries the
	// specific entity for systems that want incremental refreshes.
	++spatialRevision;
	spatialRevisionByEntity[entityId] = spatialRevision;
	if (onSpatialObjectDirty)
	{
		onSpatialObjectDirty(entityId);
	}
}

void ObjectManager::notifyHierarchyDirty()
{
	// Hierarchy snapshots depend only on parent/child identity and ordering.
	// Keeping this revision separate from transform edits lets runtime systems
	// reuse flattened traversal when only local transform values changed.
	++hierarchyRevision;
}

void ObjectManager::invalidateRuntimeActiveMembership()
{
	// Runtime membership changes whenever hot systems must rebuild their
	// active-only entity list. Authored metadata remains in metadataMap so
	// editor and serialization callers can still inspect inactive objects.
	++runtimeActiveMembershipRevision;
}

void ObjectManager::rebuildRuntimeActiveMembershipCache()
{
	runtimeActiveMembershipCache.clear();
	runtimeActiveMembershipCache.reserve(metadataMap.size());
	for (const auto& [entityId, metadata] : metadataMap)
	{
		if (!registry.valid(entityId))
		{
			continue;
		}
		TerraEngine::Scene::RuntimeActiveMembership membership;
		membership.entity = entityId;
		membership.objectHandle = makeObjectHandle(entityId);
		membership.objectId = getUUID(entityId);
		membership.objectActive = metadata.active && registry.any_of<TerraEngine::ActiveObjectTag>(entityId);
		membership.componentActiveMask = membership.objectActive ? 1u : 0u;
		membership.membershipRevision = runtimeActiveMembershipRevision;
		membership.sourceMembershipRevision = runtimeActiveMembershipRevision;
		membership.authoredStateVisible = true;
		membership.reusedFromCache = false;
		membership.fallbackScannedFromMetadata = true;
		runtimeActiveMembershipCache.push_back(membership);
	}
	std::sort(runtimeActiveMembershipCache.begin(), runtimeActiveMembershipCache.end(), [](const TerraEngine::Scene::RuntimeActiveMembership& left, const TerraEngine::Scene::RuntimeActiveMembership& right) {
		return TerraEngine::ToHexString(left.objectId) < TerraEngine::ToHexString(right.objectId);
	});
	cachedRuntimeActiveMembershipRevision = runtimeActiveMembershipRevision;
}

const std::vector<TerraEngine::Scene::RuntimeActiveMembership>& ObjectManager::GetRuntimeActiveMembershipSnapshot()
{
	if (cachedRuntimeActiveMembershipRevision != runtimeActiveMembershipRevision)
	{
		rebuildRuntimeActiveMembershipCache();
	}
	else
	{
		for (TerraEngine::Scene::RuntimeActiveMembership& membership : runtimeActiveMembershipCache)
		{
			membership.reusedFromCache = true;
			membership.fallbackScannedFromMetadata = false;
		}
	}
	return runtimeActiveMembershipCache;
}

void ObjectManager::MarkObjectTransformDirty(const Object& obj)
{
	MarkObjectTransformDirty(obj.EntityId());
}

void ObjectManager::MarkObjectTransformDirty(entt::entity entityId)
{
	if (!registry.valid(entityId))
	{
		return;
	}

	// Transform edits dirty both the world-transform cache and the spatial
	// bounds that depend on it. The two counters stay separate so future systems
	// can reason about transform-only work without forcing metadata refreshes.
	++transformRevision;
	transformRevisionByEntity[entityId] = transformRevision;
	notifySpatialObjectDirty(entityId);
}

// Create a new Object
// ObjectManager::createObject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
Object ObjectManager::createObject(const std::string& name) {
	if (ReportWorkerObjectMutation("ObjectManager.createObject", "Worker code attempted to create a live object directly."))
	{
		throw std::runtime_error("Worker code cannot create live objects directly.");
	}

	// Every entity begins life with a Transform so systems can safely assume
	// spatial data exists without guarding for missing components.
	const entt::entity entityId = registry.create();
	registry.emplace<Transform>(entityId);
	// ActiveObjectTag is intentionally empty, so EnTT's emplace returns void.
	// Attaching it through the registry keeps Object::addComponent available
	// for real component types that return references to the new component.
	registry.emplace<TerraEngine::ActiveObjectTag>(entityId);

	// Assign a stable identity immediately upon creation so the entity can be
	// tracked across serialization and undo sessions.
	TerraEngine::UUID uuid = TerraEngine::GenerateUUID();
	entityToUuid.emplace(entityId, uuid);
	uuidToEntity.emplace(uuid, entityId);
	generationByEntity[entityId] = nextObjectGeneration++;
	transformRevisionByEntity[entityId] = ++transformRevision;
	Object object = makeHandle(entityId);
	
	ObjectMetadata metadata;
	metadata.name = name;
	metadataMap.insert({ entityId, metadata });
	addLookupIndex(entityId, metadata);
	parentMap.emplace(entityId, entt::null);
	childrenMap.emplace(entityId, std::vector<entt::entity>{});
	
	// Structured modifications trigger the observer callback to sync dirty state.
	invalidateRuntimeActiveMembership();
	notifySpatialObjectDirty(entityId);
	notifyHierarchyDirty();
	if (onModified) onModified();
	
	return object;
}

void ObjectManager::removeObject(entt::entity entityId)
{
	if (!registry.valid(entityId))
	{
		return;
	}
	removeObject(makeHandle(entityId));
}

// Remove an Object
// ObjectManager::removeObject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ObjectManager::removeObject(Object obj) {
	if (ReportWorkerObjectMutation("ObjectManager.removeObject", "Worker code attempted to remove a live object directly."))
	{
		return;
	}

	const entt::entity entityId = obj.EntityId();
	if (registry.valid(entityId))
	{
		MarkObjectTransformDirty(entityId);
	}
	if (auto metadataIter = metadataMap.find(entityId); metadataIter != metadataMap.end())
	{
		removeLookupIndex(entityId, metadataIter->second);
	}

	// Destroying the entity purges every component attached to it (Transform and any others),
	// ensuring no dangling component data survives past object lifetime.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (registry.valid(entityId))
	{
		// Purge the UUID mapping before the component is destroyed to maintain
		// consistency in the lookup table.
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto it = entityToUuid.find(entityId); it != entityToUuid.end())
		{
			uuidToEntity.erase(it->second);
			entityToUuid.erase(it);
		}
		generationByEntity.erase(entityId);
		registry.destroy(entityId);
	}

	// Recursively delete children so the entire branch is removed.
	// We copy the child list first because removeObject will mutate the map.
	std::vector<entt::entity> childrenToDelete;
	if (auto childIter = childrenMap.find(entityId); childIter != childrenMap.end())
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
	detachFromParent(entityId);
	parentMap.erase(entityId);

	// Erase the object from the map
	metadataMap.erase(entityId);
	invalidateRuntimeActiveMembership();
	notifyHierarchyDirty();

	if (onModified) onModified();
}

// ObjectManager::setParent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ObjectManager::setParent(Object child, std::optional<Object> parent)
{
	if (ReportWorkerObjectMutation("ObjectManager.setParent", "Worker code attempted to reparent live hierarchy directly."))
	{
		return false;
	}

	const entt::entity childId = child.EntityId();
	if (!registry.valid(childId))
	{
		return false;
	}

	const entt::entity parentId = parent ? parent->EntityId() : entt::null;

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
	notifyHierarchyDirty();
	MarkObjectTransformDirty(makeHandle(childId));

	return true;
}

bool ObjectManager::setParent(entt::entity child, std::optional<entt::entity> parent)
{
	if (ReportWorkerObjectMutation("ObjectManager.setParent", "Worker code attempted to reparent live hierarchy directly."))
	{
		return false;
	}
	if (!registry.valid(child))
	{
		return false;
	}
	const entt::entity parentId = parent.value_or(entt::null);
	if (parentId != entt::null && !registry.valid(parentId))
	{
		return false;
	}
	std::optional<Object> parentObject = parentId == entt::null
		? std::nullopt
		: std::optional<Object>(makeHandle(parentId));
	return setParent(makeHandle(child), parentObject);
}

// ObjectManager::getChildren is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<Object> ObjectManager::getChildren(const Object& parent)
{
	std::vector<Object> children;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	const entt::entity parentId = parent.EntityId();
	if (!registry.valid(parentId))
	{
		return children;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto iter = childrenMap.find(parentId); iter != childrenMap.end())
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

std::vector<entt::entity> ObjectManager::getChildEntities(entt::entity parent) const
{
	std::vector<entt::entity> entities;
	if (!registry.valid(parent))
	{
		return entities;
	}
	std::vector<Object> children = const_cast<ObjectManager*>(this)->getChildren(const_cast<ObjectManager*>(this)->makeHandle(parent));
	entities.reserve(children.size());
	for (const Object& child : children)
	{
		entities.push_back(child.EntityId());
	}
	return entities;
}

// ObjectManager::getRootObjects is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<Object> ObjectManager::getRootObjects()
{
	std::vector<Object> roots;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& [entityId, metadata] : metadataMap)
	{
		(void)metadata;
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

std::vector<entt::entity> ObjectManager::getRootEntities() const
{
	std::vector<Object> roots = const_cast<ObjectManager*>(this)->getRootObjects();
	std::vector<entt::entity> entities;
	entities.reserve(roots.size());
	for (const Object& root : roots)
	{
		entities.push_back(root.EntityId());
	}
	return entities;
}

// ObjectManager::getObjectName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ObjectManager::getObjectName(const Object& obj) const
{
	return getObjectName(obj.EntityId());
}

std::string ObjectManager::getObjectName(entt::entity entityId) const
{
	if (!registry.valid(entityId))
	{
		return "Unnamed";
	}
	auto iter = metadataMap.find(entityId);
	return iter != metadataMap.end() ? iter->second.name : "Unnamed";
}

// ObjectManager::setObjectName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ObjectManager::setObjectName(const Object& obj, const std::string& name)
{
	setObjectName(obj.EntityId(), name);
}

// ObjectManager::getObjectTag is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ObjectManager::getObjectTag(const Object& obj) const
{
	return getObjectTag(obj.EntityId());
}

std::string ObjectManager::getObjectTag(entt::entity entityId) const
{
	if (!registry.valid(entityId))
	{
		return "Untagged";
	}
	auto iter = metadataMap.find(entityId);
	return iter != metadataMap.end() ? iter->second.tag : "Untagged";
}

// ObjectManager::setObjectTag is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ObjectManager::setObjectTag(const Object& obj, const std::string& tag)
{
	setObjectTag(obj.EntityId(), tag);
}

// ObjectManager::getObjectLayer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ObjectManager::getObjectLayer(const Object& obj) const
{
	return getObjectLayer(obj.EntityId());
}

std::string ObjectManager::getObjectLayer(entt::entity entityId) const
{
	if (!registry.valid(entityId))
	{
		return "Default";
	}
	auto iter = metadataMap.find(entityId);
	return iter != metadataMap.end() ? iter->second.layer : "Default";
}

// ObjectManager::setObjectLayer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ObjectManager::setObjectLayer(const Object& obj, const std::string& layer)
{
	setObjectLayer(obj.EntityId(), layer);
}

// ObjectManager::isObjectActive is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ObjectManager::isObjectActive(const Object& obj) const
{
	return isObjectActive(obj.EntityId());
}

bool ObjectManager::isObjectActive(entt::entity entityId) const
{
	if (!registry.valid(entityId))
	{
		return false;
	}
	auto iter = metadataMap.find(entityId);
	return iter != metadataMap.end() ? iter->second.active : false;
}

// ObjectManager::setObjectActive is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ObjectManager::setObjectActive(const Object& obj, bool active)
{
	const entt::entity entityId = obj.EntityId();
	if (auto it = metadataMap.find(entityId); it != metadataMap.end())
	{
		if (it->second.active == active)
		{
			return;
		}
		it->second.active = active;
		if (active)
		{
			if (!registry.any_of<TerraEngine::ActiveObjectTag>(entityId))
			{
				registry.emplace<TerraEngine::ActiveObjectTag>(entityId);
			}
		}
		else if (registry.any_of<TerraEngine::ActiveObjectTag>(entityId))
		{
			registry.remove<TerraEngine::ActiveObjectTag>(entityId);
		}
		++it->second.metadataGeneration;
		++it->second.componentGeneration;
		++it->second.bindingGeneration;
		invalidateRuntimeActiveMembership();
		notifySpatialObjectDirty(entityId);
		notifyHierarchyDirty();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (onModified) onModified();
	}
}

void ObjectManager::setObjectActive(entt::entity entityId, bool active)
{
	if (!registry.valid(entityId))
	{
		return;
	}
	setObjectActive(makeHandle(entityId), active);
}

void ObjectManager::setObjectLayer(entt::entity entityId, const std::string& layer)
{
	if (!registry.valid(entityId))
	{
		return;
	}
	if (auto it = metadataMap.find(entityId); it != metadataMap.end())
	{
		if (it->second.layer == layer)
		{
			return;
		}
		it->second.layer = layer;
		++it->second.metadataGeneration;
		if (onModified) onModified();
	}
}

void ObjectManager::setObjectTag(entt::entity entityId, const std::string& tag)
{
	if (!registry.valid(entityId))
	{
		return;
	}
	if (auto it = metadataMap.find(entityId); it != metadataMap.end())
	{
		if (it->second.tag == tag)
		{
			return;
		}
		removeFromLookupBucket(tagIndex, it->second.tag, entityId);
		it->second.tag = tag;
		++it->second.metadataGeneration;
		tagIndex[tag].push_back(entityId);
		if (onModified) onModified();
	}
}

void ObjectManager::setObjectName(entt::entity entityId, const std::string& name)
{
	if (!registry.valid(entityId))
	{
		return;
	}
	if (auto it = metadataMap.find(entityId); it != metadataMap.end())
	{
		if (it->second.name == name)
		{
			return;
		}
		removeFromLookupBucket(nameIndex, it->second.name, entityId);
		it->second.name = name;
		++it->second.metadataGeneration;
		nameIndex[name].push_back(entityId);
		if (onModified) onModified();
	}
}

// ObjectManager::isObjectStatic is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ObjectManager::isObjectStatic(const Object& obj) const
{
	return isObjectStatic(obj.EntityId());
}

bool ObjectManager::isObjectStatic(entt::entity entityId) const
{
	if (!registry.valid(entityId))
	{
		return false;
	}
	auto iter = metadataMap.find(entityId);
	return iter != metadataMap.end() ? iter->second.isStatic : false;
}

// ObjectManager::setObjectStatic is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ObjectManager::setObjectStatic(const Object& obj, bool isStatic)
{
	const entt::entity entityId = obj.EntityId();
	if (auto it = metadataMap.find(entityId); it != metadataMap.end())
	{
		if (it->second.isStatic == isStatic)
		{
			return;
		}
		it->second.isStatic = isStatic;
		++it->second.metadataGeneration;
		notifySpatialObjectDirty(entityId);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (onModified) onModified();
	}
}

void ObjectManager::setObjectStatic(entt::entity entityId, bool isStatic)
{
	if (!registry.valid(entityId))
	{
		return;
	}
	setObjectStatic(makeHandle(entityId), isStatic);
}

void ObjectManager::notifyObjectStructureChanged(const Object& obj)
{
	if (!registry.valid(obj.EntityId()))
	{
		return;
	}

	// Component membership changes affect script wrapper validity even when the
	// object's transform did not move, so reuse the existing monotonic revision.
	if (auto it = metadataMap.find(obj.EntityId()); it != metadataMap.end())
	{
		++it->second.componentGeneration;
		++it->second.bindingGeneration;
	}
	invalidateRuntimeActiveMembership();
	notifySpatialObjectDirty(obj.EntityId());
	if (onModified) onModified();
}

void ObjectManager::notifyObjectStructureChanged(entt::entity entityId)
{
	if (!registry.valid(entityId))
	{
		return;
	}
	notifyObjectStructureChanged(makeHandle(entityId));
}

std::uint64_t ObjectManager::GetObjectTransformRevision(entt::entity entityId) const
{
	if (auto it = transformRevisionByEntity.find(entityId); it != transformRevisionByEntity.end())
	{
		return it->second;
	}
	return 0;
}

std::uint64_t ObjectManager::GetObjectSpatialRevision(entt::entity entityId) const
{
	if (auto it = spatialRevisionByEntity.find(entityId); it != spatialRevisionByEntity.end())
	{
		return it->second;
	}
	return 0;
}

std::uint64_t ObjectManager::getObjectMetadataGeneration(const Object& obj) const
{
	return getObjectMetadataGeneration(obj.EntityId());
}

std::uint64_t ObjectManager::getObjectMetadataGeneration(entt::entity entityId) const
{
	if (!registry.valid(entityId))
	{
		return 0;
	}
	if (auto it = metadataMap.find(entityId); it != metadataMap.end())
	{
		return it->second.metadataGeneration;
	}
	return 0;
}

std::uint64_t ObjectManager::getObjectComponentGeneration(const Object& obj) const
{
	return getObjectComponentGeneration(obj.EntityId());
}

std::uint64_t ObjectManager::getObjectComponentGeneration(entt::entity entityId) const
{
	if (!registry.valid(entityId))
	{
		return 0;
	}
	if (auto it = metadataMap.find(entityId); it != metadataMap.end())
	{
		return it->second.componentGeneration;
	}
	return 0;
}

std::uint64_t ObjectManager::getObjectBindingGeneration(const Object& obj) const
{
	return getObjectBindingGeneration(obj.EntityId());
}

std::uint64_t ObjectManager::getObjectBindingGeneration(entt::entity entityId) const
{
	if (!registry.valid(entityId))
	{
		return 0;
	}
	if (auto it = metadataMap.find(entityId); it != metadataMap.end())
	{
		return it->second.bindingGeneration;
	}
	return 0;
}

// ObjectManager::FindObjectByName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<Object> ObjectManager::FindObjectByName(std::string_view name) const
{
	if (std::optional<entt::entity> entityId = FindEntityByName(name))
	{
		return const_cast<ObjectManager*>(this)->makeHandle(*entityId);
	}
	return std::nullopt;
}

std::optional<entt::entity> ObjectManager::FindEntityByName(std::string_view name) const
{
	auto bucket = nameIndex.find(std::string(name));
	if (bucket == nameIndex.end())
	{
		return std::nullopt;
	}

	for (entt::entity entityId : bucket->second)
	{
		if (!registry.valid(entityId))
		{
			continue;
		}

		auto metadata = metadataMap.find(entityId);
		if (metadata != metadataMap.end() && metadata->second.name == name)
		{
			return entityId;
		}
	}

	return std::nullopt;
}

// ObjectManager::FindObjectByTag is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<Object> ObjectManager::FindObjectByTag(std::string_view tag) const
{
	if (std::optional<entt::entity> entityId = FindEntityByTag(tag))
	{
		return const_cast<ObjectManager*>(this)->makeHandle(*entityId);
	}
	return std::nullopt;
}

std::optional<entt::entity> ObjectManager::FindEntityByTag(std::string_view tag) const
{
	auto bucket = tagIndex.find(std::string(tag));
	if (bucket == tagIndex.end())
	{
		return std::nullopt;
	}

	for (entt::entity entityId : bucket->second)
	{
		if (!registry.valid(entityId))
		{
			continue;
		}

		auto metadata = metadataMap.find(entityId);
		if (metadata != metadataMap.end() && metadata->second.active && metadata->second.tag == tag)
		{
			return entityId;
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
	if (std::optional<entt::entity> entityId = getEntity(id))
	{
		return getObject(*entityId);
	}
	return std::nullopt;
}

std::optional<entt::entity> ObjectManager::getEntity(const TerraEngine::UUID& id) const
{
	auto it = uuidToEntity.find(id);
	if (it != uuidToEntity.end() && registry.valid(it->second))
	{
		return it->second;
	}
	return std::nullopt;
}

Object ObjectManager::makeObjectHandle(entt::entity entityId) const
{
	Object reference;
	reference.entityValue = static_cast<std::uint32_t>(entityId);
	if (auto generation = generationByEntity.find(entityId); generation != generationByEntity.end())
	{
		reference.generation = TerraEngine::CompressObjectGeneration(generation->second);
	}
	if (auto uuid = entityToUuid.find(entityId); uuid != entityToUuid.end())
	{
		reference.stableIdHash = TerraEngine::MakeObjectStableHash(uuid->second);
	}
	return reference;
}

TerraEngine::ObjectIdentityResolution ObjectManager::validateObjectHandle(const Object& reference) const
{
	TerraEngine::ObjectIdentityResolution resolution;
	resolution.reference = reference;
	const entt::entity entityId = static_cast<entt::entity>(reference.entityValue);
	if (reference.IsEmpty() || !registry.valid(entityId))
	{
		resolution.validity = TerraEngine::ObjectIdentityValidity::Missing;
		resolution.message = "Referenced object is missing.";
		return resolution;
	}
	const auto uuid = entityToUuid.find(entityId);
	if (reference.stableIdHash == 0 || uuid == entityToUuid.end())
	{
		resolution.validity = TerraEngine::ObjectIdentityValidity::Stale;
		resolution.message = "Referenced object stable identity fingerprint is missing.";
		return resolution;
	}
	const auto generation = generationByEntity.find(entityId);
	if (generation == generationByEntity.end() || TerraEngine::CompressObjectGeneration(generation->second) != reference.generation)
	{
		resolution.validity = TerraEngine::ObjectIdentityValidity::Stale;
		resolution.message = "Referenced object generation is stale.";
		return resolution;
	}
	if (TerraEngine::MakeObjectStableHash(uuid->second) != reference.stableIdHash)
	{
		resolution.validity = TerraEngine::ObjectIdentityValidity::Stale;
		resolution.message = "Referenced object stable identity fingerprint no longer matches.";
		return resolution;
	}
	resolution.validity = TerraEngine::ObjectIdentityValidity::Valid;
	resolution.message = "Referenced object is valid.";
	return resolution;
}

std::optional<Object> ObjectManager::resolveObjectHandle(const Object& reference)
{
	if (validateObjectHandle(reference).validity != TerraEngine::ObjectIdentityValidity::Valid)
	{
		return std::nullopt;
	}
	return getObject(static_cast<entt::entity>(reference.entityValue));
}

void ObjectManager::SortObjectsByStableId(std::vector<Object>& objects)
{
	std::sort(objects.begin(), objects.end(), [this](const Object& lhs, const Object& rhs) {
		return TerraEngine::ToHexString(getUUID(lhs.EntityId())) < TerraEngine::ToHexString(getUUID(rhs.EntityId()));
	});
}

void ObjectManager::SortEntitiesByStableId(std::vector<entt::entity>& entities)
{
	std::sort(entities.begin(), entities.end(), [this](entt::entity lhs, entt::entity rhs) {
		return TerraEngine::ToHexString(getUUID(lhs)) < TerraEngine::ToHexString(getUUID(rhs));
	});
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
	generationByEntity.try_emplace(entity, nextObjectGeneration++);
	invalidateRuntimeActiveMembership();
	notifySpatialObjectDirty(entity);
	notifyHierarchyDirty();
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
	transformRevisionByEntity.clear();
	spatialRevisionByEntity.clear();
	generationByEntity.clear();
	transformRevision = 0;
	spatialRevision = 0;
	hierarchyRevision = 0;
	runtimeActiveMembershipRevision = 0;
	cachedRuntimeActiveMembershipRevision = static_cast<std::uint64_t>(-1);
	runtimeActiveMembershipCache.clear();
	nextObjectGeneration = 1;
	nameIndex.clear();
	tagIndex.clear();
}
