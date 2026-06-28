// Object.h - compact validated object identity for runtime and editor paths.

#pragma once

#include "Utilities/UUID.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>

#include <entt/entt.hpp>

// Object is a small value, not a component facade. It can be copied into worker
// snapshots and must be validated through ObjectManager before mutable access.
struct Object
{
	std::uint32_t entityValue = 0;
	std::uint32_t generation = 0;
	std::uint64_t stableIdHash = 0;

	bool operator==(const Object& other) const = default;
	bool IsEmpty() const;
	entt::entity EntityId() const { return static_cast<entt::entity>(entityValue); }
};

namespace TerraEngine {

	enum class ObjectIdentityValidity {
		Valid,
		Stale,
		Missing,
		Unresolved
	};

	struct ObjectIdentityResolution {
		Object reference;
		ObjectIdentityValidity validity = ObjectIdentityValidity::Unresolved;
		std::string message;
	};

	constexpr std::size_t kObjectIdentityTargetBytes = 16;

	static_assert(std::is_trivially_copyable_v<::Object>, "Object must remain cheap to copy into worker snapshots.");
	static_assert(sizeof(::Object) <= kObjectIdentityTargetBytes, "Object exceeded the hot snapshot footprint target.");

	constexpr std::size_t ObjectIdentityFootprintBytes()
	{
		return sizeof(::Object);
	}

	constexpr bool IsObjectIdentityWithinFootprint()
	{
		return ObjectIdentityFootprintBytes() <= kObjectIdentityTargetBytes;
	}

	constexpr bool IsResolvedObjectIdentity(ObjectIdentityValidity validity)
	{
		return validity == ObjectIdentityValidity::Valid;
	}

	constexpr std::uint32_t CompressObjectGeneration(std::uint64_t generation)
	{
		constexpr std::uint64_t maxCompactGeneration = std::numeric_limits<std::uint32_t>::max();
		return generation == 0
			? 0
			: static_cast<std::uint32_t>(((generation - 1) % maxCompactGeneration) + 1);
	}

	std::uint64_t MakeObjectStableHash(const TerraEngine::UUID& stableId);
	bool IsResolvedObjectIdentity(const ObjectIdentityResolution& resolution);
	std::string ToString(ObjectIdentityValidity validity);
	std::string FormatObjectIdentity(const Object& reference);

}

namespace std {
	template<>
	struct hash<Object>
	{
		std::size_t operator()(const Object& value) const noexcept
		{
			std::size_t result = std::hash<std::uint32_t>{}(value.entityValue);
			result ^= std::hash<std::uint32_t>{}(value.generation) + 0x9e3779b9u + (result << 6) + (result >> 2);
			result ^= std::hash<std::uint64_t>{}(value.stableIdHash) + 0x9e3779b97f4a7c15ull + (result << 6) + (result >> 2);
			return result;
		}
	};
}
