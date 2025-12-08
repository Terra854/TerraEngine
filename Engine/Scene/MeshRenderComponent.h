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
	TerraEngine::UUID textureId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID shaderCollectionId = TerraEngine::EMPTY_UUID;
};
