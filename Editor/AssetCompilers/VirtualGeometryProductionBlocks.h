// VirtualGeometryProductionBlocks.h - deterministic canonical TEMF block assembly.
//
// The cooker supplies decoded stream bytes and receives one self-contained
// TEMFPRD1 file. This boundary intentionally has no reader compatibility path:
// callers recook source content whenever the canonical contract changes.

#pragma once

#include "Engine/File Formats/TerraEngineMeshFile.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Editor
{
	struct TemfProductionStreamInput
	{
		TemfStreamKind streamKind = TemfStreamKind::Metadata;
		TemfCodecProfile codecProfile = TemfCodecProfile::ExpandedProduction;
		// Metadata streams use this stable bit to identify their runtime record.
		std::uint64_t attributeMask = 0u;
		std::uint32_t quantizationDomainId = 0u;
		std::uint32_t elementCount = 0u;
		std::uint32_t elementStride = 0u;
		std::span<const std::uint8_t> decodedBytes;
		// Most streams are packed automatically. Page-local hierarchy payloads
		// instead name the exact persistent page consumed by selection, streaming,
		// and deformation. The direct-resident sentinel means "automatic" here;
		// direct-only files continue to use that value for every descriptor.
		std::uint32_t logicalPageIdOverride = kTemfDirectResidentPageId;
	};

	struct TemfProductionBuildRequest
	{
		std::uint32_t representationFlags = kTemfRepresentationVirtualGeometry;
		std::uint32_t logicalPageSizeBytes = kTemfPageSize64KiB;
		std::uint64_t assetIdentity = 0u;
		std::uint64_t sourceContentHash = 0u;
		std::uint64_t buildRecipeHash = 0u;
		std::uint64_t toolIdentity = 0u;
		std::uint64_t generationIdentity = 0u;
		std::uint32_t selectedQuantizationProfileId = 0u;
		std::span<const TemfQuantizationDomain> quantizationDomains;
		std::span<const TemfProductionStreamInput> streams;
	};

	struct TemfProductionBuildResult
	{
		std::vector<std::uint8_t> bytes;
		std::string error;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return error.empty() && !bytes.empty();
		}
	};

	// Packs deterministic direct-resident or paged stream blocks. Expanded
	// streams may be split at element boundaries to meet 32/64/128 KiB pages;
	// meshoptimizer streams remain indivisible because their codecs operate over
	// their complete element sequence.
	[[nodiscard]] TemfProductionBuildResult BuildTemfProductionBlocks(
		const TemfProductionBuildRequest& request);
} // namespace TerraEngine::Editor
