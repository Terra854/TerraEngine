// ObjectManager.cpp — Implementation of the engine's central entity and hierarchy manager.
//
// This file implements the lifecycle management for all scene objects, including
// creation, destruction, and complex hierarchical parenting. It maintains the
// primary registry of entities, handles UUID mapping for persistence, and
// provides sophisticated query functions for locating objects by name or tag.

#include "ObjectManager.h"

#include "Core/Scheduling/WorkerSafety.h"

#include <glm/gtc/quaternion.hpp>

#include <atomic>
#include <cmath>
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

bool ObjectManager::isObjectValid(const Object& object) const
{
	const entt::entity entityId = object.EntityId();
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
	if (bulkPublicationActive)
	{
		// Per-object bounds are intentionally invisible until the complete
		// packet is published.  The final boundary advances one shared revision.
		bulkPublicationChanged = true;
		return;
	}
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
	if (bulkPublicationActive)
	{
		bulkPublicationChanged = true;
		return;
	}
	// Hierarchy snapshots depend only on parent/child identity and ordering.
	// Keeping this revision separate from transform edits lets runtime systems
	// reuse flattened traversal when only local transform values changed.
	++hierarchyRevision;
}

TerraEngine::Scene::SceneObjectRevisionSnapshot ObjectManager::captureSceneObjectRevision(const Object& object) const
{
	TerraEngine::Scene::SceneObjectRevisionSnapshot snapshot;
	const entt::entity entityId = object.EntityId();
	if (!registry.valid(entityId))
	{
		return snapshot;
	}

	// Copy every consumer-facing revision while the protected path owns the
	// registry. The resulting record can then be read by workers without a
	// live ObjectManager lookup or a race with later transactional mutations.
	snapshot.object = object;
	snapshot.objectId = getEntityUUID(entityId);
	snapshot.lifetimeGeneration = getObjectLifetimeGeneration(object);
	snapshot.metadataGeneration = getObjectMetadataGeneration(object);
	snapshot.componentGeneration = getObjectComponentGeneration(object);
	snapshot.transformRevision = GetObjectTransformRevision(object);
	snapshot.hierarchyRevision = hierarchyRevision;
	snapshot.spatialRevision = GetObjectSpatialRevision(object);
	if (const auto metadata = metadataMap.find(entityId); metadata != metadataMap.end())
	{
		snapshot.isStatic = metadata->second.isStatic;
	}
	if (const auto parent = parentMap.find(entityId); parent != parentMap.end() && parent->second != entt::null)
	{
		snapshot.parentObjectId = getEntityUUID(parent->second);
	}
	for (entt::entity cursor = entityId; cursor != entt::null; cursor = getParentEntityInternal(cursor))
	{
		if (getParentEntityInternal(cursor) != entt::null)
		{
			++snapshot.hierarchyDepth;
		}
	}
	return snapshot;
}

void ObjectManager::recordSceneChange(TerraEngine::Scene::SceneChangeKind kind, const TerraEngine::Scene::SceneObjectRevisionSnapshot& snapshot)
{
	if (bulkPublicationActive)
	{
		// The caller will publish a complete replacement, not thousands of
		// incremental records that no consumer may observe independently.
		bulkPublicationChanged = true;
		return;
	}
	if (snapshot.objectId == TerraEngine::EMPTY_UUID)
	{
		return;
	}

	// Multiple edits to one object coalesce into one exact record per category.
	// The newest revision evidence is retained so a consumer never validates an
	// earlier transform or metadata version after the protected boundary.
	PendingSceneChange& pending = pendingSceneChanges[snapshot.objectId];
	pending.snapshot = snapshot;
	pending.kinds |= 1u << static_cast<std::uint32_t>(kind);
}

