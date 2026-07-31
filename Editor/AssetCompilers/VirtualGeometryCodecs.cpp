// VirtualGeometryCodecs.cpp - Deterministic expanded and Terra block encoders.

#include "VirtualGeometryCodecs.h"

#include <cstring>
#include <limits>

namespace TerraEngine::Editor
{
	namespace
	{
		void AppendBytes(std::vector<std::uint8_t> &destination, const void *source, std::size_t byteCount)
		{
			const std::size_t oldSize = destination.size();
			destination.resize(oldSize + byteCount);
			if (byteCount != 0u)
			{
				std::memcpy(destination.data() + oldSize, source, byteCount);
			}
		}

		std::vector<std::uint8_t> EncodeTerraRunLength(std::span<const std::uint8_t> source)
		{
			std::vector<std::uint8_t> encoded;
			encoded.reserve(source.size());
			for (std::size_t index = 0u; index < source.size();)
			{
				std::size_t runLength = 1u;
				while (index + runLength < source.size() && source[index + runLength] == source[index] && runLength < 128u)
				{
					++runLength;
				}
				if (runLength >= 3u)
				{
					encoded.push_back(static_cast<std::uint8_t>(0x80u | (runLength - 1u)));
					encoded.push_back(source[index]);
					index += runLength;
					continue;
				}

				const std::size_t literalBegin = index;
				index += runLength;
				while (index < source.size() && index - literalBegin < 128u)
				{
					std::size_t nextRun = 1u;
					while (index + nextRun < source.size() && source[index + nextRun] == source[index] && nextRun < 128u)
					{
						++nextRun;
					}
					if (nextRun >= 3u)
					{
						break;
					}
					// A two-byte run can straddle the 128-byte literal limit. Stop
					// before adding it so the high control bit remains the run/literal
					// discriminator instead of accidentally encoding a 129-byte literal.
					if (index - literalBegin + nextRun > 128u)
					{
						break;
					}
					index += nextRun;
				}
				const std::size_t literalSize = index - literalBegin;
				encoded.push_back(static_cast<std::uint8_t>(literalSize - 1u));
				encoded.insert(encoded.end(), source.begin() + static_cast<std::ptrdiff_t>(literalBegin), source.begin() + static_cast<std::ptrdiff_t>(index));
			}
			return encoded;
		}
	} // namespace

	VirtualGeometryEncodedBlock EncodeVirtualGeometryPrototypeBlock(VirtualGeometryPrototypeCodec codec, const VirtualGeometryCodecEncodeRequest &request)
	{
		VirtualGeometryEncodedBlock result;
		if (request.source.empty())
		{
			result.error = "Virtual geometry codec input cannot be empty.";
			return result;
		}
		if (request.source.size() > std::numeric_limits<std::uint32_t>::max())
		{
			result.error = "Virtual geometry codec input exceeds the v1 block byte range.";
			return result;
		}
		if (request.elementCount == 0u || request.elementStride == 0u)
		{
			result.error = "Virtual geometry codec input needs non-zero element metadata.";
			return result;
		}

		std::vector<std::uint8_t> payload;
		VirtualGeometryCompactCodec blockCodec = VirtualGeometryCompactCodec::Expanded;
		std::uint64_t requiredCapability = kVirtualGeometryCodecCapabilityExpanded;
		if (codec == VirtualGeometryPrototypeCodec::Expanded)
		{
			payload.assign(request.source.begin(), request.source.end());
		}
		else
		{
			payload = EncodeTerraRunLength(request.source);
			blockCodec = VirtualGeometryCompactCodec::Terra;
			requiredCapability = kVirtualGeometryCodecCapabilityTerra;
		}
		if (payload.size() > std::numeric_limits<std::uint32_t>::max())
		{
			result.error = "Virtual geometry encoded payload exceeds the v1 block byte range.";
			return result;
		}

		const VirtualGeometryCompactBlockHeader header{ kVirtualGeometryCompactBlockMagicValue,
			kVirtualGeometryCompactBlockVersion,
			static_cast<std::uint16_t>(sizeof(VirtualGeometryCompactBlockHeader)),
			static_cast<std::uint32_t>(blockCodec),
			request.flags,
			static_cast<std::uint32_t>(request.source.size()),
			static_cast<std::uint32_t>(payload.size()),
			request.elementCount,
			request.elementStride,
			requiredCapability,
			CalculateVirtualGeometryCompactHash(request.source) };
		AppendBytes(result.bytes, &header, sizeof(header));
		AppendBytes(result.bytes, payload.data(), payload.size());
		return result;
	}
} // namespace TerraEngine::Editor
