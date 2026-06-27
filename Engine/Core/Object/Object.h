// Object.h — Handle-based interface for scene entities in the ECS.
//
// This file defines the Object class, which wraps an EnTT entity identifier and
// a registry pointer to provide a consistent API for entity manipulation. It
// manages component lifecycle and provides stable access to entity-specific
// metadata through the ObjectManager.

#pragma once

#include <entt/entt.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "Core/Scheduling/WorkerSafety.h"
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

	// Component management with EnTT. Adding a component changes live storage,
	// so worker phases must enqueue a deferred SceneMutationQueue request
	// instead of calling this protected-only API directly.
	template<typename T, typename... Args>
	T& addComponent(Args&&... args) {
		if (TerraEngine::Scheduling::IsWorkerSafetyScopeActive()) {
			TerraEngine::Scheduling::ReportWorkerSafetyViolation(
				TerraEngine::Scheduling::WorkerSafetySeverity::Error,
				"Object.addComponent",
				"Worker code attempted to add a live component directly.",
				"Capture a SceneMutationQueue AddComponent request and apply it at a protected scheduler flush.");
			throw std::runtime_error("Worker code cannot add live components directly.");
		}
		return registry->emplace<T>(id, std::forward<Args>(args)...);
	}

	// Function to get a component
	template<typename T>
	T* getComponent() {
		return registry->try_get<T>(id);
	}

	// Function to remove a component. Removal is protected-only for the same
	// reason as addComponent: worker phases record intent and let the scheduler
	// apply it at a mutation flush point.
	template<typename T>
	void removeComponent() {
		if (TerraEngine::Scheduling::IsWorkerSafetyScopeActive()) {
			TerraEngine::Scheduling::ReportWorkerSafetyViolation(
				TerraEngine::Scheduling::WorkerSafetySeverity::Error,
				"Object.removeComponent",
				"Worker code attempted to remove a live component directly.",
				"Capture a SceneMutationQueue RemoveComponent request and apply it at a protected scheduler flush.");
			return;
		}
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