TerraEngine::Scene::SceneChangePacket ObjectManager::PublishSceneChangePacket()
{
	if (TerraEngine::Scheduling::IsWorkerSafetyScopeActive())
	{
		TerraEngine::Scheduling::ReportWorkerSafetyViolation(
			TerraEngine::Scheduling::WorkerSafetySeverity::Error,
			"ObjectManager.PublishSceneChangePacket",
			"Worker code attempted to publish live scene changes directly.",
			"Publish the immutable packet at a protected scheduler boundary, then pass it to worker preparation.");
		return {};
	}
	const auto hasKind = [](const PendingSceneChange& pending, TerraEngine::Scene::SceneChangeKind kind) {
		return (pending.kinds & (1u << static_cast<std::uint32_t>(kind))) != 0;
	};

	// World transforms and world bounds of every descendant depend on an edited
	// ancestor. Capture those exact identities while protected ownership still
	// has a coherent hierarchy, without marking unrelated objects dirty.
	std::unordered_set<TerraEngine::UUID> directDirtyIds;
	std::vector<entt::entity> descendantStack;
	for (const auto& [objectId, pending] : pendingSceneChanges)
	{
		if (hasKind(pending, TerraEngine::Scene::SceneChangeKind::Removed)
			|| (!hasKind(pending, TerraEngine::Scene::SceneChangeKind::Transform)
				&& !hasKind(pending, TerraEngine::Scene::SceneChangeKind::Hierarchy)))
		{
			continue;
		}
		directDirtyIds.insert(objectId);
		if (const auto entity = uuidToEntity.find(objectId); entity != uuidToEntity.end())
		{
			if (const auto children = childrenMap.find(entity->second); children != childrenMap.end())
			{
				descendantStack.insert(descendantStack.end(), children->second.begin(), children->second.end());
			}
		}
	}

	std::unordered_map<TerraEngine::UUID, TerraEngine::Scene::SceneObjectRevisionSnapshot> propagatedDescendants;
	std::unordered_set<entt::entity> visitedDescendants;
	while (!descendantStack.empty())
	{
		const entt::entity entityId = descendantStack.back();
		descendantStack.pop_back();
		if (!registry.valid(entityId) || !visitedDescendants.insert(entityId).second)
		{
			continue;
		}
		const TerraEngine::UUID objectId = getEntityUUID(entityId);
		if (objectId != TerraEngine::EMPTY_UUID)
		{
			if (directDirtyIds.find(objectId) == directDirtyIds.end())
			{
				// A descendant's local transform revision remains authoritative and
				// unchanged, while its world bounds receive a new spatial revision.
				notifySpatialObjectDirty(entityId);
			}
			propagatedDescendants[objectId] = captureSceneObjectRevision(makeObjectHandle(entityId));
		}
		if (const auto children = childrenMap.find(entityId); children != childrenMap.end())
		{
			descendantStack.insert(descendantStack.end(), children->second.begin(), children->second.end());
		}
	}

	std::vector<TerraEngine::Scene::SceneObjectRevisionSnapshot> added;
	std::vector<TerraEngine::Scene::SceneObjectRevisionSnapshot> removed;
	std::vector<TerraEngine::Scene::SceneObjectRevisionSnapshot> metadataChanged;
	std::vector<TerraEngine::Scene::SceneObjectRevisionSnapshot> componentChanged;
	std::vector<TerraEngine::Scene::SceneObjectRevisionSnapshot> transformChanged;
	std::vector<TerraEngine::Scene::SceneObjectRevisionSnapshot> hierarchyChanged;
	std::vector<TerraEngine::Scene::SceneObjectRevisionSnapshot> boundsChanged;
	for (const auto& [objectId, pending] : pendingSceneChanges)
	{
		(void)objectId;
		// An object created and removed before publication was never observable,
		// so publishing it would make consumers perform useless create/delete work.
		if (hasKind(pending, TerraEngine::Scene::SceneChangeKind::Added)
			&& hasKind(pending, TerraEngine::Scene::SceneChangeKind::Removed))
		{
			continue;
		}
		if (hasKind(pending, TerraEngine::Scene::SceneChangeKind::Removed))
		{
			removed.push_back(pending.snapshot);
			continue;
		}
		if (hasKind(pending, TerraEngine::Scene::SceneChangeKind::Added)) { added.push_back(pending.snapshot); }
		if (hasKind(pending, TerraEngine::Scene::SceneChangeKind::Metadata)) { metadataChanged.push_back(pending.snapshot); }
		if (hasKind(pending, TerraEngine::Scene::SceneChangeKind::Component)) { componentChanged.push_back(pending.snapshot); }
		if (hasKind(pending, TerraEngine::Scene::SceneChangeKind::Transform)) { transformChanged.push_back(pending.snapshot); }
		if (hasKind(pending, TerraEngine::Scene::SceneChangeKind::Hierarchy)) { hierarchyChanged.push_back(pending.snapshot); }
		if (hasKind(pending, TerraEngine::Scene::SceneChangeKind::Bounds)) { boundsChanged.push_back(pending.snapshot); }
	}
	for (const auto& [objectId, snapshot] : propagatedDescendants)
	{
		const auto pending = pendingSceneChanges.find(objectId);
		if (pending == pendingSceneChanges.end() || !hasKind(pending->second, TerraEngine::Scene::SceneChangeKind::Transform))
		{
			transformChanged.push_back(snapshot);
		}
		if (pending == pendingSceneChanges.end() || !hasKind(pending->second, TerraEngine::Scene::SceneChangeKind::Bounds))
		{
			boundsChanged.push_back(snapshot);
		}
	}

	const auto sortByObjectId = [](std::vector<TerraEngine::Scene::SceneObjectRevisionSnapshot>& snapshots) {
		std::sort(snapshots.begin(), snapshots.end(), [](const auto& left, const auto& right) {
			return left.objectId < right.objectId;
		});
	};
	const auto sortHierarchy = [](std::vector<TerraEngine::Scene::SceneObjectRevisionSnapshot>& snapshots) {
		std::sort(snapshots.begin(), snapshots.end(), [](const auto& left, const auto& right) {
			return left.hierarchyDepth == right.hierarchyDepth
				? left.objectId < right.objectId
				: left.hierarchyDepth < right.hierarchyDepth;
		});
	};
	sortByObjectId(added); sortByObjectId(removed); sortByObjectId(metadataChanged); sortByObjectId(componentChanged);
	// Transform and bounds packet validation uses stable identity order; the
	// separate hierarchy category retains parent-before-child dependency order.
	sortByObjectId(transformChanged); sortHierarchy(hierarchyChanged); sortByObjectId(boundsChanged);
	pendingSceneChanges.clear();

	TerraEngine::Scene::SceneChangePacket packet = TerraEngine::Scene::SceneChangePacket::Create(
		sceneGeneration,
		++sceneChangePublicationRevision,
		std::move(added), std::move(removed), std::move(metadataChanged), std::move(componentChanged),
		std::move(transformChanged), std::move(hierarchyChanged), std::move(boundsChanged));
	std::atomic_store_explicit(
		&publishedSceneChangePacket,
		std::make_shared<const TerraEngine::Scene::SceneChangePacket>(packet),
		std::memory_order_release);
	return packet;
}

