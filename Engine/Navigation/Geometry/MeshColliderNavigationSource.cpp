// MeshColliderNavigationSource.cpp -- Checked immutable mesh/collider capture.

#include "Navigation/Geometry/MeshColliderNavigationSource.h"

#include <unordered_map>
#include <utility>

namespace TerraEngine::Navigation
{
	namespace
	{
		[[nodiscard]] bool IsSupportedSourceKind(NavigationSourceKind sourceKind) noexcept
		{
			return sourceKind == NavigationSourceKind::Mesh || sourceKind == NavigationSourceKind::Collider;
		}

		[[nodiscard]] bool IsCompatibleGeometryKind(const MeshColliderNavigationSourceInput& input) noexcept
		{
			if(input.geometryKind == MeshColliderGeometryKind::Auto) return true;
			if(input.geometryKind == MeshColliderGeometryKind::MeshTriangles || input.geometryKind == MeshColliderGeometryKind::ColliderTriangles
				|| input.geometryKind == MeshColliderGeometryKind::ConvexCollider) {
				return !input.geometry.triangles.empty();
			}
			return input.geometryKind != MeshColliderGeometryKind::AnalyticCollider || !input.geometry.primitives.empty();
		}
	}

	MeshColliderNavigationCaptureResult CaptureMeshColliderNavigationSources(
		const std::vector<MeshColliderNavigationSourceInput>& inputs)
	{
		MeshColliderNavigationCaptureResult result{};
		std::vector<MeshColliderNavigationSourceSnapshot> captured;
		captured.reserve(inputs.size());
		std::unordered_map<std::uint64_t, std::uint64_t> deduplicatedFingerprints;

		for(const MeshColliderNavigationSourceInput& input : inputs) {
			if(!input.enabled) continue;
			if(!IsSupportedSourceKind(input.sourceKind)) {
				result.reason = "MeshColliderSourceKindIsNotSupported";
				return result;
			}
			if(!IsCompatibleGeometryKind(input)) {
				result.reason = "MeshColliderSourceGeometryKindDoesNotMatchPayload";
				return result;
			}

			// Snapshot creation performs finite/count/index validation before copying
			// vectors. No input record is moved or modified during a rejected capture.
			NavigationGeometrySnapshotDescriptor descriptor = input.descriptor;
			// The descriptor is the immutable provenance recorded in the snapshot.
			// Derive its kind from the adapter boundary, not from a caller default.
			descriptor.sourceKind = input.sourceKind;
			const NavigationGeometrySnapshotCreateResult snapshot = NavigationGeometrySnapshot::Create(
				descriptor, input.geometry);
			if(!snapshot.accepted || snapshot.snapshot == nullptr) {
				result.reason = snapshot.reason.empty() ? "MeshColliderSourceSnapshotRejected" : snapshot.reason;
				return result;
			}

			const std::uint64_t fingerprint = snapshot.snapshot->ContentFingerprint();
			if(input.deduplicationGroup != 0U) {
				const auto [found, inserted] = deduplicatedFingerprints.emplace(input.deduplicationGroup, fingerprint);
				if(!inserted) {
					if(found->second != fingerprint) {
						result.reason = "MeshColliderDeduplicationGroupHasDifferentGeometry";
						return result;
					}
					continue;
				}
			}

			captured.push_back({ input.sourceKind, input.contribution, input.geometryKind, snapshot.snapshot });
		}

		result.accepted = true;
		result.sources = std::move(captured);
		return result;
	}
}
