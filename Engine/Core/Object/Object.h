// Object.h — Handle-based interface for scene entities in the ECS.
//
// This file defines the Object class, which wraps an EnTT entity identifier and
// a registry pointer to provide a consistent API for entity manipulation. It
// manages component lifecycle and provides stable access to entity-specific
// metadata through the ObjectManager.

#pragma once

#include <entt/entt.hpp>

#include <memory>
#include <string>
#include <utility>

#include "Utilities/UUID.h"

// Object now serves as an interface to manage entities in EnTT
class Object {
public:
	explicit Object(entt::registry& registry);
	// Construct an Object handle that references an existing entity without
	// creating a new one. Restricted to the ObjectManager so stray handles
	// cannot be built without registry ownership.
	Object(entt::registry& registry, entt::entity existingId);

	TerraEngine::UUID getUUID() const;

	std::string getName() const;
	void setName(const std::string& name);

	std::string getTag() const;
	void setTag(const std::string& tag);

	std::string getLayer() const;
	void setLayer(const std::string& layer);

	bool isActive() const;
	void setActive(bool active);

	bool isStatic() const;
	void setStatic(bool isStatic);

	// Component management with EnTT
	template<typename T, typename... Args>
	T& addComponent(Args&&... args) {
		return registry->emplace<T>(id, std::forward<Args>(args)...);
	}

	// Function to get a component
	template<typename T>
	T* getComponent() {
		return registry->try_get<T>(id);
	}

	// Function to remove a component
	template<typename T>
	void removeComponent() {
		registry->remove<T>(id);
	}

	// Expose the underlying entity identifier for systems that need stable IDs
	// when building UI lists or other external bookkeeping.
	entt::entity EntityId() const { return id; }

	// Needed for using Object as a key in std::unordered_map
	bool operator==(const Object& other) const {
		return id == other.id;
	}

private:
	friend struct std::hash<Object>;
	friend class ObjectManager; // Needed to delete objects
	entt::entity id{}; // EnTT entity ID
	entt::registry* registry{};
};

// Needed for using Object as a key in std::unordered_map
namespace std {
	template<>
	struct hash<Object> {
		// operator is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		std::size_t operator()(const Object& k) const {
			return static_cast<std::size_t>(k.id);
		}
	};
}
