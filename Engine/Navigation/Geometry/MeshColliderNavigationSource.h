// MeshColliderNavigationSource.h -- Provider-neutral mesh/collider capture.
//
// This boundary copies authoring/physics geometry into immutable snapshot
// ownership. It deliberately exposes no renderer, physics, Recast, or Detour
// object, so workers can safely retain the result after a scene object changes.

#pragma once

#include "Navigation/Geometry/NavigationGeometrySnapshot.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Navigation
{
	// A source may provide walkable floor contact or solid collision. Keeping the
	// role explicit prevents a collider wall from becoming floor support merely
	// because it has triangles or an analytic primitive.
	enum class GroundGeometryContribution : std::uint8_t
	{
		WalkableSupport,
		SolidObstruction
	};

	// The adapter kind records which engine-owned input path produced the neutral
	// document. Convex colliders become a finite triangle hull; analytic collider
	// dimensions remain analytic for final clearance validation.
	enum class MeshColliderGeometryKind : std::uint8_t
	{
		Auto,
		MeshTriangles,
		ColliderTriangles,
		ConvexCollider,
		AnalyticCollider
	};

	struct MeshColliderNavigationSourceInput
	{
		NavigationGeometrySnapshotDescriptor descriptor{};
		NavigationSourceKind sourceKind = NavigationSourceKind::Mesh;
		GroundGeometryContribution contribution = GroundGeometryContribution::WalkableSupport;
		MeshColliderGeometryKind geometryKind = MeshColliderGeometryKind::Auto;
		bool enabled = true;
		// A non-zero group identifies equivalent authored representations of one
		// physical surface. The capture result retains only one matching payload.
		std::uint64_t deduplicationGroup = 0U;
		NavigationGeometryDocument geometry{};
	};

	struct MeshColliderNavigationSourceSnapshot
	{
		NavigationSourceKind sourceKind = NavigationSourceKind::Mesh;
		GroundGeometryContribution contribution = GroundGeometryContribution::WalkableSupport;
		MeshColliderGeometryKind geometryKind = MeshColliderGeometryKind::Auto;
		std::shared_ptr<const NavigationGeometrySnapshot> geometry;
	};

	struct MeshColliderNavigationCaptureResult
	{
		bool accepted = false;
		std::string reason;
		std::vector<MeshColliderNavigationSourceSnapshot> sources;
	};

	// A small copied scene description is intentionally provider neutral. Route
	// preparation may capture it through the adapter before converting it into
	// immutable worker snapshots; it must never borrow a live component pointer.
	struct GroundRouteScene
	{
		std::vector<MeshColliderNavigationSourceInput> sources;
	};

	// Validates every source before constructing result ownership. A failure is
	// atomic: no partially captured source list is returned to a caller.
	[[nodiscard]] TERRAENGINE_API MeshColliderNavigationCaptureResult CaptureMeshColliderNavigationSources(
		const std::vector<MeshColliderNavigationSourceInput>& inputs);
}
