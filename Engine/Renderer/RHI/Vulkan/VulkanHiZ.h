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

enum class VulkanHiZViewClass : std::uint32_t
{
	Unknown = 0u,
	Game = 1u,
	Scene = 2u,
	Shadow = 3u,
	ExportRuntime = 4u
};

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
	SelfOcclusionRisk = 8u,
	TargetMismatch = 9u,
	CameraCut = 10u,
	ShadowView = 11u,
	NearPlaneRisk = 12u,
	ThinGeometryRisk = 13u,
	DepthSceneChanged = 14u
};

struct VulkanHiZHistoryMetadata
{
	// The history owner is the renderer output/view that produced the pyramid.
	// The current implementation keeps one warm pyramid per frame slot, so
	// ownership mismatches are diagnostic-only for editor camera/game views.
	uint32_t viewId = 0u;
	VulkanHiZViewClass viewClass = VulkanHiZViewClass::Unknown;
	// targetId separates Scene View, Game View, export, and future render-texture
	// outputs that may share a camera index but render into different surfaces.
	uint32_t targetId = 0u;
	uint32_t width = 0u;
	uint32_t height = 0u;
	// The projection signature includes the view-projection matrix for
	// diagnostics. Interactive editor movement must not hard-disable Hi-Z,
	// because virtual geometry needs occlusion feedback while the camera moves.
	std::uint64_t projectionSignature = 0u;
	// Identifies the draw list and transforms that produced this depth pyramid.
	// This stays diagnostic-only: the task shader already fails open when the
	// sampled depth cannot conservatively prove the meshlet is hidden.
	std::uint64_t depthSceneSignature = 0u;
	std::uint64_t frameId = 0u;
	bool valid = false;
	VulkanHiZInvalidReason invalidReason = VulkanHiZInvalidReason::FirstFrame;
};

struct VulkanHiZValidationInput
{
	uint32_t viewId = 0u;
	VulkanHiZViewClass viewClass = VulkanHiZViewClass::Unknown;
	uint32_t targetId = 0u;
	uint32_t width = 0u;
	uint32_t height = 0u;
	std::uint64_t projectionSignature = 0u;
	std::uint64_t depthSceneSignature = 0u;
	bool pyramidReady = false;
	bool qualityAllowsOcclusion = false;
	bool viewSupported = true;
	bool cameraCut = false;
	bool selfOcclusionGuardPassed = false;
	bool nearPlaneGuardPassed = false;
	bool thinGeometryGuardPassed = false;
};

struct VulkanHiZValidationResult
{
	bool historyValid = false;
	bool occlusionAllowed = false;
	VulkanHiZInvalidReason invalidReason = VulkanHiZInvalidReason::FirstFrame;
	VulkanHiZViewClass ownerViewClass = VulkanHiZViewClass::Unknown;
	bool sameViewHistory = false;
	bool sameTargetHistory = false;
	bool projectionMatches = false;
	bool depthSceneMatches = false;
	bool failOpen = true;
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
	case VulkanHiZInvalidReason::TargetMismatch: return "target-mismatch";
	case VulkanHiZInvalidReason::CameraCut: return "camera-cut";
	case VulkanHiZInvalidReason::ShadowView: return "shadow-view";
	case VulkanHiZInvalidReason::NearPlaneRisk: return "near-plane-risk";
	case VulkanHiZInvalidReason::ThinGeometryRisk: return "thin-geometry-risk";
	case VulkanHiZInvalidReason::DepthSceneChanged: return "depth-scene-changed";
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
	result.ownerViewClass = history.viewClass;
	result.sameViewHistory = history.viewId == input.viewId && history.viewClass == input.viewClass;
	result.sameTargetHistory = history.targetId == input.targetId;
	result.projectionMatches = history.projectionSignature == input.projectionSignature;
	result.depthSceneMatches = history.depthSceneSignature == input.depthSceneSignature;

	if (!input.pyramidReady)
	{
		result.invalidReason = VulkanHiZInvalidReason::MissingPyramid;
		return result;
	}

	// All other validation inputs are diagnostics only. The task shader already
	// fails open per meshlet when the pyramid cannot conservatively prove
	// occlusion, so quality toggles, ownership, camera motion, guard metadata,
	// and first-frame history must not latch Hi-Z off for the running editor.

	result.historyValid = true;
	result.occlusionAllowed = true;
	result.invalidReason = VulkanHiZInvalidReason::None;
	result.failOpen = false;
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
	VulkanHiZValidationResult latestValidation{};
	bool latestOcclusionAllowed = false;

	void Create(VkDevice device, VmaAllocator allocator, uint32_t screenWidth, uint32_t screenHeight, VkFormat format);
	void Destroy(VkDevice device, VmaAllocator allocator);
};
