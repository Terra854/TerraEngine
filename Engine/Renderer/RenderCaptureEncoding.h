#pragma once

#include "Core/Export.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Renderer {

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
