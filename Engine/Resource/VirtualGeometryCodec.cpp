// VirtualGeometryCodec.cpp - Bounded compact-block validation and reference decode.

#include "VirtualGeometryCodec.h"

#include "meshoptimizer.h"

#include <cstring>
#include <limits>
#include <utility>

namespace TerraEngine::Resource
{
	namespace
	{
		bool IsRangeInside(std::size_t offset, std::size_t size, std::size_t total) { return offset <= total && size <= total - offset; }

		void SetError(VirtualGeometryCodecDecodeResult &result, std::string error)
		{
			result.bytes.clear();
			result.error = std::move(error);
		}

		bool DecodeTerraRunLength(std::span<const std::uint8_t> payload, std::size_t decodedSize, std::vector<std::uint8_t> &output)
		{
			std::size_t inputOffset = 0u;
			while (inputOffset < payload.size() && output.size() < decodedSize)
			{
				const std::uint8_t control = payload[inputOffset++];
				const std::size_t count = static_cast<std::size_t>((control & 0x7fu) + 1u);
				if (count > decodedSize - output.size())
				{
					return false;
				}
				if ((control & 0x80u) != 0u)
				{
					if (inputOffset >= payload.size())
					{
						return false;
					}
					output.insert(output.end(), count, payload[inputOffset++]);
				}
				else
				{
					if (!IsRangeInside(inputOffset, count, payload.size()))
					{
						return false;
					}
					output.insert(output.end(),
						payload.begin() + static_cast<std::ptrdiff_t>(inputOffset),
						payload.begin() + static_cast<std::ptrdiff_t>(inputOffset + count));
					inputOffset += count;
				}
			}
			return inputOffset == payload.size() && output.size() == decodedSize;
		}
	} // namespace

	VirtualGeometryCodecDecodeResult DecodeVirtualGeometryCompactBlock(std::span<const std::uint8_t> block, const VirtualGeometryCodecDecodeLimits &limits)
	{
		VirtualGeometryCodecDecodeResult result;
		if (block.size() < sizeof(VirtualGeometryCompactBlockHeader))
		{
			SetError(result, "Virtual geometry compact block is smaller than its header.");
			return result;
		}
		std::memcpy(&result.header, block.data(), sizeof(result.header));
		const VirtualGeometryCompactBlockHeader &header = result.header;
		if (header.magic != kVirtualGeometryCompactBlockMagicValue || header.version != kVirtualGeometryCompactBlockVersion ||
			header.headerSize != sizeof(VirtualGeometryCompactBlockHeader))
		{
			SetError(result, "Virtual geometry compact block has an unsupported header identity.");
			return result;
		}
		if (header.encodedByteSize > limits.maximumEncodedBytes || header.decodedByteSize > limits.maximumDecodedBytes || header.elementCount == 0u ||
			header.elementCount > limits.maximumElementCount || header.elementStride == 0u ||
			header.elementCount > header.decodedByteSize / header.elementStride ||
			static_cast<std::size_t>(header.elementCount) * header.elementStride != header.decodedByteSize)
		{
			SetError(result, "Virtual geometry compact block exceeds its declared decode limits.");
			return result;
		}
		if (!IsRangeInside(header.headerSize, header.encodedByteSize, block.size()) ||
			static_cast<std::size_t>(header.headerSize) + header.encodedByteSize != block.size())
		{
			SetError(result, "Virtual geometry compact block payload range is malformed.");
			return result;
		}
		if ((header.requiredCapabilities & ~limits.availableCapabilities) != 0u)
		{
			SetError(result, "Virtual geometry compact block requires unavailable decoder capabilities.");
			return result;
		}

		const std::span<const std::uint8_t> payload = block.subspan(header.headerSize, header.encodedByteSize);
		result.bytes.reserve(header.decodedByteSize);
		switch (static_cast<VirtualGeometryCompactCodec>(header.codec))
		{
		case VirtualGeometryCompactCodec::Expanded:
			if (header.requiredCapabilities != kVirtualGeometryCodecCapabilityExpanded || payload.size() != header.decodedByteSize)
			{
				SetError(result, "Expanded virtual geometry block metadata is inconsistent.");
				return result;
			}
			result.bytes.assign(payload.begin(), payload.end());
			break;
		case VirtualGeometryCompactCodec::Terra:
			if (header.requiredCapabilities != kVirtualGeometryCodecCapabilityTerra || !DecodeTerraRunLength(payload, header.decodedByteSize, result.bytes))
			{
				SetError(result, "Terra virtual geometry run-length stream is malformed.");
				return result;
			}
			break;
		case VirtualGeometryCompactCodec::Meshoptimizer:
			if (header.requiredCapabilities != kVirtualGeometryCodecCapabilityMeshoptimizer)
			{
				SetError(result, "meshoptimizer virtual geometry block metadata is inconsistent.");
				return result;
			}
			result.bytes.resize(header.decodedByteSize);
			if ((header.flags & kVirtualGeometryCompactBlockFlagMeshoptIndexStream) != 0u)
			{
				if (header.elementStride != sizeof(std::uint32_t) || (header.flags & kVirtualGeometryCompactBlockFlagMeshoptVertexStream) != 0u ||
					meshopt_decodeIndexBuffer(result.bytes.data(), header.elementCount, header.elementStride, payload.data(), payload.size()) != 0)
				{
					SetError(result, "meshoptimizer virtual geometry index stream is malformed.");
					return result;
				}
			}
			else if ((header.flags & kVirtualGeometryCompactBlockFlagMeshoptVertexStream) != 0u)
			{
				if (meshopt_decodeVertexBuffer(result.bytes.data(), header.elementCount, header.elementStride, payload.data(), payload.size()) != 0)
				{
					SetError(result, "meshoptimizer virtual geometry attribute stream is malformed.");
					return result;
				}
			}
			else
			{
				SetError(result, "meshoptimizer virtual geometry block does not identify its stream kind.");
				return result;
			}
			break;
		default:
			SetError(result, "Virtual geometry compact block names an unknown codec.");
			return result;
		}

		if (CalculateVirtualGeometryCompactHash(result.bytes) != header.decodedHash)
		{
			SetError(result, "Virtual geometry compact block failed its decoded integrity hash.");
		}
		return result;
	}

