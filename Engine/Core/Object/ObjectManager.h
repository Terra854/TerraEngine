#pragma once

#include <entt/entt.hpp>
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Object.h"
#include "Transform.h"

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
	// when the object is missing from the internal name table.
	std::string getObjectName(const Object& obj) const;
	// Build an Object handle for an existing entity id if it is valid.
	std::optional<Object> getObject(entt::entity id);

	// Invoke a caller-provided function for every entity that owns the given
	// component set. The callable is perfect-forwarded so lambdas (and their
	// captures) avoid the overhead of std::function type erasure. The first
	// parameter is an Object handle to the entity followed by component
	// references.
	template<typename... Components, typename Func>
	void forEachObjectWithComponents(Func&& func)
	{
		registry.view<Components...>().each([&](entt::entity entity, Components&... comps) {
			if ((comps.active && ...)) {
				func(makeHandle(entity), comps...);
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
			// Retrieve components to check their active state
			auto tuple = view.get(entity);
			if (std::apply([](auto&... comps) { return (comps.active && ...); }, tuple))
			{
				objects.push_back(makeHandle(entity));
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

private:
	ObjectManager() = default;
	~ObjectManager() = default;

	friend class Object;

	std::unordered_map<Object, std::string> objectMap;
	entt::registry registry;
	std::unordered_map<entt::entity, entt::entity> parentMap;
	std::unordered_map<entt::entity, std::vector<entt::entity>> childrenMap;

	Object makeHandle(entt::entity entityId);
	void detachFromParent(entt::entity childId);
	bool isDescendant(entt::entity ancestor, entt::entity potentialChild) const;
};
