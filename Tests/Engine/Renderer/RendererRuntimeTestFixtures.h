#pragma once

#include <cstdint>

namespace TerraEngine::Tests
{
	// CPU-only renderer fixture generations make invalidation tests reproducible
	// without requiring Vulkan ownership or a live editor process.
	struct RendererFixtureGeneration
	{
		std::uint64_t scene = 1u;
		std::uint64_t topology = 1u;
		std::uint64_t view = 1u;
		std::uint64_t renderer = 1u;
	};

	inline RendererFixtureGeneration MakeRendererFixtureGeneration(std::uint64_t seed = 1u)
	{
		return { seed, seed + 1u, seed + 2u, seed + 3u };
	}

	inline void MutateRendererTopology(RendererFixtureGeneration& generation)
	{
		++generation.topology;
	}

	inline void MutateRendererView(RendererFixtureGeneration& generation)
	{
		++generation.view;
	}
}
