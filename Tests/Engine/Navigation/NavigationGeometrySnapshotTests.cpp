// Test-first contract tests for checked provider-neutral navigation geometry.
//
// The planned format and snapshot headers are intentionally included directly.
// Until the geometry snapshot implementation exists, this translation unit must
// fail to compile. A fallback that skips unavailable functionality would turn a
// missing navigation foundation into a misleading passing test.

#include "../TestHarness.h"

#include "Navigation/Geometry/NavigationGeometryFormat.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationBudget.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace TerraEngine::Navigation;

		bool Contains(const std::vector<std::string>& values, std::string_view expected)
		{
			for(const std::string& value : values) {
				if(value == expected) return true;
			}
			return false;
		}

		void WriteLittleEndianU64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value)
		{
			for(std::size_t byteIndex = 0U; byteIndex < sizeof(value); ++byteIndex) {
				bytes.at(offset + byteIndex) = static_cast<std::byte>((value >> (byteIndex * 8U)) & 0xffU);
			}
		}

		NavigationGeometryDocument MakeTriangleDocument()
		{
			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {
				{0.0F, 0.0F, 0.0F},
				{2.0F, 0.0F, 0.0F},
				{0.0F, 0.0F, 2.0F}
			};
			document.triangles = {{0U, 1U, 2U}};
			return document;
		}

		NavigationGeometrySnapshotDescriptor MakeSnapshotDescriptor()
		{
			NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceUuid = NavigationUuid::FromString("d86d4a52-6724-4fa8-9af8-74b9f08b12e9");
			descriptor.sourceRevision = 7U;
			descriptor.canonicalPlacementRevision = 11U;
			descriptor.physicalReadyRevision = 13U;
			descriptor.sourceKind = NavigationSourceKind::Mesh;
			descriptor.subshapeIndex = 0U;
			return descriptor;
		}

		NavigationEligibleSourceCandidate MakeEligibleCandidate(
			std::string_view sourceId, NavigationSourceKind kind, std::uint32_t subshapeIndex = 0U)
		{
			NavigationEligibleSourceCandidate candidate{};
			candidate.identity.sourceId = NavigationUuid::FromString(sourceId).value;
			candidate.identity.objectId = NavigationUuid::FromString("c9ed7ec1-0441-460b-bd23-9af6736b4abc").value;
			candidate.identity.objectLifetimeGeneration = 3U;
			candidate.identity.assetId = NavigationUuid::FromString("be548fbc-2e49-49ec-8a0a-1e67f0453fdb").value;
			candidate.identity.kind = kind;
			candidate.identity.subshapeIndex = subshapeIndex;
			candidate.revisions = {3U, 5U, 7U, 11U, 13U};
			candidate.activeInHierarchy = true;
			candidate.physicalBackingCurrent = true;
			NavigationGeometrySnapshotDescriptor descriptor = MakeSnapshotDescriptor();
			descriptor.sourceUuid.value = candidate.identity.sourceId;
			descriptor.sourceKind = kind;
			descriptor.subshapeIndex = subshapeIndex;
			descriptor.sourceRevision = ComputeNavigationEligibleSourceRevision(
				candidate.identity, candidate.revisions);
			candidate.geometry = NavigationGeometrySnapshot::Create(descriptor, MakeTriangleDocument()).snapshot;
			return candidate;
		}
	}

	void RegisterNavigationGeometrySnapshotTests(TestRegistry& registry)
	{
		registry.Add("Navigation.GeometryFormat.DecodesCheckedTrianglePayload", [](TestContext& context) {
			const NavigationGeometryDocument original = MakeTriangleDocument();
			const NavigationGeometryEncodeResult encoded = EncodeNavigationGeometry(original);
			context.Expect(encoded.accepted, "A finite indexed triangle payload must encode into .tenavgeom bytes.");
			context.Expect(!encoded.bytes.empty(), "A successful .tenavgeom encoding must own its output bytes.");

			const NavigationGeometryDecodeResult decoded = DecodeNavigationGeometry(encoded.bytes);
			context.Expect(decoded.accepted, "A .tenavgeom payload produced by the checked encoder must decode.");
			context.ExpectEqual(NavigationGeometryPayloadKind::TriangleMesh, decoded.document.kind,
				"The decoder must preserve the declared provider-neutral payload kind.");
			context.ExpectEqual(std::size_t(3U), decoded.document.vertices.size(),
				"The decoder must preserve the bounded triangle vertex count.");
			context.ExpectEqual(std::size_t(1U), decoded.document.triangles.size(),
				"The decoder must preserve the bounded triangle primitive count.");
			context.ExpectEqual(std::uint32_t(2U), decoded.document.triangles.front().thirdVertex,
				"Triangle index records must survive checked serialization without provider data.");
		});

		registry.Add("Navigation.GeometryFormat.RejectsMalformedOffsetsCountsAndNonFiniteInput", [](TestContext& context) {
			const NavigationGeometryEncodeResult valid = EncodeNavigationGeometry(MakeTriangleDocument());
			context.Expect(valid.accepted, "Malformed-input checks require one independently valid baseline payload.");

			std::vector<std::byte> malformedOffset = valid.bytes;
			WriteLittleEndianU64(malformedOffset, NavigationGeometryWireLayout::kTrianglePayloadOffset,
				static_cast<std::uint64_t>(malformedOffset.size()) + 1U);
			context.Expect(!DecodeNavigationGeometry(malformedOffset).accepted,
				"A payload offset beyond the supplied byte range must reject before any payload allocation.");

			std::vector<std::byte> malformedCount = valid.bytes;
			WriteLittleEndianU64(malformedCount, NavigationGeometryWireLayout::kVertexCount,
				std::numeric_limits<std::uint64_t>::max());
			context.Expect(!DecodeNavigationGeometry(malformedCount).accepted,
				"An overflowing declared count must reject before multiplication or allocation.");

			NavigationGeometryDocument nonFinite = MakeTriangleDocument();
			nonFinite.vertices.front().x = std::numeric_limits<float>::quiet_NaN();
			context.Expect(!EncodeNavigationGeometry(nonFinite).accepted,
				"NaN geometry must be rejected before it can become an immutable snapshot.");

			nonFinite = MakeTriangleDocument();
			nonFinite.triangles.front().thirdVertex = 3U;
			context.Expect(!EncodeNavigationGeometry(nonFinite).accepted,
				"Triangle indices outside the declared vertex count must be rejected before publication.");
		});

		registry.Add("Navigation.GeometrySnapshot.OwnsImmutableTrianglesPrimitivesBvhAndLease", [](TestContext& context) {
			NavigationGeometryDocument source = MakeTriangleDocument();
			NavigationGeometryPrimitive box{};
			box.kind = NavigationGeometryPrimitiveKind::Box;
			box.center = {4.0F, 1.0F, 2.0F};
			box.halfExtents = {1.0F, 2.0F, 3.0F};
			source.primitives.push_back(box);

			const NavigationGeometrySnapshotCreateResult created = NavigationGeometrySnapshot::Create(
				MakeSnapshotDescriptor(), source);
			context.Expect(created.accepted, "Finite triangles and exact analytic primitives must create a snapshot.");
			context.Expect(created.snapshot != nullptr, "A successful snapshot result must return retained immutable ownership.");
			if(!created.accepted || created.snapshot == nullptr) return;

			const std::shared_ptr<const NavigationGeometrySnapshot> snapshot = created.snapshot;
			source.vertices.front().x = 999.0F;
			source.primitives.front().halfExtents.x = 999.0F;
			context.Expect(snapshot->Triangles().front().firstVertex.x == 0.0F,
				"Snapshots must copy provider-neutral triangles instead of borrowing mutable source memory.");
			context.Expect(snapshot->Primitives().front().halfExtents.x == 1.0F,
				"Snapshots must retain exact primitive descriptors independently of their source document.");
			context.Expect(snapshot->Bvh().PrimitiveRecordCount() == 2U,
				"The immutable BVH must cover both triangle and analytic primitive records.");
			context.Expect(snapshot->Bvh().RootRecord().primitiveCount > 0U,
				"A non-empty snapshot must publish a valid root primitive range for worker traversal.");

			{
				const NavigationGeometrySnapshotLease lease = snapshot->AcquireLease({120U, 2.0});
				context.Expect(lease.IsValid(), "A live immutable snapshot must issue an owned bounded worker/query lease.");
				context.Expect(!snapshot->CanRetire(), "A snapshot with an outstanding lease must not retire its worker-owned data.");
			}
			context.Expect(snapshot->CanRetire(),
				"Releasing the final lease must make an otherwise unreferenced snapshot eligible for deferred retirement.");
		});

		registry.Add("Navigation.GeometryBudget.ChargesOwnershipAndRejectsBeforePublication", [](TestContext& context) {
			NavigationBudgetLimits limits{};
			limits.geometryBytes = 1024U;
			NavigationBudget budget(limits);

			const NavigationBudgetReservation sourceCopy = budget.TryReserve(
				NavigationBudgetCategory::Geometry, 384U, "source CPU copy");
			const NavigationBudgetReservation bvhCopy = budget.TryReserve(
				NavigationBudgetCategory::Geometry, 512U, "snapshot BVH");
			context.Expect(sourceCopy.accepted && bvhCopy.accepted,
				"Source and BVH ownership must be charged before their snapshot can publish.");
			context.ExpectEqual(std::size_t(896U), budget.UsedBytes(NavigationBudgetCategory::Geometry),
				"Budget accounting must include each separately owned CPU and BVH allocation exactly once.");

			const NavigationBudgetReservation overBudget = budget.TryReserve(
				NavigationBudgetCategory::Geometry, 129U, "replacement snapshot");
			context.Expect(!overBudget.accepted,
				"A reservation that exceeds the geometry cap must reject before a replacement can publish.");
			context.ExpectEqual(std::size_t(896U), budget.UsedBytes(NavigationBudgetCategory::Geometry),
				"A rejected reservation must leave the published accounting unchanged.");
			context.Expect(Contains(budget.RejectionReasons(), "GeometryBudgetExceeded"),
				"Budget rejection must retain a stable observable reason instead of silently dropping geometry.");
		});

		registry.Add("Navigation.GeometrySnapshot.SelectsColliderBeforeEquivalentMesh", [](TestContext& context) {
			NavigationEligibleSourceCandidate mesh = MakeEligibleCandidate(
				"c569d771-40a2-481c-8e89-1a4d12c0620f", NavigationSourceKind::Mesh);
			NavigationEligibleSourceCandidate collider = MakeEligibleCandidate(
				"c569d771-40a2-481c-8e89-1a4d12c06210", NavigationSourceKind::Collider);
			// Deliberately submit mesh first: selection must use representation policy,
			// not input arrival order, when an object owns both alternatives.
			const NavigationEligibleSourceIndexBuildResult built = BuildNavigationEligibleSourceIndex(
				std::span<const NavigationEligibleSourceCandidate>{&mesh, 1U});
			const NavigationEligibleSourceIndexBuildResult colliderOnly = BuildNavigationEligibleSourceIndex(
				std::span<const NavigationEligibleSourceCandidate>{&collider, 1U});
			const std::array<NavigationEligibleSourceCandidate, 2U> alternatives{mesh, collider};
			const NavigationEligibleSourceIndexBuildResult selected = BuildNavigationEligibleSourceIndex(alternatives);
			context.Expect(built.accepted && colliderOnly.accepted && selected.accepted,
				"Valid copied physical candidates must build deterministic immutable source indexes.");
			context.Expect(selected.index.Records().size() == 1U
				&& selected.index.Records().front().identity.kind == NavigationSourceKind::Collider,
				"A current collider must replace the equivalent mesh representation before worker publication.");
			context.Expect(selected.index.Exclusions().size() == 1U
				&& selected.index.Exclusions().front().code == NavigationEligibleSourceSelectionCode::SupersededByCollider,
				"Collider preference must remain observable as an immutable exclusion diagnostic.");
		});

		registry.Add("Navigation.GeometrySnapshot.ExcludesStaleAndInactiveSourcesWithoutMutatingIndex", [](TestContext& context) {
			NavigationEligibleSourceCandidate active = MakeEligibleCandidate(
				"2b7330fd-1aa2-4a7a-a5ed-4b87b6e95d01", NavigationSourceKind::Terrain);
			NavigationEligibleSourceCandidate inactive = MakeEligibleCandidate(
				"2b7330fd-1aa2-4a7a-a5ed-4b87b6e95d02", NavigationSourceKind::Mesh);
			inactive.activeInHierarchy = false;
			const std::array<NavigationEligibleSourceCandidate, 2U> candidates{active, inactive};
			const NavigationEligibleSourceIndexBuildResult built = BuildNavigationEligibleSourceIndex(candidates);
			context.Expect(built.accepted && built.index.Records().size() == 1U && built.index.HasCurrentTerrain(),
				"Inactive supplementary geometry must not remove an active current terrain source from the candidate index.");
			context.Expect(built.index.Exclusions().size() == 1U
				&& built.index.Exclusions().front().code == NavigationEligibleSourceSelectionCode::Inactive,
				"An inactive object must be excluded with a stable reason rather than captured from live state later.");
			active.revisions.transformRevision++;
			context.Expect(ComputeNavigationEligibleSourceRevision(active.identity, active.revisions)
				!= built.index.Records().front().geometry->Descriptor().sourceRevision,
				"A changed canonical transform revision must invalidate the previously captured source revision.");
		});

		registry.Add("Navigation.GeometrySnapshot.AcceptsTypedProceduralInputWithoutRenderVisibility", [](TestContext& context) {
			NavigationEligibleSourceCandidate procedural = MakeEligibleCandidate(
				"739bd917-50a2-4ca5-a8f6-7f325af95d23", NavigationSourceKind::Procedural);
			// Eligibility has no render-visible flag: typed physical publication and
			// stable revisions are the complete worker-safe admission evidence.
			const NavigationEligibleSourceIndexBuildResult built = BuildNavigationEligibleSourceIndex(
				std::span<const NavigationEligibleSourceCandidate>{&procedural, 1U});
			context.Expect(built.accepted && built.index.Records().size() == 1U
				&& built.index.Records().front().identity.kind == NavigationSourceKind::Procedural,
				"A current typed procedural packet must participate without a renderer visibility dependency.");
		});
	}
}