bool ObjectManager::CanApplySceneChangePacketToLiveState(
	const TerraEngine::Scene::SceneChangePacket& packet,
	std::uint64_t previousPublicationRevision) const
{
	// Publication order alone is insufficient: Inspector or another protected
	// producer can edit after frame-start publication, before render capture.
	// In that case consumers must rebuild from live state, not bless an older
	// delta with the new component revision and retain stale bindings forever.
	return pendingSceneChanges.empty()
		&& packet.PublicationRevision() == sceneChangePublicationRevision
		&& packet.PublicationRevision() == previousPublicationRevision + 1U
		&& packet.Validate(sceneGeneration).valid;
}

TerraEngine::Scene::SceneChangePacket ObjectManager::LoadPublishedSceneChangePacket() const
{
	const std::shared_ptr<const TerraEngine::Scene::SceneChangePacket> packet =
		LoadPublishedSceneChangePacketShared();
	return packet != nullptr ? *packet : TerraEngine::Scene::SceneChangePacket{};
}

std::shared_ptr<const TerraEngine::Scene::SceneChangePacket>
	ObjectManager::LoadPublishedSceneChangePacketShared() const noexcept
{
	return std::atomic_load_explicit(&publishedSceneChangePacket, std::memory_order_acquire);
}

void ObjectManager::invalidateRuntimeActiveMembership()
{
	if (bulkPublicationActive)
	{
		bulkPublicationChanged = true;
		return;
	}
	// Runtime membership changes whenever hot systems must rebuild their
	// active-only entity list. Authored metadata remains in metadataMap so
	// editor and serialization callers can still inspect inactive objects.
	++runtimeActiveMembershipRevision;
}

