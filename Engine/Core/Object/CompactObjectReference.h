// CompactObjectReference.h - compact validated object identifiers for hot paths.

#pragma once

#include "Utilities/UUID.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace TerraEngine {

	enum class CompactObjectReferenceValidity {
		Valid,
		Stale,
		Missing,
		Unresolved
	};

	struct CompactObjectReference {
		// Hot snapshots carry only identity data: a transient entity value for
		// fast lookup, a manager-owned generation for stale detection, and the
		// stable authored UUID for mismatch checks at protected boundaries.
		std::uint32_t entityValue = 0;
		std::uint64_t generation = 0;
		TerraEngine::UUID stableId = TerraEngine::EMPTY_UUID;

		bool operator==(const CompactObjectReference& other) const = default;
		bool IsEmpty() const;
	};

	struct CompactObjectReferenceResolution {
		CompactObjectReference reference;
		CompactObjectReferenceValidity validity = CompactObjectReferenceValidity::Unresolved;
		std::string message;
	};

	constexpr std::size_t kCompactObjectReferenceTargetBytes = 40;

	static_assert(std::is_trivially_copyable_v<CompactObjectReference>, "CompactObjectReference must remain cheap to copy into worker snapshots.");
	static_assert(sizeof(CompactObjectReference) <= kCompactObjectReferenceTargetBytes, "CompactObjectReference exceeded the hot snapshot footprint target.");

	constexpr std::size_t CompactObjectReferenceFootprintBytes()
	{
		return sizeof(CompactObjectReference);
	}

	constexpr bool IsCompactObjectReferenceWithinFootprint()
	{
		return CompactObjectReferenceFootprintBytes() <= kCompactObjectReferenceTargetBytes;
	}

	constexpr bool IsResolvedCompactObjectReference(CompactObjectReferenceValidity validity)
	{
		return validity == CompactObjectReferenceValidity::Valid;
	}

	bool IsResolvedCompactObjectReference(const CompactObjectReferenceResolution& resolution);
	std::string ToString(CompactObjectReferenceValidity validity);
	std::string FormatCompactObjectReference(const CompactObjectReference& reference);

}
