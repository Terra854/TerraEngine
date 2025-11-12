#pragma once

#include <entt/entt.hpp>

#include <memory>
#include <string>
#include <utility>

// Object now serves as an interface to manage entities in EnTT
class Object {
public:
    explicit Object(entt::registry& registry);

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
        std::size_t operator()(const Object& k) const {
            return static_cast<std::size_t>(k.id);
        }
    };
}