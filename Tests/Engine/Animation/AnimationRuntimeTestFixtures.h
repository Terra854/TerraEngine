#pragma once

#include <cstdint>

namespace TerraEngine::Tests
{
	// Small deterministic generation state used by runtime tests. The fixture is
	// intentionally independent of authored assets and production storage.
	struct AnimationFixtureGeneration
	{
		std::uint64_t scene = 1u;
		std::uint64_t membership = 1u;
		std::uint64_t resource = 1u;
		std::uint64_t renderer = 1u;
	};

	inline AnimationFixtureGeneration MakeAnimationFixtureGeneration(std::uint64_t seed = 1u)
	{
		return { seed, seed + 1u, seed + 2u, seed + 3u };
	}

	inline void MutateAnimationResource(AnimationFixtureGeneration& generation)
	{
		++generation.resource;
	}

	inline void MutateAnimationMembership(AnimationFixtureGeneration& generation)
	{
		++generation.membership;
	}
}
