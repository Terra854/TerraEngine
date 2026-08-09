#include "VulkanPipelineStateKeys.h"

#include <algorithm>
#include <unordered_set>

namespace TerraEngine::Renderer::Vulkan {
	namespace
	{
		struct PipelineStateIdentity
		{
			TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
			TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;

			bool operator==(const PipelineStateIdentity&) const = default;
		};

		struct PipelineStateIdentityHash
		{
			std::size_t operator()(const PipelineStateIdentity& identity) const noexcept
			{
				std::size_t seed = std::hash<TerraEngine::UUID>{}(identity.materialId);
				seed ^= std::hash<TerraEngine::UUID>{}(identity.meshId)
					+ static_cast<std::size_t>(0x9e3779b9U)
					+ (seed << 6U) + (seed >> 2U);
				return seed;
			}
		};

		void HashCombine(std::uint64_t& seed, const std::uint64_t value) noexcept
		{
			seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
		}

		MaterialCapabilitySignature MakePublicationFallbackCapability(
			const TerraEngine::UUID& materialId)
		{
			// RenderFramePublication intentionally stores immutable resource identities,
			// not live Material objects. Until a loader supplies an optional TMVG
			// signature, use one typed indexed bin that is safe for every material.
			MaterialCapabilitySignature capability{};
			capability.maximumShadingBins = 1u;
			capability.compatibilitySignature =
				static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(materialId));
			capability.valid = false;
			return capability;
		}
	}

	PipelineStateKey BuildPipelineStateKey(
		const TerraEngine::UUID& materialId,
		const TerraEngine::UUID& meshId,
		const MaterialCapabilitySignature& capability,
		bool usesConventionalVisibilityIdentity)
	{
		PipelineStateKey key;
		const bool boundedSignature = capability.valid
			&& capability.compatibilitySignature != 0u
			&& capability.maximumShadingBins > 0u
			&& capability.shadingBin < capability.maximumShadingBins;
		key.usesIndexedFallback = !boundedSignature;
		key.shadingBin = boundedSignature ? capability.shadingBin : 0u;
		key.compatibilitySignature = boundedSignature
			? capability.compatibilitySignature : 0u;
		key.usesConventionalVisibilityIdentity = usesConventionalVisibilityIdentity;
		key.sortKey = static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(materialId));
		HashCombine(key.sortKey,
			static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(meshId)));
		HashCombine(key.sortKey, key.compatibilitySignature);
		HashCombine(key.sortKey, key.shadingBin);
		HashCombine(key.sortKey, capability.requiredFeatureMask);
		HashCombine(key.sortKey, capability.programmableFeatureMask);
		HashCombine(key.sortKey, key.usesConventionalVisibilityIdentity ? 1u : 0u);
		key.label = TerraEngine::ToHexString(materialId)
			+ (key.usesIndexedFallback ? ":indexed-fallback" : ":virtual-geometry")
			+ (key.usesConventionalVisibilityIdentity ? ":conventional-identity" : ":target0-only");
		return key;
	}

	std::vector<PipelineStateKey> BuildPipelineStateKeys(
		const RenderFramePublication& publication)
	{
		std::vector<PipelineStateKey> keys;
		std::unordered_set<PipelineStateIdentity, PipelineStateIdentityHash>
			uniqueIdentities;
		std::size_t slotCount = 0u;
		if(publication.persistent != nullptr)
		{
			for(const std::shared_ptr<const RenderPersistentPage>& page :
				publication.persistent->pages)
			{
				slotCount += page != nullptr ? page->slots.size() : 0u;
			}
		}
		uniqueIdentities.reserve(slotCount);
		keys.reserve(slotCount);
		if(publication.persistent == nullptr)
		{
			return keys;
		}
		for(const std::shared_ptr<const RenderPersistentPage>& page :
			publication.persistent->pages)
		{
			if(page == nullptr)
			{
				continue;
			}
			for(const RenderPersistentSlot& slot : page->slots)
			{
				if(!slot.IsComplete() || !uniqueIdentities.emplace(
					PipelineStateIdentity{slot.materialId, slot.meshId}).second)
				{
					continue;
				}
				keys.push_back(BuildPipelineStateKey(slot.materialId, slot.meshId,
					MakePublicationFallbackCapability(slot.materialId)));
			}
		}
		std::stable_sort(keys.begin(), keys.end(), [](const PipelineStateKey& left, const PipelineStateKey& right) {
			return left.sortKey < right.sortKey;
		});
		return keys;
	}

}