void ObjectManager::rebuildRuntimeActiveMembershipCache()
{
	runtimeActiveMembershipCache.clear();
	runtimeActiveMembershipCache.reserve(metadataMap.size());

	// Resolve inherited activation once in parent-before-child order. Runtime
	// systems consume this effective state, while metadataMap keeps every
	// object's own checkbox independently for editor display and serialization.
	std::unordered_map<entt::entity, bool> effectiveActiveByEntity;
	effectiveActiveByEntity.reserve(metadataMap.size());
	std::vector<entt::entity> traversalQueue;
	traversalQueue.reserve(metadataMap.size());
	for (const auto& [entityId, metadata] : metadataMap)
	{
		(void)metadata;
		const entt::entity parentId = getParentEntityInternal(entityId);
		if (parentId == entt::null || !registry.valid(parentId) || !metadataMap.contains(parentId))
		{
			traversalQueue.push_back(entityId);
		}
	}
	for (std::size_t queueIndex = 0; queueIndex < traversalQueue.size(); ++queueIndex)
	{
		const entt::entity entityId = traversalQueue[queueIndex];
		const auto metadata = metadataMap.find(entityId);
		if (metadata == metadataMap.end() || effectiveActiveByEntity.contains(entityId))
		{
			continue;
		}
		const entt::entity parentId = getParentEntityInternal(entityId);
		const bool parentActive = parentId == entt::null
			|| !effectiveActiveByEntity.contains(parentId)
			|| effectiveActiveByEntity.at(parentId);
		effectiveActiveByEntity[entityId] = metadata->second.active && parentActive;
		if (const auto children = childrenMap.find(entityId); children != childrenMap.end())
		{
			for (const entt::entity childId : children->second)
			{
				if (registry.valid(childId) && metadataMap.contains(childId))
				{
					traversalQueue.push_back(childId);
				}
			}
		}
	}
	// Hierarchy mutation rejects cycles. If corrupted imported bookkeeping ever
	// leaves an unreachable row, fail it closed instead of exposing it to runtime.
	for (const auto& [entityId, metadata] : metadataMap)
	{
		(void)metadata;
		effectiveActiveByEntity.try_emplace(entityId, false);
	}

	for (const auto& [entityId, metadata] : metadataMap)
	{
		if (!registry.valid(entityId))
		{
			continue;
		}
		TerraEngine::Scene::RuntimeActiveMembership membership;
		membership.entity = entityId;
		membership.objectHandle = makeObjectHandle(entityId);
		membership.objectId = getEntityUUID(entityId);
		membership.objectActive = effectiveActiveByEntity.at(entityId)
			&& registry.any_of<TerraEngine::ActiveObjectTag>(entityId);
		membership.componentActiveMask = membership.objectActive ? 1u : 0u;
		membership.membershipRevision = runtimeActiveMembershipRevision;
		membership.sourceMembershipRevision = runtimeActiveMembershipRevision;
		membership.authoredStateVisible = true;
		membership.reusedFromCache = false;
		membership.fallbackScannedFromMetadata = true;
		runtimeActiveMembershipCache.push_back(membership);
	}
	std::sort(runtimeActiveMembershipCache.begin(), runtimeActiveMembershipCache.end(), [](const TerraEngine::Scene::RuntimeActiveMembership& left, const TerraEngine::Scene::RuntimeActiveMembership& right) {
		// UUID byte order exactly matches its fixed-width hexadecimal order and
		// avoids temporary string allocation while publishing large scene caches.
		return left.objectId < right.objectId;
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
	if (ReportWorkerObjectMutation("ObjectManager.MarkObjectTransformDirty", "Worker code attempted to mark a live transform dirty directly."))
	{
		return;
	}
	const entt::entity entityId = obj.EntityId();
	if (!registry.valid(entityId))
	{
		return;
	}

	// Transform edits dirty both the world-transform cache and the spatial
	// bounds that depend on it. The two counters stay separate so future systems
	// can reason about transform-only work without forcing metadata refreshes.
	++transformRevision;
	transformRevisionByEntity[entityId] = transformRevision;
	if (Transform* transform = registry.try_get<Transform>(entityId))
	{
		// Keep the value contract and manager journal in lockstep. Snapshot users
		// therefore need no side map to learn the revision of a canonical pose.
		transform->worldPose.transformRevision = transformRevision;
	}
	notifySpatialObjectDirty(entityId);
	const TerraEngine::Scene::SceneObjectRevisionSnapshot snapshot = captureSceneObjectRevision(obj);
	recordSceneChange(TerraEngine::Scene::SceneChangeKind::Transform, snapshot);
	recordSceneChange(TerraEngine::Scene::SceneChangeKind::Bounds, snapshot);
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
	registry.get<Transform>(entityId).worldPose.transformRevision = transformRevision;
	Object object = makeHandle(entityId);
	
	ObjectMetadata metadata;
	metadata.name = name;
	metadataMap.insert({ entityId, metadata });
	addLookupIndex(entityId, metadata);
	parentMap.emplace(entityId, entt::null);
	childrenMap.emplace(entityId, std::vector<entt::entity>{});
	recordSceneChange(TerraEngine::Scene::SceneChangeKind::Added, captureSceneObjectRevision(object));
	
	// Structured modifications trigger the observer callback to sync dirty state.
	invalidateRuntimeActiveMembership();
	notifySpatialObjectDirty(entityId);
	notifyHierarchyDirty();
	if (onModified && !bulkPublicationActive) onModified();
	
	return object;
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
		MarkObjectTransformDirty(obj);
		recordSceneChange(TerraEngine::Scene::SceneChangeKind::Removed, captureSceneObjectRevision(obj));
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

	if (onModified && !bulkPublicationActive) onModified();
}

// ObjectManager::setParent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ObjectManager::setParent(Object child, std::optional<Object> parent, ReparentMode mode)
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
	if (parent && !registry.valid(parentId))
	{
		return false;
	}

	// Prevent self-parenting and cycle creation by walking ancestors.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (parentId == childId || isDescendant(childId, parentId))
	{
		return false;
	}

	Transform* childTransform = registry.try_get<Transform>(childId);
	if (childTransform == nullptr)
	{
		return false;
	}

	// Reparenting keeps the exact canonical pose. Only the bounded relative pose
	// changes, and it is fully prepared before parent maps are mutated so an
	// out-of-range transform space leaves live hierarchy state untouched.
	TerraEngine::Scene::ParentRelativePose preparedRelative = childTransform->parentRelativePose;
	if (parentId != entt::null && mode == ReparentMode::KeepWorld)
	{
		Transform* parentTransform = registry.try_get<Transform>(parentId);
		if (parentTransform == nullptr)
		{
			return false;
		}
		const TerraEngine::Scene::WorldLocalizationResult local = TerraEngine::Scene::LocalizeWorldPosition(
			childTransform->worldPose.position,
			parentTransform->worldPose.position,
			TerraEngine::Scene::kTransformSpaceMaximumRelativeRadiusMetres);
		if (!local.Succeeded())
		{
			return false;
		}
		const glm::quat inverseParentRotation = glm::inverse(parentTransform->worldPose.rotation);
		const float kMinimumScale = 1e-6f;
		if (std::abs(parentTransform->worldPose.scale.x) < kMinimumScale
			|| std::abs(parentTransform->worldPose.scale.y) < kMinimumScale
			|| std::abs(parentTransform->worldPose.scale.z) < kMinimumScale)
		{
			return false;
		}
		preparedRelative.translationMetres = glm::dvec3(inverseParentRotation * glm::vec3(local.offsetMetres))
			/ glm::dvec3(parentTransform->worldPose.scale);
		preparedRelative.rotation = inverseParentRotation * childTransform->worldPose.rotation;
		preparedRelative.scale = childTransform->worldPose.scale / parentTransform->worldPose.scale;
		preparedRelative.transformSpaceId = parentTransform->worldPose.coordinateFrameRevision;
		if (!TerraEngine::Scene::IsParentRelativePoseInRange(preparedRelative))
		{
			return false;
		}
	}

	const entt::entity previousParentId = getParentEntityInternal(childId);
	std::unordered_map<entt::entity, TerraEngine::Scene::WorldPose> originalBranchPoses;
	if (mode == ReparentMode::KeepLocal)
	{
		std::function<void(entt::entity)> captureBranch = [&](entt::entity entityId) {
			if (Transform* transform = registry.try_get<Transform>(entityId))
			{
				originalBranchPoses.emplace(entityId, transform->worldPose);
			}
		const auto children = childrenMap.find(entityId);
		if (children == childrenMap.end())
		{
			return;
		}
		for (entt::entity descendant : children->second)
		{
			captureBranch(descendant);
		}
		};
		captureBranch(childId);
	}

	// Delay bookkeeping allocation until every rejecting preflight above has
	// completed. A failed protected reparent must not leave even empty map
	// entries behind as an observable partial hierarchy mutation.
	childrenMap.try_emplace(childId, std::vector<entt::entity>{});
	parentMap.try_emplace(childId, entt::null);
	if (parentId != entt::null)
	{
		childrenMap.try_emplace(parentId, std::vector<entt::entity>{});
		parentMap.try_emplace(parentId, entt::null);
	}

	detachFromParent(childId);

	parentMap[childId] = parentId;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (parentId != entt::null)
	{
		childrenMap[parentId].push_back(childId);
	}

	if (mode == ReparentMode::KeepWorld)
	{
		childTransform->parentRelativePose = preparedRelative;
		++childTransform->worldPose.coordinateFrameRevision;
	}
	else if (parentId != entt::null)
	{
		// KeepLocal is intentionally protected-only: it recomputes canonical
		// world poses for the affected branch after the parent edge is accepted.
		// No worker observes this intermediate live hierarchy state.
		std::function<bool(entt::entity)> propagateLocalBranch = [&](entt::entity entityId) {
			Transform* transform = registry.try_get<Transform>(entityId);
			const entt::entity directParent = getParentEntityInternal(entityId);
			if (transform == nullptr || directParent == entt::null)
			{
				return transform != nullptr;
			}
			Transform* directParentTransform = registry.try_get<Transform>(directParent);
			if (directParentTransform == nullptr || !TerraEngine::Scene::IsParentRelativePoseInRange(transform->parentRelativePose))
			{
				return false;
			}
			const glm::vec3 boundedRelative = directParentTransform->worldPose.rotation
				* (directParentTransform->worldPose.scale * glm::vec3(transform->parentRelativePose.translationMetres));
			const TerraEngine::Scene::WorldCoordinateResult translated = TerraEngine::Scene::ApplyWorldDelta(
				directParentTransform->worldPose.position, { {}, glm::dvec3(boundedRelative) });
			if (!translated.Succeeded())
			{
				return false;
			}
			transform->worldPose.position = translated.position;
			transform->worldPose.rotation = directParentTransform->worldPose.rotation * transform->parentRelativePose.rotation;
			transform->worldPose.scale = directParentTransform->worldPose.scale * transform->parentRelativePose.scale;
			++transform->worldPose.coordinateFrameRevision;
			for (entt::entity descendant : childrenMap[entityId])
			{
				if (!propagateLocalBranch(descendant))
				{
					return false;
				}
			}
			return true;
		};
		if (!propagateLocalBranch(childId))
		{
			// Restore every touched pose and parent edge. This keeps direct protected
			// calls atomic even when a descendant overflows a canonical cell.
			detachFromParent(childId);
			parentMap[childId] = previousParentId;
			if (previousParentId != entt::null)
			{
				childrenMap[previousParentId].push_back(childId);
			}
			for (const auto& [entityId, pose] : originalBranchPoses)
			{
				if (Transform* transform = registry.try_get<Transform>(entityId))
				{
					transform->worldPose = pose;
				}
			}
			return false;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (onModified && !bulkPublicationActive) onModified();
	notifyHierarchyDirty();
	// Reparenting can inherit a different active state for this entire branch,
	// even though none of its authored checkboxes changed.
	invalidateRuntimeActiveMembership();
	MarkObjectTransformDirty(child);
	recordSceneChange(TerraEngine::Scene::SceneChangeKind::Hierarchy, captureSceneObjectRevision(child));

	return true;
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
		return getUUID(lhs) < getUUID(rhs);
	});
	return children;
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
		return getUUID(lhs) < getUUID(rhs);
	});
	return roots;
}

