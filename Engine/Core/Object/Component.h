#pragma once

#include <rttr/registration>
#include <rttr/variant.h>

#include <memory>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <unordered_map>
#include <functional>
#include <string_view>
#include <mutex>
#include <nlohmann/json.hpp>
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>
#include "Core/Export.h"
#include "Object.h"
#include "Utilities/UUID.h"

// Helper utilities that keep the CRTP component base lightweight while still
// allowing each component to expose metadata to RTTR in a consistent way.
template <typename T, typename = void>
struct component_has_custom_name : std::false_type
{
};

template <typename T>
struct component_has_custom_name<T, std::void_t<decltype(T::kComponentName)>> : std::true_type
{
};

template <typename T, typename = void>
struct component_has_reflect : std::false_type
{
};

template <typename T>
struct component_has_reflect<T, std::void_t<decltype(T::reflect(std::declval<rttr::registration::class_<T>&>()))>> : std::true_type
{
};

template <typename T>
inline constexpr bool component_has_reflect_v = component_has_reflect<T>::value;

template <typename T>
inline std::string component_name_for()
{
	if constexpr (component_has_custom_name<T>::value)
	{
		return std::string(T::kComponentName);
	}
	else
	{
		return rttr::type::get<T>().get_name().to_string();
	}
}

// Type-erased property hooks that allow runtime access to any component
// registered through the Component CRTP without exposing the concrete type.
struct ComponentAccessFunctions
{
	std::function<rttr::variant(Object&, std::string_view)> getter;
	std::function<bool(Object&, std::string_view, const rttr::variant&)> setter;
	std::function<bool(Object&)> hasComponent;
	std::function<void(Object&)> addComponent;
	std::function<void(Object&)> removeComponent;
	std::function<nlohmann::json(Object&)> serialize;
	std::function<void(Object&, const nlohmann::json&)> deserialize;
};

TERRAENGINE_API std::unordered_map<std::string, ComponentAccessFunctions>& ComponentAccessRegistry();
TERRAENGINE_API std::mutex& ComponentAccessRegistryMutex();

template <typename ComponentType>
void RegisterComponentAccess(const std::string& name)
{
	// Populate the access registry once per component type so callers can
	// query by name and run the appropriate thread-safe getter/setter.
	std::lock_guard lock(ComponentAccessRegistryMutex());
	auto& reg = ComponentAccessRegistry();
	if (reg.find(name) != reg.end())
	{
		return;
	}

	ComponentAccessFunctions functions;
	functions.getter = [](Object& object, std::string_view property) -> rttr::variant
	{
		// Fetch the concrete component by type-safe API, then take a shared
		// lock so concurrent reads remain safe while writers block.
		ComponentType* component = object.getComponent<ComponentType>();
		if (!component)
		{
			return {};
		}

		auto guard = component->acquireReadLock();
		rttr::property prop = rttr::type::get<ComponentType>().get_property(std::string(property).c_str());
		if (!prop.is_valid())
		{
			return {};
		}
		return prop.get_value(*component);
	};

	functions.setter = [](Object& object, std::string_view property, const rttr::variant& value) -> bool
	{
		// Promote to exclusive access before mutating reflected properties
		// to keep writer updates serialized relative to readers.
		ComponentType* component = object.getComponent<ComponentType>();
		if (!component)
		{
			return false;
		}

		auto guard = component->acquireWriteLock();
		rttr::property prop = rttr::type::get<ComponentType>().get_property(std::string(property).c_str());
		if (!prop.is_valid())
		{
			return false;
		}
		return prop.set_value(*component, value);
	};

	functions.hasComponent = [](Object& object) -> bool
	{
		return object.getComponent<ComponentType>() != nullptr;
	};

	functions.addComponent = [](Object& object) -> void
	{
		if (!object.getComponent<ComponentType>()) {
			object.addComponent<ComponentType>();
		}
	};

	functions.removeComponent = [](Object& object) -> void
	{
		object.removeComponent<ComponentType>();
	};

	functions.serialize = [name](Object& object) -> nlohmann::json
	{
		nlohmann::json compJson = nlohmann::json::object();
		ComponentType* component = object.getComponent<ComponentType>();
		if (!component) return compJson;

		auto guard = component->acquireReadLock();
		rttr::type type = rttr::type::get<ComponentType>();
		for (auto& prop : type.get_properties())
		{
			std::string propName = prop.get_name().to_string();
			rttr::variant val = prop.get_value(*component);

			if (val.is_type<float>()) compJson[propName] = val.get_value<float>();
			else if (val.is_type<int>()) compJson[propName] = val.get_value<int>();
			else if (val.is_type<bool>()) compJson[propName] = val.get_value<bool>();
			else if (val.is_type<std::string>()) compJson[propName] = val.get_value<std::string>();
			else if (val.is_type<glm::vec3>()) {
				auto v = val.get_value<glm::vec3>();
				compJson[propName] = { v.x, v.y, v.z };
			}
			else if (val.is_type<glm::quat>()) {
				auto q = val.get_value<glm::quat>();
				compJson[propName] = { q.w, q.x, q.y, q.z };
			}
			else if (val.is_type<TerraEngine::UUID>()) {
				auto uuid = val.get_value<TerraEngine::UUID>();
				compJson[propName] = TerraEngine::ToHexString(uuid);
			}
		}
		return compJson;
	};

	functions.deserialize = [](Object& object, const nlohmann::json& compData) -> void
	{
		ComponentType* component = object.getComponent<ComponentType>();
		if (!component) return;

		auto guard = component->acquireWriteLock();
		rttr::type type = rttr::type::get<ComponentType>();
		for (auto& [propName, propVal] : compData.items())
		{
			rttr::property prop = type.get_property(propName);
			if (prop.is_valid())
			{
				rttr::variant val;
				if (propVal.is_number_float()) val = propVal.get<float>();
				else if (propVal.is_number_integer()) val = propVal.get<int>();
				else if (propVal.is_boolean()) val = propVal.get<bool>();
				else if (propVal.is_string()) {
					if (prop.get_type() == rttr::type::get<TerraEngine::UUID>()) {
						auto parsed = TerraEngine::ParseUUID(propVal.get<std::string>());
						if (parsed) val = *parsed;
					}
					else {
						val = propVal.get<std::string>();
					}
				}
				else if (propVal.is_array()) {
					if (propVal.size() == 3) {
						val = glm::vec3(propVal[0], propVal[1], propVal[2]);
					}
					else if (propVal.size() == 4) {
						if (prop.get_type() == rttr::type::get<glm::quat>()) {
							val = glm::quat(propVal[0], propVal[1], propVal[2], propVal[3]);
						}
					}
				}

				if (val.is_valid()) {
					prop.set_value(*component, val);
				}
			}
		}
	};

	reg.emplace(name, std::move(functions));
}

