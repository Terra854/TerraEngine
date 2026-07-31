// VirtualGeometryDisplacementBuilder.cpp - Deterministic bounded region plans.

#include "VirtualGeometryDisplacementBuilder.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace TerraEngine::Editor
{
	namespace
	{
		std::uint64_t Mix(std::uint64_t hash, std::uint64_t value) noexcept
		{
			hash ^= value;
			hash *= 1099511628211ull;
			return hash;
		}

		bool CheckedAdd(
			std::uint64_t left,
			std::uint64_t right,
			std::uint64_t &value) noexcept
		{
			if (right > std::numeric_limits<std::uint64_t>::max() - left)
			{
				return false;
			}
			value = left + right;
			return true;
		}
	} // namespace

	VirtualGeometryDisplacementBuildResult BuildVirtualGeometryDisplacement(
		const VirtualGeometryDisplacementBuildRequest &request)
	{
		VirtualGeometryDisplacementBuildResult result{};
		if (request.regions.empty()
			|| request.maximumEdgeFactor == 0u
			|| request.maximumGeneratedVertices == 0u
			|| !std::isfinite(request.maximumDisplacement)
			|| request.maximumDisplacement < 0.0F
			|| !std::isfinite(request.maximumError)
			|| request.maximumError <= 0.0F
			|| request.sourceGeneration == 0u)
		{
			result.fallbackRequired = true;
			result.diagnostic = "displacement-input-invalid";
			return result;
		}

		result.regions.reserve(request.regions.size());
		std::uint64_t generationHash =
			Mix(1469598103934665603ull, request.sourceGeneration);
		for (const auto &region : request.regions)
		{
			const std::uint64_t expectedSamples =
				static_cast<std::uint64_t>(region.sampleWidth)
				* static_cast<std::uint64_t>(region.sampleHeight);
			if (region.regionIdentity == 0u
				|| region.sampleWidth < 2u
				|| region.sampleHeight < 2u
				|| region.heightSamples.size() != expectedSamples
				|| !std::isfinite(region.origin[0])
				|| !std::isfinite(region.origin[1])
				|| !std::isfinite(region.origin[2])
				|| !std::isfinite(region.extent[0])
				|| !std::isfinite(region.extent[1])
				|| region.extent[0] <= 0.0F
				|| region.extent[1] <= 0.0F)
			{
				result.fallbackRequired = true;
				result.diagnostic = "displacement-region-invalid";
				result.regions.clear();
				return result;
			}
			for (const float height : region.heightSamples)
			{
				if (!std::isfinite(height)
					|| std::abs(height) > request.maximumDisplacement)
				{
					result.fallbackRequired = true;
					result.diagnostic = "displacement-height-out-of-range";
					result.regions.clear();
					return result;
				}
			}

			VirtualGeometryDisplacementRegionResult built{};
			built.regionIdentity = region.regionIdentity;
			std::uint32_t maximumFactor = 1u;
			for (std::size_t edge = 0u; edge < 4u; ++edge)
			{
				built.agreedEdgeFactors[edge] = std::clamp(
					std::max(
						region.requestedEdgeFactors[edge],
						region.neighborEdgeFactors[edge]),
					1u,
					request.maximumEdgeFactor);
				maximumFactor = std::max(
					maximumFactor,
					built.agreedEdgeFactors[edge]);
			}
			built.generatedVertexCount =
				static_cast<std::uint64_t>(maximumFactor + 1u)
				* static_cast<std::uint64_t>(maximumFactor + 1u);
			std::uint64_t newTotal = 0u;
			if (!CheckedAdd(
				result.totalGeneratedVertices,
				built.generatedVertexCount,
				newTotal)
				|| newTotal > request.maximumGeneratedVertices)
			{
				result.fallbackRequired = true;
				result.diagnostic = "displacement-generation-budget-exceeded";
				result.regions.clear();
				result.totalGeneratedVertices = 0u;
				return result;
			}
			result.totalGeneratedVertices = newTotal;
			built.sharedBoundaryDomainIdentity = Mix(
				Mix(1469598103934665603ull, region.regionIdentity),
				maximumFactor);
			generationHash = Mix(generationHash, region.regionIdentity);
			generationHash = Mix(
				generationHash,
				built.sharedBoundaryDomainIdentity);
			generationHash = Mix(
				generationHash,
				std::bit_cast<std::uint32_t>(
					request.maximumDisplacement));
			result.regions.push_back(built);
		}
		result.generationIdentity = generationHash == 0u
			? 1u
			: generationHash;
		return result;
	}
} // namespace TerraEngine::Editor
