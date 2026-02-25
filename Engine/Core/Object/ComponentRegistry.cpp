#include "ComponentRegistry.h"
#include "Transform.h"
#include "Scene/CameraComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/MeshRenderComponent.h"

void ComponentRegistry::RegisterAllComponents() {
	Transform::registerType();
	CameraComponent::registerType();
	LightComponent::registerType();
	MeshRenderComponent::registerType();
}
