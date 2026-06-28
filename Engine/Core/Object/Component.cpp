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
