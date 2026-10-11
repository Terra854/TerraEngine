#pragma once

#include <cstdint>
#include <span>

namespace TerraEngine::Renderer::Vulkan
{
	// The caller supplies its existing protected completion proof. Descriptor
	// retirement adds no wait and cannot turn an unready fallback into an owner.
	struct TextureDescriptorRetirementProof
	{
		bool deviceReady = false;
		bool completionProven = false;
		bool setsReady = false;
		bool fallbackImage = false;
		bool fallbackView = false;
		bool fallbackSampler = false;
		bool fallbackShaderReadable = false;
		std::uint32_t frameSetCount = 0;
		std::uint32_t descriptorCapacity = 0;
	};

	template<class Rewrite, class RewriteSharedSampler>
	bool RetireTextureDescriptorSlots(const TextureDescriptorRetirementProof& proof,
		std::span<const std::uint32_t> slots, Rewrite&& rewrite,
		RewriteSharedSampler&& rewriteSharedSampler)
	{
		if (!proof.deviceReady || !proof.completionProven || !proof.setsReady
			|| !proof.fallbackImage || !proof.fallbackView || !proof.fallbackSampler
			|| !proof.fallbackShaderReadable || proof.frameSetCount == 0) return false;
		for (auto slot : slots)
			if (slot <= 2 || slot >= proof.descriptorCapacity) return false;
		// Some slots may already have been rewritten when a later write fails.
		// That is safe while owners remain alive; false forbids their destruction.
		for (auto slot : slots)
			for (std::uint32_t frame = 0; frame < proof.frameSetCount; ++frame)
				if (!rewrite(slot, frame)) return false;
		// The shared point sampler can outlive every active point slot in shader
		// bindings. Rewrite it even when this rebuild has no 2D descriptors.
		for (std::uint32_t frame = 0; frame < proof.frameSetCount; ++frame)
			if (!rewriteSharedSampler(frame)) return false;
		return true;
	}

	template<class Allocate>
	std::uint32_t AcquireRetainedTextureDescriptorSlot(std::uint32_t assigned,
		std::uint32_t capacity, Allocate&& allocate)
	{
		// A retained semantic assignment stays stable across physical replacement.
		// Only zero denotes a never-assigned slot; other invalid indices fail closed.
		if (assigned != 0) return assigned > 2 && assigned < capacity ? assigned : 0;
		const auto slot = allocate();
		return slot > 2 && slot < capacity ? slot : 0;
	}
}
