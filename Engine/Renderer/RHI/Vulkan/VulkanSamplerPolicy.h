// VulkanSamplerPolicy.h -- capability-normalized terrain sampler and alias contracts.

#pragma once

#include "Core/Export.h"
#include "Terrain/TerrainMaterial.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace TerraEngine::Renderer::Vulkan
{
	enum class VulkanSamplerFilter : std::uint8_t { Nearest, Linear };
	enum class VulkanSamplerMipFilter : std::uint8_t { Nearest, Linear };
	enum class VulkanSamplerFallback : std::uint8_t
	{
		None,
		RequestedLevelClamped,
		AnisotropyUnsupported,
		InvalidDeviceLimit
	};

	struct SamplerKey
	{
		VulkanSamplerFilter minFilter = VulkanSamplerFilter::Linear;
		VulkanSamplerFilter magFilter = VulkanSamplerFilter::Linear;
		VulkanSamplerMipFilter mipFilter = VulkanSamplerMipFilter::Linear;
		SamplerAddressMode addressU = SamplerAddressMode::Repeat;
		SamplerAddressMode addressV = SamplerAddressMode::Repeat;
		SamplerAddressMode addressW = SamplerAddressMode::Repeat;
		bool anisotropyEnabled = false;
		float effectiveAnisotropy = 1.0F;

		[[nodiscard]] constexpr bool operator==(const SamplerKey&) const noexcept = default;
	};

	struct SamplerKeyHasher
	{
		[[nodiscard]] std::size_t operator()(const SamplerKey& key) const noexcept;
	};

	struct VulkanSamplerCapabilities
	{
		bool anisotropySupported = false;
		float maximumAnisotropy = 1.0F;
	};

	struct VulkanSamplerPolicyResult
	{
		SamplerKey key{};
		SamplerFilter requestedFilter = SamplerFilter::Trilinear;
		SamplerFilter effectiveFilter = SamplerFilter::Trilinear;
		VulkanSamplerFallback fallback = VulkanSamplerFallback::None;
	};

	[[nodiscard]] TERRAENGINE_API VulkanSamplerPolicyResult NormalizeTerrainSamplerPolicy(
		const TerraEngine::Terrain::TerrainSurfaceSamplerRequest& request,
		VulkanSamplerCapabilities capabilities,
		float requestedAnisotropy) noexcept;

	struct SampledTextureAliasSnapshot
	{
		std::uint32_t descriptorIndex = 0xffffffffu;
		std::uint64_t imageViewGeneration = 0u;
	};

	struct SampledTextureAliasAcquireResult : SampledTextureAliasSnapshot
	{
		bool accepted = false;
		bool aliasReused = false;
		bool samplerReused = false;
		bool capacityExhausted = false;
	};

	// This CPU ownership model mirrors the protected Vulkan boundary. It contains
	// no handles, which makes normalization, capacity, and retirement testable on
	// machines without a Vulkan device; the backend performs the matching Vulkan
	// mutations while holding its descriptor lock.
	class TERRAENGINE_API SampledTextureAliasCache
	{
	public:
		static constexpr std::size_t kDefaultCapacity = 1024u;

		explicit SampledTextureAliasCache(std::size_t capacity = kDefaultCapacity);
		[[nodiscard]] SampledTextureAliasAcquireResult Acquire(
			TerraEngine::UUID textureId, const SamplerKey& key,
			std::uint64_t imageViewGeneration);
		[[nodiscard]] std::optional<SampledTextureAliasSnapshot> Find(
			TerraEngine::UUID textureId, const SamplerKey& key) const noexcept;
		std::size_t ReplaceTextureView(
			TerraEngine::UUID textureId, std::uint64_t imageViewGeneration) noexcept;
		bool Release(TerraEngine::UUID textureId, const SamplerKey& key,
			std::uint64_t lastReferencedFrame, std::uint64_t safeFrameLag);
		std::size_t RetireCompleted(std::uint64_t completedFrame);

		[[nodiscard]] std::size_t ActiveAliasCount() const noexcept;
		[[nodiscard]] std::size_t PendingRetirementCount() const noexcept;
		[[nodiscard]] std::size_t SamplerCount() const noexcept;

	private:
		struct AliasRecord
		{
			TerraEngine::UUID textureId = TerraEngine::EMPTY_UUID;
			SamplerKey key{};
			SampledTextureAliasSnapshot snapshot{};
		};
		struct RetirementRecord
		{
			std::uint32_t descriptorIndex = 0xffffffffu;
			std::uint64_t safeAfterFrame = 0u;
		};

		std::size_t capacity = kDefaultCapacity;
		std::uint32_t nextDescriptorIndex = 0u;
		std::vector<std::uint32_t> freeDescriptorIndices;
		std::vector<AliasRecord> aliases;
		std::vector<RetirementRecord> pendingRetirements;
		std::vector<SamplerKey> samplers;
	};
}
