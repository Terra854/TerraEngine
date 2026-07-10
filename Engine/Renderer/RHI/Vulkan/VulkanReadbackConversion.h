// VulkanReadbackConversion.h - CPU conversion helpers for Vulkan image captures.
//
// Keeping conversion independent from live Vulkan objects makes channel order,
// byte-count validation, and HDR handling deterministic and directly testable.

#pragma once

#include "Core/Export.h"
#include "VulkanCommonIncludes.h"

#include <cstdint>
#include <span>
#include <vector>

namespace TerraEngine::Renderer::VulkanReadback
{
	// Returns the tightly packed source pixel size accepted by the capture path.
	// A zero result means the Vulkan format is intentionally unsupported.
	[[nodiscard]] TERRAENGINE_API std::uint32_t BytesPerPixel(VkFormat format);

	// Converts one tightly packed Vulkan color image into RGBA8 bytes suitable
	// for the engine PNG encoder. Invalid dimensions, byte counts, and formats
	// throw std::invalid_argument instead of risking an out-of-bounds read.
	[[nodiscard]] TERRAENGINE_API std::vector<std::uint8_t> ConvertToRgba8(
		std::span<const std::uint8_t> sourceBytes,
		std::uint32_t width,
		std::uint32_t height,
		VkFormat format);
}