std::optional<Object> ObjectManager::getParent(const Object& child) const
{
	const entt::entity parentId = getParentEntityInternal(child.EntityId());
	if (parentId == entt::null || !registry.valid(parentId))
	{
		return std::nullopt;
	}
	return const_cast<ObjectManager*>(this)->makeHandle(parentId);
}

// ObjectManager::getObjectName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ObjectManager::getObjectName(const Object& obj) const
{
	const entt::entity entityId = obj.EntityId();
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
	if (ReportWorkerObjectMutation("ObjectManager.setObjectName", "Worker code attempted to update live object metadata directly."))
	{
		return;
	}
	const entt::entity entityId = obj.EntityId();
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
		recordSceneChange(TerraEngine::Scene::SceneChangeKind::Metadata, captureSceneObjectRevision(obj));
		if (onModified && !bulkPublicationActive) onModified();
	}
}

// ObjectManager::setObjectActive is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ObjectManager::getObjectTag(const Object& obj) const
{
	const entt::entity entityId = obj.EntityId();
	if (!registry.valid(entityId))
	{
		return "Untagged";
	}
	auto iter = metadataMap.find(entityId);
	return iter != metadataMap.end() ? iter->second.tag : "Untagged";
}

void ObjectManager::setObjectTag(const Object& obj, const std::string& tag)
{
	if (ReportWorkerObjectMutation("ObjectManager.setObjectTag", "Worker code attempted to update live object metadata directly."))
	{
		return;
	}
	const entt::entity entityId = obj.EntityId();
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
		recordSceneChange(TerraEngine::Scene::SceneChangeKind::Metadata, captureSceneObjectRevision(obj));
		if (onModified && !bulkPublicationActive) onModified();
	}
}

