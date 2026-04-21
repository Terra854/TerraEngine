#pragma once

#include <entt/entt.hpp>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Object.h"
#include "Components/Transform.h"
#include "Utilities/UUID.h"

struct ObjectMetadata {
	std::string name;
	std::string tag = "Untagged";
	std::string layer = "Default";
	bool active = true;
	bool isStatic = false;
};

class ObjectManager {
public:
	static ObjectManager& instance();

	ObjectManager(const ObjectManager&) = delete;
	ObjectManager& operator=(const ObjectManager&) = delete;

	// Method to create a new Object.
	Object createObject(const std::string& name);
	// Assign or clear a parent for the given object. Passing std::nullopt
	// detaches the object to the root. The call validates cycles before
	// mutating internal state and returns false on invalid input.
	bool setParent(Object child, std::optional<Object> parent);
	// Query the immediate children for a parent object, returning empty when
	// none are present or the parent is invalid.
	std::vector<Object> getChildren(const Object& parent);
	// Enumerate all root-level objects (entities without a parent) so editors
	// can display a hierarchy tree.
	std::vector<Object> getRootObjects();
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

	// Get the list of all globally available tags defined in the project.
	const std::vector<std::string>& getAvailableTags() const { return availableTags; }
	// Register a new gameplay tag to the global project list.
	void registerTag(const std::string& tag);

	// Get the list of all globally available layers defined in the project.
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
	
	// Get UUID for an entity.
	TerraEngine::UUID getUUID(entt::entity entity);

	// Update the UUID mapping for an entity. Used during deserialization to restore IDs.
	void UpdateEntityUUID(entt::entity entity, const TerraEngine::UUID& newId);

	// Invoke a caller-provided function for every entity that owns the given
	// component set. The callable is perfect-forwarded so lambdas (and their
	// captures) avoid the overhead of std::function type erasure. The first
	// parameter is an Object handle to the entity followed by component
	// references.
	template<typename... Components, typename Func>
	void forEachObjectWithComponents(Func&& func)
	{
		registry.view<Components...>().each([&](entt::entity entity, Components&... comps) {
			auto obj = makeHandle(entity);
			if (isObjectActive(obj) && (comps.active && ...)) {
				func(obj, comps...);
			}
		});
	};

	// Get all objects that have the following components.
	template<typename... Components>
	std::vector<Object> getObjectsWithComponents()
	{
		std::vector<Object> objects;

		auto view = registry.view<Components...>();
		for (auto entity : view)
		{
			auto obj = makeHandle(entity);
			if (!isObjectActive(obj)) continue;

			// Retrieve components to check their active state
			auto tuple = view.get(entity);
			if (std::apply([](auto&... comps) { return (comps.active && ...); }, tuple))
			{
				objects.push_back(obj);
			}
		}
		return objects;
	}

	// Method to remove an Object.
	void removeObject(Object obj);
	// Fetch the parent entity for a given child, or entt::null if unparented.
	entt::entity getParentEntity(entt::entity child) const;

	void clear();

	entt::registry& getRegistry() { return registry; }

	// Registers a callback that fires whenever the scene graph structure changes
	// (creation, deletion, or parenting). This allows external systems like the
	// SceneManager to track dirty state without tight coupling.
	void SetOnModifiedCallback(std::function<void()> callback) { onModified = callback; }
	// Spatial systems need a narrower signal so they can refresh object bounds
	// without assuming every metadata edit changes placement or visibility.
	void SetOnSpatialObjectDirtyCallback(std::function<void(entt::entity)> callback) { onSpatialObjectDirty = callback; }
	std::uint64_t GetSpatialRevision() const { return spatialRevision; }

private:
	ObjectManager();
	~ObjectManager();

	friend class Object;

	std::function<void()> onModified;
	std::function<void(entt::entity)> onSpatialObjectDirty;
	std::uint64_t spatialRevision = 0;
	std::unordered_map<Object, ObjectMetadata> metadataMap;
	entt::registry registry;
	std::unordered_map<entt::entity, entt::entity> parentMap;
	std::unordered_map<entt::entity, std::vector<entt::entity>> childrenMap;
	// Secondary lookup table that pairs stable UUIDs with transient EnTT entity
	// identifiers. This mapping is essential for maintaining object identity
	// across undo/redo and serialization boundaries.
	std::unordered_map<TerraEngine::UUID, entt::entity> uuidToEntity;
	std::unordered_map<entt::entity, TerraEngine::UUID> entityToUuid;

	std::vector<std::string> availableTags;
	std::vector<std::string> availableLayers;

	Object makeHandle(entt::entity entityId);
	void notifySpatialObjectDirty(entt::entity entityId);
	void detachFromParent(entt::entity childId);
	bool isDescendant(entt::entity ancestor, entt::entity potentialChild) const;
};
