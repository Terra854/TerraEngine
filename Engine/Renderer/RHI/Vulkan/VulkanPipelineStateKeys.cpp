#include "VulkanPipelineStateKeys.h"

#include <algorithm>

namespace TerraEngine::Renderer::Vulkan {
	namespace
	{
		void HashCombine(std::uint64_t& seed, const std::uint64_t value) noexcept
		{
			seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
		}

		MaterialCapabilitySignature MakeSnapshotFallbackCapability(
			const TerraEngine::UUID& materialId)
		{
			// RenderFrameSnapshot intentionally stores immutable resource identities,
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

	std::vector<PipelineStateKey> BuildPipelineStateKeys(const RenderFrameSnapshot& snapshot)
	{
		std::vector<PipelineStateKey> keys;
		keys.reserve(snapshot.renderables.size());
		for (const RenderFrameRenderableRecord& renderable : snapshot.renderables)
		{
			keys.push_back(BuildPipelineStateKey(renderable.materialId,
				renderable.meshId,
				MakeSnapshotFallbackCapability(renderable.materialId)));
		}
		std::stable_sort(keys.begin(), keys.end(), [](const PipelineStateKey& left, const PipelineStateKey& right) {
			return left.sortKey < right.sortKey;
		});
		return keys;
	}

}
