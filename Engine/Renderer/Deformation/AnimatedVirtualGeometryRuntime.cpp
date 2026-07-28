#include "AnimatedVirtualGeometryRuntime.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <tuple>

namespace TerraEngine::Renderer {

	namespace {

		bool Finite(float value)
		{
			return std::isfinite(value);
		}

		bool Finite3(const std::array<float, 3>& value)
		{
			return Finite(value[0]) && Finite(value[1]) && Finite(value[2]);
		}

		std::array<float, 3> Transform(
			const CpuAffinePalette& matrix,
			const std::array<float, 3>& value,
			float translation)
		{
			return {
				matrix.rows[0][0] * value[0] + matrix.rows[0][1] * value[1]
					+ matrix.rows[0][2] * value[2] + matrix.rows[0][3] * translation,
				matrix.rows[1][0] * value[0] + matrix.rows[1][1] * value[1]
					+ matrix.rows[1][2] * value[2] + matrix.rows[1][3] * translation,
				matrix.rows[2][0] * value[0] + matrix.rows[2][1] * value[1]
					+ matrix.rows[2][2] * value[2] + matrix.rows[2][3] * translation
			};
		}

		bool Normalize(std::array<float, 3>& value)
		{
			const float lengthSquared = value[0] * value[0]
				+ value[1] * value[1] + value[2] * value[2];
			if(!Finite(lengthSquared) || lengthSquared <= 1.0e-12f)
			{
				return false;
			}
			const float inverseLength = 1.0f / std::sqrt(lengthSquared);
			for(float& component : value)
			{
				component *= inverseLength;
			}
			return Finite3(value);
		}

		bool TryAdd(std::uint64_t left, std::uint64_t right, std::uint64_t& output)
		{
			if(right > std::numeric_limits<std::uint64_t>::max() - left)
			{
				return false;
			}
			output = left + right;
			return true;
		}

		bool TryMultiply(std::uint64_t left, std::uint64_t right, std::uint64_t& output)
		{
			if(left != 0u && right > std::numeric_limits<std::uint64_t>::max() / left)
			{
				return false;
			}
			output = left * right;
			return true;
		}

	}

	CpuDeformationResult EvaluateAnimatedPageVertexReference(
		const CpuDeformationVertex& source,
		std::span<const CpuMorphDelta> targetMajorDeltas,
		std::span<const float> morphWeights,
		const CpuSkinRecord& skin,
		std::span<const CpuAffinePalette> palette,
		const GpuDeformedVertexRecord* compatibleHistory)
	{
		CpuDeformationResult result;
		if(!Finite3(source.position) || !Finite3(source.normal)
			|| !Finite3(source.tangent) || !Finite(source.tangentSign)
			|| targetMajorDeltas.size() != morphWeights.size())
		{
			result.rejectionReason = "Source data or morph shape is invalid.";
			return result;
		}

		std::array<float, 3> position = source.position;
		std::array<float, 3> normal = source.normal;
		std::array<float, 3> tangent = source.tangent;
		for(std::size_t target = 0u; target < morphWeights.size(); ++target)
		{
			const float weight = morphWeights[target];
			const CpuMorphDelta& delta = targetMajorDeltas[target];
			if(!Finite(weight) || !Finite3(delta.position)
				|| !Finite3(delta.normal) || !Finite3(delta.tangent))
			{
				result.rejectionReason = "Morph weight or delta is non-finite.";
				return result;
			}
			for(std::size_t lane = 0u; lane < 3u; ++lane)
			{
				position[lane] += delta.position[lane] * weight;
				normal[lane] += delta.normal[lane] * weight;
				tangent[lane] += delta.tangent[lane] * weight;
			}
		}

		std::uint32_t weightSum = 0u;
		for(std::uint8_t weight : skin.weights)
		{
			weightSum += weight;
		}
		if(weightSum != 0u)
		{
			std::array<float, 3> skinnedPosition{};
			std::array<float, 3> skinnedNormal{};
			std::array<float, 3> skinnedTangent{};
			for(std::size_t influence = 0u; influence < skin.weights.size(); ++influence)
			{
				const std::uint8_t weightByte = skin.weights[influence];
				if(weightByte == 0u)
				{
					continue;
				}
				const std::uint8_t joint = skin.joints[influence];
				if(joint >= palette.size())
				{
					result.rejectionReason = "A skin record references a missing page-local joint.";
					return result;
				}
				const float weight = static_cast<float>(weightByte)
					/ static_cast<float>(weightSum);
				const auto transformedPosition = Transform(palette[joint], position, 1.0f);
				const auto transformedNormal = Transform(palette[joint], normal, 0.0f);
				const auto transformedTangent = Transform(palette[joint], tangent, 0.0f);
				for(std::size_t lane = 0u; lane < 3u; ++lane)
				{
					skinnedPosition[lane] += transformedPosition[lane] * weight;
					skinnedNormal[lane] += transformedNormal[lane] * weight;
					skinnedTangent[lane] += transformedTangent[lane] * weight;
				}
			}
			position = skinnedPosition;
			normal = skinnedNormal;
			tangent = skinnedTangent;
		}

		if(!Finite3(position) || !Normalize(normal) || !Normalize(tangent))
		{
			result.rejectionReason = "Deformation produced a non-finite or degenerate direction.";
			return result;
		}
		result.vertex.currentPosition = { position[0], position[1], position[2], 1.0f };
		result.vertex.previousPosition = compatibleHistory != nullptr
			? compatibleHistory->currentPosition
			: result.vertex.currentPosition;
		result.vertex.normal = { normal[0], normal[1], normal[2], 0.0f };
		result.vertex.tangentAndSign = {
			tangent[0], tangent[1], tangent[2], source.tangentSign
		};
		result.valid = true;
		return result;
	}

