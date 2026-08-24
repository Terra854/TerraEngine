// TerrainRenderProxyRegistry.h -- Runtime render bindings for durable terrain assets.
//
// TerrainComponent stores only the durable terrain asset UUID. Generated meshes
// and materials are derived runtime data, so they belong in this renderer-owned
// registry instead of an authored MeshRenderComponent on every terrain object.

#pragma once

#include "Core/Export.h"
#include "Utilities/UUID.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace TerraEngine::Renderer::Terrain
{
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

		// Component removal retires only the affected asset binding. Other terrain
		// instances keep their generated resources and do not need to rebuild.
		[[nodiscard]] bool Remove(const TerraEngine::UUID& terrainAssetId)
		{
			if(terrainAssetId == TerraEngine::EMPTY_UUID) return false;
			std::unique_lock lock(mutex);
			if(bindings.erase(terrainAssetId) == 0U) return false;
			generation.fetch_add(1U, std::memory_order_release);
			return true;
		}

		void Clear()
		{
			std::unique_lock lock(mutex);
			if(bindings.empty()) return;
			bindings.clear();
			generation.fetch_add(1U, std::memory_order_release);
		}

	private:
		TerrainRenderProxyRegistry() = default;

		mutable std::shared_mutex mutex;
		std::unordered_map<TerraEngine::UUID, TerrainRenderProxyBinding> bindings;
		std::atomic<std::uint64_t> generation{0U};
	};
}
