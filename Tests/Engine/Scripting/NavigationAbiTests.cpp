// NavigationAbiTests.cpp - fixed-width managed navigation bridge coverage.

#include "../TestHarness.h"

#include "Navigation/NavigationSystem.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Scripting/ScriptGlue.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Navigation;

		NativeNavigationAbiHeader Header(std::size_t size) {
			return {kNativeNavigationAbiVersion, static_cast<std::uint32_t>(size)};
		}

		NativeNavigationWorldHandle World() { return {{1U}, 1U}; }
		NativeNavigationOwnerHandle Owner(std::uint64_t id = 7U, std::uint64_t generation = 1U) {
			return {World(), id, generation};
		}

		NativeNavigationLocation Location(double x = 0.0) {
			return {World(), 0, 0, 0, x, 0.0, 0.0};
		}

		NativeNavigationProfile Profile() {
			const NavigationProfile source = NavigationProfile::Defaults();
			NativeNavigationProfile value{};
			value.profileId = source.profileId;
			value.revision = source.revision;
			value.radiusMetres = source.radiusMetres;
			value.heightMetres = source.heightMetres;
			value.stepHeightMetres = source.stepHeightMetres;
			value.maxSlopeDegrees = source.maxSlopeDegrees;
			value.clearanceMarginMetres = source.clearanceMarginMetres;
			value.horizontalResolutionMetres = source.horizontalResolutionMetres;
			value.verticalResolutionMetres = source.verticalResolutionMetres;
			value.excludedAreasMask = source.excludedAreasMask;
			for(std::size_t index = 0U; index < source.areaPenalties.size(); ++index)
				value.areaPenalties[index] = source.areaPenalties[index];
			return value;
		}

		NativeNavigationRouteRequest RouteRequest(NativeNavigationOwnerHandle owner = Owner()) {
			NativeNavigationRouteRequest request{};
			request.header = Header(sizeof(request));
			request.owner = owner;
			request.profile = Profile();
			request.start = Location(0.0);
			request.destination = Location(10.0);
			request.allowPartial = 1U;
			request.allowEndpointAdjustment = 1U;
			request.maximumExpansions = 128U;
			request.correlationId = 41U;
			return request;
		}

		NativeNavigationSubmitReceipt SubmitReceipt() {
			NativeNavigationSubmitReceipt receipt{};
			receipt.header = Header(sizeof(receipt));
			return receipt;
		}

		void StartNavigation(bool publishGround = false) {
			NavigationSystem::Instance().Shutdown();
			NavigationSystem::Instance().Initialize();
			if(!publishGround) return;
			// ABI route-copy coverage must use the same real immutable geometry and
			// private Recast/Detour path as gameplay. It may not rely on a synthetic
			// endpoint-only route that bypasses coverage publication.
			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{-1.0F, 0.0F, -1.0F}, {33.0F, 0.0F, 33.0F},
				{33.0F, 0.0F, -1.0F}, {-1.0F, 0.0F, 33.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceRevision = 1U;
			descriptor.canonicalPlacementRevision = 1U;
			descriptor.physicalReadyRevision = 1U;
			const auto geometry = NavigationGeometrySnapshot::Create(descriptor, document);
			if(geometry.accepted && geometry.snapshot
				&& NavigationSystem::Instance().QueueInitialGroundBuild(NavigationProfile::Defaults(), {geometry.snapshot}))
				NavigationSystem::Instance().Update(0.0F);
		}
	}

	void RegisterNavigationAbiTests(TestRegistry& registry) {
		registry.Add("Navigation.ManagedAbi.VersionFixedWidthsAndOffsets", [](TestContext& context) {
			context.ExpectEqual(kNativeNavigationAbiVersion, Native_Navigation_GetAbiVersion(),
				"The managed navigation bridge must advertise ABI v1.");
			context.ExpectEqual(static_cast<std::size_t>(8U), sizeof(NativeNavigationAbiHeader),
				"The ABI header must remain two fixed-width words.");
			context.ExpectEqual(static_cast<std::size_t>(528U), sizeof(NativeNavigationRouteRequest),
				"Route requests must retain their fixed-width stride.");
			context.ExpectEqual(static_cast<std::size_t>(264U), sizeof(NativeNavigationSubmitReceipt),
				"Submit receipts must retain their fixed-width stride.");
			context.ExpectEqual(static_cast<std::size_t>(72U), sizeof(NativeNavigationRoutePoint),
				"Route points must retain their fixed-width stride.");
			context.Expect(offsetof(NativeNavigationRouteRequest, header) == 0U
				&& offsetof(NativeNavigationSubmitReceipt, header) == 0U
				&& offsetof(NativeNavigationInvalidationRecord, header) == 0U,
				"Every exported navigation record must begin with its ABI header.");
		});

		registry.Add("Navigation.ManagedAbi.RejectsMalformedVersionSizeCountAndCoordinates", [](TestContext& context) {
			StartNavigation();
			NativeNavigationRouteRequest request = RouteRequest();
			NativeNavigationSubmitReceipt receipt = SubmitReceipt();
			request.header.abiVersion = kNativeNavigationAbiVersion + 1U;
			context.Expect(Native_Navigation_SubmitRoute(&request, &receipt) == NativeNavigationStatus::VersionMismatch,
				"Unknown ABI versions must be rejected before payload inspection.");
			request = RouteRequest();
			request.header.structSize = sizeof(NativeNavigationAbiHeader);
			context.Expect(Native_Navigation_SubmitRoute(&request, &receipt) == NativeNavigationStatus::VersionMismatch,
				"Truncated route records must be rejected before payload inspection.");
			request = RouteRequest();
			request.maximumExpansions = 0U;
			context.Expect(Native_Navigation_SubmitRoute(&request, &receipt) == NativeNavigationStatus::InvalidArgument,
				"Zero search budgets must be rejected as invalid bounded input.");
			request = RouteRequest();
			request.start.offsetX = 40000.0;
			context.Expect(Native_Navigation_SubmitRoute(&request, &receipt) == NativeNavigationStatus::InvalidArgument,
				"Non-canonical world offsets must not be silently normalized.");
			NavigationSystem::Instance().Shutdown();
		});

		registry.Add("Navigation.ManagedAbi.OwnerAndStaleHandlesMapExplicitly", [](TestContext& context) {
			StartNavigation();
			NativeNavigationRouteRequest request = RouteRequest();
			NativeNavigationSubmitReceipt receipt = SubmitReceipt();
			context.Expect(Native_Navigation_SubmitRoute(&request, &receipt) == NativeNavigationStatus::Accepted,
				"A valid copied request should receive an accepted receipt.");
			const NativeNavigationRequestHandle handle = receipt.request;
			NativeNavigationRequestSnapshot snapshot{};
			snapshot.header = Header(sizeof(snapshot));
			NativeNavigationRequestHandle wrongOwner = handle;
			wrongOwner.owner.ownerId = 999U;
			context.Expect(Native_Navigation_ReadRequest(&wrongOwner, &snapshot) == NativeNavigationStatus::StaleTarget,
				"A valid-shaped handle from another owner must be stale, not dereferenced.");
			NativeNavigationRequestHandle stale = handle;
			stale.slotGeneration++;
			context.Expect(Native_Navigation_ReadRequest(&stale, &snapshot) == NativeNavigationStatus::StaleTarget,
				"A superseded slot generation must map to StaleTarget.");
			NavigationSystem::Instance().Shutdown();
		});

		registry.Add("Navigation.ManagedAbi.RouteCopyUsesBoundedTwoCallAtomicWrites", [](TestContext& context) {
			StartNavigation(true);
			NativeNavigationRouteRequest request = RouteRequest();
			NativeNavigationSubmitReceipt receipt = SubmitReceipt();
			context.Expect(Native_Navigation_SubmitRoute(&request, &receipt) == NativeNavigationStatus::Accepted,
				"The route must be admitted before its copied result is inspected.");
			NavigationSystem::Instance().Update(0.0F);
			NativeNavigationRequestSnapshot snapshot{};
			snapshot.header = Header(sizeof(snapshot));
			Native_Navigation_ReadRequest(&receipt.request, &snapshot);
			NativeNavigationBufferInfo info{};
			info.header = Header(sizeof(info));
			context.Expect(Native_Navigation_CopyRoute(&snapshot.route, nullptr, 0U, &info) == NativeNavigationStatus::Success
				&& info.requiredCount > 0U && info.copiedCount == 0U,
				"The first route-copy call must report the exact required count without writing points.");
			const std::uint32_t requiredCount = info.requiredCount;
			NativeNavigationRoutePoint sentinel{};
			sentinel.header = Header(sizeof(sentinel));
			const NativeNavigationRoutePoint before = sentinel;
			info = {};
			info.header = Header(sizeof(info));
			context.Expect(Native_Navigation_CopyRoute(&snapshot.route, &sentinel, requiredCount - 1U, &info)
				== NativeNavigationStatus::LimitExceeded && std::memcmp(&sentinel, &before, sizeof(sentinel)) == 0,
				"An undersized destination must fail without a partial route write.");
			std::vector<NativeNavigationRoutePoint> points(requiredCount);
			info = {};
			info.header = Header(sizeof(info));
			context.Expect(Native_Navigation_CopyRoute(&snapshot.route, points.data(), static_cast<std::uint32_t>(points.size()), &info)
				== NativeNavigationStatus::Success && info.copiedCount == info.requiredCount,
				"The second route-copy call must copy the complete retained route.");
			NativeNavigationCancellationRequest release{};
			release.header = Header(sizeof(release));
			release.request = receipt.request;
			NativeNavigationSubmitReceipt releaseReceipt = SubmitReceipt();
			context.Expect(Native_Navigation_ReleaseResult(&release, &releaseReceipt) == NativeNavigationStatus::Success,
				"Explicit result release must reclaim the request and copied route together.");
			snapshot = {};
			snapshot.header = Header(sizeof(snapshot));
			context.Expect(Native_Navigation_ReadRequest(&receipt.request, &snapshot) == NativeNavigationStatus::StaleTarget,
				"A released generation-qualified request must never alias recycled storage.");
			NavigationSystem::Instance().Shutdown();
		});

		registry.Add("Navigation.ManagedAbi.InvalidationCopyAndOutcomeMapping", [](TestContext& context) {
			StartNavigation();
			NativeNavigationRouteRequest request = RouteRequest();
			NativeNavigationSubmitReceipt receipt = SubmitReceipt();
			context.Expect(Native_Navigation_SubmitRoute(&request, &receipt) == NativeNavigationStatus::Accepted,
				"A valid route submission must map to Accepted.");
			NativeNavigationRequestSnapshot snapshot{};
			snapshot.header = Header(sizeof(snapshot));
			context.Expect(Native_Navigation_ReadRequest(&receipt.request, &snapshot) == NativeNavigationStatus::Accepted,
				"A queued request must map to the explicit Accepted status.");
			NativeNavigationCancellationRequest cancellation{};
			cancellation.header = Header(sizeof(cancellation));
			cancellation.request = receipt.request;
			NativeNavigationSubmitReceipt cancellationReceipt = SubmitReceipt();
			context.Expect(Native_Navigation_CancelRequest(&cancellation, &cancellationReceipt) == NativeNavigationStatus::Success,
				"Cancellation must map to Success and publish copied invalidation state.");
			NativeNavigationInvalidationCursor cursor{};
			cursor.header = Header(sizeof(cursor));
			cursor.owner = request.owner;
			NativeNavigationBufferInfo info{};
			info.header = Header(sizeof(info));
			context.Expect(Native_Navigation_CopyInvalidations(&cursor, nullptr, 0U, &info) == NativeNavigationStatus::Success
				&& info.requiredCount == 0U && info.copiedCount == 0U,
				"The invalidation count query must be bounded and side-effect free.");
			NativeNavigationInvalidationRecord sentinel{};
			sentinel.header = Header(sizeof(sentinel));
			const NativeNavigationInvalidationRecord before = sentinel;
			info = {};
			info.header = Header(sizeof(info));
			context.Expect(Native_Navigation_CopyInvalidations(&cursor, &sentinel, 0U, &info)
				== NativeNavigationStatus::Success && std::memcmp(&sentinel, &before, sizeof(sentinel)) == 0,
				"An empty invalidation destination must remain unchanged on the second bounded call.");
			NavigationSystem::Instance().Shutdown();
		});
	}
}
