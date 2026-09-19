// NavigationOutcomeParityTests.cpp - public navigation outcome ABI parity checks.
//
// The managed bridge receives these numeric values through copied ABI packets.
// These tests therefore exercise the real ABI read/cancel functions instead of
// adding a test-only setter to ScriptGlue or NavigationSystem.

#include "../TestHarness.h"

#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Navigation/Ground/GroundRouteQuery.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationSystem.h"
#include "Navigation/NavigationTypes.h"
#include "Scripting/ScriptGlue.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

namespace TerraEngine::Tests {
	namespace {
		struct OutcomeCase {
			Navigation::NavigationOutcome outcome;
			Navigation::NavigationReason reason;
			std::string_view outcomeName;
			std::string_view reasonName;
			std::uint32_t outcomeCode;
			std::uint32_t reasonCode;
		};

		// These are the public semantic values shared by native packets, managed
		// casts, and editor strings. Keep one row per externally observable class
		// so a new outcome cannot silently acquire an unspecified bridge meaning.
		constexpr std::array<OutcomeCase, 11U> kOutcomeCases{{
			{Navigation::NavigationOutcome::Complete, Navigation::NavigationReason::None, "Complete", "None", 3U, 0U},
			{Navigation::NavigationOutcome::PendingPreparation, Navigation::NavigationReason::DirtyCoverage, "PendingPreparation", "DirtyCoverage", 1U, 3U},
			{Navigation::NavigationOutcome::UnavailableCoverage, Navigation::NavigationReason::MissingTerrain, "UnavailableCoverage", "MissingTerrain", 8U, 1U},
			{Navigation::NavigationOutcome::InvalidEndpoint, Navigation::NavigationReason::InvalidDestination, "InvalidEndpoint", "InvalidDestination", 7U, 10U},
			{Navigation::NavigationOutcome::Unreachable, Navigation::NavigationReason::MissingSupport, "Unreachable", "MissingSupport", 5U, 12U},
			{Navigation::NavigationOutcome::BudgetExhausted, Navigation::NavigationReason::NodeLimit, "BudgetExhausted", "NodeLimit", 9U, 7U},
			{Navigation::NavigationOutcome::Cancelled, Navigation::NavigationReason::Cancelled, "Cancelled", "Cancelled", 11U, 16U},
			{Navigation::NavigationOutcome::Stale, Navigation::NavigationReason::SceneReplaced, "Stale", "SceneReplaced", 13U, 14U},
			{Navigation::NavigationOutcome::RejectedCapacity, Navigation::NavigationReason::OutputLimit, "RejectedCapacity", "OutputLimit", 10U, 8U},
			{Navigation::NavigationOutcome::Failed, Navigation::NavigationReason::ProviderFailure, "Failed", "ProviderFailure", 14U, 15U},
			{Navigation::NavigationOutcome::Partial, Navigation::NavigationReason::OutputLimit, "Partial", "OutputLimit", 4U, 8U}
		}};

		NativeNavigationRequestHandle ToNative(Navigation::NavigationRequestHandle handle) {
			NativeNavigationRequestHandle native{};
			native.owner.world = {handle.owner.world.worldId.value, handle.owner.world.worldGeneration};
			native.owner.ownerId = handle.owner.ownerId.value;
			native.owner.ownerGeneration = handle.owner.ownerGeneration;
			native.slotIndex = handle.slotIndex;
			native.slotGeneration = handle.slotGeneration;
			return native;
		}

		Navigation::Ground::GroundRouteQuery::Request MakeRequest(Navigation::NavigationWorldHandle world,
			std::uint64_t ownerId) {
			Navigation::Ground::GroundRouteQuery::Request request{};
			request.owner = {world, {ownerId}, 1U};
			request.start = {world, {}};
			request.destination = {world, {}};
			request.destination.position.offsetMetres.x = 4.0;
			request.profile = Navigation::NavigationProfile::Defaults();
			request.maxExpansions = 64U;
			return request;
		}

