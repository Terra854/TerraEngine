// ComponentRegistry.cpp — Implementation of the global component registration logic.
//
// This file serves as the central location for registering all native engine
// components and common math types with the reflection system. It ensures that
// basic types like vectors, quaternions, and UUIDs are correctly described for
// use in the editor and serialization pipelines.

#include "ComponentRegistry.h"
#include "Components/Transform.h"
#include "Components/TerrainComponent.h"
#include "Components/AnimatorComponent.h"
#include "Components/AudioListenerComponent.h"
#include "Components/AudioSourceComponent.h"
#include "Components/ScriptComponent.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MeshRenderComponent.h"
#include "Components/SkyComponent.h"
#include "Components/PrefabLinkComponent.h"
#include "Components/UIComponent.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Physics/ConstraintSetComponent.h"
#include "Components/Physics/RigidBodyComponent.h"
#include "Components/Navigation/NavigationAgentComponent.h"
#include "Components/Navigation/NavigationConnectionComponent.h"
#include "Components/Navigation/NavigationSourceComponent.h"
#include "Physics/PhysicsTypes.h"

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

	// Physics enums are reflected under engine-owned names. Serialized files use
	// symbolic strings, so neither editor metadata nor scene JSON exposes Jolt IDs.
	entt::meta_factory<TerraEngine::Physics::PhysicsMotionType> motionTypeFactory{};
	TerraEngine::Reflection::ConfigureTypeMetadata(motionTypeFactory, "PhysicsMotionType");
	entt::meta_factory<TerraEngine::Physics::PhysicsMotionQuality> motionQualityFactory{};
	TerraEngine::Reflection::ConfigureTypeMetadata(motionQualityFactory, "PhysicsMotionQuality");
	entt::meta_factory<TerraEngine::Physics::PhysicsMassMode> massModeFactory{};
	TerraEngine::Reflection::ConfigureTypeMetadata(massModeFactory, "PhysicsMassMode");
	entt::meta_factory<TerraEngine::Physics::PhysicsInterpolationMode> interpolationFactory{};
	TerraEngine::Reflection::ConfigureTypeMetadata(interpolationFactory, "PhysicsInterpolationMode");
	// Constraint and character choices are engine-owned reflection types. Their
	// symbolic scene representations are defined by Serializer.cpp, keeping the
	// provider's integer values and headers out of durable authoring data.
	entt::meta_factory<TerraEngine::Physics::PhysicsConstraintKind> constraintKindFactory{};
	TerraEngine::Reflection::ConfigureTypeMetadata(constraintKindFactory, "PhysicsConstraintKind");
	entt::meta_factory<TerraEngine::Physics::PhysicsConstraintEndpointKind> endpointKindFactory{};
	TerraEngine::Reflection::ConfigureTypeMetadata(endpointKindFactory, "PhysicsConstraintEndpointKind");
	entt::meta_factory<TerraEngine::Physics::PhysicsConstraintMotorMode> motorModeFactory{};
	TerraEngine::Reflection::ConfigureTypeMetadata(motorModeFactory, "PhysicsConstraintMotorMode");
	entt::meta_factory<TerraEngine::Physics::PhysicsCharacterShapeKind> characterShapeFactory{};
	TerraEngine::Reflection::ConfigureTypeMetadata(characterShapeFactory, "PhysicsCharacterShapeKind");
	entt::meta_factory<TerraEngine::Physics::PhysicsCharacterRotationPolicy> rotationPolicyFactory{};
	TerraEngine::Reflection::ConfigureTypeMetadata(rotationPolicyFactory, "PhysicsCharacterRotationPolicy");

	Transform::registerType();
	// Terrain must be reflected before scene loading so a saved terrain instance
	// can be recreated without waiting for a live component constructor.
	TerrainComponent::registerType();
	// Animator authoring metadata must exist before the first scene is loaded so
	// the editor can enumerate and add the component without relying on lazy
	// serializer registration from an already-authored object.
	AnimatorComponent::registerType();
	// Audio authoring must be registered before an editor opens a saved scene so
	// generic Inspector and Serializer paths can discover it without lazy setup.
	AudioSourceComponent::registerType();
	AudioListenerComponent::registerType();
	ScriptComponent::registerType();
	CameraComponent::registerType();
	LightComponent::registerType();
	MeshRenderComponent::registerType();
	SkyComponent::registerType();
	PrefabLinkComponent::registerType();
	UIComponent::registerType();
	RigidBodyComponent::registerType();
	ColliderSetComponent::registerType();
	ConstraintSetComponent::registerType();
	CharacterControllerComponent::registerType();
	NavigationSourceComponent::registerType();
	NavigationAgentComponent::registerType();
	NavigationConnectionComponent::registerType();
}
