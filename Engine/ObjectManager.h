#pragma once
#include <entt/entt.hpp>
#include <unordered_map>
#include <memory>
#include "Object.h"

class ObjectManager {
public:
    static ObjectManager& instance();

    ObjectManager(const ObjectManager&) = delete;
    ObjectManager& operator=(const ObjectManager&) = delete;

    // Method to create a new Object
    Object createObject(const std::string& name);

    // Method to remove an Object
    void removeObject(Object obj);

private:
    ObjectManager() = default;
    ~ObjectManager() = default;

    friend class Object;

    std::unordered_map<Object, std::string> objectMap;
    entt::registry registry;
};
