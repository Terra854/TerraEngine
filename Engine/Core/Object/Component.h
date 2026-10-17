// Component.h — Base template and registration utilities for the engine's component system.
//
// This file defines the CRTP-based Component base class, providing automatic
// registration with the reflection system and type-safe data access. It also
// implements the infrastructure for runtime component manipulation, serialization,
// and thread-safe property access using EnTT and nlohmann/json.

#pragma once

#include <memory>
#include <cmath>
#include <cstdint>
#include <limits>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <unordered_map>
#include <vector>
#include <functional>
#include <concepts>
#include <string_view>
#include <mutex>
#include <nlohmann/json.hpp>
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>
#include "Core/Export.h"
#include "Reflection.h"
#include "Object.h"
#include "Utilities/UUID.h"

// Eligibility names concrete audited types, never a user-controlled registry
// string. Third-party components must explicitly attest to side-effect safety.
class Transform;
class ScriptComponent;
class UIComponent;
struct AnimatorComponent;
struct NavigationAgentComponent;
struct CharacterLocomotionComponent;
struct RigidBodyComponent;
struct ColliderSetComponent;
struct ConstraintSetComponent;
struct CharacterControllerComponent;
struct AudioSourceComponent;
struct AudioListenerComponent;
struct CameraComponent;
struct MeshRenderComponent;
struct LightComponent;
struct SkyComponent;
struct PrefabLinkComponent;
struct TerrainComponent;

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

// Engine components declare automation without depending on editor headers.
// The editor registry translates these typed engine enums into its transport
// descriptors when the owning subsystem elects to publish the declarations.
enum class ComponentAutomationPermission
{
	Read,
	ProjectWrite,
	GlobalWrite
};

enum class ComponentAutomationApproval
{
	None,
	Destructive,
	HighImpact
};

enum class ComponentAutomationMutation
{
	ReadOnly,
	Project,
	Scene,
	Asset,
	Destructive
};

struct ComponentAutomationDeclaration
{
	std::string name;
	std::string category;
	std::string description;
	nlohmann::json inputSchema = {{"type", "object"}};
	nlohmann::json resultSchema = {{"type", "object"}};
	ComponentAutomationPermission permission = ComponentAutomationPermission::Read;
	ComponentAutomationApproval approval = ComponentAutomationApproval::None;
	ComponentAutomationMutation mutation = ComponentAutomationMutation::ReadOnly;
	bool requiresProject = false;
	bool requiresScene = false;
	bool requiresRenderer = false;
	bool workerPreparation = true;
	bool protectedCommit = false;
	std::string visibilityBoundary = "immutable_snapshot";
	std::function<nlohmann::json(const nlohmann::json&)> handler;
};

template <typename T, typename = void>
struct component_has_automation_declarations : std::false_type
{
};

template <typename T>
struct component_has_automation_declarations<T, std::void_t<decltype(T::automation())>> : std::true_type
{
};

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
// Prepared values own concrete component copies, not JSON or live pointers.
// Preparation may throw; restoration assigns into retained storage without
// calling reflected setters, registering types, or allocating new state.
struct PreparedComponentValue {
	virtual ~PreparedComponentValue() = default;
	virtual void Apply(entt::registry& registry, entt::entity entity) = 0;
	virtual bool Restore(entt::registry& registry, entt::entity entity) noexcept = 0;
	virtual std::shared_ptr<PreparedComponentValue> Copy() const = 0;
};

