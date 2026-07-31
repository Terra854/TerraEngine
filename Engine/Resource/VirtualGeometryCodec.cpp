// VirtualGeometryCodec.cpp - Bounded compact-block validation and reference decode.

#include "VirtualGeometryCodec.h"

#include "meshoptimizer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>

namespace TerraEngine::Resource
{
	namespace
	{
		bool IsRangeInside(std::size_t offset, std::size_t size, std::size_t total) { return offset <= total && size <= total - offset; }

		bool IsPowerOfTwo(std::uint32_t value)
		{
			return value != 0u && (value & (value - 1u)) == 0u;
		}

		bool IsSupportedPageSize(std::uint32_t value)
		{
			return value == kTemfPageSize32KiB || value == kTemfPageSize64KiB
				|| value == kTemfPageSize128KiB;
		}

		bool TryByteCount(std::uint32_t count, std::size_t stride, std::size_t& outBytes)
		{
			if(count > std::numeric_limits<std::size_t>::max() / stride)
			{
				return false;
			}
			outBytes = static_cast<std::size_t>(count) * stride;
			return true;
		}

		void SetProductionError(
			TemfProductionValidationResult& result,
			TemfProductionValidationCode code,
			std::string message)
		{
			result.code = code;
			result.directory = {};
			result.diagnostic = std::move(message);
		}

