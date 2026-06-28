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
