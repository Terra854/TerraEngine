// CompactObjectReference.cpp - formatting helpers for hot-path object ids.

#include "CompactObjectReference.h"

#include <sstream>

namespace TerraEngine {
	bool CompactObjectReference::IsEmpty() const
	{
		return entityValue == 0 && generation == 0 && stableId == TerraEngine::EMPTY_UUID;
	}

	std::string ToString(CompactObjectReferenceValidity validity)
	{
		switch (validity)
		{
		case CompactObjectReferenceValidity::Valid:
			return "valid";
		case CompactObjectReferenceValidity::Stale:
			return "stale";
		case CompactObjectReferenceValidity::Missing:
			return "missing";
		case CompactObjectReferenceValidity::Unresolved:
		default:
			return "unresolved";
		}
	}

	std::string FormatCompactObjectReference(const CompactObjectReference& reference)
	{
		std::ostringstream stream;
		stream << "entity=" << reference.entityValue
			<< " generation=" << reference.generation
			<< " uuid=" << TerraEngine::ToHexString(reference.stableId);
		return stream.str();
	}
}
