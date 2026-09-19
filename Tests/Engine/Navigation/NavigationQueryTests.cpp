// Test-first contracts for the provider-neutral ground route query.
//
// These tests intentionally include the future query boundary directly. They
// must fail until the query implementation provides bounded private scratch, deterministic search,
// explicit incomplete outcomes, and immutable retained route data.
#include "../TestHarness.h"

#include "Navigation/Ground/GroundRouteQuery.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationSettings.h"
#include "Navigation/NavigationRouteValidity.h"
#include "Navigation/NavigationSystem.h"
#include "Navigation/NavigationTypes.h"
#include "Core/Scheduling/JobGraph.h"
#include "Core/Scheduling/SystemScheduler.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

namespace TerraEngine::Tests
{
	using namespace TerraEngine::Navigation;
	using namespace TerraEngine::Navigation::Ground;

	namespace
	{
		NavigationOwnerHandle TestOwner(std::uint64_t value = 41U)
		{
			return {{NavigationWorldId{7U}, 3U}, NavigationOwnerId{value}, 11U};
		}

		NavigationLocation Location(std::int64_t x, double offsetX)
		{
			NavigationLocation location{};
			location.world = {NavigationWorldId{7U}, 3U};
			location.position.cell = {x, 0, 0};
			location.position.offsetMetres = {offsetX, 0.0, 0.0};
			return location;
		}

		GroundRouteQuery::Request Request(bool allowPartial = false)
		{
			GroundRouteQuery::Request request{};
			request.owner = TestOwner();
			request.start = Location(0, 0.0);
			request.destination = Location(0, 4.0);
			request.profile = NavigationProfile::Defaults();
			request.allowPartial = allowPartial;
			request.maxExpansions = 128U;
			return request;
		}

		// The provider-facing budget tests need actual Recast/Detour coverage rather
		// than a synthetic GroundRouteQuery snapshot. This small floor keeps setup
		// deterministic while exercising NavigationSystem's configured limits.
		std::shared_ptr<const NavigationGeometrySnapshot> FlatTerrain()
		{
			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{1.0F, 0.0F, 1.0F}, {8.0F, 0.0F, 8.0F},
				{8.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 8.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceKind = NavigationSourceKind::Terrain;
			descriptor.sourceRevision = 1U;
			descriptor.canonicalPlacementRevision = 1U;
			descriptor.physicalReadyRevision = 1U;
			return NavigationGeometrySnapshot::Create(descriptor, std::move(document)).snapshot;
		}

		NavigationRequestHandle SubmitPreparedRoute(NavigationSystem& system)
		{
			const std::shared_ptr<const NavigationWorldRoot> world = system.PublishedWorld();
			if(!world) return {};
			GroundRouteQuery::Request request = Request();
			request.owner.world = world->world;
			request.start.world = world->world;
			request.destination.world = world->world;
			return system.SubmitOneShotRoute(std::move(request));
		}

		struct ProviderBudgetCaseResult
		{
			NavigationOutcome outcome = NavigationOutcome::Failed;
			NavigationReason reason = NavigationReason::ProviderFailure;
		};

		ProviderBudgetCaseResult RunConfiguredProviderBudgetCase(NavigationSettings settings)
		{
			NavigationSystem& system = NavigationSystem::Instance();
			const NavigationSettings original = system.SettingsSnapshot();
			system.Shutdown();
			// Always restore the singleton's pre-test configuration even when a later
			// assertion fails. Other navigation tests use independent capacities.
			struct SettingsRestore final {
				NavigationSystem& system;
				NavigationSettings original;
				~SettingsRestore()
				{
					system.Shutdown();
					(void)system.ConfigureSettings(original);
				}
			} restore{system, original};
			if(!system.ConfigureSettings(std::move(settings))) {
				return {NavigationOutcome::InvalidRequest, NavigationReason::InvalidDestination};
			}
			system.Initialize();
			const std::shared_ptr<const NavigationGeometrySnapshot> terrain = FlatTerrain();
			if(!terrain || !system.QueueInitialGroundBuild(NavigationProfile::Defaults(), {terrain})) {
				return {NavigationOutcome::UnavailableCoverage, NavigationReason::MissingTerrain};
			}
			system.Update(0.0F);
			const NavigationRequestHandle handle = SubmitPreparedRoute(system);
			if(!handle.IsValid()) return {NavigationOutcome::RejectedCapacity, NavigationReason::OutputLimit};
			for(std::uint32_t frame = 0U; frame < 4U; ++frame) system.Update(0.0F);
			const NavigationStoreReadResult<NavigationRequestRecord> read = system.ReadRequest(handle);
			return read.record
				? ProviderBudgetCaseResult{read.record->outcome, read.record->reason}
				: ProviderBudgetCaseResult{};
		}
	}

