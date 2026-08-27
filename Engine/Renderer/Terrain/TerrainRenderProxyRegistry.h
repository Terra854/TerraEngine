// TerrainRenderProxyRegistry.h -- Runtime render bindings for durable terrain assets.
//
// TerrainComponent stores only the durable terrain asset UUID. Generated meshes
// and materials are derived runtime data, so they belong in this renderer-owned
// registry instead of an authored MeshRenderComponent on every terrain object.

#pragma once

#include "Core/Export.h"
#include "Terrain/TerrainTypes.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace TerraEngine::Renderer::Terrain
{
	struct TerrainAreaRenderState
	{
		std::uint16_t desiredLod = 0u;
		std::uint16_t balancedLod = 0u;
		std::uint16_t residentLod = 0u;
		float projectedErrorPixels = 0.0F;
		bool hysteresisActive = false;
		std::uint8_t stitchMask = 0u;
		bool holePinned = false;
		std::uint32_t desiredPageId = 0xffffffffu;
		std::uint32_t actualPageId = 0xffffffffu;
		std::uint16_t fallbackDepth = 0u;
		std::uint64_t residentBytes = 0u;
		std::uint8_t byteState = 0u;
		bool missingOuterHalo = false;
		bool rolledBack = false;
		std::string stableCode;
	};

	struct TerrainAreaRenderBinding
	{
		TerraEngine::Terrain::TerrainAreaCoordinate coordinate{};
		TerraEngine::Terrain::ContentHash stableAreaIdentity{};
		TerraEngine::Terrain::TerrainSampleBounds sampleBounds{};
		TerraEngine::Terrain::ContentHash authoredHash{};
		std::uint64_t authoredGeneration = 0u;
		TerraEngine::Terrain::ContentHash pageSourceIdentity{};
		TerraEngine::Terrain::ContentHash controlIdentity{};
		TerraEngine::Terrain::ContentHash holeIdentity{};
		TerraEngine::Terrain::ContentHash hierarchyIdentity{};
		TerraEngine::Terrain::ContentHash pageDirectoryIdentity{};
		TerraEngine::UUID directMeshId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
		std::uint32_t coarsePageId = 0xffffffffu;
		TerrainAreaRenderState renderState{};

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return stableAreaIdentity.IsValid() && sampleBounds.IsValid()
				&& authoredHash.IsValid() && authoredGeneration != 0u
				&& pageSourceIdentity.IsValid() && controlIdentity.IsValid()
				&& holeIdentity.IsValid() && hierarchyIdentity.IsValid()
				&& pageDirectoryIdentity.IsValid()
				&& directMeshId != TerraEngine::EMPTY_UUID
				&& materialId != TerraEngine::EMPTY_UUID
				&& coarsePageId != 0xffffffffu;
		}
	};

	struct TerrainAssetRenderSnapshot
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		std::uint64_t publicationGeneration = 0u;
		std::vector<TerrainAreaRenderBinding> areas;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			if(assetId == TerraEngine::EMPTY_UUID || publicationGeneration == 0u
				|| areas.empty()) return false;
			for(std::size_t index = 0u; index < areas.size(); ++index)
			{
				if(!areas[index].IsComplete()) return false;
				for(std::size_t prior = 0u; prior < index; ++prior)
					if(areas[prior].coordinate == areas[index].coordinate
						|| areas[prior].stableAreaIdentity == areas[index].stableAreaIdentity) return false;
			}
			return true;
		}

		[[nodiscard]] const TerrainAreaRenderBinding* FindArea(
			TerraEngine::Terrain::TerrainAreaCoordinate coordinate) const noexcept
		{
			const auto found = std::find_if(areas.begin(), areas.end(), [&](const auto& area) {
				return area.coordinate == coordinate;
			});
			return found == areas.end() ? nullptr : &*found;
		}
	};

	struct TerrainRenderProxyBinding
	{
		TerraEngine::UUID terrainAssetId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
		std::uint64_t publicationGeneration = 0U;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return terrainAssetId != TerraEngine::EMPTY_UUID
				&& meshId != TerraEngine::EMPTY_UUID
				&& materialId != TerraEngine::EMPTY_UUID
				&& publicationGeneration != 0U;
		}
	};

	class TERRAENGINE_API TerrainRenderProxyRegistry final
	{
	public:
		// The singleton storage lives in the Engine DLL. Defining this accessor
		// inline would give the Editor executable a different function-local static
		// on Windows, causing editor publication and renderer lookup to use separate
		// registries even though both called Instance().
		static TerrainRenderProxyRegistry& Instance();

		// Publish accepts only complete derived bindings. The generation advances
		// under the same lock as the map replacement, giving frame capture one
		// monotonic invalidation value for membership-cache refreshes.
		[[nodiscard]] bool Publish(TerrainRenderProxyBinding binding)
		{
			if(binding.terrainAssetId == TerraEngine::EMPTY_UUID
				|| binding.meshId == TerraEngine::EMPTY_UUID
				|| binding.materialId == TerraEngine::EMPTY_UUID)
			{
				return false;
			}
			std::unique_lock lock(mutex);
			binding.publicationGeneration = generation.fetch_add(1U, std::memory_order_acq_rel) + 1U;
			bindings[binding.terrainAssetId] = binding;
			return true;
		}

		[[nodiscard]] std::optional<TerrainRenderProxyBinding> Resolve(
			const TerraEngine::UUID& terrainAssetId) const
		{
			std::shared_lock lock(mutex);
			const auto found = bindings.find(terrainAssetId);
			return found != bindings.end()
				? std::optional<TerrainRenderProxyBinding>{found->second}
				: std::nullopt;
		}

		[[nodiscard]] std::uint64_t PublicationGeneration() const noexcept
		{
			return generation.load(std::memory_order_acquire);
		}

		[[nodiscard]] bool PublishSnapshot(TerrainAssetRenderSnapshot snapshot)
		{
			if(!snapshot.IsComplete()) return false;
			std::unique_lock lock(mutex);
			const auto prior = snapshots.find(snapshot.assetId);
			if(prior != snapshots.end()
				&& prior->second.publicationGeneration >= snapshot.publicationGeneration) return false;
			snapshots[snapshot.assetId] = std::move(snapshot);
			generation.fetch_add(1u, std::memory_order_release);
			return true;
		}

		[[nodiscard]] std::optional<TerrainAssetRenderSnapshot> ResolveSnapshot(
			const TerraEngine::UUID& terrainAssetId) const
		{
			std::shared_lock lock(mutex);
			const auto found = snapshots.find(terrainAssetId);
			return found == snapshots.end() ? std::nullopt
				: std::optional<TerrainAssetRenderSnapshot>{found->second};
		}

		// Frame preparation publishes copied observability only when it still
		// targets the exact immutable asset snapshot it selected from.
		[[nodiscard]] bool UpdateAreaRenderState(
			const TerraEngine::UUID& terrainAssetId,
			std::uint64_t expectedPublicationGeneration,
			TerraEngine::Terrain::TerrainAreaCoordinate coordinate,
			TerrainAreaRenderState state);

		// Component removal retires only the affected asset binding. Other terrain
		// instances keep their generated resources and do not need to rebuild.
		[[nodiscard]] bool Remove(const TerraEngine::UUID& terrainAssetId)
		{
			if(terrainAssetId == TerraEngine::EMPTY_UUID) return false;
			std::unique_lock lock(mutex);
			const std::size_t removed = bindings.erase(terrainAssetId)
				+ snapshots.erase(terrainAssetId);
			if(removed == 0u) return false;
			generation.fetch_add(1U, std::memory_order_release);
			return true;
		}

		void Clear()
		{
			std::unique_lock lock(mutex);
			if(bindings.empty() && snapshots.empty()) return;
			bindings.clear();
			snapshots.clear();
			generation.fetch_add(1U, std::memory_order_release);
		}

	private:
		TerrainRenderProxyRegistry() = default;

		mutable std::shared_mutex mutex;
		std::unordered_map<TerraEngine::UUID, TerrainRenderProxyBinding> bindings;
		std::unordered_map<TerraEngine::UUID, TerrainAssetRenderSnapshot> snapshots;
		std::atomic<std::uint64_t> generation{0U};
	};
}
