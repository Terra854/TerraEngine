// CompactObjectReference.h - compact validated object identifiers for hot paths.

#pragma once

#include "Utilities/UUID.h"

#include <cstdint>
#include <string>

namespace TerraEngine {

	enum class CompactObjectReferenceValidity {
		Valid,
		Stale,
		Missing,
		Unresolved
	};

	struct CompactObjectReference {
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

	std::string ToString(CompactObjectReferenceValidity validity);
	std::string FormatCompactObjectReference(const CompactObjectReference& reference);

}
