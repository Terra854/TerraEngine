// VulkanHiZ.h — Vulkan Hierarchical Z-Buffer Wrapper
//
// Defines the Vulkan implementation of a Hierarchical Z-buffer (Hi-Z) for efficient occlusion culling.
// It manages a depth pyramid image, its mip-level views, and the samplers required for depth reduction passes.
// The Hi-Z pyramid is used to accelerate visibility tests and improve overall rendering performance.

#pragma once

#include "VulkanCommonIncludes.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <cstring>
#include <vector>

enum class VulkanHiZInvalidReason : std::uint32_t
{
	None = 0u,
	FirstFrame = 1u,
	MissingPyramid = 2u,
	Resize = 3u,
	ProjectionChanged = 4u,
	ViewMismatch = 5u,
	QualityDisabled = 6u,
	UnsupportedView = 7u,
	SelfOcclusionRisk = 8u
};

struct VulkanHiZHistoryMetadata
{
	// The history owner is the renderer output/view that produced the pyramid.
	// A later view must match it before task-shader occlusion can sample Hi-Z.
	uint32_t viewId = 0u;
	uint32_t width = 0u;
	uint32_t height = 0u;
	std::uint64_t projectionSignature = 0u;
	std::uint64_t frameId = 0u;
	bool valid = false;
	VulkanHiZInvalidReason invalidReason = VulkanHiZInvalidReason::FirstFrame;
};

struct VulkanHiZValidationInput
{
	uint32_t viewId = 0u;
	uint32_t width = 0u;
	uint32_t height = 0u;
	std::uint64_t projectionSignature = 0u;
	bool pyramidReady = false;
	bool qualityAllowsOcclusion = false;
	bool viewSupported = true;
	bool selfOcclusionGuardPassed = false;
};

struct VulkanHiZValidationResult
{
	bool historyValid = false;
	bool occlusionAllowed = false;
	VulkanHiZInvalidReason invalidReason = VulkanHiZInvalidReason::FirstFrame;
};

inline const char* ToString(VulkanHiZInvalidReason reason)
{
	switch (reason)
	{
	case VulkanHiZInvalidReason::None: return "none";
	case VulkanHiZInvalidReason::FirstFrame: return "first-frame";
	case VulkanHiZInvalidReason::MissingPyramid: return "missing-pyramid";
	case VulkanHiZInvalidReason::Resize: return "resize";
	case VulkanHiZInvalidReason::ProjectionChanged: return "projection-changed";
	case VulkanHiZInvalidReason::ViewMismatch: return "view-mismatch";
	case VulkanHiZInvalidReason::QualityDisabled: return "quality-disabled";
	case VulkanHiZInvalidReason::UnsupportedView: return "unsupported-view";
	case VulkanHiZInvalidReason::SelfOcclusionRisk: return "self-occlusion-risk";
	default: return "unknown";
	}
}

inline void HashCombineVulkanHiZ(std::uint64_t& seed, std::uint64_t value)
{
	seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
}

inline std::uint64_t BuildVulkanHiZProjectionSignature(const glm::mat4& viewProjection)
{
	std::uint64_t signature = 1469598103934665603ull;
	for (int column = 0; column < 4; ++column)
	{
		for (int row = 0; row < 4; ++row)
		{
			std::uint32_t bits = 0u;
			const float value = viewProjection[column][row];
			std::memcpy(&bits, &value, sizeof(float));
			HashCombineVulkanHiZ(signature, bits);
		}
	}
	return signature;
}

inline VulkanHiZValidationResult ValidateVulkanHiZHistory(const VulkanHiZHistoryMetadata& history, const VulkanHiZValidationInput& input)
{
	VulkanHiZValidationResult result{};
	if (!input.viewSupported)
	{
		result.invalidReason = VulkanHiZInvalidReason::UnsupportedView;
		return result;
	}
	if (!input.qualityAllowsOcclusion)
	{
		result.invalidReason = VulkanHiZInvalidReason::QualityDisabled;
		return result;
	}
	if (!history.valid)
	{
		result.invalidReason = VulkanHiZInvalidReason::FirstFrame;
		return result;
	}
	if (!input.pyramidReady)
	{
		result.invalidReason = VulkanHiZInvalidReason::MissingPyramid;
		return result;
	}
	if (history.viewId != input.viewId)
	{
		result.invalidReason = VulkanHiZInvalidReason::ViewMismatch;
		return result;
	}
	if (history.width != input.width || history.height != input.height)
	{
		result.invalidReason = VulkanHiZInvalidReason::Resize;
		return result;
	}
	if (history.projectionSignature != input.projectionSignature)
	{
		result.invalidReason = VulkanHiZInvalidReason::ProjectionChanged;
		return result;
	}
	if (!input.selfOcclusionGuardPassed)
	{
		result.invalidReason = VulkanHiZInvalidReason::SelfOcclusionRisk;
		result.historyValid = true;
		return result;
	}

	result.historyValid = true;
	result.occlusionAllowed = true;
	result.invalidReason = VulkanHiZInvalidReason::None;
	return result;
}

struct VulkanHiZ {
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation allocation = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	VkImageView fullView = VK_NULL_HANDLE;
	std::vector<VkImageView> mipViews;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t mipLevels = 0;
	VulkanHiZHistoryMetadata history{};
	VulkanHiZInvalidReason latestInvalidReason = VulkanHiZInvalidReason::FirstFrame;
	bool latestOcclusionAllowed = false;

	void Create(VkDevice device, VmaAllocator allocator, uint32_t screenWidth, uint32_t screenHeight, VkFormat format);
	void Destroy(VkDevice device, VmaAllocator allocator);
};