std::string ObjectManager::getObjectLayer(const Object& obj) const
{
	const entt::entity entityId = obj.EntityId();
	if (!registry.valid(entityId))
	{
		return "Default";
	}
	auto iter = metadataMap.find(entityId);
	return iter != metadataMap.end() ? iter->second.layer : "Default";
}

void ObjectManager::setObjectLayer(const Object& obj, const std::string& layer)
{
	if (ReportWorkerObjectMutation("ObjectManager.setObjectLayer", "Worker code attempted to update live object metadata directly."))
	{
		return;
	}
	const entt::entity entityId = obj.EntityId();
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
		recordSceneChange(TerraEngine::Scene::SceneChangeKind::Metadata, captureSceneObjectRevision(obj));
		if (onModified && !bulkPublicationActive) onModified();
	}
}

bool ObjectManager::isObjectActive(const Object& obj) const
{
	const entt::entity entityId = obj.EntityId();
	if (!registry.valid(entityId))
	{
		return false;
	}
	auto iter = metadataMap.find(entityId);
	return iter != metadataMap.end() ? iter->second.active : false;
}

bool ObjectManager::isObjectActiveInHierarchy(const Object& obj) const
{
	entt::entity entityId = obj.EntityId();
	std::size_t remainingAncestors = metadataMap.size() + 1u;
	while (entityId != entt::null && registry.valid(entityId) && remainingAncestors-- > 0u)
	{
		const auto metadata = metadataMap.find(entityId);
		if (metadata == metadataMap.end() || !metadata->second.active
			|| !registry.any_of<TerraEngine::ActiveObjectTag>(entityId))
		{
			return false;
		}
		entityId = getParentEntityInternal(entityId);
	}
	// A valid hierarchy reaches entt::null. Exhausting the bounded walk means a
	// corrupt cycle was encountered, so runtime behavior fails closed.
	return entityId == entt::null;
}

void ObjectManager::setObjectActive(const Object& obj, bool active)
{
	if (ReportWorkerObjectMutation("ObjectManager.setObjectActive", "Worker code attempted to update live object activity directly."))
	{
		return;
	}
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
		const TerraEngine::Scene::SceneObjectRevisionSnapshot snapshot = captureSceneObjectRevision(obj);
		recordSceneChange(TerraEngine::Scene::SceneChangeKind::Metadata, snapshot);
		recordSceneChange(TerraEngine::Scene::SceneChangeKind::Component, snapshot);
		recordSceneChange(TerraEngine::Scene::SceneChangeKind::Hierarchy, snapshot);
		recordSceneChange(TerraEngine::Scene::SceneChangeKind::Bounds, snapshot);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (onModified && !bulkPublicationActive) onModified();
	}
}

// ObjectManager::isObjectStatic is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ObjectManager::isObjectStatic(const Object& obj) const
{
	const entt::entity entityId = obj.EntityId();
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
	if (ReportWorkerObjectMutation("ObjectManager.setObjectStatic", "Worker code attempted to update live object metadata directly."))
	{
		return;
	}
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
		const TerraEngine::Scene::SceneObjectRevisionSnapshot snapshot = captureSceneObjectRevision(obj);
		recordSceneChange(TerraEngine::Scene::SceneChangeKind::Metadata, snapshot);
		recordSceneChange(TerraEngine::Scene::SceneChangeKind::Bounds, snapshot);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (onModified && !bulkPublicationActive) onModified();
	}
}

void ObjectManager::notifyObjectStructureChanged(const Object& obj)
{
	if (ReportWorkerObjectMutation("ObjectManager.notifyObjectStructureChanged", "Worker code attempted to alter live component structure directly."))
	{
		return;
	}
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
	++componentMutationRevision;
	if (componentMutationRevision == 0)
	{
		componentMutationRevision = 1;
	}
	invalidateRuntimeActiveMembership();
	notifySpatialObjectDirty(obj.EntityId());
	const TerraEngine::Scene::SceneObjectRevisionSnapshot snapshot = captureSceneObjectRevision(obj);
	recordSceneChange(TerraEngine::Scene::SceneChangeKind::Component, snapshot);
	recordSceneChange(TerraEngine::Scene::SceneChangeKind::Bounds, snapshot);
	if (onModified && !bulkPublicationActive) onModified();
}

void ObjectManager::notifyObjectComponentValueChanged(
	const Object& obj,
	std::string_view componentName)
{
	if (ReportWorkerObjectMutation("ObjectManager.notifyObjectComponentValueChanged", "Worker code attempted to alter a live component value directly."))
	{
		return;
	}
	if (!registry.valid(obj.EntityId()))
	{
		return;
	}

	// Value edits preserve component and managed-wrapper identity. The global
	// token invalidates cached binding observations without forcing unrelated
	// spatial, hierarchy, or active-membership snapshots to rebuild.
	++componentMutationRevision;
	if (componentMutationRevision == 0)
	{
		componentMutationRevision = 1;
	}
	if(!componentName.empty()) {
		// Reflected edits execute at a protected boundary. Retaining one monotonic
		// token per component type prevents an unrelated frequently-written
		// component from invalidating every specialized runtime binder.
		auto [revisionIterator, inserted] =
			componentValueMutationRevisions.try_emplace(
				std::string{componentName}, 1U);
		(void)inserted;
		std::uint64_t& revision = revisionIterator->second;
		++revision;
		if(revision == 0U) revision = 1U;
	}
	recordSceneChange(
		TerraEngine::Scene::SceneChangeKind::Component,
		captureSceneObjectRevision(obj));
	if (onModified && !bulkPublicationActive) onModified();
}