	void RegisterNavigationQueryTests(TestRegistry& registry)
	{
		registry.Add("Navigation.Query.ProducesExplicitCompletePartialAndUnavailableOutcomes", [](TestContext& context) {
			GroundRouteQuery::Snapshot snapshot = GroundRouteQuery::Snapshot::StraightGround(Location(0, 0.0), Location(0, 4.0));
			const GroundRouteQuery::Result complete = GroundRouteQuery::Search(Request(), snapshot);
			context.Expect(complete.outcome == NavigationOutcome::Complete,
				"A supported ground corridor must report Complete only after endpoint and clearance validation.");
			context.Expect(!complete.points.empty(), "A complete route must expose copied canonical points.");

			const GroundRouteQuery::Result partial = GroundRouteQuery::Search(Request(true), snapshot.WithFrontierLimit(1U));
			context.Expect(partial.outcome == NavigationOutcome::Partial,
				"An allowed clipped frontier must report Partial instead of pretending to reach the destination.");
			context.Expect(partial.reason == NavigationReason::OutputLimit || partial.reason == NavigationReason::DomainLimit,
				"A partial route must preserve the bounded reason that prevented completion.");

			const GroundRouteQuery::Result unavailable = GroundRouteQuery::Search(Request(), GroundRouteQuery::Snapshot{});
			context.Expect(unavailable.outcome == NavigationOutcome::UnavailableCoverage,
				"Missing coverage must remain distinct from definitive geometric unreachability.");
		});

		registry.Add("Navigation.Query.ExhaustionCancellationAndInvalidEndpointsRemainExplicit", [](TestContext& context) {
			GroundRouteQuery::Snapshot disconnected = GroundRouteQuery::Snapshot::DisconnectedGround(Location(0, 0.0), Location(0, 4.0));
			GroundRouteQuery::Request exhaustedRequest = Request();
			exhaustedRequest.maxExpansions = 1U;
			const GroundRouteQuery::Result exhausted = GroundRouteQuery::Search(exhaustedRequest, disconnected);
			context.Expect(exhausted.outcome == NavigationOutcome::BudgetExhausted,
				"A bounded search that cannot finish must report BudgetExhausted, not Unreachable.");
			context.Expect(exhausted.reason == NavigationReason::NodeLimit,
				"Search expansion exhaustion must retain an explicit node-limit reason.");

			GroundRouteQuery::Request invalid = Request();
			invalid.destination.position.offsetMetres.x = 32768.0;
			const GroundRouteQuery::Result invalidResult = GroundRouteQuery::Search(invalid, disconnected);
			context.Expect(invalidResult.outcome == NavigationOutcome::InvalidEndpoint,
				"An unnormalizable destination must reject before provider search begins.");

			GroundRouteQuery::Cancellation cancellation;
			cancellation.Cancel();
			const GroundRouteQuery::Result cancelled = GroundRouteQuery::Search(Request(), disconnected, cancellation);
			context.Expect(cancelled.outcome == NavigationOutcome::Cancelled
				&& cancelled.reason == NavigationReason::Cancelled,
				"Cancellation must be idempotent and visible as a terminal outcome.");
		});

		registry.Add("Navigation.Query.DeterministicTieBreakAndRetainedRoutesAreImmutable", [](TestContext& context) {
			const GroundRouteQuery::Snapshot equalCost = GroundRouteQuery::Snapshot::EqualCostFork(Location(0, 0.0), Location(0, 8.0));
			const GroundRouteQuery::Result first = GroundRouteQuery::Search(Request(), equalCost);
			const GroundRouteQuery::Result second = GroundRouteQuery::Search(Request(), equalCost);
			context.Expect(first.outcome == NavigationOutcome::Complete && second.outcome == NavigationOutcome::Complete,
				"A valid equal-cost fork must complete before deterministic output is compared.");
			context.Expect(first.points == second.points,
				"Equal-cost searches must use canonical node and parent keys rather than worker timing.");

			const NavigationRouteHandle handle = first.Retain(TestOwner());
			const GroundRouteQuery::RetainedRoute retained = first.ReadRetained(handle);
			context.Expect(retained.IsValid() && retained.points == first.points,
				"Retained route reads must return copied immutable points under a generation-qualified handle.");
			GroundRouteQuery::Snapshot changed = equalCost.WithRevision(12U);
			context.Expect(!retained.ValidateAgainst(changed).usable,
				"A changed coverage generation must invalidate usability without mutating retained route points.");
			context.Expect(retained.points == first.points,
				"Route invalidation must not rewrite the immutable retained route geometry.");
		});

		registry.Add("Navigation.Query.PublicRequestsNeverCompleteWithoutPublishedCoverage", [](TestContext& context) {
			// This exercises NavigationSystem's public admission boundary rather than
			// passing a test-only Snapshot. Script, editor automation, and destination
			// commands all rely on this same captured-root path.
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();
			const auto world = system.PublishedWorld();
			context.Expect(world != nullptr, "The public request fixture requires an initialized navigation world.");
			if(!world) { system.Shutdown(); return; }
			GroundRouteQuery::Request request = Request();
			request.owner.world = world->world;
			request.start.world = world->world;
			request.destination.world = world->world;
			const NavigationRequestHandle handle = system.SubmitOneShotRoute(request);
			context.Expect(handle.IsValid(), "A bounded request may be admitted before worker coverage evaluation.");
			system.Update(0.0F);
			const auto read = system.ReadRequest(handle);
			context.Expect(read.record.has_value() && read.record->outcome == NavigationOutcome::UnavailableCoverage
				&& (read.record->reason == NavigationReason::MissingTerrain
					|| read.record->reason == NavigationReason::MissingNeighbor),
				"Absent published coverage must terminate as UnavailableCoverage, never a synthetic Complete route.");
			system.Shutdown();
		});

		registry.Add("Navigation.Query.RetainedObjectRoutesRejectStaleWorldAndMissingTerrain", [](TestContext& context) {
			const NavigationObservationEvidence captured{
				1U, 3U, 4U, 6U, 7U, 8U};
			const NavigationRouteObservationValidation stale = ValidateNavigationRouteObservation(
				captured, {2U, 3U, 4U, 6U, 9U, 8U});
			const NavigationRouteObservationValidation missingTerrain = ValidateNavigationRouteObservation(
				captured, {2U, 3U, 4U, 0U, 7U, 8U});

			context.Expect(!stale.usable && stale.outcome == NavigationOutcome::Stale
				&& stale.reason == NavigationReason::DirtyCoverage,
				"A retained route must reject a newer world publication instead of consuming mixed-generation coverage.");
			context.Expect(!missingTerrain.usable && missingTerrain.outcome == NavigationOutcome::UnavailableCoverage
				&& missingTerrain.reason == NavigationReason::MissingTerrain,
				"An unavailable terrain publication must stay distinct from a geometrically unreachable destination.");
		});

		registry.Add("Navigation.Query.ConfiguredDetourNodeAndRoutePointBudgetsRemainIndependent", [](TestContext& context) {
			NavigationSettings pointLimited = NavigationSettings::Defaults();
			pointLimited.maxDetourNodesPerQuery = 64U;
			pointLimited.maxRoutePoints = 1U;
			const ProviderBudgetCaseResult pointResult = RunConfiguredProviderBudgetCase(pointLimited);

			NavigationSettings nodeLimited = NavigationSettings::Defaults();
			nodeLimited.maxDetourNodesPerQuery = 1U;
			nodeLimited.maxRoutePoints = 8U;
			const ProviderBudgetCaseResult nodeResult = RunConfiguredProviderBudgetCase(nodeLimited);

			context.Expect(pointResult.outcome == NavigationOutcome::BudgetExhausted
				&& pointResult.reason == NavigationReason::OutputLimit,
				"A configured one-point public route budget must reject the mandatory stitched start/end pair instead of publishing truncated guidance.");
			context.Expect(nodeResult.outcome == NavigationOutcome::BudgetExhausted
				&& nodeResult.reason == NavigationReason::NodeLimit,
				"A configured Detour node budget must bound provider search independently from the larger route-output budget.");
		});

		registry.Add("Navigation.Query.RouteTimingAggregatesCoverSynchronousLifecycle", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();
			const std::shared_ptr<const NavigationGeometrySnapshot> terrain = FlatTerrain();
			const bool queued = terrain != nullptr
				&& system.QueueInitialGroundBuild(NavigationProfile::Defaults(), {terrain});
			context.Expect(queued,
				"The timing fixture requires one valid copied terrain source.");
			if(!queued) {
				system.Shutdown();
				return;
			}
			system.Update(0.0F);
			const NavigationRequestHandle handle = SubmitPreparedRoute(system);
			context.Expect(handle.IsValid(), "The timing fixture request must be admitted.");
			for(std::uint32_t frame = 0U; frame < 4U; ++frame) system.Update(0.0F);
			const auto diagnostics = system.PublishedDiagnostics();
			context.Expect(diagnostics != nullptr
				&& diagnostics->routeAdmissionToCaptureCount >= 1U
				&& diagnostics->routeWorkerSearchCount >= 1U,
				"Synchronous Update must publish route admission/capture and executed worker-search samples.");
			context.Expect(diagnostics != nullptr
				&& diagnostics->routeAdmissionToCaptureCount == diagnostics->routeWorkerSearchCount,
				"A completed synchronous packet must contribute one capture sample per executed search.");
			context.Expect(diagnostics != nullptr
				&& diagnostics->routeAdmissionToCaptureNanoseconds >= diagnostics->routeAdmissionToCaptureMaxNanoseconds
				&& diagnostics->routeWorkerSearchNanoseconds >= diagnostics->routeWorkerSearchMaxNanoseconds,
				"Route timing totals must dominate their corresponding maxima.");
			system.Shutdown();
		});

