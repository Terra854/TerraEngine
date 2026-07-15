// PerformanceTestFixtures.h — Supplies deterministic engine-side performance workload test data.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::Tests
{
// PerformanceFixtureBodyKind mirrors the canonical workload categories without
// depending on live physics objects, keeping tests reproducible and worker-safe.
enum class PerformanceFixtureBodyKind
{
	Static,
	Dynamic,
	Kinematic
};

// PerformanceFixtureBody uses integer positions so its state hash is stable on
// every supported platform and does not depend on floating-point formatting.
struct PerformanceFixtureBody
{
	std::uint32_t stableId = 0u;
	PerformanceFixtureBodyKind kind = PerformanceFixtureBodyKind::Static;
	std::int32_t positionX = 0;
	std::int32_t positionY = 0;
	std::int32_t positionZ = 0;
};

// PerformanceFixture is an immutable-by-convention value fixture. Builders
// return a complete ordered body list so state hashes are deterministic.
struct PerformanceFixture
{
	std::string scenarioId;
	std::vector<PerformanceFixtureBody> bodies;
	std::uint32_t measuredStepCount = 0u;
};

[[nodiscard]] PerformanceFixture BuildCanonicalPerformanceFixture();
[[nodiscard]] PerformanceFixture BuildSmallScenePerformanceFixture();
[[nodiscard]] std::uint64_t ComputePerformanceFixtureStateHash(const PerformanceFixture& fixture);
}
