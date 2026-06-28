// Object.cpp - compact object identity helpers.

#include "Object.h"

#include <sstream>

bool Object::IsEmpty() const
{
	return entityValue == 0 && generation == 0 && stableIdHash == 0;
}

namespace TerraEngine {

	std::uint64_t MakeObjectStableHash(const TerraEngine::UUID& stableId)
	{
		if (stableId == TerraEngine::EMPTY_UUID)
		{
			return 0;
		}

		// FNV-1a gives worker snapshots a deterministic UUID fingerprint while
		// ObjectManager keeps the authoritative full UUID in protected tables.
		std::uint64_t hash = 14695981039346656037ull;
		for (std::uint8_t value : stableId)
		{
			hash ^= value;
			hash *= 1099511628211ull;
		}
		return hash == 0 ? 1 : hash;
	}

	bool IsResolvedObjectIdentity(const ObjectIdentityResolution& resolution)
	{
		return IsResolvedObjectIdentity(resolution.validity);
	}

	std::string ToString(ObjectIdentityValidity validity)
	{
		switch (validity)
		{
		case ObjectIdentityValidity::Valid:
			return "valid";
		case ObjectIdentityValidity::Stale:
			return "stale";
		case ObjectIdentityValidity::Missing:
			return "missing";
		case ObjectIdentityValidity::Unresolved:
		default:
			return "unresolved";
		}
	}

	std::string FormatObjectIdentity(const Object& reference)
	{
		std::ostringstream stream;
		stream << "entity=" << reference.entityValue
			<< " generation=" << reference.generation
			<< " stableHash=" << reference.stableIdHash
			<< " bytes=" << ObjectIdentityFootprintBytes();
		return stream.str();
	}

}
