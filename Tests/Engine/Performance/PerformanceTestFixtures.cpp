// PerformanceTestFixtures.cpp — Builds stable performance workloads and hashes their state.

#include "Performance/PerformanceTestFixtures.h"

#include <cstddef>

namespace TerraEngine::Tests
{
namespace
{
// AppendBody records one body in stable-id order. The simple integer grid gives
// tests varied state without relying on scene loading, random numbers, or jobs.
void AppendBody(
	PerformanceFixture& fixture,
	std::uint32_t stableId,
	PerformanceFixtureBodyKind kind)
{
	fixture.bodies.push_back({
		stableId,
		kind,
		static_cast<std::int32_t>(stableId % 100u),
		static_cast<std::int32_t>((stableId / 100u) % 100u),
		static_cast<std::int32_t>(stableId / 10000u)
	});
}

// HashBytes is a small fixed FNV-1a step. Strings are already byte sequences,
// while numeric fields use HashUnsigned so their byte order is explicitly stable.
void HashBytes(std::uint64_t& hash, const void* bytes, std::size_t byteCount)
{
	const auto* input = static_cast<const unsigned char*>(bytes);
	for (std::size_t index = 0u; index < byteCount; ++index)
	{
		hash ^= input[index];
		hash *= 1099511628211ULL;
	}
}

// HashUnsigned writes least-significant bytes first, avoiding host-endianness
// differences when fixture hashes are compared on different development hosts.
void HashUnsigned(std::uint64_t& hash, std::uint64_t value, std::size_t byteCount)
{
	for (std::size_t index = 0u; index < byteCount; ++index)
	{
		const unsigned char byte = static_cast<unsigned char>((value >> (index * 8u)) & 0xffu);
		HashBytes(hash, &byte, 1u);
	}
}
}

PerformanceFixture BuildCanonicalPerformanceFixture()
{
	PerformanceFixture fixture;
	fixture.scenarioId = "canonical-10000-body";
	fixture.measuredStepCount = 100u;
	fixture.bodies.reserve(10000u);
	for (std::uint32_t stableId = 1u; stableId <= 8000u; ++stableId)
	{
		AppendBody(fixture, stableId, PerformanceFixtureBodyKind::Static);
	}
	for (std::uint32_t stableId = 8001u; stableId <= 9000u; ++stableId)
	{
		AppendBody(fixture, stableId, PerformanceFixtureBodyKind::Dynamic);
	}
	for (std::uint32_t stableId = 9001u; stableId <= 10000u; ++stableId)
	{
		AppendBody(fixture, stableId, PerformanceFixtureBodyKind::Kinematic);
	}
	return fixture;
}

PerformanceFixture BuildSmallScenePerformanceFixture()
{
	PerformanceFixture fixture;
	fixture.scenarioId = "small-scene-control";
	fixture.measuredStepCount = 100u;
	fixture.bodies.reserve(100u);
	for (std::uint32_t stableId = 1u; stableId <= 80u; ++stableId)
	{
		AppendBody(fixture, stableId, PerformanceFixtureBodyKind::Static);
	}
	for (std::uint32_t stableId = 81u; stableId <= 90u; ++stableId)
	{
		AppendBody(fixture, stableId, PerformanceFixtureBodyKind::Dynamic);
	}
	for (std::uint32_t stableId = 91u; stableId <= 100u; ++stableId)
	{
		AppendBody(fixture, stableId, PerformanceFixtureBodyKind::Kinematic);
	}
	return fixture;
}

std::uint64_t ComputePerformanceFixtureStateHash(const PerformanceFixture& fixture)
{
	std::uint64_t hash = 14695981039346656037ULL;
	HashBytes(hash, fixture.scenarioId.data(), fixture.scenarioId.size());
	HashUnsigned(hash, fixture.measuredStepCount, sizeof(fixture.measuredStepCount));
	for (const PerformanceFixtureBody& body : fixture.bodies)
	{
		HashUnsigned(hash, body.stableId, sizeof(body.stableId));
		HashUnsigned(hash, static_cast<std::uint32_t>(body.kind), sizeof(std::uint32_t));
		HashUnsigned(hash, static_cast<std::uint32_t>(body.positionX), sizeof(body.positionX));
		HashUnsigned(hash, static_cast<std::uint32_t>(body.positionY), sizeof(body.positionY));
		HashUnsigned(hash, static_cast<std::uint32_t>(body.positionZ), sizeof(body.positionZ));
	}
	return hash;
}
}
