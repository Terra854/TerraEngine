// ObjectManager.h — Interface for the engine's central entity and hierarchy manager.
//
// This file declares the ObjectManager singleton, which acts as the authoritative
// source for all scene entities and their relationships. It manages object
// metadata, hierarchy tracking, and coordinate space conversions for parent-child
// relationships, while providing observers for tracking scene modifications.

#pragma once

#include <entt/entt.hpp>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Object.h"
#include "ActiveMembership.h"
#include "Components/Transform.h"
#include "Core/Scheduling/WorkerSafety.h"
#include "Scene/RuntimeLayout.h"
#include "Utilities/UUID.h"

struct ObjectMetadata {
	std::string name;
	std::string tag = "Untagged";
	std::string layer = "Default";
	bool active = true;
	bool isStatic = false;
	std::uint64_t metadataGeneration = 1;
	std::uint64_t componentGeneration = 1;
	std::uint64_t bindingGeneration = 1;
};

// class ObjectManager groups related state and operations so callers have one documented contract.
class ObjectManager {
public:
	static ObjectManager& instance();

	ObjectManager(const ObjectManager&) = delete;
	ObjectManager& operator=(const ObjectManager&) = delete;

	// Method to create a new Object. This mutates the live registry and must
	// stay on a protected scheduler path; worker code should enqueue a deferred
	// SceneMutationQueue create request instead.
	Object createObject(const std::string& name);
	bool isObjectValid(const Object& object) const;
	// Assign or clear a parent for the given object. Passing std::nullopt
	// detaches the object to the root. The call validates cycles before
	// mutating internal state and returns false on invalid input. Worker code
	// must request this through SceneMutationQueue so hierarchy commits stay
	// ordered at protected flush points.
	bool setParent(Object child, std::optional<Object> parent);
	// Query the immediate children for a parent object, returning empty when
	// none are present or the parent is invalid.
	std::vector<Object> getChildren(const Object& parent);
	// Enumerate all root-level objects (entities without a parent) so editors
	// can display a hierarchy tree.
	std::vector<Object> getRootObjects();
	std::optional<Object> getParent(const Object& child) const;
	// Return a stable display name for the object, defaulting to "Unnamed"
	// when the object is missing from the internal metadata table.
	std::string getObjectName(const Object& obj) const;
	// Update the display name for the object.
	void setObjectName(const Object& obj, const std::string& name);

	// Return the object's gameplay-specific tag, defaulting to "Untagged"
	// when the object is missing from the internal metadata table.
	std::string getObjectTag(const Object& obj) const;
	// Update the gameplay-specific tag for the object.
	void setObjectTag(const Object& obj, const std::string& tag);

	// Return the object's layer assignment, defaulting to "Default"
	// when the object is missing from the internal metadata table.
	std::string getObjectLayer(const Object& obj) const;
	// Update the layer assignment for the object.
	void setObjectLayer(const Object& obj, const std::string& layer);

	// Return whether the object is considered active in the scene.
	bool isObjectActive(const Object& obj) const;
	// Set the object's active state in the scene.
	void setObjectActive(const Object& obj, bool active);

	// Return whether the object is marked as static for engine optimizations.
	bool isObjectStatic(const Object& obj) const;
	// Set the object's static state for engine optimizations.
	void setObjectStatic(const Object& obj, bool isStatic);
	// Notify systems and managed wrapper caches that component membership or
	// other object-local structure changed without changing transform values.
	void notifyObjectStructureChanged(const Object& obj);
	// Managed assembly reloads invalidate wrapper lifetimes without changing
	// native component membership. This protected helper advances only binding
	// generations so stale C# physics/component facades fail validation.
	void notifyManagedBindingsReloaded();
	// Mark a transform-affecting edit. This is separate from generic component
	// structure changes so transform propagation can expand only affected
	// hierarchy branches while spatial refresh still sees the object as dirty.
	void MarkObjectTransformDirty(const Object& obj);
	// Object metadata generations let managed wrappers reuse names and lookup
	// answers during one script dispatch without calling back into native code
	// on every property access.
	std::uint64_t getObjectLifetimeGeneration(const Object& obj) const;
	std::uint64_t getObjectMetadataGeneration(const Object& obj) const;
	std::uint64_t getObjectComponentGeneration(const Object& obj) const;
	std::uint64_t getObjectBindingGeneration(const Object& obj) const;

