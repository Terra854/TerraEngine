// VulkanSamplerPolicy.cpp -- deterministic sampler normalization and alias ownership.

#include "Renderer/RHI/Vulkan/VulkanSamplerPolicy.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>

namespace TerraEngine::Renderer::Vulkan
{
	namespace
	{
		template<typename Value>
		void HashCombine(std::size_t& seed, Value value) noexcept
		{
			seed ^= std::hash<Value>{}(value) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
		}
	}

	std::size_t SamplerKeyHasher::operator()(const SamplerKey& key) const noexcept
	{
		std::size_t seed = 0u;
		HashCombine(seed, static_cast<std::uint8_t>(key.minFilter));
		HashCombine(seed, static_cast<std::uint8_t>(key.magFilter));
		HashCombine(seed, static_cast<std::uint8_t>(key.mipFilter));
		HashCombine(seed, static_cast<std::uint32_t>(key.addressU));
		HashCombine(seed, static_cast<std::uint32_t>(key.addressV));
		HashCombine(seed, static_cast<std::uint32_t>(key.addressW));
		HashCombine(seed, key.anisotropyEnabled);
		HashCombine(seed, std::bit_cast<std::uint32_t>(key.effectiveAnisotropy));
		return seed;
	}

	VulkanSamplerPolicyResult NormalizeTerrainSamplerPolicy(
		const TerraEngine::Terrain::TerrainSurfaceSamplerRequest& request,
		VulkanSamplerCapabilities capabilities,
		float requestedAnisotropy) noexcept
	{
		VulkanSamplerPolicyResult result{};
		result.requestedFilter = request.filter;
		result.effectiveFilter = request.filter;
		result.key.addressU = request.addressU;
		result.key.addressV = request.addressV;
		result.key.addressW = request.addressW;

		switch(request.filter)
		{
		case SamplerFilter::Nearest:
			result.key.minFilter = VulkanSamplerFilter::Nearest;
			result.key.magFilter = VulkanSamplerFilter::Nearest;
			result.key.mipFilter = VulkanSamplerMipFilter::Nearest;
			break;
		case SamplerFilter::Bilinear:
			result.key.mipFilter = VulkanSamplerMipFilter::Nearest;
			break;
		case SamplerFilter::Anisotropic:
			if(!std::isfinite(capabilities.maximumAnisotropy)
				|| capabilities.maximumAnisotropy < 1.0F)
			{
				result.effectiveFilter = SamplerFilter::Trilinear;
				result.fallback = VulkanSamplerFallback::InvalidDeviceLimit;
				break;
			}
			if(!capabilities.anisotropySupported)
			{
				result.effectiveFilter = SamplerFilter::Trilinear;
				result.fallback = VulkanSamplerFallback::AnisotropyUnsupported;
				break;
			}
			requestedAnisotropy = std::isfinite(requestedAnisotropy)
				? std::max(1.0F, requestedAnisotropy) : 1.0F;
			result.key.effectiveAnisotropy = std::min(
				requestedAnisotropy, capabilities.maximumAnisotropy);
			result.key.anisotropyEnabled = result.key.effectiveAnisotropy > 1.0F;
			if(result.key.effectiveAnisotropy < requestedAnisotropy)
				result.fallback = VulkanSamplerFallback::RequestedLevelClamped;
			break;
		case SamplerFilter::Trilinear:
		default:
			break;
		}
		return result;
	}

	SampledTextureAliasCache::SampledTextureAliasCache(std::size_t requestedCapacity)
		: capacity(requestedCapacity)
	{
		aliases.reserve(capacity);
		freeDescriptorIndices.reserve(capacity);
	}

	SampledTextureAliasAcquireResult SampledTextureAliasCache::Acquire(
		TerraEngine::UUID textureId, const SamplerKey& key,
		std::uint64_t imageViewGeneration)
	{
		const auto existing = std::find_if(aliases.begin(), aliases.end(),
			[&](const AliasRecord& alias) { return alias.textureId == textureId && alias.key == key; });
		if(existing != aliases.end())
		{
			return {existing->snapshot, true, true, true, false};
		}

		const bool samplerReused = std::find(samplers.begin(), samplers.end(), key) != samplers.end();
		if(aliases.size() + pendingRetirements.size() >= capacity)
		{
			SampledTextureAliasAcquireResult rejected{};
			rejected.samplerReused = samplerReused;
			rejected.capacityExhausted = true;
			return rejected;
		}

		if(!samplerReused) samplers.push_back(key);
		std::uint32_t descriptorIndex = 0xffffffffu;
		if(!freeDescriptorIndices.empty())
		{
			descriptorIndex = freeDescriptorIndices.back();
			freeDescriptorIndices.pop_back();
		}
		else
		{
			descriptorIndex = nextDescriptorIndex++;
		}
		aliases.push_back({textureId, key, {descriptorIndex, imageViewGeneration}});
		return {{descriptorIndex, imageViewGeneration}, true, false, samplerReused, false};
	}

	std::optional<SampledTextureAliasSnapshot> SampledTextureAliasCache::Find(
		TerraEngine::UUID textureId, const SamplerKey& key) const noexcept
	{
		const auto found = std::find_if(aliases.begin(), aliases.end(),
			[&](const AliasRecord& alias) { return alias.textureId == textureId && alias.key == key; });
		return found == aliases.end() ? std::nullopt
			: std::optional<SampledTextureAliasSnapshot>{found->snapshot};
	}

	std::size_t SampledTextureAliasCache::ReplaceTextureView(
		TerraEngine::UUID textureId, std::uint64_t imageViewGeneration) noexcept
	{
		std::size_t updated = 0u;
		for(AliasRecord& alias : aliases)
		{
			if(alias.textureId != textureId) continue;
			alias.snapshot.imageViewGeneration = imageViewGeneration;
			++updated;
		}
		return updated;
	}

	bool SampledTextureAliasCache::Release(
		TerraEngine::UUID textureId, const SamplerKey& key,
		std::uint64_t lastReferencedFrame, std::uint64_t safeFrameLag)
	{
		const auto found = std::find_if(aliases.begin(), aliases.end(),
			[&](const AliasRecord& alias) { return alias.textureId == textureId && alias.key == key; });
		if(found == aliases.end()) return false;
		const std::uint64_t safeAfter = safeFrameLag > std::numeric_limits<std::uint64_t>::max() - lastReferencedFrame
			? std::numeric_limits<std::uint64_t>::max() : lastReferencedFrame + safeFrameLag;
		pendingRetirements.push_back({found->snapshot.descriptorIndex, safeAfter});
		aliases.erase(found);
		return true;
	}

	std::size_t SampledTextureAliasCache::RetireCompleted(std::uint64_t completedFrame)
	{
		std::size_t retired = 0u;
		for(auto item = pendingRetirements.begin(); item != pendingRetirements.end();)
		{
			if(item->safeAfterFrame > completedFrame) { ++item; continue; }
			freeDescriptorIndices.push_back(item->descriptorIndex);
			item = pendingRetirements.erase(item);
			++retired;
		}
		return retired;
	}

	std::size_t SampledTextureAliasCache::ActiveAliasCount() const noexcept { return aliases.size(); }
	std::size_t SampledTextureAliasCache::PendingRetirementCount() const noexcept { return pendingRetirements.size(); }
	std::size_t SampledTextureAliasCache::SamplerCount() const noexcept { return samplers.size(); }
}
