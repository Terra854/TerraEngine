// Test-first contracts for the provider-neutral ground route query.
//
// These tests intentionally include the future query boundary directly. They
// must fail until the query implementation provides bounded private scratch, deterministic search,
// explicit incomplete outcomes, and immutable retained route data.
#include "../TestHarness.h"

#include "Navigation/Ground/GroundRouteQuery.h"
#include "Navigation/NavigationSettings.h"
#include "Navigation/NavigationRouteValidity.h"
#include "Navigation/NavigationSystem.h"
#include "Navigation/NavigationTypes.h"

#include <cstdint>
#include <string_view>

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
	}
}
