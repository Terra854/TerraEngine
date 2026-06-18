// ComponentRegistry.cpp — Implementation of the global component registration logic.
//
// This file serves as the central location for registering all native engine
// components and common math types with the reflection system. It ensures that
// basic types like vectors, quaternions, and UUIDs are correctly described for
// use in the editor and serialization pipelines.

#include "ComponentRegistry.h"
#include "Components/Transform.h"
#include "Components/ScriptComponent.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MeshRenderComponent.h"
#include "Components/PrefabLinkComponent.h"
#include "Components/UIComponent.h"

void ComponentRegistry::RegisterAllComponents() {
	// Register common GLM types so InspectorWindow can identify them by type
	// and preserve the same field names it used with the legacy backend.
	entt::meta_factory<glm::vec3> vec3Factory{};
	TerraEngine::Reflection::ConfigureTypeMetadata(vec3Factory, "glm::vec3");
	vec3Factory.template ctor<>();
	TerraEngine::Reflection::RegisterDataProperty<&glm::vec3::x>(vec3Factory, "x");
	TerraEngine::Reflection::RegisterDataProperty<&glm::vec3::y>(vec3Factory, "y");
	TerraEngine::Reflection::RegisterDataProperty<&glm::vec3::z>(vec3Factory, "z");

	entt::meta_factory<glm::quat> quatFactory{};
	TerraEngine::Reflection::ConfigureTypeMetadata(quatFactory, "glm::quat");
	quatFactory.template ctor<>();
	TerraEngine::Reflection::RegisterDataProperty<&glm::quat::x>(quatFactory, "x");
	TerraEngine::Reflection::RegisterDataProperty<&glm::quat::y>(quatFactory, "y");
	TerraEngine::Reflection::RegisterDataProperty<&glm::quat::z>(quatFactory, "z");
	TerraEngine::Reflection::RegisterDataProperty<&glm::quat::w>(quatFactory, "w");

	entt::meta_factory<TerraEngine::UUID> uuidFactory{};
	TerraEngine::Reflection::ConfigureTypeMetadata(uuidFactory, "TerraEngine::UUID");
	uuidFactory.template ctor<>();

	Transform::registerType();
	ScriptComponent::registerType();
	CameraComponent::registerType();
	LightComponent::registerType();
	MeshRenderComponent::registerType();
	PrefabLinkComponent::registerType();
	UIComponent::registerType();
}
