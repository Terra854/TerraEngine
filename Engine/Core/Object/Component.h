#pragma once

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
#include "Reflection.h"
#include "Object.h"
#include "Utilities/UUID.h"

// Helper utilities that keep the CRTP component base lightweight while still
// allowing each component to expose metadata through the shared reflection
// backend in a consistent way.
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
struct component_has_reflect<T, std::void_t<decltype(T::reflect(std::declval<entt::meta_factory<T>&>()))>> : std::true_type
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
		return std::string(entt::type_name<T>::value());
	}
}

// Type-erased property hooks that allow runtime access to any component
// registered through the Component CRTP without exposing the concrete type.
struct ComponentAccessFunctions
{
	// Store the reflected type from the engine module so editor code can query
	// properties against the same EnTT meta context that performed registration.
	TerraEngine::Reflection::Type reflectionType;
	std::function<TerraEngine::Reflection::Variant(Object&, std::string_view)> getter;
	std::function<bool(Object&, std::string_view, const TerraEngine::Reflection::Variant&)> setter;
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
	functions.reflectionType = TerraEngine::Reflection::ResolveType<ComponentType>();
	functions.getter = [](Object& object, std::string_view property) -> TerraEngine::Reflection::Variant
	{
		// Fetch the concrete component by type-safe API, then take a shared
		// lock so concurrent reads remain safe while writers block.
		ComponentType* component = object.getComponent<ComponentType>();
		if (!component)
		{
			return {};
		}

		auto guard = component->acquireReadLock();
		TerraEngine::Reflection::Property prop = TerraEngine::Reflection::FindProperty(TerraEngine::Reflection::ResolveType<ComponentType>(), property);
		if (!TerraEngine::Reflection::IsValid(prop))
		{
			return {};
		}
		return TerraEngine::Reflection::GetPropertyValue(prop, *component);
	};

	functions.setter = [](Object& object, std::string_view property, const TerraEngine::Reflection::Variant& value) -> bool
	{
		// Promote to exclusive access before mutating reflected properties
		// to keep writer updates serialized relative to readers.
		ComponentType* component = object.getComponent<ComponentType>();
		if (!component)
		{
			return false;
		}

		auto guard = component->acquireWriteLock();
		TerraEngine::Reflection::Property prop = TerraEngine::Reflection::FindProperty(TerraEngine::Reflection::ResolveType<ComponentType>(), property);
		if (!TerraEngine::Reflection::IsValid(prop))
		{
			return false;
		}
		return TerraEngine::Reflection::SetPropertyValue(prop, *component, value);
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
		TerraEngine::Reflection::Type type = TerraEngine::Reflection::ResolveType<ComponentType>();
		for (const TerraEngine::Reflection::Property& prop : TerraEngine::Reflection::GetProperties(type))
		{
			std::string propName = TerraEngine::Reflection::GetPropertyName(prop);
			TerraEngine::Reflection::Variant val = TerraEngine::Reflection::GetPropertyValue(prop, *component);

			if (TerraEngine::Reflection::IsType<float>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<float>(val);
			else if (TerraEngine::Reflection::IsType<int>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<int>(val);
			else if (TerraEngine::Reflection::IsType<bool>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<bool>(val);
			else if (TerraEngine::Reflection::IsType<std::string>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<std::string>(val);
			else if (TerraEngine::Reflection::IsType<glm::vec3>(val)) {
				auto v = TerraEngine::Reflection::GetValue<glm::vec3>(val);
				compJson[propName] = { v.x, v.y, v.z };
			}
			else if (TerraEngine::Reflection::IsType<glm::quat>(val)) {
				auto q = TerraEngine::Reflection::GetValue<glm::quat>(val);
				compJson[propName] = { q.w, q.x, q.y, q.z };
			}
			else if (TerraEngine::Reflection::IsType<TerraEngine::UUID>(val)) {
				auto uuid = TerraEngine::Reflection::GetValue<TerraEngine::UUID>(val);
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
		TerraEngine::Reflection::Type type = TerraEngine::Reflection::ResolveType<ComponentType>();
		for (auto& [propName, propVal] : compData.items())
		{
			TerraEngine::Reflection::Property prop = TerraEngine::Reflection::FindProperty(type, propName);
			if (TerraEngine::Reflection::IsValid(prop))
			{
				TerraEngine::Reflection::Variant val;
				if (propVal.is_number_float()) val = propVal.get<float>();
				else if (propVal.is_number_integer()) val = propVal.get<int>();
				else if (propVal.is_boolean()) val = propVal.get<bool>();
				else if (propVal.is_string()) {
					if (TerraEngine::Reflection::IsType<TerraEngine::UUID>(TerraEngine::Reflection::GetPropertyType(prop))) {
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
						if (TerraEngine::Reflection::IsType<glm::quat>(TerraEngine::Reflection::GetPropertyType(prop))) {
							val = glm::quat(propVal[0], propVal[1], propVal[2], propVal[3]);
						}
					}
				}

				if (TerraEngine::Reflection::IsValid(val)) {
					TerraEngine::Reflection::SetPropertyValue(prop, *component, val);
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
	TerraEngine::Reflection::Property prop = TerraEngine::Reflection::FindProperty(TerraEngine::Reflection::ResolveType<ComponentType>(), propertyName);
	if (!TerraEngine::Reflection::IsValid(prop))
	{
		return T{};
	}
	TerraEngine::Reflection::Variant value = TerraEngine::Reflection::GetPropertyValue(prop, component);
	if (!TerraEngine::Reflection::IsType<T>(value))
	{
		return T{};
	}
	return TerraEngine::Reflection::GetValue<T>(value);
}

template <typename ComponentType, typename ValueType>
void SetComponentProperty(ComponentType& component, std::string_view propertyName, ValueType&& value)
{
	auto guard = component.acquireWriteLock();
	TerraEngine::Reflection::Property prop = TerraEngine::Reflection::FindProperty(TerraEngine::Reflection::ResolveType<ComponentType>(), propertyName);
	if (!TerraEngine::Reflection::IsValid(prop))
	{
		return;
	}
	TerraEngine::Reflection::Variant wrappedValue(std::forward<ValueType>(value));
	TerraEngine::Reflection::SetPropertyValue(prop, component, wrappedValue);
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
	// Ensures that the derived component registers itself with the EnTT-backed
	// reflection registry exactly
	// once, regardless of how many instances are constructed at runtime.
	static void ensureRegistration()
	{
		static bool registered = []() {
			const std::string name = component_name_for<Derived>();
			entt::meta_factory<Derived> factory{};
			TerraEngine::Reflection::ConfigureTypeMetadata(factory, name);
			factory.template ctor<>();
			TerraEngine::Reflection::RegisterDataProperty<&Derived::active>(factory, "active");

			if constexpr (component_has_reflect_v<Derived>)
			{
				Derived::reflect(factory);
			}
			RegisterComponentAccess<Derived>(name);
			return true;
		}();
		(void)registered;
	}
};
