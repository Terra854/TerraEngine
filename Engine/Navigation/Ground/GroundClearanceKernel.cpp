// GroundClearanceKernel.cpp -- Bounded continuous source-geometry validation.

#include "Navigation/Ground/GroundClearanceKernel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace TerraEngine::Navigation
{
	namespace
	{
		constexpr float kEpsilon = 0.0001F;

		struct SupportSample
		{
			bool found = false;
			float height = 0.0F;
			float slopeDegrees = 0.0F;
		};

		[[nodiscard]] bool IsFinite(const NavigationGeometryVector3& point) noexcept
		{
			return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
		}

		[[nodiscard]] NavigationGeometryVector3 Lerp(const NavigationGeometryVector3& first,
			const NavigationGeometryVector3& second, float t) noexcept
		{
			return { first.x + (second.x - first.x) * t, first.y + (second.y - first.y) * t,
				first.z + (second.z - first.z) * t };
		}

		[[nodiscard]] bool PointInBoxXZ(const NavigationGeometryVector3& point,
			const NavigationGeometryPrimitive& box) noexcept
		{
			return point.x >= box.center.x - box.halfExtents.x - kEpsilon
				&& point.x <= box.center.x + box.halfExtents.x + kEpsilon
				&& point.z >= box.center.z - box.halfExtents.z - kEpsilon
				&& point.z <= box.center.z + box.halfExtents.z + kEpsilon;
		}

		[[nodiscard]] bool SampleTriangleHeight(const NavigationGeometryVector3& point,
			const NavigationGeometryTriangleRecord& triangle, float& outHeight, float& outSlopeDegrees) noexcept
		{
			const float denominator = (triangle.secondVertex.z - triangle.thirdVertex.z) * (triangle.firstVertex.x - triangle.thirdVertex.x)
				+ (triangle.thirdVertex.x - triangle.secondVertex.x) * (triangle.firstVertex.z - triangle.thirdVertex.z);
			if(std::fabs(denominator) <= kEpsilon) return false;
			const float firstWeight = ((triangle.secondVertex.z - triangle.thirdVertex.z) * (point.x - triangle.thirdVertex.x)
				+ (triangle.thirdVertex.x - triangle.secondVertex.x) * (point.z - triangle.thirdVertex.z)) / denominator;
			const float secondWeight = ((triangle.thirdVertex.z - triangle.firstVertex.z) * (point.x - triangle.thirdVertex.x)
				+ (triangle.firstVertex.x - triangle.thirdVertex.x) * (point.z - triangle.thirdVertex.z)) / denominator;
			const float thirdWeight = 1.0F - firstWeight - secondWeight;
			if(firstWeight < -kEpsilon || secondWeight < -kEpsilon || thirdWeight < -kEpsilon) return false;
			outHeight = firstWeight * triangle.firstVertex.y + secondWeight * triangle.secondVertex.y + thirdWeight * triangle.thirdVertex.y;
			const NavigationGeometryVector3 edgeA{ triangle.secondVertex.x - triangle.firstVertex.x, triangle.secondVertex.y - triangle.firstVertex.y, triangle.secondVertex.z - triangle.firstVertex.z };
			const NavigationGeometryVector3 edgeB{ triangle.thirdVertex.x - triangle.firstVertex.x, triangle.thirdVertex.y - triangle.firstVertex.y, triangle.thirdVertex.z - triangle.firstVertex.z };
			const float normalY = edgeA.z * edgeB.x - edgeA.x * edgeB.z;
			const float normalX = edgeA.y * edgeB.z - edgeA.z * edgeB.y;
			const float normalZ = edgeA.x * edgeB.y - edgeA.y * edgeB.x;
			const float normalLength = std::sqrt(normalX * normalX + normalY * normalY + normalZ * normalZ);
			if(normalLength <= kEpsilon) return false;
			outSlopeDegrees = std::acos(std::clamp(std::fabs(normalY) / normalLength, 0.0F, 1.0F)) * 57.2957795F;
			return true;
		}

		[[nodiscard]] SupportSample FindSupport(const GroundClearanceScene& scene, const NavigationGeometryVector3& point) noexcept
		{
			SupportSample best{};
			float nearestDistance = std::numeric_limits<float>::max();
			for(const GroundClearanceSource& source : scene.sources) {
				if(source.contribution != GroundGeometryContribution::WalkableSupport) continue;
				for(const NavigationGeometryPrimitive& primitive : source.geometry.primitives) {
					if(primitive.kind != NavigationGeometryPrimitiveKind::Box || !PointInBoxXZ(point, primitive)) continue;
					const float height = primitive.center.y + primitive.halfExtents.y;
					const float distance = std::fabs(point.y - height);
					if(distance < nearestDistance) { best = { true, height, 0.0F }; nearestDistance = distance; }
				}
				for(const NavigationGeometryTriangle& index : source.geometry.triangles) {
					float height = 0.0F;
					float slope = 0.0F;
					const NavigationGeometryTriangleRecord triangle{ source.geometry.vertices[index.firstVertex], source.geometry.vertices[index.secondVertex], source.geometry.vertices[index.thirdVertex] };
					if(!SampleTriangleHeight(point, triangle, height, slope)) continue;
					const float distance = std::fabs(point.y - height);
					if(distance < nearestDistance) { best = { true, height, slope }; nearestDistance = distance; }
				}
			}
			return best;
		}

		[[nodiscard]] float SquaredDistanceToBox(const NavigationGeometryVector3& point,
			const NavigationGeometryPrimitive& box) noexcept
		{
			const float minimumX = box.center.x - box.halfExtents.x;
			const float maximumX = box.center.x + box.halfExtents.x;
			const float minimumY = box.center.y - box.halfExtents.y;
			const float maximumY = box.center.y + box.halfExtents.y;
			const float minimumZ = box.center.z - box.halfExtents.z;
			const float maximumZ = box.center.z + box.halfExtents.z;
			const float dx = point.x < minimumX ? minimumX - point.x : (point.x > maximumX ? point.x - maximumX : 0.0F);
			const float dy = point.y < minimumY ? minimumY - point.y : (point.y > maximumY ? point.y - maximumY : 0.0F);
			const float dz = point.z < minimumZ ? minimumZ - point.z : (point.z > maximumZ ? point.z - maximumZ : 0.0F);
			return dx * dx + dy * dy + dz * dz;
		}

		[[nodiscard]] bool CapsuleIntersectsBox(const NavigationGeometryVector3& foot,
			const NavigationGeometryPrimitive& box, float radius, float height) noexcept
		{
			const float lowerSpine = foot.y + radius;
			const float upperSpine = foot.y + height - radius;
			const float boxMinimumY = box.center.y - box.halfExtents.y;
			const float boxMaximumY = box.center.y + box.halfExtents.y;
			const float closestY = boxMaximumY < lowerSpine ? lowerSpine : (boxMinimumY > upperSpine ? upperSpine : boxMinimumY);
			return SquaredDistanceToBox({ foot.x, closestY, foot.z }, box) <= radius * radius;
		}

		[[nodiscard]] NavigationGeometryPrimitive ConservativePrimitiveBounds(
			const NavigationGeometryPrimitive& primitive) noexcept
		{
			NavigationGeometryPrimitive bounds{};
			bounds.kind = NavigationGeometryPrimitiveKind::Box;
			bounds.center = primitive.center;
			if(primitive.kind == NavigationGeometryPrimitiveKind::Box) {
				bounds.halfExtents = primitive.halfExtents;
			} else if(primitive.kind == NavigationGeometryPrimitiveKind::Sphere) {
				bounds.halfExtents = { primitive.radius, primitive.radius, primitive.radius };
			} else {
				bounds.halfExtents = { primitive.radius, primitive.height * 0.5F, primitive.radius };
			}
			return bounds;
		}

		[[nodiscard]] NavigationGeometryPrimitive ConservativeTriangleBounds(
			const NavigationGeometryTriangleRecord& triangle) noexcept
		{
			const float minimumX = std::min({ triangle.firstVertex.x, triangle.secondVertex.x, triangle.thirdVertex.x });
			const float minimumY = std::min({ triangle.firstVertex.y, triangle.secondVertex.y, triangle.thirdVertex.y });
			const float minimumZ = std::min({ triangle.firstVertex.z, triangle.secondVertex.z, triangle.thirdVertex.z });
			const float maximumX = std::max({ triangle.firstVertex.x, triangle.secondVertex.x, triangle.thirdVertex.x });
			const float maximumY = std::max({ triangle.firstVertex.y, triangle.secondVertex.y, triangle.thirdVertex.y });
			const float maximumZ = std::max({ triangle.firstVertex.z, triangle.secondVertex.z, triangle.thirdVertex.z });
			NavigationGeometryPrimitive bounds{};
			bounds.kind = NavigationGeometryPrimitiveKind::Box;
			bounds.center = { (minimumX + maximumX) * 0.5F, (minimumY + maximumY) * 0.5F, (minimumZ + maximumZ) * 0.5F };
			bounds.halfExtents = { (maximumX - minimumX) * 0.5F, (maximumY - minimumY) * 0.5F, (maximumZ - minimumZ) * 0.5F };
			// A horizontal triangle has zero thickness, but the capsule predicate
			// needs a finite closed interval for conservative contact detection.
			bounds.halfExtents.y = std::max(bounds.halfExtents.y, kEpsilon);
			return bounds;
		}

		[[nodiscard]] bool IsNearQuantizationThreshold(float clearance, const NavigationProfile& profile) noexcept
		{
			const float uncertainty = static_cast<float>(std::max(profile.horizontalResolutionMetres, profile.verticalResolutionMetres));
			return std::fabs(clearance) <= uncertainty + kEpsilon;
		}
	}

	GroundClearanceResult ValidateGroundCapsuleSegment(const GroundCapsuleSegmentRequest& request)
	{
		GroundClearanceResult result{};
		if(!request.profile.Validate().accepted || !IsFinite(request.start) || !IsFinite(request.destination)) return result;
		for(const GroundClearanceSource& source : request.scene.sources) {
			NavigationGeometryBounds ignoredBounds{};
			std::string ignoredReason;
			if(!ValidateNavigationGeometryDocument(source.geometry, ignoredBounds, ignoredReason)) return result;
		}

		const float radius = static_cast<float>(request.profile.radiusMetres + request.profile.clearanceMarginMetres);
		const float height = static_cast<float>(request.profile.heightMetres);
		const float horizontalDistance = std::hypot(request.destination.x - request.start.x, request.destination.z - request.start.z);
		const std::uint32_t sampleCount = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::ceil(horizontalDistance / 0.025F)) + 1U, 2U, 4096U);
		float previousSupportHeight = 0.0F;
		bool hasPreviousSupport = false;
		for(std::uint32_t index = 0U; index < sampleCount; ++index) {
			const float fraction = static_cast<float>(index) / static_cast<float>(sampleCount - 1U);
			const NavigationGeometryVector3 foot = Lerp(request.start, request.destination, fraction);
			const SupportSample support = FindSupport(request.scene, foot);
			if(!support.found) { result.status = GroundClearanceStatus::MissingSupport; return result; }
			if(support.slopeDegrees > request.profile.maxSlopeDegrees + kEpsilon) { result.status = GroundClearanceStatus::ExcessiveSlope; return result; }
			if(std::fabs(support.height - foot.y) > static_cast<float>(request.profile.stepHeightMetres) + kEpsilon) {
				result.status = GroundClearanceStatus::MissingSupport;
				return result;
			}
			if(hasPreviousSupport && std::fabs(support.height - previousSupportHeight) > static_cast<float>(request.profile.stepHeightMetres) + kEpsilon) {
				result.status = GroundClearanceStatus::ExcessiveStep;
				return result;
			}
			previousSupportHeight = support.height;
			hasPreviousSupport = true;

			for(const GroundClearanceSource& source : request.scene.sources) {
				if(source.contribution != GroundGeometryContribution::SolidObstruction) continue;
				auto validateObstacle = [&](const NavigationGeometryPrimitive& primitive) -> bool {
					const NavigationGeometryPrimitive bounds = ConservativePrimitiveBounds(primitive);
					const float clearance = (bounds.center.y - bounds.halfExtents.y) - (foot.y + height);
					const float horizontalDistanceSquared = SquaredDistanceToBox(
						{ foot.x, bounds.center.y, foot.z }, bounds);
					const bool nearThreshold = horizontalDistanceSquared <= radius * radius
						&& IsNearQuantizationThreshold(clearance, request.profile);
					if(nearThreshold) {
						if(request.maximumRefinementSteps == 0U) {
							result.status = GroundClearanceStatus::ResolutionLimit;
							return true;
						}
						result.refinementSteps = std::min<std::uint32_t>(request.maximumRefinementSteps, 1U);
					}
					if(!CapsuleIntersectsBox(foot, bounds, radius, height)) return false;
					result.status = bounds.center.y - bounds.halfExtents.y >= foot.y + radius
						? GroundClearanceStatus::InsufficientClearance : GroundClearanceStatus::CornerCollision;
					return true;
				};
				for(const NavigationGeometryPrimitive& primitive : source.geometry.primitives) {
					if(validateObstacle(primitive)) return result;
				}
				for(const NavigationGeometryTriangle& triangleIndex : source.geometry.triangles) {
					const NavigationGeometryTriangleRecord triangle{ source.geometry.vertices[triangleIndex.firstVertex],
						source.geometry.vertices[triangleIndex.secondVertex], source.geometry.vertices[triangleIndex.thirdVertex] };
					if(validateObstacle(ConservativeTriangleBounds(triangle))) return result;
				}
			}
		}

		result.accepted = true;
		result.status = GroundClearanceStatus::Accepted;
		result.minimumSupportFraction = 1.0;
		return result;
	}
}
