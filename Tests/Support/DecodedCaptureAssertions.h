#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace TerraEngine::TestSupport
{
	// Independent decoder output uses tightly packed RGBA8, even when the PNG
	// stores grayscale or RGB. Tests compare these pixels with promised metadata.
	struct DecodedCapture
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::vector<std::uint8_t> rgba;
	};

	// Invalid PNG/Base64 input throws std::runtime_error. Allocation failures may
	// propagate normally; these validation helpers do not manufacture a success.
	DecodedCapture DecodePng(std::span<const std::uint8_t> png);
	DecodedCapture DecodeCaptureBase64(std::string_view encoded);
}
