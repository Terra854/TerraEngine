#pragma once
#include <entt/entt.hpp>
#include <memory>
#include <string>
#include <unordered_map>

#include "Object.h"
#include "Transform.h"

class ObjectManager {
public:
    static ObjectManager& instance();

    ObjectManager(const ObjectManager&) = delete;
    ObjectManager& operator=(const ObjectManager&) = delete;

    // Method to create a new Object
    Object createObject(const std::string& name);
    
    // View all objects that have the following components
    template<typename... Components>
    void forEachObjectWithComponents(std::function<void(Components&...)> func)
    {
        registry.view<Components...>().each(func);
    };

    // Get all objects that have the following components
    template<typename... Components>
    std::vector<Object> getObjectsWithComponents() {
        std::vector<Object> objects;

        for (auto entity : registry.view<Components...>()) {
            Object o;
            o.entity_id = entity;
            objects.push_back(o);
        }
        return objects;
    }

    // Method to remove an Object
    void removeObject(Object obj);

private:
    ObjectManager() = default;
    ~ObjectManager() = default;

    friend class Object;

    std::unordered_map<Object, std::string> objectMap;
    entt::registry registry;
};

