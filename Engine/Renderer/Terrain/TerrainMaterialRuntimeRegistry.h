// TerrainMaterialRuntimeRegistry.h -- protected terrain layer bindings for rendering.

#pragma once

#include "Core/Export.h"
#include "Terrain/TerrainMaterial.h"
#include "Terrain/TerrainTypes.h"
#include "Utilities/UUID.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace TerraEngine::Renderer::Terrain
{
	inline constexpr std::size_t kTerrainGpuLayerLimit = 16u;
	inline constexpr std::uint32_t kTerrainControlExtentSamples = 256u;
	inline constexpr std::uint32_t kTerrainNeutralTextureIndex = 0xffffffffu;

	// Mapping mode values deliberately match the authored terrain enum. Keeping a
	// renderer-owned type prevents shader-facing code from depending on editor or
	// persistence records while retaining a byte-stable numeric contract.
	enum class TerrainGpuMappingMode : std::uint32_t
	{
		TerrainXZ = 0u,
		Triplanar = 1u
	};

	struct TerrainGpuLayerMapping
	{
		std::array<float, 2u> tileSizeMetres{4.0F, 4.0F};
		// cos/sin rotate canonical terrain metres by the inverse authored angle.
		// Preparing this pair on the CPU keeps every shader stage on the same basis.
		std::array<float, 2u> inverseRotationCosSin{1.0F, 0.0F};
		float triplanarSharpness = 4.0F;
		TerrainGpuMappingMode mode = TerrainGpuMappingMode::TerrainXZ;
	};

	struct TerrainGpuLayerPhase
	{
		std::uint16_t stableLayerId = 0u;
		std::uint16_t reserved = 0u;
		// The CPU reduces the inverse-rotated canonical area origin plus authored
		// offset into [0, tileSize). Shaders add only bounded local metre deltas.
		std::array<float, 2u> phaseMetres{0.0F, 0.0F};
	};

	struct TerrainAreaMappingBinding
	{
		TerraEngine::Terrain::TerrainAreaCoordinate area{};
		double sampleSpacingMetres = 1.0;
		// Page-backed packets can begin inside an area. The direct path uses zero.
		std::array<double, 2u> localSampleOrigin{0.0, 0.0};
		std::uint32_t controlExtentSamples = kTerrainControlExtentSamples;
		std::array<TerrainGpuLayerPhase, kTerrainGpuLayerLimit> layerPhases{};
		std::uint32_t layerCount = 0u;

		[[nodiscard]] bool IsComplete() const noexcept;
	};

	// Source identities remain separate from the shader record. Streaming touches
	// use UUIDs, while descriptor aliases resolve into the four fixed indices before
	// protected publication. Existing producers can leave an index invalid to select
	// the corresponding neutral semantic without fabricating a texture asset.
	struct TerrainLayerSourceBinding : TerraEngine::Terrain::TerrainLayerSurfaceSource
	{
		std::uint16_t stableLayerId = 0u;
		TerrainGpuLayerMapping mapping{};
		std::array<std::uint32_t, 4u> textureIndices{
			kTerrainNeutralTextureIndex, kTerrainNeutralTextureIndex,
			kTerrainNeutralTextureIndex, kTerrainNeutralTextureIndex};
		bool fallbackActive = false;
		// Compatibility payload consumed by the current base-color-only Vulkan
		// material upload. Dedicated terrain shaders use physical mapping instead.
		std::array<float, 4u> uvTransform{1.0F, 1.0F, 0.0F, 0.0F};
	};

	// Fixed StructuredBuffer record shared byte-for-byte with TerrainMaterial.hlsli.
	// Source UUIDs intentionally do not appear here: they are streaming identities,
	// not shader addresses, and would make backend-specific packing unavoidable.
	struct alignas(16) TerrainGpuLayerBinding
	{
		std::uint32_t baseColorTextureIndex = kTerrainNeutralTextureIndex;
		std::uint32_t normalTextureIndex = kTerrainNeutralTextureIndex;
		std::uint32_t ormTextureIndex = kTerrainNeutralTextureIndex;
		std::uint32_t emissiveTextureIndex = kTerrainNeutralTextureIndex;
		std::array<float, 4u> baseColorFactor{1.0F, 1.0F, 1.0F, 1.0F};
		std::array<float, 4u> emissiveFactor{1.0F, 1.0F, 1.0F, 0.0F};
		float normalScale = 1.0F;
		float occlusionStrength = 1.0F;
		float roughnessFactor = 1.0F;
		float metallicFactor = 1.0F;
		std::array<float, 2u> tileSizeMetres{4.0F, 4.0F};
		std::array<float, 2u> inverseRotationCosSin{1.0F, 0.0F};
		float triplanarSharpness = 4.0F;
		TerrainGpuMappingMode mappingMode = TerrainGpuMappingMode::TerrainXZ;
		std::uint32_t stableLayerId = 0u;
		std::uint32_t reserved = 0u;
	};

	static_assert(alignof(TerrainGpuLayerBinding) == 16u);
	static_assert(sizeof(TerrainGpuLayerBinding) == 96u);
	static_assert(offsetof(TerrainGpuLayerBinding, baseColorTextureIndex) == 0u);
	static_assert(offsetof(TerrainGpuLayerBinding, normalTextureIndex) == 4u);
	static_assert(offsetof(TerrainGpuLayerBinding, ormTextureIndex) == 8u);
	static_assert(offsetof(TerrainGpuLayerBinding, emissiveTextureIndex) == 12u);
	static_assert(offsetof(TerrainGpuLayerBinding, baseColorFactor) == 16u);
	static_assert(offsetof(TerrainGpuLayerBinding, emissiveFactor) == 32u);
	static_assert(offsetof(TerrainGpuLayerBinding, normalScale) == 48u);
	static_assert(offsetof(TerrainGpuLayerBinding, occlusionStrength) == 52u);
	static_assert(offsetof(TerrainGpuLayerBinding, roughnessFactor) == 56u);
	static_assert(offsetof(TerrainGpuLayerBinding, metallicFactor) == 60u);
	static_assert(offsetof(TerrainGpuLayerBinding, tileSizeMetres) == 64u);
	static_assert(offsetof(TerrainGpuLayerBinding, inverseRotationCosSin) == 72u);
	static_assert(offsetof(TerrainGpuLayerBinding, triplanarSharpness) == 80u);
	static_assert(offsetof(TerrainGpuLayerBinding, mappingMode) == 84u);
	static_assert(offsetof(TerrainGpuLayerBinding, stableLayerId) == 88u);

	// Converts one validated source record to the fixed shader record. Descriptor
	// alias preparation writes textureIndices before calling this helper; missing
	// optional sources deliberately retain the neutral sentinel.
	[[nodiscard]] TERRAENGINE_API bool BuildTerrainGpuLayerBinding(
		const TerrainLayerSourceBinding& source,
		TerrainGpuLayerBinding& outBinding) noexcept;

	struct TerrainMaterialRuntimeBinding
	{
		TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID weightTextureId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID layerIdTextureId = TerraEngine::EMPTY_UUID;
		std::array<TerrainLayerSourceBinding, kTerrainGpuLayerLimit> layers{};
		std::array<TerrainGpuLayerBinding, kTerrainGpuLayerLimit> gpuLayers{};
		std::uint32_t layerCount = 0u;
		// Each area owns control addressing independently from the surface mapping
		// phase. Material-only edits replace this immutable table without rebuilding
		// direct or page-backed geometry.
		std::vector<TerrainAreaMappingBinding> areaMappings;
		std::uint64_t publicationGeneration = 0u;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return materialId != TerraEngine::EMPTY_UUID
				&& weightTextureId != TerraEngine::EMPTY_UUID
				&& layerIdTextureId != TerraEngine::EMPTY_UUID
				&& layerCount > 0u && layerCount <= kTerrainGpuLayerLimit;
		}

		[[nodiscard]] bool HasCompleteAreaMappings() const noexcept;
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
		// Quality changes republish every immutable binding as one protected batch.
		// Returned IDs tell the RHI which material SSBO records must be rebuilt;
		// texture UUIDs and their image allocations remain unchanged.
		[[nodiscard]] std::vector<TerraEngine::UUID> RepublishSamplerQuality(
			std::uint32_t requestedAnisotropy);
		[[nodiscard]] std::uint32_t RequestedSamplerAnisotropy() const noexcept;
		void Remove(TerraEngine::UUID materialId);
		void Clear();

	private:
		mutable std::shared_mutex mutex;
		std::unordered_map<TerraEngine::UUID, TerrainMaterialRuntimeBinding> bindings;
		std::uint64_t generation = 0u;
		std::uint32_t requestedSamplerAnisotropy = 1u;
	};
}
