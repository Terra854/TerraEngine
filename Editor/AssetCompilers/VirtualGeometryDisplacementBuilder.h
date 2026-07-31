// VirtualGeometryDisplacementBuilder.h - Bounded displacement candidates.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Editor
{
	struct VirtualGeometryDisplacementRegion
	{
		std::uint64_t regionIdentity = 0u;
		std::array<float, 3u> origin{};
		std::array<float, 2u> extent{1.0F, 1.0F};
		std::span<const float> heightSamples;
		std::uint32_t sampleWidth = 0u;
		std::uint32_t sampleHeight = 0u;
		std::array<std::uint32_t, 4u> requestedEdgeFactors{1u, 1u, 1u, 1u};
		std::array<std::uint32_t, 4u> neighborEdgeFactors{1u, 1u, 1u, 1u};
	};

	struct VirtualGeometryDisplacementBuildRequest
	{
		std::span<const VirtualGeometryDisplacementRegion> regions;
		std::uint32_t maximumEdgeFactor = 64u;
		std::uint64_t maximumGeneratedVertices = 1u << 22u;
		float maximumDisplacement = 0.0F;
		float maximumError = 0.01F;
		std::uint64_t sourceGeneration = 0u;
	};

	struct VirtualGeometryDisplacementRegionResult
	{
		std::uint64_t regionIdentity = 0u;
		std::array<std::uint32_t, 4u> agreedEdgeFactors{};
		std::uint64_t sharedBoundaryDomainIdentity = 0u;
		std::uint64_t generatedVertexCount = 0u;
	};

	struct VirtualGeometryDisplacementBuildResult
	{
		std::vector<VirtualGeometryDisplacementRegionResult> regions;
		std::uint64_t totalGeneratedVertices = 0u;
		std::uint64_t generationIdentity = 0u;
		bool fallbackRequired = false;
		std::string diagnostic;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return diagnostic.empty()
				&& generationIdentity != 0u
				&& !regions.empty();
		}
	};

	[[nodiscard]] VirtualGeometryDisplacementBuildResult
		BuildVirtualGeometryDisplacement(
			const VirtualGeometryDisplacementBuildRequest &request);
} // namespace TerraEngine::Editor
