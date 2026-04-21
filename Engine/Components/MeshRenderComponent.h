#pragma once

#include <string_view>

#include "Core/Object/Component.h"
#include "Utilities/UUID.h"

// Simple render binding description that keeps track of authored assets the
// renderer should use once it exists. The component adheres to the CRTP
// contract so reflection stays consistent across all engine components.
struct MeshRenderComponent : public Component<MeshRenderComponent>
{
	static constexpr std::string_view kComponentName = "MeshRenderComponent";

	TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
	// Optional stable spatial identity. Empty means the Object UUID is used.
	// Keeping this separate lets imported or generated renderables preserve
	// culling identity even if their runtime entity is recreated.
	TerraEngine::UUID spatialObjectId = TerraEngine::EMPTY_UUID;
	// Bounds failures must fail open: when mesh bounds are missing or invalid,
	// the spatial index treats the renderable as a fallback candidate so the
	// renderer includes it instead of hiding visible geometry.
	bool taskShaderCullingEnabled = true;

	// Exposes the component to the EnTT-backed reflection layer so tools and serialization can introspect
	// the authored asset bindings safely.
	static void reflect(entt::meta_factory<MeshRenderComponent>& factory)
	{
		TerraEngine::Reflection::RegisterDataProperty<&MeshRenderComponent::meshId>(factory, "meshId");
		TerraEngine::Reflection::RegisterDataProperty<&MeshRenderComponent::materialId>(factory, "materialId");
		TerraEngine::Reflection::RegisterDataProperty<&MeshRenderComponent::spatialObjectId>(factory, "spatialObjectId");
		TerraEngine::Reflection::RegisterDataProperty<&MeshRenderComponent::taskShaderCullingEnabled>(factory, "taskShaderCullingEnabled");
	}
};