	DynamicBoundsResult ReduceAnimatedPageBounds(
		std::span<const GpuDeformedVertexRecord> vertices,
		std::array<std::uint32_t, 4> identity)
	{
		DynamicBoundsResult result;
		if(vertices.empty())
		{
			result.rejectionReason = "Bounds reduction requires at least one vertex.";
			return result;
		}

		std::array<float, 3> minimum{
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max()
		};
		std::array<float, 3> maximum{
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest()
		};
		for(const GpuDeformedVertexRecord& vertex : vertices)
		{
			for(std::size_t lane = 0u; lane < 3u; ++lane)
			{
				const float value = vertex.currentPosition[lane];
				if(!Finite(value))
				{
					result.proof = DynamicBoundsProof::NonFinite;
					result.rejectionReason = "A deformed position is non-finite.";
					return result;
				}
				minimum[lane] = std::min(minimum[lane], value);
				maximum[lane] = std::max(maximum[lane], value);
			}
		}
		std::array<float, 3> center{};
		for(std::size_t lane = 0u; lane < 3u; ++lane)
		{
			center[lane] = (minimum[lane] + maximum[lane]) * 0.5f;
		}
		float radiusSquared = 0.0f;
		for(const GpuDeformedVertexRecord& vertex : vertices)
		{
			const float x = vertex.currentPosition[0] - center[0];
			const float y = vertex.currentPosition[1] - center[1];
			const float z = vertex.currentPosition[2] - center[2];
			radiusSquared = std::max(radiusSquared, x * x + y * y + z * z);
		}
		result.bounds.minimum = { minimum[0], minimum[1], minimum[2], 0.0f };
		result.bounds.maximum = { maximum[0], maximum[1], maximum[2], 0.0f };
		result.bounds.sphere = {
			center[0], center[1], center[2], std::sqrt(radiusSquared)
		};
		result.bounds.identity = identity;
		result.proof = DynamicBoundsProof::Current;
		result.valid = true;
		return result;
	}