	// Get the list of all globally available tags defined in the project.
	// getAvailableTags is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const std::vector<std::string>& getAvailableTags() const { return availableTags; }
	// Register a new gameplay tag to the global project list.
	void registerTag(const std::string& tag);

	// Get the list of all globally available layers defined in the project.
	// getAvailableLayers is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const std::vector<std::string>& getAvailableLayers() const { return availableLayers; }
	// Register a new system layer to the global project list.
	void registerLayer(const std::string& layer);

	// Find the first valid object whose authored display name matches the
	// requested text exactly. Returns std::nullopt when no match exists.
	std::optional<Object> FindObjectByName(std::string_view name) const;
	// Find the first valid object whose tag string matches exactly.
	// Returns std::nullopt when no tagged object matches.
	std::optional<Object> FindObjectByTag(std::string_view tag) const;
	// Build an Object handle for an existing entity id if it is valid.
	std::optional<Object> getObject(entt::entity id);
	// Build an Object handle for an existing entity by UUID.
	std::optional<Object> getObject(const TerraEngine::UUID& id);

	// Object handles are cheap identifiers for snapshots and worker results.
	// They include a generation so stale handles are rejected when an EnTT
	// entity id is destroyed and later reused.
	Object makeObjectHandle(entt::entity entityId) const;
	TerraEngine::ObjectIdentityResolution validateObjectHandle(const Object& reference) const;
	std::optional<Object> resolveObjectHandle(const Object& reference);
	const std::vector<TerraEngine::Scene::RuntimeActiveMembership>& GetRuntimeActiveMembershipSnapshot();
	std::uint64_t GetRuntimeActiveMembershipRevision() const { return runtimeActiveMembershipRevision; }
	
	// Get UUID for an object.
	TerraEngine::UUID getUUID(const Object& object) const;

	// Update the UUID mapping for an object. Used during deserialization to restore IDs.
	void UpdateObjectUUID(const Object& object, const TerraEngine::UUID& newId);

	// Invoke a caller-provided function for every entity that owns the given
	// component set. The callable is perfect-forwarded so lambdas (and their
	// captures) avoid the overhead of std::function type erasure. The first
	// parameter is an Object handle to the entity followed by component
	// references.
	template<typename... Components, typename Func>
	void forEachObjectWithComponents(Func&& func)
	{
		// Runtime systems scan the compact active membership list first, then
		// touch component storage only for entities that survived object-level
		// active filtering. This keeps inactive authored objects visible to
		// editor paths while removing a metadata lookup from hot queries.
		for (const TerraEngine::Scene::RuntimeActiveMembership& membership : GetRuntimeActiveMembershipSnapshot()) {
			if (!membership.objectActive || !registry.valid(membership.entity) || !registry.all_of<Components...>(membership.entity)) {
				continue;
			}
			auto activeComps = std::forward_as_tuple(registry.get<Components>(membership.entity)...);
			auto obj = makeHandle(membership.entity);
			if (std::apply([](auto&... comps) { return (IsRuntimeComponentActive(comps) && ...); }, activeComps)) {
				std::apply([&](auto&... comps) { func(obj, comps...); }, activeComps);
			}
		}
	};

	// Include inactive objects for editor, serialization, diagnostics, and
	// migration checks that must inspect authored state instead of gameplay
	// runtime membership.
	template<typename... Components, typename Func>
	void forEachObjectWithComponentsIncludingInactive(Func&& func)
	{
		registry.view<Components...>().each([&](entt::entity entity, Components&... comps) {
			auto obj = makeHandle(entity);
			func(obj, comps...);
		});
	};

