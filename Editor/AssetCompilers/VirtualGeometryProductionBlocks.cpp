// VirtualGeometryProductionBlocks.cpp - canonical TEMFPRD1 block writer.

#include "VirtualGeometryProductionBlocks.h"

#include "meshoptimizer.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace TerraEngine::Editor
{
	namespace
	{
		struct PackedBlock
		{
			TemfBlockDescriptor descriptor{};
			std::vector<std::uint8_t> encoded;
		};

		std::size_t AlignDirectory(std::size_t value)
		{
			return (value + kTemfDirectoryAlignment - 1u) & ~(kTemfDirectoryAlignment - 1u);
		}

		bool IsSupportedPageSize(std::uint32_t size)
		{
			return size == kTemfPageSize32KiB || size == kTemfPageSize64KiB
				|| size == kTemfPageSize128KiB;
		}

		void AppendRecord(std::vector<std::uint8_t>& bytes, std::size_t offset, const void* record, std::size_t size)
		{
			std::memcpy(bytes.data() + offset, record, size);
		}

		bool EncodeStream(const TemfProductionStreamInput& stream, std::vector<std::uint8_t>& encoded, std::string& error)
		{
			if(stream.decodedBytes.empty() || stream.elementCount == 0u || stream.elementStride == 0u
				|| static_cast<std::uint64_t>(stream.elementCount) * stream.elementStride != stream.decodedBytes.size())
			{
				error = "Production stream byte and element metadata must agree.";
				return false;
			}
			if(stream.codecProfile == TemfCodecProfile::ExpandedProduction)
			{
				encoded.assign(stream.decodedBytes.begin(), stream.decodedBytes.end());
				return true;
			}
			if(stream.codecProfile != TemfCodecProfile::MeshoptimizerProductionStatic
				&& stream.codecProfile != TemfCodecProfile::MeshoptimizerProductionUpdateable)
			{
				error = "The cooker selected an unsupported production codec profile.";
				return false;
			}
			if(stream.streamKind == TemfStreamKind::Topology)
			{
				if(stream.elementStride != sizeof(std::uint32_t) || stream.elementCount % 3u != 0u)
				{
					error = "Production topology streams require uint32 triangle-list indices.";
					return false;
				}
				std::vector<std::uint32_t> indices(stream.elementCount);
				std::memcpy(indices.data(), stream.decodedBytes.data(), stream.decodedBytes.size());
				const auto maxIndex = *std::max_element(indices.begin(), indices.end());
				encoded.resize(meshopt_encodeIndexBufferBound(stream.elementCount, maxIndex + 1u));
				const std::size_t written = meshopt_encodeIndexBuffer(encoded.data(), encoded.size(), indices.data(), stream.elementCount);
				if(written == 0u)
				{
					error = "meshoptimizer failed to encode a production topology stream.";
					return false;
				}
				encoded.resize(written);
				return true;
			}
			encoded.resize(meshopt_encodeVertexBufferBound(stream.elementCount, stream.elementStride));
			const std::size_t written = meshopt_encodeVertexBuffer(encoded.data(), encoded.size(),
				stream.decodedBytes.data(), stream.elementCount, stream.elementStride);
			if(written == 0u)
			{
				error = "meshoptimizer failed to encode a production attribute stream.";
				return false;
			}
			encoded.resize(written);
			return true;
		}
	} // namespace

	TemfProductionBuildResult BuildTemfProductionBlocks(const TemfProductionBuildRequest& request)
	{
		TemfProductionBuildResult result;
		const bool directResident = (request.representationFlags & kTemfRepresentationDirectMeshlets) != 0u
			&& (request.representationFlags & kTemfRepresentationVirtualGeometry) == 0u;
		if((directResident ? request.logicalPageSizeBytes != 0u : !IsSupportedPageSize(request.logicalPageSizeBytes))
			|| request.streams.empty()
			|| (request.representationFlags & ~kTemfRepresentationKnownMask) != 0u
			|| (request.representationFlags & (kTemfRepresentationDirectMeshlets | kTemfRepresentationVirtualGeometry)) == 0u)
		{
			result.error = "Production TEMF assembly needs supported page sizing, streams, and representation flags.";
			return result;
		}
		if(request.quantizationDomains.empty()
			|| request.quantizationDomains.size() > kTemfMaximumQuantizationDomainCount
			|| request.streams.size() > kTemfMaximumBlockCount)
		{
			result.error = "Production TEMF domain or stream counts exceed the canonical limits.";
			return result;
		}
		for(std::size_t index = 0u; index < request.quantizationDomains.size(); ++index)
		{
			const TemfQuantizationDomain& domain = request.quantizationDomains[index];
			if(domain.domainId != index || domain.domainVersion == 0u
				|| domain.componentCount == 0u || domain.componentCount > 4u
				|| domain.domainHash == 0u)
			{
				result.error = "Production TEMF quantization domains must have canonical identities and metadata.";
				return result;
			}
		}

		std::vector<PackedBlock> blocks;
		std::vector<TemfPageDescriptor> pages;
		std::uint32_t nextPageId = 0u;
		std::uint64_t nextBlockId = 1u;
		std::uint32_t activePageBytes = 0u;
		std::uint64_t activePageDecodedBytes = 0u;
		std::uint32_t activePageFirstBlock = 0u;
		std::uint32_t activePageBlockCount = 0u;
		std::uint32_t activePageDependencyCount = 0u;
		for(const TemfProductionStreamInput& stream : request.streams)
		{
			if(stream.quantizationDomainId >= request.quantizationDomains.size())
			{
				result.error = "Production stream references a missing quantization domain.";
				return result;
			}
			std::vector<std::uint8_t> encoded;
			if(!EncodeStream(stream, encoded, result.error))
			{
				return result;
			}
			if(!directResident && encoded.size() > request.logicalPageSizeBytes
				&& stream.codecProfile != TemfCodecProfile::ExpandedProduction)
			{
				result.error = "A compressed production stream exceeds one page and must be recooked with a smaller source block.";
				return result;
			}
			for(std::size_t begin = 0u; begin < encoded.size();)
			{
				const bool topology = stream.streamKind == TemfStreamKind::Topology;
				// The physical page budget reserves its directory records, one
				// dependency slot per topology block, alignment padding, and the
				// element-alignment tail waste before selecting payload bytes.
				const std::size_t descriptorBudget = sizeof(TemfPageDescriptor)
					+ static_cast<std::size_t>(activePageBlockCount + 1u) * sizeof(TemfBlockDescriptor)
					+ static_cast<std::size_t>(activePageDependencyCount + (topology ? 1u : 0u)) * sizeof(TemfBlockDependency)
					+ (kTemfDirectoryAlignment - 1u);
				const std::size_t remainingBudget = directResident ? encoded.size() - begin
					: (request.logicalPageSizeBytes > activePageBytes + descriptorBudget
						? request.logicalPageSizeBytes - activePageBytes - descriptorBudget : 0u);
				if(!directResident && stream.codecProfile != TemfCodecProfile::ExpandedProduction
					&& activePageBytes != 0u
					&& encoded.size() - begin > remainingBudget)
				{
					pages.push_back({ nextPageId++, 0u, activePageFirstBlock,
						static_cast<std::uint32_t>(blocks.size()) - activePageFirstBlock, 0u, 0u,
						activePageBytes, activePageDecodedBytes, 0u, { 0u, 0u } });
					activePageFirstBlock = static_cast<std::uint32_t>(blocks.size());
					activePageBytes = 0u;
					activePageDecodedBytes = 0u;
					activePageBlockCount = 0u;
					activePageDependencyCount = 0u;
					continue;
				}
				std::size_t size = directResident ? encoded.size() - begin
					: std::min<std::size_t>(remainingBudget, encoded.size() - begin);
				if(!directResident && stream.codecProfile == TemfCodecProfile::ExpandedProduction)
				{
					size -= size % stream.elementStride;
					if(size == 0u)
					{
						if(activePageBytes == 0u || stream.elementStride > request.logicalPageSizeBytes)
						{
							result.error = "Expanded production elements cannot fit within the selected page size.";
							return result;
						}
						pages.push_back({ nextPageId++, 0u, activePageFirstBlock,
							static_cast<std::uint32_t>(blocks.size()) - activePageFirstBlock, 0u, 0u,
							activePageBytes, activePageDecodedBytes, 0u, { 0u, 0u } });
						activePageFirstBlock = static_cast<std::uint32_t>(blocks.size());
						activePageBytes = 0u;
						activePageDecodedBytes = 0u;
						activePageBlockCount = 0u;
						activePageDependencyCount = 0u;
						continue;
					}
				}
				if(!directResident && size == 0u)
				{
					result.error = "Production page capacity was exhausted before a stream block could be emitted.";
					return result;
				}
				if(!directResident && stream.codecProfile != TemfCodecProfile::ExpandedProduction
					&& size != encoded.size() - begin)
				{
					result.error = "A compressed production stream cannot be split after page-directory overhead is reserved.";
					return result;
				}
				PackedBlock packed{};
				packed.encoded.assign(encoded.begin() + static_cast<std::ptrdiff_t>(begin),
					encoded.begin() + static_cast<std::ptrdiff_t>(begin + size));
				packed.descriptor.blockId = nextBlockId++;
				packed.descriptor.logicalPageId = directResident ? kTemfDirectResidentPageId : nextPageId;
				const std::size_t pageOrdinal = directResident ? blocks.size()
					: blocks.size() - activePageFirstBlock;
				if(pageOrdinal > std::numeric_limits<std::uint16_t>::max())
				{
					result.error = "Production page contains too many blocks for the persistent block ordinal.";
					return result;
				}
				packed.descriptor.blockOrdinal = static_cast<std::uint16_t>(pageOrdinal);
				packed.descriptor.streamKind = static_cast<std::uint16_t>(stream.streamKind);
				packed.descriptor.attributeMask = stream.attributeMask;
				packed.descriptor.codecProfileId = static_cast<std::uint32_t>(stream.codecProfile);
				packed.descriptor.codecVersion = 1u;
				packed.descriptor.requiredCapabilityMask = stream.codecProfile == TemfCodecProfile::ExpandedProduction
					? 0u : kVirtualGeometryCodecCapabilityMeshoptimizer;
				packed.descriptor.quantizationDomainId = stream.quantizationDomainId;
				packed.descriptor.flags = directResident ? kTemfBlockFlagDirectResident : 0u;
				packed.descriptor.encodedBytes = packed.encoded.size();
				packed.descriptor.decodedBytes = stream.codecProfile == TemfCodecProfile::ExpandedProduction ? size : stream.decodedBytes.size();
				packed.descriptor.decodedElementCount = stream.codecProfile == TemfCodecProfile::ExpandedProduction
					? static_cast<std::uint32_t>(size / stream.elementStride) : stream.elementCount;
				packed.descriptor.decodedStride = stream.elementStride;
				packed.descriptor.requiredAlignment = 1u;
				packed.descriptor.encodedIntegrity = CalculateVirtualGeometryCompactHash(packed.encoded);
				packed.descriptor.decodedIntegrity = stream.codecProfile == TemfCodecProfile::ExpandedProduction
					? packed.descriptor.encodedIntegrity : CalculateVirtualGeometryCompactHash(stream.decodedBytes);
				activePageBytes += static_cast<std::uint32_t>(size);
				activePageDecodedBytes += packed.descriptor.decodedBytes;
				++activePageBlockCount;
				activePageDependencyCount += topology ? 1u : 0u;
				blocks.push_back(std::move(packed));
				begin += size;
			}
		}
		if(!directResident && !blocks.empty())
		{
			pages.push_back({ nextPageId, 0u, activePageFirstBlock,
				static_cast<std::uint32_t>(blocks.size()) - activePageFirstBlock, 0u, 0u,
				activePageBytes, activePageDecodedBytes, 0u, { 0u, 0u } });
		}
		// Topology is meaningful only after every stable-remap block is ready.
		// Attribute and topology streams are deliberately page-independent, so
		// this dependency crosses page boundaries instead of requiring both
		// streams to be physically co-located.
		std::vector<TemfBlockDependency> dependencies;
		for(PackedBlock& block : blocks)
		{
			if(block.descriptor.streamKind != static_cast<std::uint16_t>(TemfStreamKind::Topology))
			{
				continue;
			}
			const std::uint32_t firstDependency =
				static_cast<std::uint32_t>(dependencies.size());
			for (const PackedBlock& candidate : blocks)
			{
				if (candidate.descriptor.streamKind
					== static_cast<std::uint16_t>(
						TemfStreamKind::VertexRemapStableIdentity))
				{
					dependencies.push_back(
						{ candidate.descriptor.blockId, 0u, 0u });
				}
			}
			if(dependencies.size() == firstDependency)
			{
				result.error = "Production topology blocks require stable-identity remap blocks.";
				return result;
			}
			block.descriptor.firstDependencyIndex = firstDependency;
			block.descriptor.dependencyCount =
				static_cast<std::uint32_t>(dependencies.size())
				- firstDependency;
		}
		for(TemfPageDescriptor& page : pages)
		{
			const std::uint32_t first = page.firstBlockIndex;
			const std::uint32_t last = first + page.blockCount;
			for(std::uint32_t index = first; index < last; ++index)
			{
				if(blocks[index].descriptor.dependencyCount != 0u)
				{
					page.firstDependencyIndex = blocks[index].descriptor.firstDependencyIndex;
					page.dependencyCount += blocks[index].descriptor.dependencyCount;
				}
			}
		}
		if(pages.size() > kTemfMaximumPageCount || blocks.size() > kTemfMaximumBlockCount
			|| dependencies.size() > kTemfMaximumDependencyCount)
		{
			result.error = "Production TEMF directory exceeds the persistent page, block, or dependency limits.";
			return result;
		}

		// Direct-resident assets intentionally have no page table or page size.
		// Paged files begin their table directly after the immutable root header.
		const std::size_t headerOffset = 0u;
		const std::size_t pageOffset = directResident ? 0u : AlignDirectory(sizeof(TemfProductionHeader));
		const std::size_t blockOffset = AlignDirectory((directResident ? sizeof(TemfProductionHeader) : pageOffset + pages.size() * sizeof(TemfPageDescriptor)));
		const std::size_t domainOffset = AlignDirectory(blockOffset + blocks.size() * sizeof(TemfBlockDescriptor));
		const std::size_t dependencyOffset = AlignDirectory(domainOffset + request.quantizationDomains.size() * sizeof(TemfQuantizationDomain));
		const std::size_t payloadOffset = AlignDirectory(
			dependencyOffset + dependencies.size() * sizeof(TemfBlockDependency));
		std::size_t payloadBytes = 0u;
		for(PackedBlock& block : blocks)
		{
			block.descriptor.encodedPayloadOffset = payloadOffset + payloadBytes;
			payloadBytes += block.encoded.size();
		}
		result.bytes.assign(payloadOffset + payloadBytes, 0u);
		for(std::size_t index = 0u; index < blocks.size(); ++index)
		{
			AppendRecord(result.bytes, blockOffset + index * sizeof(TemfBlockDescriptor),
				&blocks[index].descriptor, sizeof(TemfBlockDescriptor));
			std::memcpy(result.bytes.data() + blocks[index].descriptor.encodedPayloadOffset,
				blocks[index].encoded.data(), blocks[index].encoded.size());
		}
		for(std::size_t index = 0u; index < pages.size(); ++index)
		{
			const std::uint32_t begin = pages[index].firstBlockIndex;
			const std::uint32_t count = pages[index].blockCount;
			std::vector<std::uint8_t> pageBytes;
			for(std::uint32_t blockIndex = begin; blockIndex < begin + count; ++blockIndex)
			{
				pageBytes.insert(pageBytes.end(), blocks[blockIndex].encoded.begin(), blocks[blockIndex].encoded.end());
			}
			pages[index].pageIntegrity = CalculateVirtualGeometryCompactHash(pageBytes);
			AppendRecord(result.bytes, pageOffset + index * sizeof(TemfPageDescriptor),
				&pages[index], sizeof(TemfPageDescriptor));
		}
		for(std::size_t index = 0u; index < request.quantizationDomains.size(); ++index)
		{
			AppendRecord(result.bytes, domainOffset + index * sizeof(TemfQuantizationDomain),
				&request.quantizationDomains[index], sizeof(TemfQuantizationDomain));
		}
		for(std::size_t index = 0u; index < dependencies.size(); ++index)
		{
			AppendRecord(result.bytes, dependencyOffset + index * sizeof(TemfBlockDependency),
				&dependencies[index], sizeof(TemfBlockDependency));
		}

		TemfProductionHeader header{};
		header.magic = kMeshMagicValue;
		header.formatVersion = kMeshCurrentVersion;
		header.minorVersion = kMeshCurrentMinorVersion;
		header.headerBytes = kTemfProductionHeaderBytes;
		header.representationFlags = request.representationFlags;
		header.assetIdentity = request.assetIdentity;
		header.sourceContentHash = request.sourceContentHash;
		header.buildRecipeHash = request.buildRecipeHash;
		header.toolIdentity = request.toolIdentity;
		header.generationIdentity = request.generationIdentity;
		const auto selectedProfile = std::find_if(request.streams.begin(), request.streams.end(), [](const TemfProductionStreamInput& stream) {
			return stream.codecProfile != TemfCodecProfile::ExpandedProduction;
		});
		header.selectedCodecProfileId = static_cast<std::uint32_t>(selectedProfile == request.streams.end()
			? request.streams.front().codecProfile : selectedProfile->codecProfile);
		for (const PackedBlock& block : blocks)
		{
			header.requiredCapabilityMask |= block.descriptor.requiredCapabilityMask;
		}
		header.selectedQuantizationProfileId = request.selectedQuantizationProfileId;
		header.logicalPageSizeBytes = directResident ? 0u : request.logicalPageSizeBytes;
		header.pageCount = static_cast<std::uint32_t>(pages.size());
		header.blockCount = static_cast<std::uint32_t>(blocks.size());
		header.quantizationDomainCount = static_cast<std::uint32_t>(request.quantizationDomains.size());
		header.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
		header.pageTableOffset = pageOffset;
		header.blockTableOffset = blockOffset;
		header.quantizationTableOffset = domainOffset;
		header.dependencyTableOffset = dependencyOffset;
		header.payloadOffset = payloadOffset;
		header.payloadBytes = payloadBytes;
		header.formatContractId = kTemfProductionContractId;
		header.directoryBytes = payloadOffset - sizeof(TemfProductionHeader);
		header.directoryHash = CalculateVirtualGeometryCompactHash(
			std::span<const std::uint8_t>(result.bytes).subspan(sizeof(TemfProductionHeader), header.directoryBytes));
		header.payloadHash = CalculateVirtualGeometryCompactHash(
			std::span<const std::uint8_t>(result.bytes).subspan(payloadOffset, payloadBytes));
		AppendRecord(result.bytes, headerOffset, &header, sizeof(header));
		return result;
	}
} // namespace TerraEngine::Editor
