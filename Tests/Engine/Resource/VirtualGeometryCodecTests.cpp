#include "TestHarness.h"

#include "Resource/VirtualGeometryCodec.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		std::vector<std::uint8_t> MakeCanonicalDirectProductionFile()
		{
			// This small file exercises the complete binary boundary: header,
			// directory tables, direct-resident payload, and both integrity values.
			constexpr std::size_t blockOffset = sizeof(TemfProductionHeader);
			constexpr std::size_t domainOffset = blockOffset + sizeof(TemfBlockDescriptor);
			constexpr std::size_t dependencyOffset = domainOffset + sizeof(TemfQuantizationDomain);
			constexpr std::size_t payloadOffset = 448u;
			std::vector<std::uint8_t> bytes(payloadOffset + 3u, 0u);
			const std::array<std::uint8_t, 3u> payload = { 7u, 11u, 13u };

			TemfProductionHeader header{};
			header.magic = kMeshMagicValue;
			header.formatVersion = kMeshCurrentVersion;
			header.minorVersion = kMeshCurrentMinorVersion;
			header.headerBytes = kTemfProductionHeaderBytes;
			header.representationFlags = kTemfRepresentationDirectMeshlets;
			header.selectedCodecProfileId = static_cast<std::uint32_t>(TemfCodecProfile::ExpandedProduction);
			header.logicalPageSizeBytes = 0u; // Direct-resident files never publish a page table.
			header.blockCount = 1u;
			header.quantizationDomainCount = 1u;
			header.blockTableOffset = blockOffset;
			header.quantizationTableOffset = domainOffset;
			header.dependencyTableOffset = dependencyOffset;
			header.payloadOffset = payloadOffset;
			header.payloadBytes = payload.size();
			header.directoryBytes = payloadOffset - sizeof(TemfProductionHeader);
			header.formatContractId = kTemfProductionContractId;

			TemfBlockDescriptor block{};
			block.blockId = 1u;
			block.logicalPageId = kTemfDirectResidentPageId;
			block.streamKind = static_cast<std::uint16_t>(TemfStreamKind::Metadata);
			block.codecProfileId = static_cast<std::uint32_t>(TemfCodecProfile::ExpandedProduction);
			block.codecVersion = 1u;
			block.flags = kTemfBlockFlagDirectResident;
			block.encodedPayloadOffset = payloadOffset;
			block.encodedBytes = payload.size();
			block.decodedBytes = payload.size();
			block.decodedElementCount = static_cast<std::uint32_t>(payload.size());
			block.decodedStride = 1u;
			block.requiredAlignment = 1u;
			block.encodedIntegrity = CalculateVirtualGeometryCompactHash(payload);
			block.decodedIntegrity = block.encodedIntegrity;

			TemfQuantizationDomain domain{};
			domain.domainVersion = 1u;
			domain.domainScope = static_cast<std::uint16_t>(TemfQuantizationDomainScope::Asset);
			domain.streamKind = static_cast<std::uint16_t>(TemfStreamKind::Metadata);
			domain.componentCount = 1u;
			domain.domainHash = 1u;

			std::memcpy(bytes.data() + blockOffset, &block, sizeof(block));
			std::memcpy(bytes.data() + domainOffset, &domain, sizeof(domain));
			std::memcpy(bytes.data() + payloadOffset, payload.data(), payload.size());
			header.directoryHash = CalculateVirtualGeometryCompactHash(
				std::span<const std::uint8_t>(bytes).subspan(sizeof(header), header.directoryBytes));
			header.payloadHash = CalculateVirtualGeometryCompactHash(
				std::span<const std::uint8_t>(bytes).subspan(payloadOffset, payload.size()));
			std::memcpy(bytes.data(), &header, sizeof(header));
			return bytes;
		}
	}

	void RegisterVirtualGeometryCodecTests(TestRegistry &registry)
	{
		registry.Add("Resource.VirtualGeometryCodec.ProductionFileRejectsLegacyAndDecodesDirectResidentBlock",
			[](TestContext& context)
			{
				std::vector<std::uint8_t> bytes = MakeCanonicalDirectProductionFile();
				const auto validated = TerraEngine::Resource::ValidateTemfProductionFile(bytes, {});
				context.Expect(validated.IsComplete(),
					"A canonical TEMFPRD1 direct-resident block must validate before publication.");
				if(validated.IsComplete())
				{
					const auto decoded = TerraEngine::Resource::DecodeTemfProductionBlock({
						validated.directory.blocks.front(), validated.directory.payload,
						validated.directory.quantizationDomains, 0u }, {});
					context.Expect(decoded.IsComplete()
						&& decoded.bytes == std::vector<std::uint8_t>({ 7u, 11u, 13u }),
						"ExpandedProduction must decode direct-resident bytes exactly.");
				}

				bytes[0] = 0u;
				const auto legacy = TerraEngine::Resource::ValidateTemfProductionFile(bytes, {});
				context.Expect(legacy.code == TerraEngine::Resource::TemfProductionValidationCode::RecookRequired,
					"Historical TEMF and noncanonical identities must return recook-required without a compatibility reader.");
			});
	}

} // namespace TerraEngine::Tests
