// Component.h — Base template and registration utilities for the engine's component system.
//
// This file defines the CRTP-based Component base class, providing automatic
// registration with the reflection system and type-safe data access. It also
// implements the infrastructure for runtime component manipulation, serialization,
// and thread-safe property access using EnTT and nlohmann/json.

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
// struct component_has_custom_name groups related state and operations so callers have one documented contract.
struct component_has_custom_name<T, std::void_t<decltype(T::kComponentName)>> : std::true_type
{
};

template <typename T, typename = void>
// struct component_has_reflect groups related state and operations so callers have one documented contract.
struct component_has_reflect : std::false_type
{
};

template <typename T>
// struct component_has_reflect groups related state and operations so callers have one documented contract.
struct component_has_reflect<T, std::void_t<decltype(T::reflect(std::declval<entt::meta_factory<T>&>()))>> : std::true_type
{
};

template <typename T>
inline constexpr bool component_has_reflect_v = component_has_reflect<T>::value;

template <typename T>
// component_name_for is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
	std::function<TerraEngine::Reflection::Variant(entt::registry&, entt::entity, std::string_view)> entityGetter;
	std::function<bool(entt::registry&, entt::entity, std::string_view, const TerraEngine::Reflection::Variant&)> entitySetter;
	std::function<bool(entt::registry&, entt::entity)> entityHasComponent;
	std::function<void(entt::registry&, entt::entity)> entityAddComponent;
	std::function<void(entt::registry&, entt::entity)> entityRemoveComponent;
	std::function<nlohmann::json(entt::registry&, entt::entity)> entitySerialize;
	std::function<void(entt::registry&, entt::entity, const nlohmann::json&)> entityDeserialize;
};

struct ComponentAccessObservation
{
	bool registered = false;
	bool hasComponent = false;
	std::string componentName;
	std::uint64_t componentGeneration = 0;
};

TERRAENGINE_API std::unordered_map<std::string, ComponentAccessFunctions>& ComponentAccessRegistry();
TERRAENGINE_API std::mutex& ComponentAccessRegistryMutex();
TERRAENGINE_API entt::registry& ComponentAccessObjectRegistry();
TERRAENGINE_API ComponentAccessObservation ObserveRegisteredComponentAccess(entt::registry& registry, entt::entity entity, std::string_view componentName, std::uint64_t componentGeneration);