	DeformationWorkingSetPlan PlanDeformationWorkingSet(
		const GpuDeformationPlan& plan,
		std::uint64_t limitBytes)
	{
		DeformationWorkingSetPlan output;
		output.limitBytes = limitBytes;
		if(!plan.IsComplete())
		{
			output.rejectionReason = "The deformation plan is incomplete.";
			return output;
		}
		if(!TryMultiply(plan.totalVertexCount, sizeof(GpuDeformedVertexRecord),
			output.requested.currentBytes)
			|| !TryMultiply(plan.totalVertexCount, sizeof(GpuDeformedVertexRecord),
				output.requested.historyBytes)
			|| !TryMultiply(
				static_cast<std::uint64_t>(plan.totalMeshletBoundsCount)
					+ plan.totalClusterBoundsCount,
				sizeof(GpuDynamicBoundsRecord),
				output.requested.boundsBytes)
			|| !TryAdd(output.requested.currentBytes,
				output.requested.historyBytes, output.totalBytes)
			|| !TryAdd(output.totalBytes,
				output.requested.boundsBytes, output.totalBytes))
		{
			output.rejectionReason = "Deformation working-set arithmetic overflowed.";
			return output;
		}
		if(limitBytes != 0u && output.totalBytes > limitBytes)
		{
			output.rejectionReason = "Deformation working set exceeds its configured budget.";
			return output;
		}
		output.committed = output.requested;
		output.accepted = true;
		return output;
	}

	bool DeformedPagePublication::IsComplete() const noexcept
	{
		return publicationGeneration != 0u && key.IsComplete()
			&& staticAllocationId != 0u && currentAllocationId != 0u
			&& previousAllocationId != 0u && boundsAllocationId != 0u
			&& completionFence != 0u && geometryValid
			&& boundsValid && refinementValid;
	}

	DeformedPagePublicationResult PublishDeformedPageAtomically(
		const DeformedPagePublication& prior,
		const DeformedPagePublication& candidate)
	{
		DeformedPagePublicationResult result;
		result.visible = prior;
		if(!candidate.IsComplete())
		{
			result.rejectionReason = "Candidate publication is incomplete.";
			return result;
		}
		if(prior.publicationGeneration != 0u
			&& candidate.publicationGeneration <= prior.publicationGeneration)
		{
			result.rejectionReason = "Candidate publication generation is not newer.";
			return result;
		}
		if(candidate.historyValid
			&& candidate.previousAllocationId == candidate.currentAllocationId)
		{
			result.rejectionReason = "Valid history cannot alias the current output.";
			return result;
		}
		result.visible = candidate;
		result.accepted = true;
		return result;
	}

	AnimatedGeometryDiagnosticSnapshot CoalesceAnimatedGeometryDiagnostics(
		std::span<const AnimatedGeometryDiagnostic> diagnostics,
		std::size_t capacity)
	{
		AnimatedGeometryDiagnosticSnapshot output;
		std::vector<AnimatedGeometryDiagnostic> ordered(diagnostics.begin(), diagnostics.end());
		std::sort(ordered.begin(), ordered.end(),
			[](const AnimatedGeometryDiagnostic& left,
				const AnimatedGeometryDiagnostic& right)
			{
				return std::tie(left.viewId, left.stableInstanceId, left.pageId,
					left.fallback, left.relevantGeneration)
					< std::tie(right.viewId, right.stableInstanceId, right.pageId,
						right.fallback, right.relevantGeneration);
			});
		for(const AnimatedGeometryDiagnostic& diagnostic : ordered)
		{
			const bool duplicate = !output.details.empty()
				&& output.details.back().viewId == diagnostic.viewId
				&& output.details.back().stableInstanceId == diagnostic.stableInstanceId
				&& output.details.back().pageId == diagnostic.pageId
				&& output.details.back().fallback == diagnostic.fallback
				&& output.details.back().relevantGeneration == diagnostic.relevantGeneration;
			if(duplicate)
			{
				++output.coalesced;
				continue;
			}
			if(output.details.size() >= capacity)
			{
				++output.dropped;
				continue;
			}
			output.details.push_back(diagnostic);
		}
		output.emitted = static_cast<std::uint32_t>(output.details.size());
		return output;
	}

}
