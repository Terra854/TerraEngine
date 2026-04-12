#include "ComponentRegistry.h"
#include "Components/Transform.h"
#include "Components/TagComponent.h"
#include "Components/ScriptComponent.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MeshRenderComponent.h"
#include <rttr/registration>

void ComponentRegistry::RegisterAllComponents() {
	// Register common GLM types so InspectorWindow can identify them by type
	rttr::registration::class_<glm::vec3>("glm::vec3")
		.property("x", &glm::vec3::x)
		.property("y", &glm::vec3::y)
		.property("z", &glm::vec3::z);

	rttr::registration::class_<glm::quat>("glm::quat")
		.property("x", &glm::quat::x)
		.property("y", &glm::quat::y)
		.property("z", &glm::quat::z)
		.property("w", &glm::quat::w);

	Transform::registerType();
	TagComponent::registerType();
	ScriptComponent::registerType();
	CameraComponent::registerType();
	LightComponent::registerType();
	MeshRenderComponent::registerType();
}