std::uint64_t ObjectManager::GetComponentValueMutationRevision(
	std::string_view componentName) const
{
	const auto found = componentValueMutationRevisions.find(
		std::string{componentName});
	return found != componentValueMutationRevisions.end()
		? found->second : 1U;
}

void ObjectManager::notifyManagedBindingsReloaded()
{
	// ScriptingSystem calls this from its protected reload/unload boundary after
	// managed callbacks have stopped. Component generations deliberately remain
	// unchanged because the authored scene structure did not change.
	for (auto& [entity, metadata] : metadataMap)
	{
		if (registry.valid(entity))
		{
			++metadata.bindingGeneration;
		}
	}
	// Do not invoke the authored-object modification callback here. Reloading a
	// managed assembly invalidates runtime wrapper identities only; marking the
	// scene dirty would create a false save prompt and can block unattended editor
	// shutdown after an explicit compile even though no serialized bytes changed.
}

std::uint64_t ObjectManager::GetObjectTransformRevision(const Object& object) const
{
	const entt::entity entityId = object.EntityId();
	if (auto it = transformRevisionByEntity.find(entityId); it != transformRevisionByEntity.end())
	{
		return it->second;
	}
	return 0;
}

std::uint64_t ObjectManager::GetObjectSpatialRevision(const Object& object) const
{
	const entt::entity entityId = object.EntityId();
	if (auto it = spatialRevisionByEntity.find(entityId); it != spatialRevisionByEntity.end())
	{
		return it->second;
	}
	return 0;
}

std::optional<TerraEngine::Scene::WorldPose> ObjectManager::GetCanonicalWorldPose(const Object& object) const
{
	const entt::entity entityId = object.EntityId();
	if (!registry.valid(entityId))
	{
		return std::nullopt;
	}
	const Transform* transform = registry.try_get<Transform>(entityId);
	return transform != nullptr ? std::optional<TerraEngine::Scene::WorldPose>(transform->worldPose) : std::nullopt;
}

TerraEngine::Scene::WorldCoordinateError ObjectManager::SetCanonicalWorldPose(
	const Object& object,
	TerraEngine::Scene::WorldPose pose)
{
	if (ReportWorkerObjectMutation("ObjectManager.SetCanonicalWorldPose", "Worker code attempted to set a live canonical pose directly."))
	{
		return TerraEngine::Scene::WorldCoordinateError::UnsafeLocalRange;
	}
	const entt::entity entityId = object.EntityId();
	Transform* transform = registry.valid(entityId) ? registry.try_get<Transform>(entityId) : nullptr;
	if (transform == nullptr)
	{
		return TerraEngine::Scene::WorldCoordinateError::UnsafeLocalRange;
	}
	const TerraEngine::Scene::WorldCoordinateResult normalized = TerraEngine::Scene::NormalizeWorldPosition(pose.position);
	if (!normalized.Succeeded())
	{
		return normalized.error;
	}
	if (!TerraEngine::Scene::IsWorldPoseFinite(pose))
	{
		return TerraEngine::Scene::WorldCoordinateError::UnsafeLocalRange;
	}

	const bool changedCell = transform->worldPose.position.cell != normalized.position.cell;
	pose.position = normalized.position;
	pose.coordinateFrameRevision = transform->worldPose.coordinateFrameRevision + (changedCell ? 1u : 0u);
	transform->worldPose = pose;
	MarkObjectTransformDirty(object);
	return TerraEngine::Scene::WorldCoordinateError::None;
}

std::uint64_t ObjectManager::getObjectMetadataGeneration(const Object& obj) const
{
	const entt::entity entityId = obj.EntityId();
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

std::uint64_t ObjectManager::getObjectLifetimeGeneration(const Object& obj) const
{
	const entt::entity entityId = obj.EntityId();
	if (!registry.valid(entityId))
	{
		return 0;
	}
	if (auto it = generationByEntity.find(entityId); it != generationByEntity.end())
	{
		return it->second;
	}
	return 0;
}

std::uint64_t ObjectManager::getObjectComponentGeneration(const Object& obj) const
{
	const entt::entity entityId = obj.EntityId();
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
	const entt::entity entityId = obj.EntityId();
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
			return const_cast<ObjectManager*>(this)->makeHandle(entityId);
		}
	}

	return std::nullopt;
}

// ObjectManager::FindObjectByTag is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<Object> ObjectManager::FindObjectByTag(std::string_view tag) const
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
			return const_cast<ObjectManager*>(this)->makeHandle(entityId);
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
	if (it != uuidToEntity.end() && registry.valid(it->second))
	{
		return makeHandle(it->second);
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
		return getUUID(lhs) < getUUID(rhs);
	});
}

