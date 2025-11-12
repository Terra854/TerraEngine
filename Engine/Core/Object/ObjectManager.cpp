#include "ObjectManager.h"

ObjectManager& ObjectManager::instance() {
    static ObjectManager instance;
    return instance;
}

// Create a new Object
Object ObjectManager::createObject(const std::string& name) {
    auto object = Object(registry);
    objectMap.insert({ object, name });
    return object;
}

// Remove an Object
void ObjectManager::removeObject(Object obj) {
    // Remove the object from the registry
    registry.destroy(obj.id);

    // Erase the object from the map
    objectMap.erase(obj);
}

