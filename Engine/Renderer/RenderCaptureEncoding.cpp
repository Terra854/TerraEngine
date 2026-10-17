#include "Renderer/RenderCaptureEncoding.h"

#include <array>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace TerraEngine::Renderer {
namespace {

	// Counts are validated before arithmetic, including platforms with narrower
	// size_t/ptrdiff_t. No image-resolution policy is imposed by these limits.
	std::size_t CheckedAdd(std::size_t left, std::size_t right, std::size_t limit, const char* reason = "Capture storage size is not representable.")
	{
		if (left > limit || right > limit - left) throw std::length_error(reason);
		return left + right;
	}
	std::size_t CheckedMultiply(std::size_t left, std::size_t right, std::size_t limit, const char* reason = "Capture storage size is not representable.")
	{
		if (right != 0 && left > limit / right) throw std::length_error(reason);
		return left * right;
	}
	std::size_t ByteLimit()
	{
		return std::min(std::vector<std::uint8_t>().max_size(), static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()));
	}
	std::size_t RgbaByteCount(std::uint32_t width, std::uint32_t height)
	{
		if (width == 0 || height == 0) throw std::invalid_argument("Capture dimensions must be non-zero.");
		return CheckedMultiply(CheckedMultiply(width, height, ByteLimit()), 4, ByteLimit(), "RGBA capture byte count exceeds storage capacity.");
	}

	void AppendUint32BigEndian(std::vector<std::uint8_t>& out, std::uint32_t value)
	{
		out.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xffu));
		out.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xffu));
		out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
		out.push_back(static_cast<std::uint8_t>(value & 0xffu));
	}

	std::uint32_t UpdateCrc32(std::uint32_t crc, std::uint8_t byte)
	{
		crc ^= byte;
		for (std::uint32_t bit = 0u; bit < 8u; ++bit)
		{
			const std::uint32_t mask = 0u - (crc & 1u);
			crc = (crc >> 1u) ^ (0xedb88320u & mask);
		}
		return crc;
	}

	std::uint32_t CalculateChunkCrc(std::span<const std::uint8_t, 4> chunkType, std::span<const std::uint8_t> chunkData)
	{
		std::uint32_t crc = 0xffffffffu;
		for (std::uint8_t byte : chunkType)
		{
			crc = UpdateCrc32(crc, byte);
		}
		for (std::uint8_t byte : chunkData)
		{
			crc = UpdateCrc32(crc, byte);
		}
		return crc ^ 0xffffffffu;
	}

	void AppendChunk(std::vector<std::uint8_t>& png, std::span<const std::uint8_t, 4> chunkType, std::span<const std::uint8_t> chunkData)
	{
		if (chunkData.size() > 0x7fffffffu)
		{
			throw std::invalid_argument("PNG chunk is too large to encode.");
		}

		(void)CheckedAdd(png.size(), CheckedAdd(chunkData.size(), 12, ByteLimit()), ByteLimit());
		AppendUint32BigEndian(png, static_cast<std::uint32_t>(chunkData.size()));
		png.insert(png.end(), chunkType.begin(), chunkType.end());
		png.insert(png.end(), chunkData.begin(), chunkData.end());
		AppendUint32BigEndian(png, CalculateChunkCrc(chunkType, chunkData));
	}

	std::uint32_t CalculateAdler32(std::span<const std::uint8_t> bytes)
	{
		constexpr std::uint32_t kModAdler = 65521u;
		std::uint32_t a = 1u;
		std::uint32_t b = 0u;
		for (std::uint8_t byte : bytes)
		{
			a = (a + byte) % kModAdler;
			b = (b + a) % kModAdler;
		}
		return (b << 16u) | a;
	}

	void AppendZlibStoredStream(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> raw)
	{
		// 0x78 0x01 is a valid zlib header for DEFLATE with fastest/no compression.
		out.push_back(0x78u);
		out.push_back(0x01u);

		std::size_t offset = 0u;
		while (offset < raw.size() || raw.empty())
		{
			const std::size_t remaining = raw.size() - offset;
			const std::uint16_t blockSize = static_cast<std::uint16_t>(std::min<std::size_t>(remaining, 65535u));
			const bool finalBlock = offset + blockSize >= raw.size();
			out.push_back(finalBlock ? 0x01u : 0x00u);
			out.push_back(static_cast<std::uint8_t>(blockSize & 0xffu));
			out.push_back(static_cast<std::uint8_t>((blockSize >> 8u) & 0xffu));
			const std::uint16_t inverseSize = static_cast<std::uint16_t>(~blockSize);
			out.push_back(static_cast<std::uint8_t>(inverseSize & 0xffu));
			out.push_back(static_cast<std::uint8_t>((inverseSize >> 8u) & 0xffu));
			out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset), raw.begin() + static_cast<std::ptrdiff_t>(offset + blockSize));
			offset += blockSize;

			if (raw.empty())
			{
				break;
			}
		}

		AppendUint32BigEndian(out, CalculateAdler32(raw));
	}

}

