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

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
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
	}
}
