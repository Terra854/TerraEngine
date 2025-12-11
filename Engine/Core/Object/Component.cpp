#include "Component.h"

#include <unordered_map>
#include <mutex>

// Centralized registry storage so the Editor and Engine share the same
// component access table across module boundaries.
std::unordered_map<std::string, ComponentAccessFunctions>& ComponentAccessRegistry()
{
	static std::unordered_map<std::string, ComponentAccessFunctions> registry;
	return registry;
}

std::mutex& ComponentAccessRegistryMutex()
{
	static std::mutex mutex;
	return mutex;
}