		registry.Add("Navigation.Query.RouteTimingScheduledRecoveryDoesNotDoubleCountCapture", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();
			const NavigationRequestHandle handle = SubmitPreparedRoute(system);
			context.Expect(handle.IsValid(), "The scheduled timing fixture request must be admitted.");
			if(!handle.IsValid()) {
				system.Shutdown();
				return;
			}
			system.SetContractTestWorkAvailable(true);
			const auto firstWorkloads = system.DeclareFrameWorkloads(0.0F);
			const auto findWorkload = [](const auto& workloads, std::string_view id) {
				return std::find_if(workloads.begin(), workloads.end(), [id](const auto& workload) {
					return workload.stableDescriptorId == id;
				});
			};
			const auto firstCapture = findWorkload(firstWorkloads, "NavigationSystem/Navigation/Capture");
			context.Expect(firstCapture != firstWorkloads.end() && firstCapture->work,
				"The scheduled timing fixture must expose protected route capture.");
			if(firstCapture == firstWorkloads.end() || !firstCapture->work) {
				system.Shutdown();
				return;
			}
			TerraEngine::Scheduling::JobGraphContext firstContext{};
			firstCapture->work(firstContext);
			// A second capture before preparation models a cancelled worker frame. The
			// protected implementation restores the packet and must retain its original
			// admission stamp rather than charging capture twice.
			const auto secondWorkloads = system.DeclareFrameWorkloads(0.0F);
			const auto secondCapture = findWorkload(secondWorkloads, "NavigationSystem/Navigation/Capture");
			const auto secondPrepare = findWorkload(secondWorkloads, "NavigationSystem/Navigation/Prepare");
			const auto secondPublish = findWorkload(secondWorkloads, "NavigationSystem/Navigation/Publish");
			context.Expect(secondCapture != secondWorkloads.end() && secondPrepare != secondWorkloads.end()
				&& secondPublish != secondWorkloads.end() && secondCapture->work && secondPrepare->rangeWork
				&& secondPublish->work,
				"The recovered scheduled packet must redeclare capture, preparation, and publication.");
			if(secondCapture == secondWorkloads.end() || secondPrepare == secondWorkloads.end()
				|| secondPublish == secondWorkloads.end() || !secondCapture->work || !secondPrepare->rangeWork
				|| !secondPublish->work) {
				system.Shutdown();
				return;
			}
			TerraEngine::Scheduling::JobGraphContext secondContext{};
			secondCapture->work(secondContext);
			secondPrepare->rangeWork({.partitionIndex = 0U, .partitionCount = 1U,
				.itemBegin = 0U, .itemEnd = 1U}, secondContext);
			secondPublish->work(secondContext);
			const auto diagnostics = system.PublishedDiagnostics();
			context.Expect(diagnostics != nullptr
				&& diagnostics->routeAdmissionToCaptureCount == 1U
				&& diagnostics->routeWorkerSearchCount == 1U,
				"A recovered scheduled packet must contribute one capture and one executed-search sample.");
			context.Expect(diagnostics != nullptr && diagnostics->routeWorkerToPublicationCount == 0U,
				"An unavailable route without a stored route handle must not count as successful publication.");
			system.Shutdown();
		});
	}
}
