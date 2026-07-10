// VulkanReadbackConversion.cpp - Validated Vulkan capture pixel conversion.

#include "VulkanReadbackConversion.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace TerraEngine::Renderer::VulkanReadback
{
	namespace
	{
		// Vulkan half floats use the IEEE 754 binary16 layout. Decoding the bits
		// explicitly avoids adding a platform-specific half-float dependency.
		float HalfFloatToFloat(std::uint16_t value)
		{
			const std::uint32_t sign = (static_cast<std::uint32_t>(value) & 0x8000u) << 16u;
			const std::uint32_t exponent = (static_cast<std::uint32_t>(value) >> 10u) & 0x1fu;
			std::uint32_t mantissa = static_cast<std::uint32_t>(value) & 0x03ffu;
			std::uint32_t bits = sign;

			if (exponent == 0u)
			{
				if (mantissa != 0u)
				{
					int adjustedExponent = -14;
					while ((mantissa & 0x0400u) == 0u)
					{
						mantissa <<= 1u;
						--adjustedExponent;
					}
					mantissa &= 0x03ffu;
					bits = sign
						| (static_cast<std::uint32_t>(adjustedExponent + 127) << 23u)
						| (mantissa << 13u);
				}
			}
			else if (exponent == 0x1fu)
			{
				bits = sign | 0x7f800000u | (mantissa << 13u);
			}
			else
			{
				bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
			}

			float decoded = 0.0f;
			std::memcpy(&decoded, &bits, sizeof(decoded));
			return decoded;
		}

		std::uint8_t LinearColorToDisplayByte(float value)
		{
			if (!std::isfinite(value))
			{
				return 0u;
			}
			const float clamped = std::clamp(value, 0.0f, 1.0f);
			const float gammaEncoded = std::pow(clamped, 1.0f / 2.2f);
			return static_cast<std::uint8_t>(std::clamp(gammaEncoded * 255.0f + 0.5f, 0.0f, 255.0f));
		}

		std::uint8_t LinearAlphaToByte(float value)
		{
			if (!std::isfinite(value))
			{
				return 255u;
			}
			const float clamped = std::clamp(value, 0.0f, 1.0f);
			return static_cast<std::uint8_t>(std::clamp(clamped * 255.0f + 0.5f, 0.0f, 255.0f));
		}
	}

	std::uint32_t BytesPerPixel(VkFormat format)
	{
		switch (format)
		{
		case VK_FORMAT_R8G8B8A8_UNORM:
		case VK_FORMAT_R8G8B8A8_SRGB:
		case VK_FORMAT_B8G8R8A8_UNORM:
		case VK_FORMAT_B8G8R8A8_SRGB:
			return 4u;
		case VK_FORMAT_R16G16B16A16_SFLOAT:
			return 8u;
		default:
			return 0u;
		}
	}

	std::vector<std::uint8_t> ConvertToRgba8(
		std::span<const std::uint8_t> sourceBytes,
		std::uint32_t width,
		std::uint32_t height,
		VkFormat format)
	{
		const std::uint32_t bytesPerPixel = BytesPerPixel(format);
		if (bytesPerPixel == 0u)
		{
			throw std::invalid_argument("Unsupported Vulkan readback format.");
		}

		const std::uint64_t pixelCount64 = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
		const std::uint64_t sourceSize64 = pixelCount64 * bytesPerPixel;
		const std::uint64_t outputSize64 = pixelCount64 * 4u;
		if (sourceSize64 > std::numeric_limits<std::size_t>::max()
			|| outputSize64 > std::numeric_limits<std::size_t>::max())
		{
			throw std::invalid_argument("Vulkan readback dimensions exceed host addressable memory.");
		}
		if (sourceBytes.size() != static_cast<std::size_t>(sourceSize64))
		{
			throw std::invalid_argument("Vulkan readback byte count does not match its dimensions and format.");
		}

		std::vector<std::uint8_t> rgba(static_cast<std::size_t>(outputSize64));
		if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB)
		{
			std::copy(sourceBytes.begin(), sourceBytes.end(), rgba.begin());
			return rgba;
		}

		if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB)
		{
			for (std::size_t pixelIndex = 0u; pixelIndex < static_cast<std::size_t>(pixelCount64); ++pixelIndex)
			{
				const std::size_t offset = pixelIndex * 4u;
				rgba[offset + 0u] = sourceBytes[offset + 2u];
				rgba[offset + 1u] = sourceBytes[offset + 1u];
				rgba[offset + 2u] = sourceBytes[offset + 0u];
				rgba[offset + 3u] = sourceBytes[offset + 3u];
			}
			return rgba;
		}

		for (std::size_t pixelIndex = 0u; pixelIndex < static_cast<std::size_t>(pixelCount64); ++pixelIndex)
		{
			const std::size_t sourceOffset = pixelIndex * 8u;
			const std::size_t targetOffset = pixelIndex * 4u;
			const auto readHalf = [&](std::size_t byteOffset) {
				return static_cast<std::uint16_t>(
					static_cast<std::uint16_t>(sourceBytes[sourceOffset + byteOffset])
					| (static_cast<std::uint16_t>(sourceBytes[sourceOffset + byteOffset + 1u]) << 8u));
			};
			rgba[targetOffset + 0u] = LinearColorToDisplayByte(HalfFloatToFloat(readHalf(0u)));
			rgba[targetOffset + 1u] = LinearColorToDisplayByte(HalfFloatToFloat(readHalf(2u)));
			rgba[targetOffset + 2u] = LinearColorToDisplayByte(HalfFloatToFloat(readHalf(4u)));
			rgba[targetOffset + 3u] = LinearAlphaToByte(HalfFloatToFloat(readHalf(6u)));
		}
		return rgba;
	}
}
