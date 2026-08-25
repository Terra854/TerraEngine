// TerrainMaterialRuntimeRegistry.h -- protected terrain layer bindings for rendering.

#pragma once

#include "Core/Export.h"
#include "Utilities/UUID.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace TerraEngine::Renderer::Terrain
{
	inline constexpr std::size_t kTerrainGpuLayerLimit = 16u;

	struct TerrainGpuLayerBinding
	{
		std::uint16_t stableLayerId = 0u;
		TerraEngine::UUID baseColorTextureId = TerraEngine::EMPTY_UUID;
		std::array<float, 4u> baseColorFactor{1.0F, 1.0F, 1.0F, 1.0F};
		// xy scales terrain UVs and zw offsets them before texture sampling.
		std::array<float, 4u> uvTransform{1.0F, 1.0F, 0.0F, 0.0F};
	};

	struct TerrainMaterialRuntimeBinding
	{
		TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID weightTextureId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID layerIdTextureId = TerraEngine::EMPTY_UUID;
		std::array<TerrainGpuLayerBinding, kTerrainGpuLayerLimit> layers{};
		std::uint32_t layerCount = 0u;
		std::uint64_t publicationGeneration = 0u;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return materialId != TerraEngine::EMPTY_UUID
				&& weightTextureId != TerraEngine::EMPTY_UUID
				&& layerIdTextureId != TerraEngine::EMPTY_UUID
				&& layerCount > 0u && layerCount <= kTerrainGpuLayerLimit;
		}
	};

	// Terrain authoring publishes this renderer-owned value at its protected
	// boundary. Vulkan resolves a copy while uploading the generated material;
	// neither side receives mutable access to the other's live state.
	class TERRAENGINE_API TerrainMaterialRuntimeRegistry final
	{
	public:
		static TerrainMaterialRuntimeRegistry& Instance();
		[[nodiscard]] bool Publish(TerrainMaterialRuntimeBinding binding);
		[[nodiscard]] std::optional<TerrainMaterialRuntimeBinding> Resolve(
			TerraEngine::UUID materialId) const;
		void Remove(TerraEngine::UUID materialId);
		void Clear();

	private:
		mutable std::shared_mutex mutex;
		std::unordered_map<TerraEngine::UUID, TerrainMaterialRuntimeBinding> bindings;
		std::uint64_t generation = 0u;
	};
}
