// Optional owned MAX pyramid built exclusively from this command's geometry.
#pragma once
#include "VulkanPostProcessScreenSpaceLighting.h"
#include <span>

namespace TerraEngine::Renderer::Vulkan
{
	struct ScreenSpaceLightingHiZ;
	struct ScreenSpaceLightingHiZCreateInfo
	{
		VkPhysicalDevice physicalDevice=VK_NULL_HANDLE;
		VkDevice device=VK_NULL_HANDLE;
		VmaAllocator allocator=nullptr;
		ScreenSpaceInputImage depth;
		std::span<const std::uint32_t> shader; // Actual bundled HiZGenerate main.
	};
	// Null is optional acceleration failure, never a screen-lighting capability.
	// Caller retains the genuine depth/raster owner through GPU completion.
	std::shared_ptr<ScreenSpaceLightingHiZ> PrepareScreenSpaceLightingHiZ(
		const ScreenSpaceLightingHiZCreateInfo& info,std::string& reason) noexcept;
	const ScreenSpaceInputImage& DescribeScreenSpaceLightingHiZ(const ScreenSpaceLightingHiZ& owner) noexcept;
	bool CanRecordScreenSpaceLightingHiZ(VkCommandBuffer command,const ScreenSpaceLightingHiZ& owner,
		const ScreenSpaceGeometryRecordedUse& geometry) noexcept;
	// Depth has already been transitioned to compute depth-read by the caller.
	// All owned mip layouts are discarded/rebuilt each recording, never previous
	// depth reused. Output proof is same-command/frame; parent owns real submit.
	bool RecordScreenSpaceLightingHiZ(VkCommandBuffer command,const ScreenSpaceLightingHiZ& owner,
		const ScreenSpaceGeometryRecordedUse& geometry,ScreenSpaceHiZRecordedUse& recorded,
		std::uint32_t& dispatches) noexcept;
}