template <typename ComponentType>
// RegisterComponentAccess is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
	functions.entityGetter = [](entt::registry& registry, entt::entity entity, std::string_view property) -> TerraEngine::Reflection::Variant
	{
		// Fetch the concrete component by type-safe API, then take a shared
		// lock so concurrent reads remain safe while writers block.
		ComponentType* component = registry.valid(entity) ? registry.try_get<ComponentType>(entity) : nullptr;
		if (!component)
		{
			return {};
		}

		auto guard = component->acquireReadLock();
		TerraEngine::Reflection::Property prop = TerraEngine::Reflection::FindProperty(TerraEngine::Reflection::ResolveType<ComponentType>(), property);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TerraEngine::Reflection::IsValid(prop))
		{
			return {};
		}
		return TerraEngine::Reflection::GetPropertyValue(prop, *component);
	};

	functions.entitySetter = [](entt::registry& registry, entt::entity entity, std::string_view property, const TerraEngine::Reflection::Variant& value) -> bool
	{
		// Promote to exclusive access before mutating reflected properties
		// to keep writer updates serialized relative to readers.
		ComponentType* component = registry.valid(entity) ? registry.try_get<ComponentType>(entity) : nullptr;
		if (!component)
		{
			return false;
		}

		auto guard = component->acquireWriteLock();
		TerraEngine::Reflection::Property prop = TerraEngine::Reflection::FindProperty(TerraEngine::Reflection::ResolveType<ComponentType>(), property);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TerraEngine::Reflection::IsValid(prop))
		{
			return false;
		}
		return TerraEngine::Reflection::SetPropertyValue(prop, *component, value);
	};

	functions.entityHasComponent = [](entt::registry& registry, entt::entity entity) -> bool
	{
		return registry.valid(entity) && registry.any_of<ComponentType>(entity);
	};

	functions.entityAddComponent = [](entt::registry& registry, entt::entity entity) -> void
	{
		if (!registry.valid(entity) || registry.any_of<ComponentType>(entity)) {
			return;
		}
		registry.emplace<ComponentType>(entity);
	};

	functions.entityRemoveComponent = [](entt::registry& registry, entt::entity entity) -> void
	{
		if (registry.valid(entity) && registry.any_of<ComponentType>(entity)) {
			registry.remove<ComponentType>(entity);
		}
	};

	functions.entitySerialize = [name](entt::registry& registry, entt::entity entity) -> nlohmann::json
	{
		nlohmann::json compJson = nlohmann::json::object();
		ComponentType* component = registry.valid(entity) ? registry.try_get<ComponentType>(entity) : nullptr;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!component) return compJson;

		auto guard = component->acquireReadLock();
		TerraEngine::Reflection::Type type = TerraEngine::Reflection::ResolveType<ComponentType>();
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const TerraEngine::Reflection::Property& prop : TerraEngine::Reflection::GetProperties(type))
		{
			std::string propName = TerraEngine::Reflection::GetPropertyName(prop);
			TerraEngine::Reflection::Variant val = TerraEngine::Reflection::GetPropertyValue(prop, *component);

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (TerraEngine::Reflection::IsType<float>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<float>(val);
			else if (TerraEngine::Reflection::IsType<int>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<int>(val);
			else if (TerraEngine::Reflection::IsType<bool>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<bool>(val);
			else if (TerraEngine::Reflection::IsType<std::string>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<std::string>(val);
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (TerraEngine::Reflection::IsType<glm::vec3>(val)) {
				auto v = TerraEngine::Reflection::GetValue<glm::vec3>(val);
				compJson[propName] = { v.x, v.y, v.z };
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (TerraEngine::Reflection::IsType<glm::quat>(val)) {
				auto q = TerraEngine::Reflection::GetValue<glm::quat>(val);
				compJson[propName] = { q.w, q.x, q.y, q.z };
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (TerraEngine::Reflection::IsType<TerraEngine::UUID>(val)) {
				auto uuid = TerraEngine::Reflection::GetValue<TerraEngine::UUID>(val);
				compJson[propName] = TerraEngine::ToHexString(uuid);
			}
		}
		return compJson;
	};

	functions.entityDeserialize = [](entt::registry& registry, entt::entity entity, const nlohmann::json& compData) -> void
	{
		ComponentType* component = registry.valid(entity) ? registry.try_get<ComponentType>(entity) : nullptr;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!component) return;

		auto guard = component->acquireWriteLock();
		TerraEngine::Reflection::Type type = TerraEngine::Reflection::ResolveType<ComponentType>();
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto& [propName, propVal] : compData.items())
		{
			TerraEngine::Reflection::Property prop = TerraEngine::Reflection::FindProperty(type, propName);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (TerraEngine::Reflection::IsValid(prop))
			{
				TerraEngine::Reflection::Variant val;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (propVal.is_number_float()) val = propVal.get<float>();
				else if (propVal.is_number_integer()) val = propVal.get<int>();
				else if (propVal.is_boolean()) val = propVal.get<bool>();
				// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
				else if (propVal.is_string()) {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (TerraEngine::Reflection::IsType<TerraEngine::UUID>(TerraEngine::Reflection::GetPropertyType(prop))) {
						auto parsed = TerraEngine::ParseUUID(propVal.get<std::string>());
						if (parsed) val = *parsed;
					}
					else {
						val = propVal.get<std::string>();
					}
				}
				// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
				else if (propVal.is_array()) {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (propVal.size() == 3) {
						val = glm::vec3(propVal[0], propVal[1], propVal[2]);
					}
					// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
					else if (propVal.size() == 4) {
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (TerraEngine::Reflection::IsType<glm::quat>(TerraEngine::Reflection::GetPropertyType(prop))) {
							val = glm::quat(propVal[0], propVal[1], propVal[2], propVal[3]);
						}
					}
				}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (TerraEngine::Reflection::IsValid(val)) {
					TerraEngine::Reflection::SetPropertyValue(prop, *component, val);
				}
			}
		}
	};

	auto entityGetter = functions.entityGetter;
	functions.getter = [entityGetter](Object& object, std::string_view property) -> TerraEngine::Reflection::Variant
	{
		return entityGetter(ComponentAccessObjectRegistry(), object.EntityId(), property);
	};

	auto entitySetter = functions.entitySetter;
	functions.setter = [entitySetter](Object& object, std::string_view property, const TerraEngine::Reflection::Variant& value) -> bool
	{
		return entitySetter(ComponentAccessObjectRegistry(), object.EntityId(), property, value);
	};

	auto entityHasComponent = functions.entityHasComponent;
	functions.hasComponent = [entityHasComponent](Object& object) -> bool
	{
		return entityHasComponent(ComponentAccessObjectRegistry(), object.EntityId());
	};

	auto entityAddComponent = functions.entityAddComponent;
	functions.addComponent = [entityAddComponent](Object& object) -> void
	{
		entityAddComponent(ComponentAccessObjectRegistry(), object.EntityId());
	};

	auto entityRemoveComponent = functions.entityRemoveComponent;
	functions.removeComponent = [entityRemoveComponent](Object& object) -> void
	{
		entityRemoveComponent(ComponentAccessObjectRegistry(), object.EntityId());
	};

	auto entitySerialize = functions.entitySerialize;
	functions.serialize = [entitySerialize](Object& object) -> nlohmann::json
	{
		return entitySerialize(ComponentAccessObjectRegistry(), object.EntityId());
	};

	auto entityDeserialize = functions.entityDeserialize;
	functions.deserialize = [entityDeserialize](Object& object, const nlohmann::json& compData) -> void
	{
		entityDeserialize(ComponentAccessObjectRegistry(), object.EntityId(), compData);
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!TerraEngine::Reflection::IsValid(prop))
	{
		return T{};
	}
	TerraEngine::Reflection::Variant value = TerraEngine::Reflection::GetPropertyValue(prop, component);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!TerraEngine::Reflection::IsType<T>(value))
	{
		return T{};
	}
	return TerraEngine::Reflection::GetValue<T>(value);
}

template <typename ComponentType, typename ValueType>
// SetComponentProperty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SetComponentProperty(ComponentType& component, std::string_view propertyName, ValueType&& value)
{
	auto guard = component.acquireWriteLock();
	TerraEngine::Reflection::Property prop = TerraEngine::Reflection::FindProperty(TerraEngine::Reflection::ResolveType<ComponentType>(), propertyName);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!TerraEngine::Reflection::IsValid(prop))
	{
		return;
	}
	TerraEngine::Reflection::Variant wrappedValue(std::forward<ValueType>(value));
	TerraEngine::Reflection::SetPropertyValue(prop, component, wrappedValue);
}

template <typename ComponentType>
bool IsRuntimeComponentActive(const ComponentType& component)
{
	// Hot runtime membership only needs the component enable bit. Keeping this
	// helper typed and tiny lets ObjectManager scan compact entity ids first,
	// then reject disabled hot components without touching reflection metadata.
	if constexpr (requires { component.active; })
	{
		return component.active;
	}
	else
	{
		return true;
	}
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
		// active is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

	// registerType is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

	// Scheduler-approved hot loops may access the concrete component directly
	// after a ComponentAccessGrant proves no unordered writer can race this
	// workload. The helper is intentionally noisy so callers document that
	// safety comes from the scheduler contract, not from this component mutex.
	Derived& schedulerApprovedUnsafeAccess()
	{
		return static_cast<Derived&>(*this);
	}

	const Derived& schedulerApprovedUnsafeAccess() const
	{
		return static_cast<const Derived&>(*this);
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

			// constexpr is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