std::size_t ComputeBase64EncodedSize(std::size_t byteCount)
{
	const auto groups = CheckedAdd(byteCount / 3, byteCount % 3 != 0 ? 1 : 0, std::numeric_limits<std::size_t>::max());
	return CheckedMultiply(groups, 4, std::string().max_size(), "Base64 output exceeds string capacity.");
}

Rgba8PngLayout ValidateRgba8PngLayout(std::uint32_t width, std::uint32_t height)
{
	// PNG dimensions and chunk lengths are unsigned 31-bit values. This encoder
	// emits one IDAT chunk, so its complete stored stream must fit that chunk.
	if (width > 0x7fffffffu || height > 0x7fffffffu) throw std::invalid_argument("PNG dimensions exceed the format bounds.");
	Rgba8PngLayout layout;
	const auto limit = ByteLimit();
	layout.rgbaBytes = RgbaByteCount(width, height);
	layout.rowBytes = CheckedMultiply(width, 4, limit);
	layout.filteredBytes = CheckedMultiply(CheckedAdd(layout.rowBytes, 1, limit), height, limit, "PNG filtered rows exceed storage capacity.");
	const auto blocks = CheckedAdd(layout.filteredBytes / 65535, layout.filteredBytes % 65535 != 0 ? 1 : 0, limit);
	layout.zlibBytes = CheckedAdd(CheckedAdd(layout.filteredBytes, CheckedMultiply(blocks, 5, limit), limit, "PNG DEFLATE stream exceeds storage capacity."), 6, limit, "PNG DEFLATE stream exceeds storage capacity.");
	if (layout.zlibBytes > 0x7fffffffu) throw std::length_error("PNG stored stream exceeds its IDAT chunk bounds.");
	layout.pngBytes = CheckedAdd(layout.zlibBytes, 57, limit);
	layout.base64Bytes = ComputeBase64EncodedSize(layout.pngBytes);
	return layout;
}

std::vector<std::uint8_t> ResizeRgba8(
	std::span<const std::uint8_t> rgbaPixels,
	std::uint32_t sourceWidth,
	std::uint32_t sourceHeight,
	std::uint32_t targetWidth,
	std::uint32_t targetHeight)
{
	if (sourceWidth == 0u || sourceHeight == 0u || targetWidth == 0u || targetHeight == 0u)
	{
		throw std::invalid_argument("RGBA resize dimensions must be non-zero.");
	}

	const std::size_t sourceBytes = RgbaByteCount(sourceWidth, sourceHeight);
	const std::size_t targetBytes = RgbaByteCount(targetWidth, targetHeight);
	if (rgbaPixels.size() != sourceBytes)
	{
		throw std::invalid_argument("RGBA resize byte count does not match the source dimensions.");
	}

	if (sourceWidth == targetWidth && sourceHeight == targetHeight)
	{
		return std::vector<std::uint8_t>(rgbaPixels.begin(), rgbaPixels.end());
	}

	std::vector<std::uint8_t> resized(static_cast<std::size_t>(targetBytes));
	// Capture completion owns one immutable pixel buffer and publishes one result,
	// so this bounded pass stays sequential at the protected readback boundary.
	// It avoids task scheduling overhead for an operation that runs only on an
	// explicitly requested capture, never on the normal per-frame render path.
	for (std::uint32_t targetY = 0u; targetY < targetHeight; ++targetY)
	{
		const std::uint32_t sourceY = static_cast<std::uint32_t>(
			(static_cast<std::uint64_t>(targetY) * sourceHeight) / targetHeight);
		for (std::uint32_t targetX = 0u; targetX < targetWidth; ++targetX)
		{
			const std::uint32_t sourceX = static_cast<std::uint32_t>(
				(static_cast<std::uint64_t>(targetX) * sourceWidth) / targetWidth);
			const std::size_t sourceOffset = (static_cast<std::size_t>(sourceY) * sourceWidth + sourceX) * 4u;
			const std::size_t targetOffset = (static_cast<std::size_t>(targetY) * targetWidth + targetX) * 4u;
			for (std::size_t channel = 0u; channel < 4u; ++channel)
			{
				resized[targetOffset + channel] = rgbaPixels[sourceOffset + channel];
			}
		}
	}
	return resized;
}