	// Get all objects that have the following components.
	template<typename... Components>
	std::vector<Object> getObjectsWithComponents()
	{
		std::vector<Object> objects;
		// Entity iteration follows the retained active membership cache. That
		// cache is rebuilt only when object/component membership changes, so
		// repeated runtime scans avoid per-object metadata checks.
		for (const TerraEngine::Scene::RuntimeActiveMembership& membership : GetRuntimeActiveMembershipSnapshot())
		{
			if (!membership.objectActive || !registry.valid(membership.entity) || !registry.all_of<Components...>(membership.entity))
			{
				continue;
			}
			auto activeComps = std::forward_as_tuple(registry.get<Components>(membership.entity)...);
			if (std::apply([](auto&... comps) { return (IsRuntimeComponentActive(comps) && ...); }, activeComps))
			{
				objects.push_back(makeHandle(membership.entity));
			}
		}
		return objects;
	}

	template<typename... Components>
	std::vector<Object> getObjectsWithComponentsIncludingInactive()
	{
		std::vector<Object> objects;
		auto view = registry.view<Components...>();
		for (auto entity : view)
		{
			objects.push_back(makeHandle(entity));
		}
		return objects;
	}

	// Snapshot matching objects before worker dispatch so query chunks iterate a
	// stable list even if later systems request deferred structural mutations.
	template<typename... Components>
	std::vector<Object> SnapshotObjectsWithComponents()
	{
		std::vector<Object> objects = getObjectsWithComponents<Components...>();
		SortObjectsByStableId(objects);
		return objects;
	}

	// Shared stable sorter used by ECS query planning and tests. UUID order is
	// chosen over EnTT entity order because entity ids are allocation details.
	void SortObjectsByStableId(std::vector<Object>& objects);

	template<typename T>
	bool HasComponent(const Object& object) const
	{
		const entt::entity entityId = object.EntityId();
		return registry.valid(entityId) && registry.any_of<T>(entityId);
	}

	template<typename T>
	T* GetComponent(const Object& object)
	{
		const entt::entity entityId = object.EntityId();
		return registry.valid(entityId) ? registry.try_get<T>(entityId) : nullptr;
	}

	template<typename T>
	const T* GetComponent(const Object& object) const
	{
		const entt::entity entityId = object.EntityId();
		return registry.valid(entityId) ? registry.try_get<T>(entityId) : nullptr;
	}

	template<typename T, typename... Args>
	T* AddComponent(const Object& object, Args&&... args)
	{
		if (TerraEngine::Scheduling::IsWorkerSafetyScopeActive())
		{
			TerraEngine::Scheduling::ReportWorkerSafetyViolation(
				TerraEngine::Scheduling::WorkerSafetySeverity::Error,
				"ObjectManager.AddComponent",
				"Worker code attempted to add a live component directly.",
				"Capture a SceneMutationQueue AddComponent request and apply it at a protected scheduler flush.");
			return nullptr;
		}
		const entt::entity entityId = object.EntityId();
		if (!registry.valid(entityId))
		{
			return nullptr;
		}
		if (T* existing = registry.try_get<T>(entityId))
		{
			return existing;
		}
		T& component = registry.emplace<T>(entityId, std::forward<Args>(args)...);
		notifyObjectStructureChanged(object);
		if constexpr (std::is_same_v<T, Transform>)
		{
			MarkObjectTransformDirty(object);
		}
		return &component;
	}

	template<typename T>
	void RemoveComponent(const Object& object)
	{
		if (TerraEngine::Scheduling::IsWorkerSafetyScopeActive())
		{
			TerraEngine::Scheduling::ReportWorkerSafetyViolation(
				TerraEngine::Scheduling::WorkerSafetySeverity::Error,
				"ObjectManager.RemoveComponent",
				"Worker code attempted to remove a live component directly.",
				"Capture a SceneMutationQueue RemoveComponent request and apply it at a protected scheduler flush.");
			return;
		}
		const entt::entity entityId = object.EntityId();
		if (!registry.valid(entityId) || !registry.any_of<T>(entityId))
		{
			return;
		}
		registry.remove<T>(entityId);
		notifyObjectStructureChanged(object);
		if constexpr (std::is_same_v<T, Transform>)
		{
			MarkObjectTransformDirty(object);
		}
	}

