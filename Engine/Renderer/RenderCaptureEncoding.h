#pragma once

#include "Core/Export.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Renderer {

	// Resizes tightly packed RGBA8 pixels with nearest-neighbor sampling. Capture
	// requests use this bounded conversion when a live render target differs from
	// the exact output dimensions promised to editor automation.
	TERRAENGINE_API std::vector<std::uint8_t> ResizeRgba8(
		std::span<const std::uint8_t> rgbaPixels,
		std::uint32_t sourceWidth,
		std::uint32_t sourceHeight,
		std::uint32_t targetWidth,
		std::uint32_t targetHeight);

	// Encodes tightly packed RGBA8 pixels into a PNG byte stream. The encoder uses
	// PNG's standard no-filter rows and uncompressed DEFLATE blocks so captures do
	// not need any optional image library at runtime.
	TERRAENGINE_API std::vector<std::uint8_t> EncodeRgba8ToPng(
		std::span<const std::uint8_t> rgbaPixels,
		std::uint32_t width,
		std::uint32_t height);

	// Converts arbitrary bytes to RFC 4648 Base64 without inserting whitespace,
	// which matches the MCP image-content payload contract.
	TERRAENGINE_API std::string EncodeBase64(std::span<const std::uint8_t> bytes);

}