std::vector<std::uint8_t> EncodeRgba8ToPng(
	std::span<const std::uint8_t> rgbaPixels,
	std::uint32_t width,
	std::uint32_t height)
{
	const auto layout = ValidateRgba8PngLayout(width, height);
	if (rgbaPixels.size() != layout.rgbaBytes)
	{
		throw std::invalid_argument("RGBA capture byte count does not match the requested dimensions.");
	}
	const std::size_t rowBytes = layout.rowBytes;
	std::vector<std::uint8_t> raw;
	raw.reserve(layout.filteredBytes);
	for (std::uint32_t y = 0u; y < height; ++y)
	{
		raw.push_back(0u);
		const std::size_t rowOffset = static_cast<std::size_t>(y) * rowBytes;
		raw.insert(raw.end(), rgbaPixels.begin() + static_cast<std::ptrdiff_t>(rowOffset), rgbaPixels.begin() + static_cast<std::ptrdiff_t>(rowOffset + rowBytes));
	}

	std::vector<std::uint8_t> png;
	png.reserve(layout.pngBytes);
	constexpr std::array<std::uint8_t, 8> kSignature = { 137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u };
	png.insert(png.end(), kSignature.begin(), kSignature.end());

	std::array<std::uint8_t, 13> ihdr{};
	ihdr[0] = static_cast<std::uint8_t>((width >> 24u) & 0xffu);
	ihdr[1] = static_cast<std::uint8_t>((width >> 16u) & 0xffu);
	ihdr[2] = static_cast<std::uint8_t>((width >> 8u) & 0xffu);
	ihdr[3] = static_cast<std::uint8_t>(width & 0xffu);
	ihdr[4] = static_cast<std::uint8_t>((height >> 24u) & 0xffu);
	ihdr[5] = static_cast<std::uint8_t>((height >> 16u) & 0xffu);
	ihdr[6] = static_cast<std::uint8_t>((height >> 8u) & 0xffu);
	ihdr[7] = static_cast<std::uint8_t>(height & 0xffu);
	ihdr[8] = 8u;
	ihdr[9] = 6u;
	ihdr[10] = 0u;
	ihdr[11] = 0u;
	ihdr[12] = 0u;

	constexpr std::array<std::uint8_t, 4> kIhdr = { 'I', 'H', 'D', 'R' };
	constexpr std::array<std::uint8_t, 4> kIdat = { 'I', 'D', 'A', 'T' };
	constexpr std::array<std::uint8_t, 4> kIend = { 'I', 'E', 'N', 'D' };
	AppendChunk(png, std::span<const std::uint8_t, 4>(kIhdr), ihdr);

	std::vector<std::uint8_t> zlibData;
	zlibData.reserve(layout.zlibBytes);
	AppendZlibStoredStream(zlibData, raw);
	AppendChunk(png, std::span<const std::uint8_t, 4>(kIdat), zlibData);
	AppendChunk(png, std::span<const std::uint8_t, 4>(kIend), {});
	return png;
}

std::string EncodeBase64(std::span<const std::uint8_t> bytes)
{
	constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string encoded;
	encoded.reserve(ComputeBase64EncodedSize(bytes.size()));

	for (std::size_t index = 0u; index < bytes.size();)
	{
		const auto remaining = bytes.size() - index;
		const std::uint32_t b0 = bytes[index];
		const std::uint32_t b1 = (remaining > 1u) ? bytes[index + 1u] : 0u;
		const std::uint32_t b2 = (remaining > 2u) ? bytes[index + 2u] : 0u;
		const std::uint32_t packed = (b0 << 16u) | (b1 << 8u) | b2;

		encoded.push_back(kAlphabet[(packed >> 18u) & 0x3fu]);
		encoded.push_back(kAlphabet[(packed >> 12u) & 0x3fu]);
		encoded.push_back(remaining > 1u ? kAlphabet[(packed >> 6u) & 0x3fu] : '=');
		encoded.push_back(remaining > 2u ? kAlphabet[packed & 0x3fu] : '=');
		index += std::min<std::size_t>(remaining, 3);
	}

	return encoded;
}

}