	// Method to remove an Object. Deletion destroys components and hierarchy
	// bookkeeping, so direct calls are protected-only; workers must enqueue a
	// deferred delete request.
	void removeObject(Object obj);

	void clear();

	// getRegistry is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	entt::registry& getRegistry() { return registry; }

	// Registers a callback that fires whenever the scene graph structure changes
	// (creation, deletion, or parenting). This allows external systems like the
	// SceneManager to track dirty state without tight coupling.
	void SetOnModifiedCallback(std::function<void()> callback) { onModified = callback; }
	// Spatial systems need a narrower signal so they can refresh object bounds
	// without assuming every metadata edit changes placement or visibility.
	// SetOnSpatialObjectDirtyCallback is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SetOnSpatialObjectDirtyCallback(std::function<void(entt::entity)> callback) { onSpatialObjectDirty = callback; }
	std::uint64_t GetSpatialRevision() const { return spatialRevision; }
	std::uint64_t GetHierarchyRevision() const { return hierarchyRevision; }
	std::uint64_t GetObjectTransformRevision(const Object& object) const;
	std::uint64_t GetObjectSpatialRevision(const Object& object) const;

private:
	ObjectManager();
	~ObjectManager();

	friend class Object;

	std::function<void()> onModified;
	std::function<void(entt::entity)> onSpatialObjectDirty;
	std::uint64_t spatialRevision = 0;
	std::uint64_t transformRevision = 0;
	std::uint64_t hierarchyRevision = 0;
	std::uint64_t runtimeActiveMembershipRevision = 0;
	std::uint64_t cachedRuntimeActiveMembershipRevision = static_cast<std::uint64_t>(-1);
	std::vector<TerraEngine::Scene::RuntimeActiveMembership> runtimeActiveMembershipCache;
	std::unordered_map<entt::entity, ObjectMetadata> metadataMap;
	entt::registry registry;
	std::unordered_map<entt::entity, entt::entity> parentMap;
	std::unordered_map<entt::entity, std::vector<entt::entity>> childrenMap;
	// Secondary lookup table that pairs stable UUIDs with transient EnTT entity
	// identifiers. This mapping is essential for maintaining object identity
	// across undo/redo and serialization boundaries.
	std::unordered_map<TerraEngine::UUID, entt::entity> uuidToEntity;
	std::unordered_map<entt::entity, TerraEngine::UUID> entityToUuid;
	std::unordered_map<entt::entity, std::uint64_t> transformRevisionByEntity;
	std::unordered_map<entt::entity, std::uint64_t> spatialRevisionByEntity;
	std::unordered_map<entt::entity, std::uint64_t> generationByEntity;
	std::uint64_t nextObjectGeneration = 1;
	// Name and tag indexes are maintained beside metadata so C# Object.Find
	// calls do not scan every scene object during script updates.
	std::unordered_map<std::string, std::vector<entt::entity>> nameIndex;
	std::unordered_map<std::string, std::vector<entt::entity>> tagIndex;

	std::vector<std::string> availableTags;
	std::vector<std::string> availableLayers;

	Object makeHandle(entt::entity entityId);
	void addLookupIndex(entt::entity entityId, const ObjectMetadata& metadata);
	void removeLookupIndex(entt::entity entityId, const ObjectMetadata& metadata);
	void removeFromLookupBucket(std::unordered_map<std::string, std::vector<entt::entity>>& index, const std::string& key, entt::entity entityId);
	void notifySpatialObjectDirty(entt::entity entityId);
	void notifyHierarchyDirty();
	void invalidateRuntimeActiveMembership();
	void rebuildRuntimeActiveMembershipCache();
	TerraEngine::UUID getEntityUUID(entt::entity entity) const;
	entt::entity getParentEntityInternal(entt::entity child) const;
	void detachFromParent(entt::entity childId);
	bool isDescendant(entt::entity ancestor, entt::entity potentialChild) const;
};
