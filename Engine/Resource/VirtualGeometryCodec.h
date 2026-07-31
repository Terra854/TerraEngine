// VirtualGeometryCodec.h - Bounded CPU/reference decoder for compact pages.
//
// Runtime code uses this validator before a page is published. It accepts no
// process-local pointers and allocates only after all header ranges are checked.

#pragma once

#include "../Core/Export.h"
#include "../File Formats/TerraEngineMeshFile.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Resource
{
	enum class TemfProductionValidationCode : std::uint16_t
	{
		Accepted = 0u,
		RecookRequired = 1u,
		TruncatedInput = 2u,
		UnsupportedVersion = 3u,
		InvalidContract = 4u,
		InvalidRepresentation = 5u,
		UnknownRequiredCapability = 6u,
		LimitExceeded = 7u,
		ArithmeticOverflow = 8u,
		MisalignedRange = 9u,
		RangeOutOfBounds = 10u,
		OverlappingRange = 11u,
		DuplicateIdentity = 12u,
		InvalidDependency = 13u,
		DependencyCycle = 14u,
		IntegrityMismatch = 15u,
		InvalidQuantizationDomain = 16u,
		ProfileMismatch = 17u
	};

	struct TERRAENGINE_API TemfProductionValidationLimits
	{
		std::uint32_t maximumPageCount = kTemfMaximumPageCount;
		std::uint32_t maximumBlockCount = kTemfMaximumBlockCount;
		std::uint32_t maximumQuantizationDomainCount = kTemfMaximumQuantizationDomainCount;
		std::uint32_t maximumDependencyCount = kTemfMaximumDependencyCount;
		std::uint64_t maximumDecodedBlockBytes = kTemfMaximumDecodedBlockBytes;
		// The CPU decoder is always bundled with ExpandedProduction and
		// meshoptimizer support. Callers may pass a narrower explicit mask when
		// testing device-only or unavailable-capability behavior.
		std::uint64_t availableCapabilities =
			kVirtualGeometryCodecCapabilityExpanded
			| kVirtualGeometryCodecCapabilityMeshoptimizer;
	};

	// This immutable view points into caller-owned, already validated bytes. The
	// loader must retain the source storage for as long as any span is consumed.
	struct TERRAENGINE_API TemfProductionDirectoryView
	{
		TemfProductionHeader header{};
		std::span<const TemfPageDescriptor> pages;
		std::span<const TemfBlockDescriptor> blocks;
		std::span<const TemfQuantizationDomain> quantizationDomains;
		std::span<const TemfBlockDependency> dependencies;
		std::span<const std::uint8_t> payload;
	};

	struct TERRAENGINE_API TemfProductionValidationResult
	{
		TemfProductionValidationCode code = TemfProductionValidationCode::TruncatedInput;
		TemfProductionDirectoryView directory{};
		std::string diagnostic;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return code == TemfProductionValidationCode::Accepted && diagnostic.empty();
		}
	};

	struct TERRAENGINE_API TemfProductionBlockDecodeRequest
	{
		TemfBlockDescriptor descriptor{};
		std::span<const std::uint8_t> encodedBytes;
		std::span<const TemfQuantizationDomain> quantizationDomains;
		std::uint64_t availableCapabilities = 0u;
	};

	struct TERRAENGINE_API TemfProductionBlockDecodeResult
	{
		std::vector<std::uint8_t> bytes;
		TemfProductionValidationCode code = TemfProductionValidationCode::TruncatedInput;
		std::string diagnostic;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return code == TemfProductionValidationCode::Accepted
				&& diagnostic.empty()
				&& !bytes.empty();
		}
	};

	// Validation is fail-closed and performs no allocation based on file-owned
	// counts until every header/table range passes checked arithmetic.
	[[nodiscard]] TERRAENGINE_API TemfProductionValidationResult ValidateTemfProductionFile(
		std::span<const std::uint8_t> fileBytes,
		const TemfProductionValidationLimits &limits);

	[[nodiscard]] TERRAENGINE_API TemfProductionBlockDecodeResult DecodeTemfProductionBlock(
		const TemfProductionBlockDecodeRequest &request,
		const TemfProductionValidationLimits &limits);

} // namespace TerraEngine::Resource
