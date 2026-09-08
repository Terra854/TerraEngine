// TerrainGroundNavigationTests.cpp - deterministic terrain route acceptance matrix.
//
// This test deliberately names the future provider-neutral GroundRouteQuery
// boundary. The runtime must implement that boundary before this source can build;
// the expected answers below are an engine-owned semantic oracle, never Recast
// output or a provider polygon reference.

#include "TestHarness.h"

#include "Navigation/Ground/GroundRouteQuery.h"
#include "Navigation/NavigationSettings.h"
#include "Navigation/NavigationTypes.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace TerraEngine::Tests {
	namespace {
		enum class TerrainScenario : std::uint8_t {
			Flat,
			TerrainBoundary,
			TerrainHole,
			HeightStep,
			TerrainToMesh,
			StackedFloor,
			Stair,
			Ramp,
			Clearance,
			InvalidStart,
			InvalidDestination,
			UnsupportedGap,
			RenderVisibility,
			ExplicitConnection
		};

		enum class ReferenceCapsule : std::uint8_t { Small, Large };

		struct TerrainCase {
			std::string_view id;
			TerrainScenario scenario;
			ReferenceCapsule capsule;
			bool alternateCondition = false;
		};

		struct ExpectedGroundResult {
			Navigation::NavigationOutcome outcome;
			Navigation::NavigationReason reason;
		};

		// The expected answer is intentionally calculated from the fixture's named
		// physical condition. It does not inspect a generated navmesh, so provider
		// regressions cannot redefine what the terrain geometry means.
		ExpectedGroundResult EvaluateTerrainOracle(const TerrainCase& testCase) {
			using Outcome = Navigation::NavigationOutcome;
			using Reason = Navigation::NavigationReason;
			switch(testCase.scenario) {
			case TerrainScenario::Flat:
			case TerrainScenario::TerrainToMesh:
			case TerrainScenario::StackedFloor:
			case TerrainScenario::RenderVisibility:
				return {Outcome::Complete, Reason::None};
			case TerrainScenario::TerrainBoundary:
				return testCase.alternateCondition ? ExpectedGroundResult{Outcome::InvalidEndpoint, Reason::InvalidDestination}
					: ExpectedGroundResult{Outcome::Complete, Reason::None};
			case TerrainScenario::TerrainHole:
			case TerrainScenario::UnsupportedGap:
				return {Outcome::Unreachable, Reason::MissingSupport};
			case TerrainScenario::HeightStep:
			case TerrainScenario::Stair:
				return testCase.alternateCondition ? ExpectedGroundResult{Outcome::Unreachable, Reason::MissingSupport}
					: ExpectedGroundResult{Outcome::Complete, Reason::None};
			case TerrainScenario::Ramp:
				return testCase.alternateCondition ? ExpectedGroundResult{Outcome::Unreachable, Reason::MissingSupport}
					: ExpectedGroundResult{Outcome::Complete, Reason::None};
			case TerrainScenario::Clearance:
				return testCase.alternateCondition ? ExpectedGroundResult{Outcome::InvalidEndpoint, Reason::InsufficientClearance}
					: ExpectedGroundResult{Outcome::Complete, Reason::None};
			case TerrainScenario::InvalidStart:
				return {Outcome::InvalidEndpoint, Reason::InvalidStart};
			case TerrainScenario::InvalidDestination:
				return {Outcome::InvalidEndpoint, Reason::InvalidDestination};
			case TerrainScenario::ExplicitConnection:
				return testCase.alternateCondition ? ExpectedGroundResult{Outcome::Unreachable, Reason::MissingSupport}
					: ExpectedGroundResult{Outcome::Complete, Reason::None};
			}
			return {Outcome::Failed, Reason::ProviderFailure};
		}

		Navigation::NavigationProfile MakeReferenceProfile(ReferenceCapsule capsule) {
			Navigation::NavigationProfile profile = Navigation::NavigationProfile::Defaults();
			if(capsule == ReferenceCapsule::Large) {
				profile.profileId = 2U;
				profile.radiusMetres = 0.7;
				profile.heightMetres = 2.2;
				profile.stepHeightMetres = 0.3;
			}
			return profile;
		}

		// The 50 fixed identifiers are mirrored verbatim in
		// Tests/Fixtures/Navigation/manifest.json. There are exactly 25 cases for
		// each reference capsule, while each semantic category has a safe and a
		// rejecting boundary where that distinction matters.
		constexpr std::array<TerrainCase, 50U> kTerrainCases{{
			{"GroundTerrain.small.flat-open-01", TerrainScenario::Flat, ReferenceCapsule::Small},
			{"GroundTerrain.small.connection-disabled", TerrainScenario::ExplicitConnection, ReferenceCapsule::Small, true},
			{"GroundTerrain.small.boundary-inside", TerrainScenario::TerrainBoundary, ReferenceCapsule::Small},
			{"GroundTerrain.small.boundary-outside", TerrainScenario::TerrainBoundary, ReferenceCapsule::Small, true},
			{"GroundTerrain.small.hole-crossing", TerrainScenario::TerrainHole, ReferenceCapsule::Small},
			{"GroundTerrain.small.hole-destination", TerrainScenario::TerrainHole, ReferenceCapsule::Small, true},
			{"GroundTerrain.small.height-step-valid", TerrainScenario::HeightStep, ReferenceCapsule::Small},
			{"GroundTerrain.small.height-step-too-high", TerrainScenario::HeightStep, ReferenceCapsule::Small, true},
			{"GroundTerrain.small.mesh-transition-forward", TerrainScenario::TerrainToMesh, ReferenceCapsule::Small},
			{"GroundTerrain.small.mesh-transition-return", TerrainScenario::TerrainToMesh, ReferenceCapsule::Small, true},
			{"GroundTerrain.small.stacked-lower-floor", TerrainScenario::StackedFloor, ReferenceCapsule::Small},
			{"GroundTerrain.small.stacked-upper-floor", TerrainScenario::StackedFloor, ReferenceCapsule::Small, true},
			{"GroundTerrain.small.stair-valid", TerrainScenario::Stair, ReferenceCapsule::Small},
			{"GroundTerrain.small.stair-too-tall", TerrainScenario::Stair, ReferenceCapsule::Small, true},
			{"GroundTerrain.small.ramp-valid", TerrainScenario::Ramp, ReferenceCapsule::Small},
			{"GroundTerrain.small.ramp-too-steep", TerrainScenario::Ramp, ReferenceCapsule::Small, true},
			{"GroundTerrain.small.clearance-valid", TerrainScenario::Clearance, ReferenceCapsule::Small},
			{"GroundTerrain.small.clearance-low-ceiling", TerrainScenario::Clearance, ReferenceCapsule::Small, true},
			{"GroundTerrain.small.invalid-start", TerrainScenario::InvalidStart, ReferenceCapsule::Small},
			{"GroundTerrain.small.invalid-destination", TerrainScenario::InvalidDestination, ReferenceCapsule::Small},
			{"GroundTerrain.small.unsupported-gap", TerrainScenario::UnsupportedGap, ReferenceCapsule::Small},
			{"GroundTerrain.small.unsupported-narrow-gap", TerrainScenario::UnsupportedGap, ReferenceCapsule::Small, true},
			{"GroundTerrain.small.render-visible", TerrainScenario::RenderVisibility, ReferenceCapsule::Small},
			{"GroundTerrain.small.render-hidden", TerrainScenario::RenderVisibility, ReferenceCapsule::Small, true},
			{"GroundTerrain.small.connection-enabled", TerrainScenario::ExplicitConnection, ReferenceCapsule::Small},
			{"GroundTerrain.large.flat-open-01", TerrainScenario::Flat, ReferenceCapsule::Large},
			{"GroundTerrain.large.connection-disabled", TerrainScenario::ExplicitConnection, ReferenceCapsule::Large, true},
			{"GroundTerrain.large.boundary-inside", TerrainScenario::TerrainBoundary, ReferenceCapsule::Large},
			{"GroundTerrain.large.boundary-outside", TerrainScenario::TerrainBoundary, ReferenceCapsule::Large, true},
			{"GroundTerrain.large.hole-crossing", TerrainScenario::TerrainHole, ReferenceCapsule::Large},
			{"GroundTerrain.large.hole-destination", TerrainScenario::TerrainHole, ReferenceCapsule::Large, true},
			{"GroundTerrain.large.height-step-valid", TerrainScenario::HeightStep, ReferenceCapsule::Large},
			{"GroundTerrain.large.height-step-too-high", TerrainScenario::HeightStep, ReferenceCapsule::Large, true},
			{"GroundTerrain.large.mesh-transition-forward", TerrainScenario::TerrainToMesh, ReferenceCapsule::Large},
			{"GroundTerrain.large.mesh-transition-return", TerrainScenario::TerrainToMesh, ReferenceCapsule::Large, true},
			{"GroundTerrain.large.stacked-lower-floor", TerrainScenario::StackedFloor, ReferenceCapsule::Large},
			{"GroundTerrain.large.stacked-upper-floor", TerrainScenario::StackedFloor, ReferenceCapsule::Large, true},
			{"GroundTerrain.large.stair-valid", TerrainScenario::Stair, ReferenceCapsule::Large},
			{"GroundTerrain.large.stair-too-tall", TerrainScenario::Stair, ReferenceCapsule::Large, true},
			{"GroundTerrain.large.ramp-valid", TerrainScenario::Ramp, ReferenceCapsule::Large},
			{"GroundTerrain.large.ramp-too-steep", TerrainScenario::Ramp, ReferenceCapsule::Large, true},
			{"GroundTerrain.large.clearance-valid", TerrainScenario::Clearance, ReferenceCapsule::Large},
			{"GroundTerrain.large.clearance-low-ceiling", TerrainScenario::Clearance, ReferenceCapsule::Large, true},
			{"GroundTerrain.large.invalid-start", TerrainScenario::InvalidStart, ReferenceCapsule::Large},
			{"GroundTerrain.large.invalid-destination", TerrainScenario::InvalidDestination, ReferenceCapsule::Large},
			{"GroundTerrain.large.unsupported-gap", TerrainScenario::UnsupportedGap, ReferenceCapsule::Large},
			{"GroundTerrain.large.unsupported-narrow-gap", TerrainScenario::UnsupportedGap, ReferenceCapsule::Large, true},
			{"GroundTerrain.large.render-visible", TerrainScenario::RenderVisibility, ReferenceCapsule::Large},
			{"GroundTerrain.large.render-hidden", TerrainScenario::RenderVisibility, ReferenceCapsule::Large, true},
			{"GroundTerrain.large.connection-enabled", TerrainScenario::ExplicitConnection, ReferenceCapsule::Large}
		}};

		Navigation::Ground::GroundRouteFixtureRequest MakeFixtureRequest(const TerrainCase& testCase) {
			Navigation::Ground::GroundRouteFixtureRequest request{};
			request.fixtureId = "GroundTerrain";
			request.caseId = testCase.id;
			request.sourceHash = "4b9f3d957b0905fc2cae5bdc7b44532fdb1439f08ca49e1d71694f2419f3a7c1";
			request.profileHash = testCase.capsule == ReferenceCapsule::Small
				? "0f7b8a452e5d8c1bbd7a4f3c16ef17a602c6fb763ab9c4c7e0a2d612973c5aa8"
				: "d1f19e9648d7c6a2459fd49f074d6c8bd5acfe57b46d8e801f15f9b2b0479a66";
			request.profile = MakeReferenceProfile(testCase.capsule);
			request.allowPartial = false;
			return request;
		}
	}

	void RegisterTerrainGroundNavigationTests(TestRegistry& registry) {
		std::size_t smallCount = 0U;
		std::size_t largeCount = 0U;
		for(const TerrainCase& testCase : kTerrainCases) {
			if(testCase.capsule == ReferenceCapsule::Small) ++smallCount;
			else ++largeCount;

			registry.Add(std::string("Navigation.GroundTerrain.") + std::string(testCase.id), [testCase](TestContext& context) {
				const ExpectedGroundResult expected = EvaluateTerrainOracle(testCase);
				const Navigation::Ground::GroundRouteFixtureResult actual = Navigation::Ground::GroundRouteQuery::EvaluateFixture(
					MakeFixtureRequest(testCase));
				context.Expect(actual.outcome == expected.outcome && actual.reason == expected.reason,
					"The terrain route result must match the deterministic, provider-independent physical oracle.");
			});
		}

		registry.Add("Navigation.GroundTerrain.MatrixHasFiftyCasesAndTwoReferenceCapsules", [smallCount, largeCount](TestContext& context) {
			context.Expect(kTerrainCases.size() >= 50U && smallCount >= 25U && largeCount >= 25U,
				"GroundTerrain must retain at least 50 stable cases and at least 25 for each reference capsule.");
		});
	}
}
