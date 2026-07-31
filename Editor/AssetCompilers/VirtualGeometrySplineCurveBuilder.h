// VirtualGeometrySplineCurveBuilder.h - Bounded spline and curve candidates.
//
// A caller captures immutable control points before invoking this builder on a
// worker. The result owns all generated data and can be validated before the
// protected geometry-generation publication boundary.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Editor
{
	struct VirtualGeometryCurveControlPoint
	{
		std::array<float, 3u> position{};
		float width = 1.0F;
		std::uint32_t materialIdentity = 0u;
		std::uint64_t stableIdentity = 0u;
	};

	struct VirtualGeometrySplineCurveBuildRequest
	{
		std::span<const VirtualGeometryCurveControlPoint> controlPoints;
		float maximumChordError = 0.01F;
		std::uint32_t maximumSegmentsPerSpan = 64u;
		std::uint32_t maximumGeneratedSegments = 1u << 20u;
		std::uint64_t sourceGeneration = 0u;
	};

	struct VirtualGeometrySplineCurveVertex
	{
		std::array<float, 3u> position{};
		float width = 0.0F;
		std::uint32_t materialIdentity = 0u;
		std::uint64_t sectionIdentity = 0u;
	};

	struct VirtualGeometrySplineCurveBuildResult
	{
		std::vector<VirtualGeometrySplineCurveVertex> vertices;
		std::vector<std::uint32_t> lineIndices;
		std::array<float, 3u> boundsMinimum{};
		std::array<float, 3u> boundsMaximum{};
		std::uint64_t generationIdentity = 0u;
		std::uint32_t zeroLengthSpanCount = 0u;
		bool fallbackRequired = false;
		std::string diagnostic;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return diagnostic.empty()
				&& generationIdentity != 0u
				&& vertices.size() >= 2u
				&& (lineIndices.size() % 2u) == 0u;
		}
	};

	[[nodiscard]] VirtualGeometrySplineCurveBuildResult
		BuildVirtualGeometrySplineCurve(
			const VirtualGeometrySplineCurveBuildRequest &request);
} // namespace TerraEngine::Editor
