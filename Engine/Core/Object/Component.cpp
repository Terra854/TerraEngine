// Component.cpp — Implementation of the centralized component access registry.
//
// This file provides the storage for the component access functions that allow
// the engine and editor to interact with components via name-based lookups. It
// ensures that component registration is shared across module boundaries using
// a thread-safe registry implementation.

#include "Component.h"

#include "ObjectManager.h"

#include <unordered_map>
#include <mutex>

// Centralized registry storage so the Editor and Engine share the same
// component access table across module boundaries.
std::unordered_map<std::string, ComponentAccessFunctions>& ComponentAccessRegistry()
{
	static std::unordered_map<std::string, ComponentAccessFunctions> registry;
	return registry;
}

// ComponentAccessRegistryMutex is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::mutex& ComponentAccessRegistryMutex()
{
	static std::mutex mutex;
	return mutex;
}

entt::registry& ComponentAccessObjectRegistry()
{
	// Compatibility callbacks need the authoritative registry but should not
	// revive the old Object facade. Keep that bridge centralized here.
	return ObjectManager::instance().getRegistry();
}

void NotifyReflectedComponentPropertyMutation(
	entt::registry& registry,
	entt::entity entity,
	std::string_view componentName)
{
	// Transform is the only reflected component whose property values feed the
	// renderer's spatial cache directly. Other component types retain their
	// existing structure/dirty notifications at the higher-level mutation path.
	if(componentName != Transform::kComponentName) return;

	ObjectManager& objects = ObjectManager::instance();
	if(&registry != &objects.getRegistry()) {
		// Tests and tools may own isolated EnTT registries. They have no
		// ObjectManager spatial snapshot to invalidate.
		return;
	}

	const std::optional<Object> object = objects.getObject(entity);
	if(object) {
		// This runs only after the reflected write lock is released. Inspector,
		// automation, managed runtime, and exported-build writes therefore all
		// publish the same transform/bounds revisions without retaining a live
		// component pointer across the protected notification.
		objects.MarkObjectTransformDirty(*object);
	}
}

ComponentAccessObservation ObserveRegisteredComponentAccess(entt::registry& registry, entt::entity entity, std::string_view componentName, std::uint64_t componentGeneration)
{
	// Worker-side transaction builders use this read-only observation to bind
	// a reflected component write to the generation that was visible during
	// preparation. The protected commit path later revalidates that generation
	// before invoking the type-erased setter.
	std::lock_guard lock(ComponentAccessRegistryMutex());
	ComponentAccessObservation observation;
	observation.componentName = std::string(componentName);
	observation.componentGeneration = componentGeneration;
	auto component = ComponentAccessRegistry().find(observation.componentName);
	observation.registered = component != ComponentAccessRegistry().end();
	observation.hasComponent = observation.registered && component->second.entityHasComponent(registry, entity);
	return observation;
}