		std::shared_ptr<const Navigation::NavigationGeometrySnapshot> MakeTerrainPatch(
			float minimumX, float maximumX, std::string_view sourceId) {
			Navigation::NavigationGeometryDocument document{};
			document.kind = Navigation::NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{minimumX, 0.0F, 1.0F}, {maximumX, 0.0F, 8.0F},
				{maximumX, 0.0F, 1.0F}, {minimumX, 0.0F, 8.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			Navigation::NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceUuid = Navigation::NavigationUuid::FromString(sourceId);
			descriptor.sourceRevision = 1U;
			descriptor.canonicalPlacementRevision = 1U;
			descriptor.physicalReadyRevision = 1U;
			descriptor.sourceKind = Navigation::NavigationSourceKind::Terrain;
			return Navigation::NavigationGeometrySnapshot::Create(descriptor, std::move(document)).snapshot;
		}

		// Build the same immutable source-evidence packet that the protected scene
		// capture supplies.  Keeping the source index attached is important here:
		// a plain QueueInitialGroundBuild call intentionally cannot exercise the
		// deferred on-demand profile path.
		Navigation::NavigationSystem::InitialGroundBuildInput MakeAutomaticTerrainBuild(
			const Navigation::NavigationProfile& profile) {
			using namespace Navigation;
			NavigationEligibleSourceCandidate candidate{};
			candidate.identity.sourceId = GenerateUUID();
			candidate.identity.objectId = GenerateUUID();
			candidate.identity.objectLifetimeGeneration = 1U;
			candidate.identity.assetId = GenerateUUID();
			candidate.identity.kind = NavigationSourceKind::Terrain;
			candidate.revisions = {1U, 1U, 1U, 1U, 1U};
			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{-32.0F, 0.0F, -32.0F}, {32.0F, 0.0F, 32.0F},
				{32.0F, 0.0F, -32.0F}, {-32.0F, 0.0F, 32.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceUuid.value = candidate.identity.sourceId;
			descriptor.sourceRevision = ComputeNavigationEligibleSourceRevision(
				candidate.identity, candidate.revisions);
			descriptor.canonicalPlacementRevision = 1U;
			descriptor.physicalReadyRevision = 1U;
			descriptor.sourceKind = NavigationSourceKind::Terrain;
			candidate.geometry = NavigationGeometrySnapshot::Create(descriptor, std::move(document)).snapshot;
			candidate.activeInHierarchy = true;
			candidate.physicalBackingCurrent = true;
			const std::array<NavigationEligibleSourceCandidate, 1U> candidates{{candidate}};
			const NavigationEligibleSourceIndexBuildResult indexed = BuildNavigationEligibleSourceIndex(candidates);
			Navigation::NavigationSystem::InitialGroundBuildInput build{};
			build.profile = profile;
			build.sources = {candidate.geometry};
			build.sourceIndex = {{candidate.identity.sourceId, candidate.geometry->Descriptor().sourceRevision,
				candidate.revisions.geometryRevision, true, candidate.identity, candidate.revisions}};
			build.eligibleSourceIndex = indexed.accepted
				? std::make_shared<const NavigationEligibleSourceIndex>(indexed.index) : nullptr;
			build.terrainPublicationGeneration = 1U;
			return build;
		}

		Navigation::Ground::GroundRouteQuery::Request MakeProviderRequest(
			Navigation::NavigationWorldHandle world, std::uint64_t ownerId,
			Navigation::NavigationGeometryVector3 start, Navigation::NavigationGeometryVector3 destination,
			std::uint32_t maxExpansions) {
			Navigation::Ground::GroundRouteQuery::Request request = MakeRequest(world, ownerId);
			// Geometry packets use a plain float record; canonical positions retain
			// double offsets, so copy each coordinate explicitly at this test boundary.
			request.start.position.offsetMetres = {start.x, start.y, start.z};
			request.destination.position.offsetMetres = {destination.x, destination.y, destination.z};
			request.maxExpansions = maxExpansions;
			return request;
		}

		NativeNavigationProfile MakeNativeProfile(const Navigation::NavigationProfile& profile) {
			NativeNavigationProfile native{};
			native.radiusMetres = profile.radiusMetres;
			native.heightMetres = profile.heightMetres;
			native.stepHeightMetres = profile.stepHeightMetres;
			native.maxSlopeDegrees = profile.maxSlopeDegrees;
			native.clearanceMarginMetres = profile.clearanceMarginMetres;
			native.horizontalResolutionMetres = profile.horizontalResolutionMetres;
			native.verticalResolutionMetres = profile.verticalResolutionMetres;
			native.excludedAreasMask = profile.excludedAreasMask;
			for(std::size_t index = 0U; index < profile.areaPenalties.size(); ++index)
				native.areaPenalties[index] = profile.areaPenalties[index];
			return native;
		}

		NativeNavigationRequestSnapshot Read(Navigation::NavigationRequestHandle handle) {
			NativeNavigationRequestSnapshot snapshot{};
			snapshot.header = {kNativeNavigationAbiVersion, sizeof(snapshot)};
			const NativeNavigationRequestHandle nativeHandle = ToNative(handle);
			(void)Native_Navigation_ReadRequest(&nativeHandle, &snapshot);
			return snapshot;
		}
	}

	void RegisterNavigationOutcomeParityTests(TestRegistry& registry) {
		registry.Add("Navigation.OutcomeParity.CanonicalNativeOutcomeNamesAndCodesRemainStable", [](TestContext& context) {
			for(const OutcomeCase& testCase : kOutcomeCases) {
				// Raw codes cross the ABI in uint32 fields and ToString is the exact
				// text emitted by editor automation. Pin both native representations
				// without relying on a source-text mirror of managed declarations.
				context.Expect(Navigation::ToString(testCase.outcome) == testCase.outcomeName
					&& Navigation::ToString(testCase.reason) == testCase.reasonName
					&& static_cast<std::uint32_t>(testCase.outcome) == testCase.outcomeCode
					&& static_cast<std::uint32_t>(testCase.reason) == testCase.reasonCode,
					"Every navigation outcome must retain its copied ABI code and canonical editor string.");
			}
		});

		registry.Add("Navigation.OutcomeParity.PublicDirectAbiPreservesPendingUnavailableCancelledAndStale", [](TestContext& context) {
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			navigation.Shutdown();
			navigation.Initialize();
			const auto world = navigation.PublishedWorld();
			context.Expect(world != nullptr, "The public outcome ABI fixture requires an initialized navigation world.");
			if(!world) { navigation.Shutdown(); return; }

			const Navigation::NavigationRequestHandle pending = navigation.SubmitOneShotRoute(MakeRequest(world->world, 7001U));
			const NativeNavigationRequestSnapshot pendingRead = pending.IsValid() ? Read(pending) : NativeNavigationRequestSnapshot{};
			context.Expect(pending.IsValid() && pendingRead.status == NativeNavigationStatus::Accepted
				&& pendingRead.outcome == static_cast<std::uint32_t>(Navigation::NavigationOutcome::Queued)
				&& pendingRead.reason == static_cast<std::uint32_t>(Navigation::NavigationReason::None),
				"A copied request must expose Queued/None and Accepted before a protected navigation update.");

			navigation.Update(0.0F);
			const NativeNavigationRequestSnapshot unavailableRead = pending.IsValid() ? Read(pending) : NativeNavigationRequestSnapshot{};
			context.Expect(unavailableRead.status == NativeNavigationStatus::Unavailable
				&& unavailableRead.outcome == static_cast<std::uint32_t>(Navigation::NavigationOutcome::UnavailableCoverage)
				&& unavailableRead.reason == static_cast<std::uint32_t>(Navigation::NavigationReason::MissingTerrain),
				"The ABI must preserve unavailable coverage separately from invalid and unreachable route outcomes.");

			const Navigation::NavigationRequestHandle cancelled = navigation.SubmitOneShotRoute(MakeRequest(world->world, 7002U));
			NativeNavigationCancellationRequest cancel{};
			cancel.header = {kNativeNavigationAbiVersion, sizeof(cancel)};
			cancel.request = ToNative(cancelled);
			NativeNavigationSubmitReceipt cancelReceipt{};
			cancelReceipt.header = {kNativeNavigationAbiVersion, sizeof(cancelReceipt)};
			const NativeNavigationStatus cancelledStatus = Native_Navigation_CancelRequest(&cancel, &cancelReceipt);
			const NativeNavigationRequestSnapshot cancelledRead = cancelled.IsValid() ? Read(cancelled) : NativeNavigationRequestSnapshot{};
			context.Expect(cancelled.IsValid() && cancelledStatus == NativeNavigationStatus::Success
				&& cancelledRead.status == NativeNavigationStatus::Success
				&& cancelledRead.outcome == static_cast<std::uint32_t>(Navigation::NavigationOutcome::Cancelled)
				&& cancelledRead.reason == static_cast<std::uint32_t>(Navigation::NavigationReason::Cancelled),
				"Cancellation must remain a successful API operation with an explicit Cancelled/Cancelled terminal result.");

			navigation.RevokeAllOwners(Navigation::NavigationReason::SceneReplaced);
			const NativeNavigationRequestSnapshot staleRead = pending.IsValid() ? Read(pending) : NativeNavigationRequestSnapshot{};
			context.Expect(staleRead.status == NativeNavigationStatus::StaleTarget,
				"Revoking a world must make a historic copied request stale instead of reusing its generation-qualified handle.");
			navigation.Shutdown();
		});

		registry.Add("Navigation.OutcomeParity.PublicDirectAbiPreservesProviderUnreachable", [](TestContext& context) {
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			navigation.Shutdown();
			navigation.Initialize();
			const auto world = navigation.PublishedWorld();
			const auto left = MakeTerrainPatch(1.0F, 8.0F, "10000000000000000000000000000001");
			const auto right = MakeTerrainPatch(20.0F, 27.0F, "10000000000000000000000000000002");
			const bool prepared = world != nullptr && left != nullptr && right != nullptr
				&& navigation.QueueInitialGroundBuild(Navigation::NavigationProfile::Defaults(), {left, right});
			if(prepared) navigation.Update(0.0F);
			const auto published = navigation.PublishedWorld();
			Navigation::NavigationRequestHandle handle{};
			if(prepared && published) {
				handle = navigation.SubmitOneShotRoute(MakeProviderRequest(published->world, 8101U,
					{2.0, 0.0, 2.0}, {24.0, 0.0, 2.0}, 4096U));
				for(std::uint32_t frame = 0U; frame < 6U; ++frame) navigation.Update(0.0F);
			}
			const NativeNavigationRequestSnapshot snapshot = handle.IsValid() ? Read(handle) : NativeNavigationRequestSnapshot{};
			context.Expect(prepared && handle.IsValid()
				&& snapshot.status == NativeNavigationStatus::InvalidArgument
				&& snapshot.outcome == static_cast<std::uint32_t>(Navigation::NavigationOutcome::Unreachable)
				&& snapshot.reason == static_cast<std::uint32_t>(Navigation::NavigationReason::MissingSupport),
				"A real provider route between disconnected terrain islands must preserve Unreachable/MissingSupport through ReadRequest.");
			navigation.Shutdown();
		});

		registry.Add("Navigation.OutcomeParity.PublicDirectAbiPreservesProviderBudgetExhaustion", [](TestContext& context) {
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			navigation.Shutdown();
			navigation.Initialize();
			const auto world = navigation.PublishedWorld();
			const auto terrain = MakeTerrainPatch(1.0F, 31.0F, "20000000000000000000000000000001");
			const bool prepared = world != nullptr && terrain != nullptr
				&& navigation.QueueInitialGroundBuild(Navigation::NavigationProfile::Defaults(), {terrain});
			if(prepared) navigation.Update(0.0F);
			const auto published = navigation.PublishedWorld();
			Navigation::NavigationRequestHandle handle{};
			if(prepared && published) {
				handle = navigation.SubmitOneShotRoute(MakeProviderRequest(published->world, 8102U,
					{2.0, 0.0, 2.0}, {29.0, 0.0, 7.0}, 1U));
				for(std::uint32_t frame = 0U; frame < 6U; ++frame) navigation.Update(0.0F);
			}
			const NativeNavigationRequestSnapshot snapshot = handle.IsValid() ? Read(handle) : NativeNavigationRequestSnapshot{};
			context.Expect(prepared && handle.IsValid()
				&& snapshot.status == NativeNavigationStatus::LimitExceeded
				&& snapshot.outcome == static_cast<std::uint32_t>(Navigation::NavigationOutcome::BudgetExhausted)
				&& snapshot.reason == static_cast<std::uint32_t>(Navigation::NavigationReason::NodeLimit),
				"A real provider search capped at one expansion must preserve BudgetExhausted/NodeLimit through ReadRequest.");
			navigation.Shutdown();
		});

		registry.Add("Navigation.OutcomeParity.PublicDirectAbiRejectsInvalidProfileAndEndpoint", [](TestContext& context) {
			// These requests stop at the public ABI conversion boundary, before object
			// lookup or admission, so the caller receives a meaningful status and
			// diagnostic even when no scene object has been registered.
			NativeNavigationObjectToLocationRequest invalidProfile{};
			invalidProfile.header = {kNativeNavigationAbiVersion, sizeof(invalidProfile)};
			invalidProfile.sourceObjectEntity = 1U;
			invalidProfile.routePolicy = 0U;
			invalidProfile.profile = MakeNativeProfile(Navigation::NavigationProfile::Defaults());
			invalidProfile.profile.radiusMetres = 0.0;
			invalidProfile.destination.world = {1U, 1U};
			invalidProfile.correlationId = 8201U;
			NativeNavigationSubmitReceipt invalidProfileReceipt{};
			invalidProfileReceipt.header = {kNativeNavigationAbiVersion, sizeof(invalidProfileReceipt)};
			const NativeNavigationStatus invalidProfileStatus = Native_Navigation_RequestObjectToLocation(
				&invalidProfile, &invalidProfileReceipt);

			NativeNavigationObjectToLocationRequest invalidEndpoint{};
			invalidEndpoint.header = {kNativeNavigationAbiVersion, sizeof(invalidEndpoint)};
			invalidEndpoint.sourceObjectEntity = 1U;
			invalidEndpoint.routePolicy = 0U;
			invalidEndpoint.profile = MakeNativeProfile(Navigation::NavigationProfile::Defaults());
			invalidEndpoint.destination.world = {1U, 1U};
			invalidEndpoint.destination.offsetX = 32768.0;
			invalidEndpoint.correlationId = 8202U;
			NativeNavigationSubmitReceipt invalidEndpointReceipt{};
			invalidEndpointReceipt.header = {kNativeNavigationAbiVersion, sizeof(invalidEndpointReceipt)};
			const NativeNavigationStatus invalidEndpointStatus = Native_Navigation_RequestObjectToLocation(
				&invalidEndpoint, &invalidEndpointReceipt);

			context.Expect(invalidProfileStatus == NativeNavigationStatus::InvalidArgument
				&& invalidProfileReceipt.status == NativeNavigationStatus::InvalidArgument
				&& invalidProfileReceipt.correlationId == 8201U
				&& std::string_view(invalidProfileReceipt.diagnostic).find("InvalidProfile") != std::string_view::npos
				&& invalidEndpointStatus == NativeNavigationStatus::InvalidArgument
				&& invalidEndpointReceipt.status == NativeNavigationStatus::InvalidArgument
				&& invalidEndpointReceipt.correlationId == 8202U
				&& std::string_view(invalidEndpointReceipt.diagnostic).find("InvalidDestination") != std::string_view::npos,
				"The native direct facade must reject invalid profiles and canonical endpoints with caller-visible diagnostics.");
		});

		registry.Add("Navigation.OutcomeParity.PublicDirectAbiNearestReachablePreservesPartialDomainLimit", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			navigation.Shutdown();
			navigation.Initialize();
			const auto source = objects.createObject("Native Partial Route Source");
			Transform* transform = objects.AddComponent<Transform>(source);
			if(transform != nullptr)
				transform->worldPose.position.offsetMetres = {4.0, 0.0, 4.0};
			const auto left = MakeTerrainPatch(1.0F, 8.0F, "30000000000000000000000000000001");
			const auto right = MakeTerrainPatch(20.0F, 27.0F, "30000000000000000000000000000002");
			const bool prepared = transform != nullptr && left != nullptr && right != nullptr
				&& navigation.QueueInitialGroundBuild(Navigation::NavigationProfile::Defaults(), {left, right});
			if(prepared) {
				for(std::uint32_t frame = 0U; frame < 8U; ++frame)
					navigation.Update(0.0F);
			}
			const auto world = navigation.PublishedWorld();
			NativeNavigationObjectToLocationRequest request{};
			request.header = {kNativeNavigationAbiVersion, sizeof(request)};
			request.sourceObjectEntity = static_cast<std::uint32_t>(source.EntityId());
			request.routePolicy = 1U; // NavigationRoutePolicy::NearestReachable.
			request.profile = MakeNativeProfile(Navigation::NavigationProfile::Defaults());
			if(world != nullptr) {
				request.destination.world.worldId = world->world.worldId.value;
				request.destination.world.worldGeneration = world->world.worldGeneration;
			}
			request.destination.offsetX = 24.0;
			request.destination.offsetZ = 4.0;
			request.correlationId = 8203U;
			NativeNavigationSubmitReceipt receipt{};
			receipt.header = {kNativeNavigationAbiVersion, sizeof(receipt)};
			const NativeNavigationStatus submitStatus = Native_Navigation_RequestObjectToLocation(&request, &receipt);
			NativeNavigationRequestSnapshot snapshot{};
			snapshot.header = {kNativeNavigationAbiVersion, sizeof(snapshot)};
			if(receipt.request.slotGeneration != 0U) {
				const NativeNavigationRequestHandle handle = receipt.request;
				for(std::uint32_t frame = 0U; frame < 8U; ++frame)
					navigation.Update(0.0F);
				(void)Native_Navigation_ReadRequest(&handle, &snapshot);
			}
			std::array<NativeNavigationRoutePoint, 64U> points{};
			NativeNavigationBufferInfo copyInfo{};
			copyInfo.header = {kNativeNavigationAbiVersion, sizeof(copyInfo)};
			const NativeNavigationStatus copyStatus = Native_Navigation_CopyRoute(
				&snapshot.route, points.data(), static_cast<std::uint32_t>(points.size()), &copyInfo);
			NativeNavigationRouteValidationRequest validationRequest{};
			validationRequest.header = {kNativeNavigationAbiVersion, sizeof(validationRequest)};
			validationRequest.route = snapshot.route;
			NativeNavigationRouteValidationResult validation{};
			validation.header = {kNativeNavigationAbiVersion, sizeof(validation)};
			const NativeNavigationStatus validationStatus = Native_Navigation_ValidateRoute(&validationRequest, &validation);
			const bool retainedPartial = copyStatus == NativeNavigationStatus::Success
				&& copyInfo.requiredCount > 0U && copyInfo.copiedCount == copyInfo.requiredCount
				&& (points[copyInfo.requiredCount - 1U].location.offsetX != request.destination.offsetX
					|| points[copyInfo.requiredCount - 1U].location.offsetZ != request.destination.offsetZ)
				&& validationStatus == NativeNavigationStatus::Success && validation.usable != 0U
				&& validation.outcome == static_cast<std::uint32_t>(Navigation::NavigationOutcome::Partial)
				&& validation.reason == static_cast<std::uint32_t>(Navigation::NavigationReason::DomainLimit);
			context.Expect(prepared && world != nullptr && submitStatus == NativeNavigationStatus::Accepted
				&& snapshot.outcome == static_cast<std::uint32_t>(Navigation::NavigationOutcome::Partial)
				&& snapshot.reason == static_cast<std::uint32_t>(Navigation::NavigationReason::DomainLimit)
				&& retainedPartial,
				"NearestReachable must preserve Partial/DomainLimit and expose a usable retained corridor ending before the disconnected destination.");
			objects.clear();
			navigation.Shutdown();
		});

		registry.Add("Navigation.OutcomeParity.PublicDirectAbiReportsDeferredPreparationAndProviderFailure", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			navigation.Shutdown();
			navigation.Initialize();
			const auto source = objects.createObject("Deferred Preparation Source");
			Transform* transform = objects.AddComponent<Transform>(source);
			if(transform != nullptr)
				transform->worldPose.position.offsetMetres = {4.0, 0.0, 4.0};

			const Navigation::NavigationProfile defaultProfile = Navigation::NavigationProfile::Defaults();
			Navigation::NavigationSystem::InitialGroundBuildInput bootstrap =
				MakeAutomaticTerrainBuild(defaultProfile);
			const bool queuedBootstrap = transform != nullptr
				&& navigation.QueueInitialGroundBuilds({std::move(bootstrap)});
			for(std::uint32_t frame = 0U; queuedBootstrap && frame < 8U; ++frame)
				navigation.Update(0.0F);
			const auto world = navigation.PublishedWorld();

			// This remains valid at the public profile boundary, but Recast rejects
			// its resulting grid dimensions. It is a genuine provider failure rather
			// than a test-only failure injection or corrupted geometry packet.
			Navigation::NavigationProfile failingProfile = defaultProfile;
			failingProfile.horizontalResolutionMetres = 0.001;
			NativeNavigationObjectToLocationRequest request{};
			request.header = {kNativeNavigationAbiVersion, sizeof(request)};
			request.sourceObjectEntity = static_cast<std::uint32_t>(source.EntityId());
			request.routePolicy = 0U;
			request.profile = MakeNativeProfile(failingProfile);
			if(world != nullptr) {
				request.destination.world.worldId = world->world.worldId.value;
				request.destination.world.worldGeneration = world->world.worldGeneration;
			}
			request.destination.offsetX = 10.0;
			request.destination.offsetZ = 10.0;
			request.correlationId = 8210U;
			NativeNavigationSubmitReceipt receipt{};
			receipt.header = {kNativeNavigationAbiVersion, sizeof(receipt)};
			const NativeNavigationStatus submitStatus = Native_Navigation_RequestObjectToLocation(&request, &receipt);
			NativeNavigationRequestSnapshot pending{};
			pending.header = {kNativeNavigationAbiVersion, sizeof(pending)};
			if(receipt.request.slotGeneration != 0U)
				(void)Native_Navigation_ReadRequest(&receipt.request, &pending);
			const bool pendingPreparation = pending.outcome == static_cast<std::uint32_t>(Navigation::NavigationOutcome::PendingPreparation)
				&& pending.reason == static_cast<std::uint32_t>(Navigation::NavigationReason::DirtyCoverage);

			NativeNavigationRequestSnapshot failed{};
			failed.header = {kNativeNavigationAbiVersion, sizeof(failed)};
			for(std::uint32_t frame = 0U; receipt.request.slotGeneration != 0U && frame < 16U; ++frame) {
				navigation.Update(0.0F);
				(void)Native_Navigation_ReadRequest(&receipt.request, &failed);
				if(failed.status == NativeNavigationStatus::Failed)
					break;
			}
			const Navigation::NavigationSystem::StoreUsageSnapshot usageBeforeRepeatedFailures =
				navigation.PublishedStoreUsage();
			bool repeatedFailuresReleasedStages = usageBeforeRepeatedFailures.dynamicSourceStages == 1U;
			// Each request uses a distinct valid profile key that deterministically
			// reaches Recast's bounded grid rejection. A terminal failure must release
			// its on-demand dynamic stage rather than retaining one source-vector copy
			// per rejected profile for the rest of the scene lifetime.
			for(std::uint32_t attempt = 1U; attempt <= 4U; ++attempt) {
				Navigation::NavigationProfile repeatedProfile = failingProfile;
				repeatedProfile.radiusMetres += 0.01 * static_cast<double>(attempt);
				NativeNavigationObjectToLocationRequest repeated = request;
				repeated.profile = MakeNativeProfile(repeatedProfile);
				repeated.correlationId += attempt;
				NativeNavigationSubmitReceipt repeatedReceipt{};
				repeatedReceipt.header = {kNativeNavigationAbiVersion, sizeof(repeatedReceipt)};
				const NativeNavigationStatus repeatedSubmit = Native_Navigation_RequestObjectToLocation(
					&repeated, &repeatedReceipt);
				NativeNavigationRequestSnapshot repeatedFailed{};
				repeatedFailed.header = {kNativeNavigationAbiVersion, sizeof(repeatedFailed)};
				for(std::uint32_t frame = 0U; repeatedReceipt.request.slotGeneration != 0U && frame < 16U; ++frame) {
					navigation.Update(0.0F);
					(void)Native_Navigation_ReadRequest(&repeatedReceipt.request, &repeatedFailed);
					if(repeatedFailed.status == NativeNavigationStatus::Failed)
						break;
				}
				const Navigation::NavigationSystem::StoreUsageSnapshot usageAfterFailure =
					navigation.PublishedStoreUsage();
				repeatedFailuresReleasedStages = repeatedFailuresReleasedStages
					&& repeatedSubmit == NativeNavigationStatus::Accepted
					&& repeatedFailed.status == NativeNavigationStatus::Failed
					&& usageAfterFailure.dynamicSourceStages == usageBeforeRepeatedFailures.dynamicSourceStages;
			}
			context.Expect(queuedBootstrap && world != nullptr
				&& submitStatus == NativeNavigationStatus::Accepted
				&& pendingPreparation
				&& failed.status == NativeNavigationStatus::Failed
				&& failed.outcome == static_cast<std::uint32_t>(Navigation::NavigationOutcome::Failed)
				&& failed.reason == static_cast<std::uint32_t>(Navigation::NavigationReason::ProviderFailure)
				&& repeatedFailuresReleasedStages,
				"A valid new profile must be visible as PendingPreparation/DirtyCoverage, retain the provider's failure, and release terminal failed profile stages.");
			objects.clear();
			navigation.Shutdown();
		});
	}
}
