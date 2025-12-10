#pragma once

#include <entt/entt.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "Object.h"
#include "Transform.h"

class ObjectManager {
public:
	static ObjectManager& instance();

	ObjectManager(const ObjectManager&) = delete;
	ObjectManager& operator=(const ObjectManager&) = delete;

	// Method to create a new Object.
	Object createObject(const std::string& name);

	// Invoke a caller-provided function for every entity that owns the given
	// component set. The callable is perfect-forwarded so lambdas (and their
	// captures) avoid the overhead of std::function type erasure.
	template<typename... Components, typename Func>
	void forEachObjectWithComponents(Func&& func)
	{
		registry.view<Components...>().each(std::forward<Func>(func));
	};

	// Get all objects that have the following components.
	template<typename... Components>
	std::vector<Object> getObjectsWithComponents()
	{
		std::vector<Object> objects;

		for (auto entity : registry.view<Components...>())
		{
			Object o;
			o.id = entity;
			objects.push_back(o);
		}
		return objects;
	}

	// Method to remove an Object.
	void removeObject(Object obj);

private:
	ObjectManager() = default;
	~ObjectManager() = default;

	friend class Object;

	std::unordered_map<Object, std::string> objectMap;
	entt::registry registry;
};