template<typename T>
struct TypedPreparedComponentValue final : PreparedComponentValue {
	T value;
	explicit TypedPreparedComponentValue(const T& source) : value(source) {}
	std::shared_ptr<PreparedComponentValue> Copy() const override {
		return std::make_shared<TypedPreparedComponentValue<T>>(value);
	}
	void Apply(entt::registry& registry, entt::entity entity) override {
		if (!registry.any_of<T>(entity)) registry.emplace<T>(entity);
		registry.get<T>(entity) = std::move(value);
	}
	bool Restore(entt::registry& registry, entt::entity entity) noexcept override {
		static_assert(std::is_nothrow_move_assignable_v<T>);
		T* destination = registry.valid(entity) ? registry.try_get<T>(entity) : nullptr;
		if (!destination) return false;
		*destination = std::move(value);
		return true;
	}
};

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
	// Only audited authored components, or explicit custom opt-ins, receive
	// these hooks. Copy/move traits alone do not prove setter side-effect safety.
	std::function<std::shared_ptr<PreparedComponentValue>(entt::registry&, entt::entity)> capturePrepared;
	std::function<std::shared_ptr<PreparedComponentValue>(const nlohmann::json&, std::string&)> decodePrepared;
	std::function<std::shared_ptr<PreparedComponentValue>(entt::registry&, entt::entity,
		std::string_view, const TerraEngine::Reflection::Variant&, std::string&)> prepareProperty;
	// Ordered preparation feeds the preceding detached value to each setter,
	// so several edits preserve each other without writing any live component.
	std::function<std::shared_ptr<PreparedComponentValue>(const PreparedComponentValue&,
		std::string_view, const TerraEngine::Reflection::Variant&, std::string&)> prepareDetachedProperty;
	std::function<std::shared_ptr<PreparedComponentValue>(const PreparedComponentValue&,
		std::string_view, std::string&)> prepareDetachedScriptBinding;
	// Declarations are copied during component type registration. The editor can
	// publish them later without naming this component in a central dispatcher.
	std::vector<ComponentAutomationDeclaration> automationDeclarations;
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
// Reflected property setters are shared by the inspector, automation, managed
// scripting, and compatibility mutation paths. Spatial components need one
// centralized notification so render and hierarchy snapshots never keep an old
// transform after any of those callers successfully writes a value.
TERRAENGINE_API void NotifyReflectedComponentPropertyMutation(
	entt::registry& registry,
	entt::entity entity,
	std::string_view componentName);

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
	constexpr bool auditedBuiltin = std::is_same_v<ComponentType, Transform> || std::is_same_v<ComponentType, AnimatorComponent>
		|| std::is_same_v<ComponentType, NavigationAgentComponent> || std::is_same_v<ComponentType, CharacterLocomotionComponent>
		|| std::is_same_v<ComponentType, RigidBodyComponent> || std::is_same_v<ComponentType, ColliderSetComponent>
		|| std::is_same_v<ComponentType, ConstraintSetComponent> || std::is_same_v<ComponentType, CharacterControllerComponent>
		|| std::is_same_v<ComponentType, AudioSourceComponent> || std::is_same_v<ComponentType, AudioListenerComponent>
		|| std::is_same_v<ComponentType, CameraComponent> || std::is_same_v<ComponentType, MeshRenderComponent>
		|| std::is_same_v<ComponentType, LightComponent> || std::is_same_v<ComponentType, SkyComponent>
		|| std::is_same_v<ComponentType, ScriptComponent> || std::is_same_v<ComponentType, PrefabLinkComponent>
		|| std::is_same_v<ComponentType, TerrainComponent> || std::is_same_v<ComponentType, UIComponent>;
	static_assert(!auditedBuiltin || (std::is_copy_constructible_v<ComponentType>
		&& std::is_nothrow_move_constructible_v<ComponentType> && std::is_nothrow_move_assignable_v<ComponentType>
		&& std::is_nothrow_destructible_v<ComponentType>),
		"Audited built-in authoring must retain a copy and restore/remove without throwing.");
	bool optedIn = auditedBuiltin;
	if constexpr (requires { ComponentType::kTransactionalAuthoringSafe; }) {
		optedIn = optedIn || ComponentType::kTransactionalAuthoringSafe;
	}
	// Retained values can be moved back and newly added values can be removed
	// during abort without invoking a fallible component destructor.
	if constexpr (std::is_copy_constructible_v<ComponentType> && std::is_nothrow_move_constructible_v<ComponentType>
		&& std::is_nothrow_move_assignable_v<ComponentType>
		&& std::is_nothrow_destructible_v<ComponentType>) {
		if (optedIn) {
			functions.capturePrepared = [](entt::registry& registry, entt::entity entity) {
				const auto& source = registry.get<ComponentType>(entity);
				auto lock = source.acquireReadLock();
				return std::make_shared<TypedPreparedComponentValue<ComponentType>>(source);
			};
			functions.decodePrepared = [](const nlohmann::json& json, std::string& reason)
				-> std::shared_ptr<PreparedComponentValue> {
				if (!json.is_object()) { reason = "Authored component must be an object."; return {}; }
				ComponentType candidate;
				if constexpr (requires { ComponentType::DeserializeAuthored(candidate, json); }) {
					if (!ComponentType::DeserializeAuthored(candidate, json)) {
						reason = "Invalid custom authored component payload."; return {};
					}
				} else {
					const auto type = TerraEngine::Reflection::ResolveType<ComponentType>();
					for (const auto& [key, source] : json.items()) {
						const auto prop = TerraEngine::Reflection::FindProperty(type, key);
						if (!TerraEngine::Reflection::IsValid(prop)) {
							reason = "Unknown authored component property: " + key; return {};
						}
						const auto expected = TerraEngine::Reflection::GetPropertyType(prop);
						TerraEngine::Reflection::Variant value;
						if (TerraEngine::Reflection::IsType<float>(expected) && source.is_number()) value = source.get<float>();
						else if (TerraEngine::Reflection::IsType<int>(expected) && source.is_number_integer()
							&& source.get<double>() >= std::numeric_limits<int>::min() && source.get<double>() <= std::numeric_limits<int>::max()) value = source.get<int>();
						else if (TerraEngine::Reflection::IsType<std::uint32_t>(expected) && source.is_number_integer()
							&& source.get<double>() >= 0.0 && source.get<double>() <= std::numeric_limits<std::uint32_t>::max()) value = source.get<std::uint32_t>();
						else if (TerraEngine::Reflection::IsType<bool>(expected) && source.is_boolean()) value = source.get<bool>();
						else if (TerraEngine::Reflection::IsType<std::string>(expected) && source.is_string()) value = source.get<std::string>();
						else if (TerraEngine::Reflection::IsType<TerraEngine::UUID>(expected) && source.is_string()) {
							const auto id = TerraEngine::ParseUUID(source.get<std::string>()); if (id) value = *id;
						} else if (TerraEngine::Reflection::IsType<glm::vec3>(expected) && source.is_array() && source.size() == 3
							&& source[0].is_number() && source[1].is_number() && source[2].is_number()) {
							value = glm::vec3(source[0].get<float>(), source[1].get<float>(), source[2].get<float>());
						} else if (TerraEngine::Reflection::IsType<glm::quat>(expected) && source.is_array() && source.size() == 4
							&& source[0].is_number() && source[1].is_number() && source[2].is_number() && source[3].is_number()) {
							value = glm::quat(source[0].get<float>(), source[1].get<float>(), source[2].get<float>(), source[3].get<float>());
						}
						if (TerraEngine::Reflection::IsType<float>(value) && !std::isfinite(TerraEngine::Reflection::GetValue<float>(value))) {
							reason = "Nonfinite authored numeric property: " + key; return {};
						}
						if constexpr (std::is_same_v<ComponentType, ScriptComponent>) {
							if (key == "BindingsJson") {
								if (!source.is_string() || !nlohmann::json::parse(source.get<std::string>()).is_array()) {
									reason = "Script bindings require a serialized array."; return {};
								}
							}
						}
						if (!TerraEngine::Reflection::IsValid(value) || !TerraEngine::Reflection::SetPropertyValue(prop, candidate, value)) {
							reason = "Invalid authored property value: " + key; return {};
						}
						if constexpr (std::is_same_v<ComponentType, ScriptComponent>) {
							if (key == "BindingsJson" && candidate.GetBindings().size() != nlohmann::json::parse(source.get<std::string>()).size()) {
								reason = "Malformed authored script binding record."; return {};
							}
						}
					}
				}
				return std::make_shared<TypedPreparedComponentValue<ComponentType>>(candidate);
			};
			functions.prepareDetachedProperty = [](const PreparedComponentValue& previous,
				std::string_view property, const TerraEngine::Reflection::Variant& incoming, std::string& reason)
				-> std::shared_ptr<PreparedComponentValue> {
				const auto prop = TerraEngine::Reflection::FindProperty(
					TerraEngine::Reflection::ResolveType<ComponentType>(), property);
				// Use the same checked reflection conversion as transaction preflight,
				// preserving existing typed numeric-property compatibility.
				auto converted = TerraEngine::Reflection::IsValid(prop)
					? incoming.allow_cast(TerraEngine::Reflection::GetPropertyType(prop)) : TerraEngine::Reflection::Variant{};
				if (!TerraEngine::Reflection::IsValid(prop) || !TerraEngine::Reflection::IsValid(converted)) {
					reason = "Unknown property or mismatched reflected value type.";
					return {};
				}
				if ((TerraEngine::Reflection::IsType<float>(converted) && !std::isfinite(TerraEngine::Reflection::GetValue<float>(converted)))
					|| (TerraEngine::Reflection::IsType<double>(converted) && !std::isfinite(TerraEngine::Reflection::GetValue<double>(converted)))) {
					reason = "Authored property contains a nonfinite numeric value."; return {};
				}
				if constexpr (std::is_same_v<ComponentType, ScriptComponent>) {
					if (property == "BindingsJson") {
						if (!TerraEngine::Reflection::IsType<std::string>(converted)
							|| !nlohmann::json::parse(TerraEngine::Reflection::GetValue<std::string>(converted)).is_array()) {
							reason = "Script bindings require a serialized authored array."; return {};
						}
					}
				}
				const auto* source = dynamic_cast<const TypedPreparedComponentValue<ComponentType>*>(&previous);
				if (!source) { reason = "Detached component type does not match its authoring hook."; return {}; }
				auto prepared = std::make_shared<TypedPreparedComponentValue<ComponentType>>(source->value);
				if (!TerraEngine::Reflection::SetPropertyValue(prop, prepared->value, converted)) {
					reason = "Detached authored setter rejected its value.";
					return {};
				}
				if constexpr (std::is_same_v<ComponentType, ScriptComponent>) {
					if (property == "BindingsJson" && prepared->value.GetBindings().size()
						!= nlohmann::json::parse(TerraEngine::Reflection::GetValue<std::string>(converted)).size()) {
						reason = "Malformed authored script binding record."; return {};
					}
				}
				return prepared;
			};
			functions.prepareProperty = [capture = functions.capturePrepared, prepare = functions.prepareDetachedProperty](
				entt::registry& registry, entt::entity entity, std::string_view property,
				const TerraEngine::Reflection::Variant& incoming, std::string& reason) {
				auto previous = capture(registry, entity);
				return prepare(*previous, property, incoming, reason);
			};
			if constexpr (std::is_same_v<ComponentType, ScriptComponent>) {
				functions.prepareDetachedScriptBinding = [](const PreparedComponentValue& previous,
					std::string_view className, std::string& reason) -> std::shared_ptr<PreparedComponentValue> {
					const auto* source = dynamic_cast<const TypedPreparedComponentValue<ComponentType>*>(&previous);
					if (!source || className.empty()) { reason = "Script binding requires a safe Script candidate and nonempty class name."; return {}; }
					auto prepared = std::make_shared<TypedPreparedComponentValue<ComponentType>>(source->value);
					if (!prepared->value.HasBinding(className)) prepared->value.AddBinding(TerraEngine::EMPTY_UUID, className);
					return prepared;
				};
			}
		}
	}
	if constexpr (component_has_automation_declarations<ComponentType>::value)
	{
		functions.automationDeclarations = ComponentType::automation();
	}
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

		if (!TerraEngine::Reflection::IsValid(prop))
		{
			return {};
		}
		return TerraEngine::Reflection::GetPropertyValue(prop, *component);
	};

	functions.entitySetter = [name](entt::registry& registry, entt::entity entity, std::string_view property, const TerraEngine::Reflection::Variant& value) -> bool
	{
		// Promote to exclusive access before mutating reflected properties
		// to keep writer updates serialized relative to readers. Release that
		// component lock before publishing the spatial revision because the
		// revision path intentionally coordinates with other scene snapshots.
		ComponentType* component = registry.valid(entity) ? registry.try_get<ComponentType>(entity) : nullptr;
		if (!component)
		{
			return false;
		}

		{
			auto guard = component->acquireWriteLock();
			TerraEngine::Reflection::Property prop = TerraEngine::Reflection::FindProperty(TerraEngine::Reflection::ResolveType<ComponentType>(), property);

			if (!TerraEngine::Reflection::IsValid(prop)
				|| !TerraEngine::Reflection::SetPropertyValue(prop, *component, value))
			{
				return false;
			}
		}

		NotifyReflectedComponentPropertyMutation(registry, entity, name);
		return true;
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

		if (!component) return compJson;
		// Custom authored snapshots obey the same component lock as reflection.
		auto guard = component->acquireReadLock();
		if constexpr (requires(const ComponentType& value) {
			{ ComponentType::SerializeAuthored(value) } -> std::same_as<nlohmann::json>;
		}) {
			return ComponentType::SerializeAuthored(*component);
		}

		TerraEngine::Reflection::Type type = TerraEngine::Reflection::ResolveType<ComponentType>();

		for (const TerraEngine::Reflection::Property& prop : TerraEngine::Reflection::GetProperties(type))
		{
			std::string propName = TerraEngine::Reflection::GetPropertyName(prop);
			TerraEngine::Reflection::Variant val = TerraEngine::Reflection::GetPropertyValue(prop, *component);


			if (TerraEngine::Reflection::IsType<float>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<float>(val);
			else if (TerraEngine::Reflection::IsType<int>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<int>(val);
			else if (TerraEngine::Reflection::IsType<std::uint32_t>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<std::uint32_t>(val);
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

	functions.entityDeserialize = [](entt::registry& registry, entt::entity entity, const nlohmann::json& compData) -> void
	{
		ComponentType* component = registry.valid(entity) ? registry.try_get<ComponentType>(entity) : nullptr;

		if (!component) return;
		// Hold exclusive ownership through staged custom validation and assignment.
		auto guard = component->acquireWriteLock();
		if constexpr (requires(ComponentType& value, const nlohmann::json& json) {
			{ ComponentType::DeserializeAuthored(value, json) } -> std::convertible_to<bool>;
		}) {
			(void)ComponentType::DeserializeAuthored(*component, compData);
			return;
		}

		TerraEngine::Reflection::Type type = TerraEngine::Reflection::ResolveType<ComponentType>();

		for (auto& [propName, propVal] : compData.items())
		{
			TerraEngine::Reflection::Property prop = TerraEngine::Reflection::FindProperty(type, propName);

			if (TerraEngine::Reflection::IsValid(prop))
			{
				TerraEngine::Reflection::Variant val;

				const TerraEngine::Reflection::Type propertyType =
					TerraEngine::Reflection::GetPropertyType(prop);
				if (propVal.is_number_float()) val = propVal.get<float>();
				else if (propVal.is_number_unsigned()
					&& TerraEngine::Reflection::IsType<std::uint32_t>(propertyType)) {
					const std::uint64_t unsignedValue = propVal.get<std::uint64_t>();
					if (unsignedValue <= std::numeric_limits<std::uint32_t>::max()) {
						val = static_cast<std::uint32_t>(unsignedValue);
					}
				}
				else if (propVal.is_number_integer()
					&& TerraEngine::Reflection::IsType<std::uint32_t>(propertyType)) {
					const std::int64_t signedValue = propVal.get<std::int64_t>();
					if (signedValue >= 0
						&& static_cast<std::uint64_t>(signedValue) <= std::numeric_limits<std::uint32_t>::max()) {
						val = static_cast<std::uint32_t>(signedValue);
					}
				}
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

		: active(other.active)
	{
		// The mutex is recreated so cloned components do not share locking
		// state, keeping per-instance read/write access independent.
		// Mutex is default constructed.
		(void)other;
		registerType();
	}

	// A mutex belongs to its physical component instance. Move only authored
	// base state; never transfer locks or invoke registration during rollback.
	Component(Component&& other) noexcept : active(other.active) {}
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

	Component& operator=(Component&& other) noexcept {
		active = other.active;
		return *this;
	}


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