		void SetProductionDecodeError(
			TemfProductionBlockDecodeResult& result,
			TemfProductionValidationCode code,
			std::string message)
		{
			result.bytes.clear();
			result.code = code;
			result.diagnostic = std::move(message);
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

	TemfProductionValidationResult ValidateTemfProductionFile(
		std::span<const std::uint8_t> fileBytes,
		const TemfProductionValidationLimits& limits)
	{
		TemfProductionValidationResult result;
		if(fileBytes.size() < sizeof(TemfProductionHeader))
		{
			SetProductionError(result, TemfProductionValidationCode::RecookRequired,
				"The geometry file does not contain the canonical TEMF production header; recook-required.");
			return result;
		}

		TemfProductionHeader header{};
		std::memcpy(&header, fileBytes.data(), sizeof(header));
		if(header.magic != kMeshMagicValue || header.formatVersion != kMeshCurrentVersion
			|| header.minorVersion != kMeshCurrentMinorVersion
			|| header.headerBytes != kTemfProductionHeaderBytes
			|| header.formatContractId != kTemfProductionContractId)
		{
			SetProductionError(result, TemfProductionValidationCode::RecookRequired,
				"The geometry file is not TEMFPRD1 canonical data; recook-required.");
			return result;
		}
		if((header.representationFlags & ~kTemfRepresentationKnownMask) != 0u
			|| (header.representationFlags & (kTemfRepresentationDirectMeshlets | kTemfRepresentationVirtualGeometry)) == 0u)
		{
			SetProductionError(result, TemfProductionValidationCode::InvalidRepresentation,
				"TEMFPRD1 representation flags are unknown or do not select a renderable representation.");
			return result;
		}
		const bool directOnly = (header.representationFlags & kTemfRepresentationDirectMeshlets) != 0u
			&& (header.representationFlags & kTemfRepresentationVirtualGeometry) == 0u;
		if((directOnly && (header.logicalPageSizeBytes != 0u || header.pageCount != 0u
				|| header.pageTableOffset != 0u))
			|| (!directOnly && !IsSupportedPageSize(header.logicalPageSizeBytes))
			|| header.pageCount > limits.maximumPageCount
			|| header.blockCount == 0u || header.blockCount > limits.maximumBlockCount
			|| header.quantizationDomainCount > limits.maximumQuantizationDomainCount
			|| header.dependencyCount > limits.maximumDependencyCount)
		{
			SetProductionError(result, TemfProductionValidationCode::LimitExceeded,
				"TEMFPRD1 page, block, domain, dependency, or page-size limits are invalid.");
			return result;
		}
		if((header.requiredCapabilityMask & ~limits.availableCapabilities) != 0u)
		{
			SetProductionError(result, TemfProductionValidationCode::UnknownRequiredCapability,
				"TEMFPRD1 requires a decoder capability unavailable on this runtime.");
			return result;
		}

		std::size_t pageBytes = 0u;
		std::size_t blockBytes = 0u;
		std::size_t domainBytes = 0u;
		std::size_t dependencyBytes = 0u;
		if(!TryByteCount(header.pageCount, sizeof(TemfPageDescriptor), pageBytes)
			|| !TryByteCount(header.blockCount, sizeof(TemfBlockDescriptor), blockBytes)
			|| !TryByteCount(header.quantizationDomainCount, sizeof(TemfQuantizationDomain), domainBytes)
			|| !TryByteCount(header.dependencyCount, sizeof(TemfBlockDependency), dependencyBytes)
			|| header.pageTableOffset > std::numeric_limits<std::size_t>::max()
			|| header.blockTableOffset > std::numeric_limits<std::size_t>::max()
			|| header.quantizationTableOffset > std::numeric_limits<std::size_t>::max()
			|| header.dependencyTableOffset > std::numeric_limits<std::size_t>::max()
			|| header.payloadOffset > std::numeric_limits<std::size_t>::max()
			|| header.payloadBytes > std::numeric_limits<std::size_t>::max())
		{
			SetProductionError(result, TemfProductionValidationCode::ArithmeticOverflow,
				"TEMFPRD1 table arithmetic cannot be represented by this runtime.");
			return result;
		}

		const std::size_t payloadOffset = static_cast<std::size_t>(header.payloadOffset);
		const std::size_t payloadBytes = static_cast<std::size_t>(header.payloadBytes);
		const std::array<std::pair<std::size_t, std::size_t>, 5u> ranges = {{
			{ 0u, sizeof(TemfProductionHeader) },
			{ static_cast<std::size_t>(header.pageTableOffset), pageBytes },
			{ static_cast<std::size_t>(header.blockTableOffset), blockBytes },
			{ static_cast<std::size_t>(header.quantizationTableOffset), domainBytes },
			{ static_cast<std::size_t>(header.dependencyTableOffset), dependencyBytes }
		}};
		for(const auto& range : ranges)
		{
			if(range.second != 0u && ((range.first % kTemfDirectoryAlignment) != 0u
				|| !IsRangeInside(range.first, range.second, fileBytes.size())))
			{
				SetProductionError(result, (range.first % kTemfDirectoryAlignment) != 0u
					? TemfProductionValidationCode::MisalignedRange : TemfProductionValidationCode::RangeOutOfBounds,
					"TEMFPRD1 directory table is misaligned or outside the file.");
				return result;
			}
		}
		if((payloadOffset % kTemfDirectoryAlignment) != 0u
			|| !IsRangeInside(payloadOffset, payloadBytes, fileBytes.size())
			|| payloadOffset < sizeof(TemfProductionHeader)
			|| header.directoryBytes != payloadOffset - sizeof(TemfProductionHeader))
		{
			SetProductionError(result, TemfProductionValidationCode::RangeOutOfBounds,
				"TEMFPRD1 directory or payload bounds are inconsistent.");
			return result;
		}
		for(std::size_t left = 0u; left < ranges.size(); ++left)
		{
			for(std::size_t right = left + 1u; right < ranges.size(); ++right)
			{
				if(ranges[left].second != 0u && ranges[right].second != 0u
					&& ranges[left].first < ranges[right].first + ranges[right].second
					&& ranges[right].first < ranges[left].first + ranges[left].second)
				{
					SetProductionError(result, TemfProductionValidationCode::OverlappingRange,
						"TEMFPRD1 directory tables overlap.");
					return result;
				}
			}
		}
		if(CalculateVirtualGeometryCompactHash(fileBytes.subspan(sizeof(TemfProductionHeader),
			payloadOffset - sizeof(TemfProductionHeader))) != header.directoryHash
			|| CalculateVirtualGeometryCompactHash(fileBytes.subspan(payloadOffset, payloadBytes)) != header.payloadHash)
		{
			SetProductionError(result, TemfProductionValidationCode::IntegrityMismatch,
				"TEMFPRD1 directory or payload integrity does not match its header.");
			return result;
		}

		const auto pages = std::span<const TemfPageDescriptor>(reinterpret_cast<const TemfPageDescriptor*>(
			fileBytes.data() + header.pageTableOffset), header.pageCount);
		const auto blocks = std::span<const TemfBlockDescriptor>(reinterpret_cast<const TemfBlockDescriptor*>(
			fileBytes.data() + header.blockTableOffset), header.blockCount);
		const auto domains = std::span<const TemfQuantizationDomain>(reinterpret_cast<const TemfQuantizationDomain*>(
			fileBytes.data() + header.quantizationTableOffset), header.quantizationDomainCount);
		const auto dependencies = std::span<const TemfBlockDependency>(reinterpret_cast<const TemfBlockDependency*>(
			fileBytes.data() + header.dependencyTableOffset), header.dependencyCount);

		std::vector<std::uint64_t> blockIds;
		blockIds.reserve(blocks.size());
		for(const TemfQuantizationDomain& domain : domains)
		{
			if(domain.domainId >= domains.size() || domain.componentCount == 0u || domain.componentCount > 4u
				|| domain.domainVersion == 0u || static_cast<std::uint32_t>(domain.domainScope) > static_cast<std::uint32_t>(TemfQuantizationDomainScope::DeformableEnvelope)
				|| static_cast<std::uint32_t>(domain.streamKind) > static_cast<std::uint32_t>(TemfStreamKind::Metadata)
				|| domain.reserved != 0u || domain.domainHash == 0u)
			{
				SetProductionError(result, TemfProductionValidationCode::InvalidQuantizationDomain,
					"TEMFPRD1 quantization-domain metadata is invalid.");
				return result;
			}
		}
		for(const TemfBlockDescriptor& block : blocks)
		{
			if(block.blockId == 0u || std::find(blockIds.begin(), blockIds.end(), block.blockId) != blockIds.end()
				|| block.quantizationDomainId >= domains.size() || block.encodedBytes == 0u
				|| block.decodedBytes == 0u || block.decodedBytes > limits.maximumDecodedBlockBytes
				|| block.decodedElementCount == 0u || block.decodedStride == 0u
				|| block.decodedElementCount > block.decodedBytes / block.decodedStride
				|| block.decodedElementCount * static_cast<std::uint64_t>(block.decodedStride) != block.decodedBytes
				|| !IsPowerOfTwo(block.requiredAlignment)
				|| (!directOnly && block.requiredAlignment > header.logicalPageSizeBytes)
				|| (block.encodedPayloadOffset % block.requiredAlignment) != 0u
				|| !IsRangeInside(static_cast<std::size_t>(block.encodedPayloadOffset), static_cast<std::size_t>(block.encodedBytes), fileBytes.size())
				|| block.firstDependencyIndex > dependencies.size()
				|| block.dependencyCount > dependencies.size() - block.firstDependencyIndex
				|| block.reserved != 0u || (block.flags & ~kTemfBlockKnownFlags) != 0u
				|| static_cast<std::uint32_t>(block.streamKind) > static_cast<std::uint32_t>(TemfStreamKind::Metadata)
				|| block.codecProfileId < static_cast<std::uint32_t>(TemfCodecProfile::ExpandedProduction)
				|| block.codecProfileId > static_cast<std::uint32_t>(TemfCodecProfile::MeshoptimizerProductionUpdateable)
				|| block.codecVersion != 1u || (block.requiredCapabilityMask & ~limits.availableCapabilities) != 0u)
			{
				SetProductionError(result, TemfProductionValidationCode::LimitExceeded,
					"TEMFPRD1 block metadata is malformed or exceeds a declared limit.");
				return result;
			}
			if((block.flags & kTemfBlockFlagDirectResident) != 0u
				&& (block.logicalPageId != kTemfDirectResidentPageId
					|| block.codecProfileId != static_cast<std::uint32_t>(TemfCodecProfile::ExpandedProduction)))
			{
				SetProductionError(result, TemfProductionValidationCode::ProfileMismatch,
					"Direct-resident TEMFPRD1 blocks must use ExpandedProduction.");
				return result;
			}
			if(CalculateVirtualGeometryCompactHash(fileBytes.subspan(
				static_cast<std::size_t>(block.encodedPayloadOffset), static_cast<std::size_t>(block.encodedBytes))) != block.encodedIntegrity)
			{
				SetProductionError(result, TemfProductionValidationCode::IntegrityMismatch,
					"TEMFPRD1 block encoded integrity does not match its payload.");
				return result;
			}
			blockIds.push_back(block.blockId);
		}
		for(std::size_t blockIndex = 0u; blockIndex < blocks.size(); ++blockIndex)
		{
			for(std::size_t priorIndex = 0u; priorIndex < blockIndex; ++priorIndex)
			{
				if(blocks[blockIndex].logicalPageId == blocks[priorIndex].logicalPageId
					&& blocks[blockIndex].streamKind == blocks[priorIndex].streamKind
					&& blocks[blockIndex].blockOrdinal == blocks[priorIndex].blockOrdinal)
				{
					SetProductionError(result, TemfProductionValidationCode::DuplicateIdentity,
						"TEMFPRD1 duplicates a persistent page, stream, and block-ordinal slot.");
					return result;
				}
			}
		}
		for(const TemfPageDescriptor& page : pages)
		{
			if(page.logicalPageId == kTemfDirectResidentPageId || page.blockCount == 0u
				|| page.firstBlockIndex >= blocks.size()
				|| page.blockCount > blocks.size() - page.firstBlockIndex
				|| page.firstDependencyIndex > dependencies.size()
				|| page.dependencyCount > dependencies.size() - page.firstDependencyIndex
				|| page.reserved[0] != 0u || page.reserved[1] != 0u)
			{
				SetProductionError(result, TemfProductionValidationCode::InvalidDependency,
					"TEMFPRD1 page metadata has an invalid block or dependency range.");
				return result;
			}
			std::uint64_t storedBytes = 0u;
			std::uint64_t decodedBytes = 0u;
			std::size_t expectedPayloadOffset = 0u;
			for(std::uint32_t blockIndex = page.firstBlockIndex; blockIndex < page.firstBlockIndex + page.blockCount; ++blockIndex)
			{
				if(blocks[blockIndex].logicalPageId != page.logicalPageId)
				{
					SetProductionError(result, TemfProductionValidationCode::InvalidDependency,
						"TEMFPRD1 page ranges must reference blocks with the same page identity.");
					return result;
				}
				if(blockIndex == page.firstBlockIndex)
				{
					expectedPayloadOffset = static_cast<std::size_t>(blocks[blockIndex].encodedPayloadOffset);
				}
				if(static_cast<std::size_t>(blocks[blockIndex].encodedPayloadOffset) != expectedPayloadOffset)
				{
					SetProductionError(result, TemfProductionValidationCode::RangeOutOfBounds,
						"TEMFPRD1 page blocks must occupy one contiguous payload range.");
					return result;
				}
				expectedPayloadOffset += static_cast<std::size_t>(blocks[blockIndex].encodedBytes);
				storedBytes += blocks[blockIndex].encodedBytes;
				decodedBytes += blocks[blockIndex].decodedBytes;
			}
			const std::size_t pagePayloadOffset = static_cast<std::size_t>(blocks[page.firstBlockIndex].encodedPayloadOffset);
			if(page.blockCount == 0u || storedBytes != page.attributedStoredBytes
				|| decodedBytes != page.decodedBytes
				|| CalculateVirtualGeometryCompactHash(fileBytes.subspan(pagePayloadOffset,
					static_cast<std::size_t>(storedBytes))) != page.pageIntegrity)
			{
				SetProductionError(result, TemfProductionValidationCode::IntegrityMismatch,
					"TEMFPRD1 page size or integrity does not match its blocks.");
				return result;
			}
		}
		for(const TemfBlockDependency& dependency : dependencies)
		{
			if(dependency.targetBlockId == 0u || dependency.reserved != 0u
				|| std::find(blockIds.begin(), blockIds.end(), dependency.targetBlockId) == blockIds.end())
			{
				SetProductionError(result, TemfProductionValidationCode::InvalidDependency,
					"TEMFPRD1 dependency targets a missing block.");
				return result;
			}
		}
		// Dependencies are file-owned identities, so detect cycles before any
		// decoder publishes bytes. A rejected graph leaves the prior generation
		// untouched and cannot expose partially decoded geometry.
		std::vector<std::uint8_t> visitState(blocks.size(), 0u);
		const auto findBlockIndex = [&blockIds](std::uint64_t id) -> std::size_t
		{
			return static_cast<std::size_t>(std::find(blockIds.begin(), blockIds.end(), id) - blockIds.begin());
		};
		std::function<bool(std::size_t)> visitBlock;
		visitBlock = [&](std::size_t index)
		{
			if(visitState[index] == 1u)
			{
				return false;
			}
			if(visitState[index] == 2u)
			{
				return true;
			}
			visitState[index] = 1u;
			const TemfBlockDescriptor& block = blocks[index];
			for(std::uint32_t dependencyIndex = block.firstDependencyIndex;
				dependencyIndex < block.firstDependencyIndex + block.dependencyCount; ++dependencyIndex)
			{
				if(!visitBlock(findBlockIndex(dependencies[dependencyIndex].targetBlockId)))
				{
					return false;
				}
			}
			visitState[index] = 2u;
			return true;
		};
		for(std::size_t blockIndex = 0u; blockIndex < blocks.size(); ++blockIndex)
		{
			if(!visitBlock(blockIndex))
			{
				SetProductionError(result, TemfProductionValidationCode::DependencyCycle,
					"TEMFPRD1 block dependencies contain a cycle.");
				return result;
			}
		}

		result.code = TemfProductionValidationCode::Accepted;
		result.directory = { header, pages, blocks, domains, dependencies,
			fileBytes.subspan(payloadOffset, payloadBytes) };
		return result;
	}

	TemfProductionBlockDecodeResult DecodeTemfProductionBlock(
		const TemfProductionBlockDecodeRequest& request,
		const TemfProductionValidationLimits& limits)
	{
		TemfProductionBlockDecodeResult result;
		const TemfBlockDescriptor& block = request.descriptor;
		if(request.encodedBytes.size() != block.encodedBytes || block.decodedBytes == 0u
			|| block.decodedBytes > limits.maximumDecodedBlockBytes
			|| (block.requiredCapabilityMask & ~request.availableCapabilities) != 0u
			|| CalculateVirtualGeometryCompactHash(request.encodedBytes) != block.encodedIntegrity)
		{
			SetProductionDecodeError(result, TemfProductionValidationCode::IntegrityMismatch,
				"TEMFPRD1 block bytes, capability requirements, or encoded integrity are invalid.");
			return result;
		}
		result.bytes.resize(static_cast<std::size_t>(block.decodedBytes));
		const TemfCodecProfile profile = static_cast<TemfCodecProfile>(block.codecProfileId);
		if(profile == TemfCodecProfile::ExpandedProduction)
		{
			if(request.encodedBytes.size() != result.bytes.size())
			{
				SetProductionDecodeError(result, TemfProductionValidationCode::ProfileMismatch,
					"ExpandedProduction blocks must store exactly their decoded bytes.");
				return result;
			}
			std::memcpy(result.bytes.data(), request.encodedBytes.data(), result.bytes.size());
		}
		else if(profile == TemfCodecProfile::MeshoptimizerProductionStatic
			|| profile == TemfCodecProfile::MeshoptimizerProductionUpdateable)
		{
			const bool topology = static_cast<TemfStreamKind>(block.streamKind) == TemfStreamKind::Topology;
			const int decodeResult = topology
				? meshopt_decodeIndexBuffer(result.bytes.data(), block.decodedElementCount,
					block.decodedStride, request.encodedBytes.data(), request.encodedBytes.size())
				: meshopt_decodeVertexBuffer(result.bytes.data(), block.decodedElementCount,
					block.decodedStride, request.encodedBytes.data(), request.encodedBytes.size());
			if(decodeResult != 0)
			{
				SetProductionDecodeError(result, TemfProductionValidationCode::ProfileMismatch,
					"meshoptimizer could not decode the TEMFPRD1 block.");
				return result;
			}
		}
		else
		{
			SetProductionDecodeError(result, TemfProductionValidationCode::ProfileMismatch,
				"TEMFPRD1 names an unsupported production codec profile.");
			return result;
		}
		if(CalculateVirtualGeometryCompactHash(result.bytes) != block.decodedIntegrity)
		{
			SetProductionDecodeError(result, TemfProductionValidationCode::IntegrityMismatch,
				"TEMFPRD1 decoded bytes do not match the block integrity value.");
			return result;
		}
		result.code = TemfProductionValidationCode::Accepted;
		return result;
	}

} // namespace TerraEngine::Resource
