#include "ObjectManager.h"

ObjectManager& ObjectManager::instance() {
    static ObjectManager instance;
    return instance;
}

// Create a new Object
Object ObjectManager::createObject(const std::string& name) {
	// Every entity begins life with a Transform so systems can safely assume
	// spatial data exists without guarding for missing components.
	auto object = Object(registry);
	object.addComponent<Transform>();
	objectMap.insert({ object, name });
	return object;
}

// Remove an Object
void ObjectManager::removeObject(Object obj) {
	// Destroying the entity purges every component attached to it (Transform and any others),
	// ensuring no dangling component data survives past object lifetime.
	if (registry.valid(obj.id))
	{
		registry.destroy(obj.id);
	}

	// Erase the object from the map
	objectMap.erase(obj);
}
