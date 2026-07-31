// VirtualGeometrySplineCurveBuilder.cpp - Deterministic curve tessellation.

#include "VirtualGeometrySplineCurveBuilder.h"

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

		bool FinitePoint(const VirtualGeometryCurveControlPoint &point) noexcept
		{
			return std::isfinite(point.position[0])
				&& std::isfinite(point.position[1])
				&& std::isfinite(point.position[2])
				&& std::isfinite(point.width)
				&& point.width >= 0.0F;
		}

		float SpanLength(
			const VirtualGeometryCurveControlPoint &left,
			const VirtualGeometryCurveControlPoint &right) noexcept
		{
			const float x = right.position[0] - left.position[0];
			const float y = right.position[1] - left.position[1];
			const float z = right.position[2] - left.position[2];
			return std::sqrt(x * x + y * y + z * z);
		}
	} // namespace

	VirtualGeometrySplineCurveBuildResult BuildVirtualGeometrySplineCurve(
		const VirtualGeometrySplineCurveBuildRequest &request)
	{
		VirtualGeometrySplineCurveBuildResult result{};
		if (request.controlPoints.size() < 2u
			|| !std::isfinite(request.maximumChordError)
			|| request.maximumChordError <= 0.0F
			|| request.maximumSegmentsPerSpan == 0u
			|| request.maximumGeneratedSegments == 0u
			|| request.sourceGeneration == 0u)
		{
			result.fallbackRequired = true;
			result.diagnostic = "spline-curve-input-invalid";
			return result;
		}
		for (const auto &point : request.controlPoints)
		{
			if (!FinitePoint(point) || point.stableIdentity == 0u)
			{
				result.fallbackRequired = true;
				result.diagnostic = "spline-curve-control-point-invalid";
				return result;
			}
		}

		std::uint64_t requestedSegments = 0u;
		for (std::size_t index = 1u;
			index < request.controlPoints.size();
			++index)
		{
			const float length = SpanLength(
				request.controlPoints[index - 1u],
				request.controlPoints[index]);
			const std::uint32_t segments = length <= std::numeric_limits<float>::epsilon()
				? 1u
				: std::clamp(
					static_cast<std::uint32_t>(
						std::ceil(length / request.maximumChordError)),
					1u,
					request.maximumSegmentsPerSpan);
			requestedSegments += segments;
			if (requestedSegments > request.maximumGeneratedSegments)
			{
				result.fallbackRequired = true;
				result.diagnostic = "spline-curve-segment-budget-exceeded";
				return result;
			}
		}

		result.vertices.reserve(
			static_cast<std::size_t>(requestedSegments) + 1u);
		result.lineIndices.reserve(
			static_cast<std::size_t>(requestedSegments) * 2u);
		result.boundsMinimum.fill(std::numeric_limits<float>::max());
		result.boundsMaximum.fill(std::numeric_limits<float>::lowest());
		std::uint64_t generationHash =
			Mix(1469598103934665603ull, request.sourceGeneration);

		for (std::size_t spanIndex = 1u;
			spanIndex < request.controlPoints.size();
			++spanIndex)
		{
			const auto &left = request.controlPoints[spanIndex - 1u];
			const auto &right = request.controlPoints[spanIndex];
			const float length = SpanLength(left, right);
			const std::uint32_t segmentCount =
				length <= std::numeric_limits<float>::epsilon()
				? 1u
				: std::clamp(
					static_cast<std::uint32_t>(
						std::ceil(length / request.maximumChordError)),
					1u,
					request.maximumSegmentsPerSpan);
			if (length <= std::numeric_limits<float>::epsilon())
			{
				++result.zeroLengthSpanCount;
			}
			const std::uint64_t sectionIdentity = Mix(
				Mix(1469598103934665603ull, left.stableIdentity),
				right.stableIdentity);
			for (std::uint32_t segment = 0u;
				segment <= segmentCount;
				++segment)
			{
				if (spanIndex != 1u && segment == 0u) continue;
				const float t = static_cast<float>(segment)
					/ static_cast<float>(segmentCount);
				VirtualGeometrySplineCurveVertex vertex{};
				for (std::size_t component = 0u; component < 3u; ++component)
				{
					vertex.position[component] =
						left.position[component]
						+ (right.position[component] - left.position[component]) * t;
					result.boundsMinimum[component] = std::min(
						result.boundsMinimum[component],
						vertex.position[component] - std::max(left.width, right.width));
					result.boundsMaximum[component] = std::max(
						result.boundsMaximum[component],
						vertex.position[component] + std::max(left.width, right.width));
					generationHash = Mix(
						generationHash,
						std::bit_cast<std::uint32_t>(vertex.position[component]));
				}
				vertex.width = left.width + (right.width - left.width) * t;
				vertex.materialIdentity = segment == segmentCount
					? right.materialIdentity
					: left.materialIdentity;
				vertex.sectionIdentity = sectionIdentity;
				if (!result.vertices.empty())
				{
					result.lineIndices.push_back(
						static_cast<std::uint32_t>(result.vertices.size() - 1u));
					result.lineIndices.push_back(
						static_cast<std::uint32_t>(result.vertices.size()));
				}
				result.vertices.push_back(vertex);
			}
		}
		result.generationIdentity = generationHash == 0u
			? 1u
			: generationHash;
		return result;
	}
} // namespace TerraEngine::Editor