	bool ValidateVirtualGeometryExtensionDirectory(
		std::span<const std::uint8_t> directory, const VirtualGeometryCodecDecodeLimits &limits, std::string *outError)
	{
		auto Fail = [outError](std::string message)
		{
			if (outError != nullptr)
			{
				*outError = std::move(message);
			}
			return false;
		};
		if (directory.size() < sizeof(VirtualGeometryExtensionDirectoryHeader))
		{
			return Fail("Virtual geometry extension directory is smaller than its header.");
		}
		VirtualGeometryExtensionDirectoryHeader header{};
		std::memcpy(&header, directory.data(), sizeof(header));
		if (header.magic != kVirtualGeometryExtensionDirectoryMagicValue || header.version != kVirtualGeometryExtensionDirectoryVersion ||
			header.headerSize != sizeof(VirtualGeometryExtensionDirectoryHeader) || header.byteSize != directory.size())
		{
			return Fail("Virtual geometry extension directory header is inconsistent.");
		}
		const std::size_t entryBytes = static_cast<std::size_t>(header.entryCount) * sizeof(VirtualGeometryExtensionDirectoryEntry);
		if ((header.requiredCapabilities & ~limits.availableCapabilities) != 0u || header.entryCount > limits.maximumElementCount ||
			header.entryTableOffset < header.headerSize || !IsRangeInside(header.entryTableOffset, entryBytes, directory.size()))
		{
			return Fail("Virtual geometry extension directory exceeds bounds or required capabilities.");
		}
		const std::span<const std::uint8_t> entryBytesView = directory.subspan(header.entryTableOffset, entryBytes);
		if (header.directoryHash != CalculateVirtualGeometryCompactHash(entryBytesView))
		{
			return Fail("Virtual geometry extension directory failed its entry-table integrity hash.");
		}
		for (std::uint32_t index = 0u; index < header.entryCount; ++index)
		{
			VirtualGeometryExtensionDirectoryEntry entry{};
			const std::size_t entryOffset = static_cast<std::size_t>(header.entryTableOffset) + static_cast<std::size_t>(index) * sizeof(entry);
			std::memcpy(&entry, directory.data() + entryOffset, sizeof(entry));
			const bool knownType = entry.type <= static_cast<std::uint32_t>(VirtualGeometryExtensionType::MeshoptimizerBlock);
			if (entry.byteSize == 0u || entry.decodedByteSize > limits.maximumDecodedBytes ||
				!IsRangeInside(static_cast<std::size_t>(entry.byteOffset), entry.byteSize, directory.size()) ||
				(entry.requiredCapabilities & ~limits.availableCapabilities) != 0u ||
				(!knownType && (entry.flags & kVirtualGeometryExtensionFlagRequired) != 0u))
			{
				return Fail("Virtual geometry extension entry is malformed or requires unavailable capabilities.");
			}
		}
		return true;
	}
} // namespace TerraEngine::Resource