// Direct component-based accessor that returns the requested property as a
// concrete type. On missing/invalid properties or type mismatch, this returns
// a value-initialized T.
template <typename ComponentType, typename T>
T GetComponentProperty(ComponentType& component, std::string_view propertyName)
{
	auto guard = component.acquireReadLock();
	rttr::property prop = rttr::type::get<ComponentType>().get_property(std::string(propertyName).c_str());
	if (!prop.is_valid())
	{
		return T{};
	}
	rttr::variant value = prop.get_value(component);
	if (!value.is_type<T>())
	{
		return T{};
	}
	return value.get_value<T>();
}

template <typename ComponentType, typename ValueType>
void SetComponentProperty(ComponentType& component, std::string_view propertyName, ValueType&& value)
{
	auto guard = component.acquireWriteLock();
	rttr::property prop = rttr::type::get<ComponentType>().get_property(std::string(propertyName).c_str());
	if (!prop.is_valid())
	{
		return;
	}
	rttr::variant wrappedValue(std::forward<ValueType>(value));
	prop.set_value(component, wrappedValue);
}

// Base component that all other components will inherit via CRTP.
template <typename Derived>
class Component
{
private:
	// Shared mutex allows many readers to proceed concurrently while writers
	// gain exclusive access when mutating component state. Each component
	// owns its own mutex so copies do not share lock state.
	mutable std::shared_mutex access_mutex_;
public:
	// Kept last to enable Tail Padding Optimization (EBO/TPO), potentially saving 8 bytes per component depending on the rest of the component layout.
	// Does not work on Windows due to different ABI rules.
	bool active = true;

	using ReadLock = std::shared_lock<std::shared_mutex>;
	using WriteLock = std::unique_lock<std::shared_mutex>;

	Component()
	{
		registerType();
	}

	Component(const Component& other)
		: active(other.active)
	{
		// The mutex is recreated so cloned components do not share locking
		// state, keeping per-instance read/write access independent.
		// Mutex is default constructed.
		(void)other;
		registerType();
	}

	Component(Component&&) noexcept = default;
	~Component() = default;

	Component& operator=(const Component& other)
	{
		if (this != &other)
		{
			// Assignments also reset the mutex so a fresh component copy does
			// not inherit lock contention from the source object.
			// Mutex is not copyable/assignable, but we don't want to copy the lock state anyway.
			// Since we can't re-assign the mutex member, we just leave it as is (unlocked).
			// If the current object was locked, this would be dangerous, but assignment assumes unique ownership or external sync.
			
			active = other.active;
			registerType();
		}
		(void)other;
		return *this;
	}

	Component& operator=(Component&&) noexcept = default;

	static void registerType()
	{
		ensureRegistration();
	}

	// Read-only operations should take this guard to allow concurrent access
	// while still blocking if any thread promotes to a write lock.
	[[nodiscard]] ReadLock acquireReadLock() const
	{
		return ReadLock(access_mutex_);
	}

	// Mutating operations should take this guard so writers have exclusive
	// access and readers are paused until updates are complete.
	[[nodiscard]] WriteLock acquireWriteLock() const
	{
		return WriteLock(access_mutex_);
	}

protected:
	// Ensures that the derived component registers itself with RTTR exactly
	// once, regardless of how many instances are constructed at runtime.
	static void ensureRegistration()
	{
		static bool registered = []() {
			const std::string name = component_name_for<Derived>();
			auto registration = rttr::registration::class_<Derived>(name.c_str())
				.template constructor<>()
				.property("active", &Derived::active);

			if constexpr (component_has_reflect_v<Derived>)
			{
				Derived::reflect(registration);
			}
			RegisterComponentAccess<Derived>(name);
			return true;
		}();
		(void)registered;
	}
};
