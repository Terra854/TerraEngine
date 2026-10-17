#include "DecodedCaptureAssertions.h"

#include <encoder/pvpngreader.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>

namespace TerraEngine::TestSupport
{
	namespace
	{
		std::uint32_t BigEndian(std::span<const std::uint8_t> bytes, std::size_t offset)
		{
			return (std::uint32_t(bytes[offset]) << 24u) | (std::uint32_t(bytes[offset + 1]) << 16u) |
				(std::uint32_t(bytes[offset + 2]) << 8u) | std::uint32_t(bytes[offset + 3]);
		}

		std::uint32_t ChunkCrc(std::span<const std::uint8_t> bytes)
		{
			// Independent standard PNG CRC-32 implementation. Do not call the
			// producer's CRC routine: a shared mistake would invalidate this oracle.
			static constexpr auto table = []
			{
				std::array<std::uint32_t, 256> entries{};
				for (std::uint32_t i = 0; i < 256; ++i)
				{
					auto value = i;
					for (unsigned bit = 0; bit < 8; ++bit) value = (value >> 1u) ^ ((value & 1u) ? 0xedb88320u : 0u);
					entries[i] = value;
				}
				return entries;
			}();
			std::uint32_t value = 0xffffffffu;
			for (const auto byte : bytes) value = table[(value ^ byte) & 255u] ^ (value >> 8u);
			return value ^ 0xffffffffu;
		}

		std::vector<std::uint8_t> DecodeBase64(std::string_view encoded)
		{
			if (encoded.empty() || encoded.size() % 4 != 0) throw std::runtime_error("capture Base64 length is not canonical");
			// A constant-time alphabet lookup per byte avoids scanning 64 symbols
			// repeatedly for large captures. '=' is handled only in the final group.
			static constexpr auto table = []
			{
				std::array<int, 256> values{};
				values.fill(-1);
				constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
				for (std::size_t i = 0; i < alphabet.size(); ++i) values[static_cast<unsigned char>(alphabet[i])] = static_cast<int>(i);
				return values;
			}();
			const auto groups = encoded.size() / 4;
			const std::size_t padding = encoded.back() != '=' ? 0 : (encoded[encoded.size() - 2] == '=' ? 2 : 1);
			if (groups > std::numeric_limits<std::size_t>::max() / 3) throw std::runtime_error("capture Base64 size overflow");
			const auto outputSize = groups * 3 - padding;
			std::vector<std::uint8_t> output;
			if (outputSize > output.max_size()) throw std::runtime_error("capture Base64 exceeds vector capacity");
			output.resize(outputSize);
			std::size_t write = 0;
			for (std::size_t group = 0; group < groups; ++group)
			{
				const auto pos = group * 4;
				std::array<unsigned, 4> values{};
				const auto localPadding = group + 1 == groups ? padding : 0;
				for (std::size_t i = 0; i < 4; ++i)
				{
					const auto c = static_cast<unsigned char>(encoded[pos + i]);
					if (i >= 4 - localPadding)
					{
						if (c != '=') throw std::runtime_error("capture Base64 padding is misplaced");
					}
					else
					{
						if (table[c] < 0) throw std::runtime_error("capture Base64 contains invalid alphabet or padding");
						values[i] = static_cast<unsigned>(table[c]);
					}
				}
				if ((localPadding == 2 && (values[1] & 15u)) || (localPadding == 1 && (values[2] & 3u)))
					throw std::runtime_error("capture Base64 has nonzero unused padding bits");
				const auto bits = (values[0] << 18u) | (values[1] << 12u) | (values[2] << 6u) | values[3];
				output[write++] = static_cast<std::uint8_t>(bits >> 16u);
				if (localPadding < 2) output[write++] = static_cast<std::uint8_t>(bits >> 8u);
				if (localPadding == 0) output[write++] = static_cast<std::uint8_t>(bits);
			}
			return output;
		}
	}

	DecodedCapture DecodePng(std::span<const std::uint8_t> png)
	{
		constexpr std::array<std::uint8_t, 8> signature = {137, 80, 78, 71, 13, 10, 26, 10};
		// Preflight before entering the bundled decoder, whose RGBA pitch uses a
		// uint32 and whose allocation representation is limited to INT32_MAX.
		if (png.size() < 46 || !std::equal(signature.begin(), signature.end(), png.begin()) ||
			BigEndian(png, 8) != 13 || png[12] != 'I' || png[13] != 'H' || png[14] != 'D' || png[15] != 'R')
			throw std::runtime_error("capture is not a PNG with a complete first IHDR");
		// Require every chunk's CRC, including terminal IEND: the bundled row
		// decoder need not consume trailer chunks after returning its last row.
		bool ended = false;
		for (std::size_t offset = 8; offset < png.size();)
		{
			if (png.size() - offset < 12) throw std::runtime_error("capture PNG chunk is truncated");
			const auto length = BigEndian(png, offset);
			if (length > 0x7fffffffu || length > png.size() - offset - 12)
				throw std::runtime_error("capture PNG chunk length exceeds its envelope");
			if (ChunkCrc(png.subspan(offset + 4, static_cast<std::size_t>(length) + 4)) != BigEndian(png, offset + 8 + length))
				throw std::runtime_error("capture PNG chunk checksum is invalid");
			const bool iend = png[offset + 4] == 'I' && png[offset + 5] == 'E' && png[offset + 6] == 'N' && png[offset + 7] == 'D';
			offset += static_cast<std::size_t>(length) + 12;
			if (iend)
			{
				if (length != 0 || offset != png.size()) throw std::runtime_error("capture PNG has invalid IEND or trailing bytes");
				ended = true; break;
			}
		}
		if (!ended) throw std::runtime_error("capture PNG has no terminal IEND");
		const auto expectedWidth = BigEndian(png, 16), expectedHeight = BigEndian(png, 20);
		if (!expectedWidth || !expectedHeight || expectedWidth > std::numeric_limits<std::uint32_t>::max() / 4)
			throw std::runtime_error("capture PNG dimensions cannot form an RGBA row");
		const auto bytes = std::uint64_t(expectedWidth) * 4u * expectedHeight;
		if (bytes > 0x7fffffffu || bytes > std::numeric_limits<std::size_t>::max() || bytes > std::vector<std::uint8_t>().max_size())
			throw std::runtime_error("capture PNG dimensions exceed independent decoder capacity");
		std::uint32_t width = 0, height = 0, originalChannels = 0;
		// pv_png returns malloc-owned storage. Always free it, including when the
		// destination vector allocation throws. Its existing CRC/Adler checks stay
		// enabled in the unchanged bundled basisu implementation.
		std::unique_ptr<void, decltype(&std::free)> pixels(
			pv_png::load_png(png.data(), png.size(), 4, width, height, originalChannels), &std::free);
		if (!pixels || width != expectedWidth || height != expectedHeight)
			throw std::runtime_error("independent PNG decoder rejected capture pixels or checksums");
		DecodedCapture result{width, height, {}};
		const auto* first = static_cast<const std::uint8_t*>(pixels.get());
		result.rgba.assign(first, first + static_cast<std::size_t>(bytes));
		return result;
	}

	DecodedCapture DecodeCaptureBase64(std::string_view encoded)
	{
		const auto png = DecodeBase64(encoded);
		return DecodePng(png);
	}
}