// ObjectManager::UpdateObjectUUID is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ObjectManager::UpdateObjectUUID(const Object& object, const TerraEngine::UUID& newId)
{
	if (ReportWorkerObjectMutation("ObjectManager.UpdateObjectUUID", "Worker code attempted to replace a live object identity directly."))
	{
		return;
	}
	const entt::entity entity = object.EntityId();
	if (!registry.valid(entity)) return;
	const TerraEngine::Scene::SceneObjectRevisionSnapshot oldSnapshot = captureSceneObjectRevision(object);

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
	const TerraEngine::Scene::SceneObjectRevisionSnapshot newSnapshot = captureSceneObjectRevision(makeHandle(entity));
	if (auto pending = pendingSceneChanges.find(oldSnapshot.objectId); pending != pendingSceneChanges.end())
	{
		// Deserialization commonly replaces a temporary creation UUID before the
		// first publication. Preserve its pending categories under the authored
		// identity so no transient UUID escapes an immutable packet.
		PendingSceneChange moved = std::move(pending->second);
		pendingSceneChanges.erase(pending);
		moved.snapshot = newSnapshot;
		pendingSceneChanges[newSnapshot.objectId] = std::move(moved);
	}
	else
	{
		recordSceneChange(TerraEngine::Scene::SceneChangeKind::Removed, oldSnapshot);
		recordSceneChange(TerraEngine::Scene::SceneChangeKind::Added, newSnapshot);
	}
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

// ObjectManager::getParentEntityInternal is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
entt::entity ObjectManager::getParentEntityInternal(entt::entity child) const
{
	auto iter = parentMap.find(child);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (iter != parentMap.end())
	{
		return iter->second;
	}
	return entt::null;
}

TerraEngine::UUID ObjectManager::getUUID(const Object& object) const
{
	return getEntityUUID(object.EntityId());
}

TerraEngine::UUID ObjectManager::getEntityUUID(entt::entity entity) const
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
	if (ReportWorkerObjectMutation("ObjectManager.clear", "Worker code attempted to clear live scene state directly."))
	{
		return;
	}
	if (bulkPublicationActive)
	{
		// An empty prepared scene is still a complete replacement and must cross
		// the same visibility boundary as a populated packet.
		bulkPublicationChanged = true;
	}

	// A clear is a scene-lifetime boundary, not an ordinary collection edit.
	// Advancing this generation invalidates every packet prepared for the old
	// scene even when EnTT later reuses an entity slot and compact generation.
	++sceneGeneration;
	if (sceneGeneration == 0)
	{
		sceneGeneration = 1;
	}
	++componentMutationRevision;
	if (componentMutationRevision == 0)
	{
	componentMutationRevision = 1;
	componentValueMutationRevisions.clear();
	}
	pendingSceneChanges.clear();
	// A packet from the previous scene lifetime must never be observed while the
	// next scene is loading. The following protected frame capture publishes an
	// empty/new-scene packet with the advanced generation when appropriate.
	std::atomic_store_explicit(
		&publishedSceneChangePacket,
		std::shared_ptr<const TerraEngine::Scene::SceneChangePacket>{},
		std::memory_order_release);
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

void ObjectManager::BeginBulkPublication(const BulkObjectStorageReservation& reservation)
{
	if (ReportWorkerObjectMutation("ObjectManager.BeginBulkPublication", "Worker code attempted to begin a live bulk publication directly."))
	{
		return;
	}
	if (bulkPublicationActive)
	{
		throw std::logic_error("Nested bulk scene publication is not supported.");
	}

	// Reserve every manager-owned table from the validated count before any
	// object is created. EnTT components are reserved by their typed producer;
	// this manager owns identity, metadata, hierarchy, and lookup storage.
	metadataMap.reserve(reservation.objectCount);
	registry.storage<Transform>().reserve(reservation.objectCount);
	registry.storage<TerraEngine::ActiveObjectTag>().reserve(reservation.objectCount);
	parentMap.reserve(reservation.objectCount);
	childrenMap.reserve(reservation.objectCount);
	uuidToEntity.reserve(reservation.objectCount);
	entityToUuid.reserve(reservation.objectCount);
	generationByEntity.reserve(reservation.objectCount);
	transformRevisionByEntity.reserve(reservation.objectCount);
	spatialRevisionByEntity.reserve(reservation.objectCount);
	nameIndex.reserve(reservation.objectCount);
	tagIndex.reserve(reservation.objectCount);
	pendingSceneChanges.reserve(reservation.objectCount);
	bulkPublicationActive = true;
	bulkPublicationChanged = false;
}

void ObjectManager::FinishBulkPublication(bool published)
{
	if (!bulkPublicationActive)
	{
		return;
	}
	bulkPublicationActive = false;
	const bool changed = bulkPublicationChanged;
	bulkPublicationChanged = false;
	if (!published || !changed)
	{
		return;
	}

	// One revision and one observer callback are the protected visibility point
	// for the whole replacement. Consumers cannot observe partial object/index
	// construction because every per-object notification was suppressed above.
	++spatialRevision;
	++hierarchyRevision;
	++runtimeActiveMembershipRevision;
	cachedRuntimeActiveMembershipRevision = static_cast<std::uint64_t>(-1);
	if (onModified)
	{
		onModified();
	}
}
