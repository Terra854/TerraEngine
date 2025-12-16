#include "ComponentRegistry.h"
#include "Transform.h"
#include "Scene/CameraComponent.h"
#include "Scene/MeshRenderComponent.h"

void ComponentRegistry::RegisterAllComponents() {
    Transform::registerType();
    CameraComponent::registerType();
    MeshRenderComponent::registerType();
}
