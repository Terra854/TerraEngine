// Test-first contract tests for terrain-free mesh and collider ground routing.
//
// These tests deliberately include the planned provider-neutral source and route
// headers. The runtime must supply those headers and the API exercised here;
// silently skipping the matrix would make a mesh-only scene appear supported
// before it can actually be queried.

#include "../TestHarness.h"

#include "Navigation/Geometry/MeshColliderNavigationSource.h"
#include "Navigation/Ground/GroundRouteQuery.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace TerraEngine::Navigation;

		constexpr NavigationGeometryVector3 Point(float x, float y, float z)
		{
			return { x, y, z };
		}

		NavigationProfile MakeSmallProfile()
		{
			NavigationProfile profile = NavigationProfile::Defaults();
			profile.profileId = 101U;
			profile.revision = 1U;
			profile.radiusMetres = 0.25;
			profile.heightMetres = 1.20;
			profile.stepHeightMetres = 0.25;
			profile.maxSlopeDegrees = 45.0;
			return profile;
		}

		NavigationProfile MakeLargeProfile()
		{
			NavigationProfile profile = NavigationProfile::Defaults();
			profile.profileId = 102U;
			profile.revision = 1U;
			profile.radiusMetres = 0.55;
			profile.heightMetres = 2.10;
			profile.stepHeightMetres = 0.25;
			profile.maxSlopeDegrees = 45.0;
			return profile;
		}

		// The fixture helpers describe physical source geometry, not a pre-baked
		// navigation mesh. A source adapter must retain the original role so a
		// solid collider cannot accidentally become walkable support.
		void AddBox(
			GroundRouteScene& scene,
			NavigationSourceKind sourceKind,
			GroundGeometryContribution contribution,
			NavigationGeometryVector3 center,
			NavigationGeometryVector3 halfExtents)
		{
			NavigationGeometryPrimitive box{};
			box.kind = NavigationGeometryPrimitiveKind::Box;
			box.center = center;
			box.halfExtents = halfExtents;

			MeshColliderNavigationSourceInput source{};
			source.sourceKind = sourceKind;
			source.contribution = contribution;
			source.geometry.kind = NavigationGeometryPayloadKind::AnalyticPrimitives;
			source.geometry.primitives.push_back(box);
			scene.sources.push_back(std::move(source));
		}

		void AddRamp(GroundRouteScene& scene, float xOffset, float rise, float run)
		{
			MeshColliderNavigationSourceInput source{};
			source.sourceKind = NavigationSourceKind::Mesh;
			source.contribution = GroundGeometryContribution::WalkableSupport;
			source.geometry.kind = NavigationGeometryPayloadKind::TriangleMesh;
			source.geometry.vertices = {
				Point(xOffset, 0.0F, -1.0F), Point(xOffset + run, rise, -1.0F),
				Point(xOffset, 0.0F, 1.0F), Point(xOffset + run, rise, 1.0F)
			};
			source.geometry.triangles = {{ 0U, 1U, 2U }, { 2U, 1U, 3U }};
			scene.sources.push_back(std::move(source));
		}

		GroundRouteScene MakeFlatScene()
		{
			GroundRouteScene scene{};
			AddBox(scene, NavigationSourceKind::Collider, GroundGeometryContribution::WalkableSupport,
				Point(0.0F, -0.10F, 0.0F), Point(8.0F, 0.10F, 3.0F));
			return scene;
		}

		GroundRouteScene MakeLayeredScene(bool includeRamp)
		{
			GroundRouteScene scene = MakeFlatScene();
			AddBox(scene, NavigationSourceKind::Mesh, GroundGeometryContribution::WalkableSupport,
				// Keep the ramp landing at least one large-capsule radius inside
				// the upper support instead of balancing it on an unsupported edge.
				Point(0.0F, 3.90F, 0.0F), Point(12.0F, 0.10F, 3.0F));
			if(includeRamp) {
				// The ramp ends at x=8, exactly at the upper floor's edge, so this
				// fixture has a physical layer connection rather than projected overlap.
				AddRamp(scene, 2.0F, 4.0F, 6.0F);
			}
			return scene;
		}

		GroundRouteScene MakeStairScene(float riserHeight)
		{
			GroundRouteScene scene{};
			for(std::uint32_t step = 0U; step < 5U; ++step) {
				const float height = riserHeight * static_cast<float>(step + 1U);
				AddBox(scene, NavigationSourceKind::Collider, GroundGeometryContribution::WalkableSupport,
					Point(-2.0F + static_cast<float>(step) * 0.7F, height * 0.5F - 0.10F, 0.0F),
					Point(0.35F, height * 0.5F + 0.10F, 1.0F));
			}
			return scene;
		}

		GroundRouteScene MakeGapScene(float gapWidth)
		{
			GroundRouteScene scene{};
			const float halfGap = gapWidth * 0.5F;
			AddBox(scene, NavigationSourceKind::Mesh, GroundGeometryContribution::WalkableSupport,
				Point(-4.0F - halfGap, -0.10F, 0.0F), Point(4.0F, 0.10F, 2.0F));
			AddBox(scene, NavigationSourceKind::Collider, GroundGeometryContribution::WalkableSupport,
				Point(4.0F + halfGap, -0.10F, 0.0F), Point(4.0F, 0.10F, 2.0F));
			return scene;
		}

		GroundRouteScene MakeDoorwayScene(float halfWidth, float ceilingHeight)
		{
			GroundRouteScene scene = MakeFlatScene();
			// Two solid boxes leave a centered opening. The roof is also a solid
			// collider, so the query must test both horizontal and vertical capsule
			// clearance through the opening.
			AddBox(scene, NavigationSourceKind::Collider, GroundGeometryContribution::SolidObstruction,
				Point(0.0F, 1.5F, -2.0F), Point(0.25F, 1.5F, 1.5F - halfWidth));
			AddBox(scene, NavigationSourceKind::Collider, GroundGeometryContribution::SolidObstruction,
				Point(0.0F, 1.5F, 2.0F), Point(0.25F, 1.5F, 1.5F - halfWidth));
			AddBox(scene, NavigationSourceKind::Collider, GroundGeometryContribution::SolidObstruction,
				Point(0.0F, ceilingHeight + 0.50F, 0.0F), Point(0.25F, 0.50F, halfWidth));
			return scene;
		}

		GroundRouteResult Evaluate(const GroundRouteScene& scene, const NavigationProfile& profile,
			NavigationGeometryVector3 start, NavigationGeometryVector3 destination)
		{
			GroundRouteRequest request{};
			request.scene = scene;
			request.profile = profile;
			request.start = start;
			request.destination = destination;
			request.allowEndpointAdjustment = false;
			// Use the parent provider-neutral facade explicitly. The implementation
			// remains in Ground, whose ADL candidate would otherwise be ambiguous.
			return ::TerraEngine::Navigation::QueryGroundRoute(request);
		}

		struct MeshGroundCase
		{
			std::string_view name;
			GroundRouteScene scene;
			NavigationGeometryVector3 start{};
			NavigationGeometryVector3 destination{};
			NavigationOutcome smallOutcome = NavigationOutcome::Complete;
			NavigationReason smallReason = NavigationReason::None;
			NavigationOutcome largeOutcome = NavigationOutcome::Complete;
			NavigationReason largeReason = NavigationReason::None;
		};

		std::vector<MeshGroundCase> MakeMeshGroundMatrix()
		{
			// 26 terrain-free cases multiplied by two profiles produce 52 deterministic
			// requests. They intentionally mix mesh and collider sources, so support is
			// never inferred from render visibility or from the presence of terrain.
			std::vector<MeshGroundCase> cases;
			cases.reserve(26U);
			auto add = [&cases](std::string_view name, GroundRouteScene scene,
				NavigationGeometryVector3 start, NavigationGeometryVector3 destination,
				NavigationOutcome smallOutcome, NavigationReason smallReason,
				NavigationOutcome largeOutcome, NavigationReason largeReason) {
				cases.push_back({ name, std::move(scene), start, destination,
					smallOutcome, smallReason, largeOutcome, largeReason });
			};

			for(std::uint32_t lane = 0U; lane < 6U; ++lane) {
				const float z = -2.5F + static_cast<float>(lane);
				add("flat-mixed-support", MakeFlatScene(), Point(-6.0F, 0.0F, z), Point(6.0F, 0.0F, z),
					NavigationOutcome::Complete, NavigationReason::None, NavigationOutcome::Complete, NavigationReason::None);
			}
			for(std::uint32_t lane = 0U; lane < 3U; ++lane) {
				const float z = -1.0F + static_cast<float>(lane);
				add("layered-floors-have-no-projected-connectivity", MakeLayeredScene(false),
					Point(-5.0F, 0.0F, z), Point(5.0F, 4.0F, z),
					NavigationOutcome::Unreachable, NavigationReason::MissingSupport,
					NavigationOutcome::Unreachable, NavigationReason::MissingSupport);
			}
			for(std::uint32_t lane = 0U; lane < 3U; ++lane) {
				const float z = -0.75F + static_cast<float>(lane) * 0.75F;
				add("mesh-ramp-connects-real-layers", MakeLayeredScene(true), Point(-5.0F, 0.0F, z), Point(7.0F, 4.0F, z),
					NavigationOutcome::Complete, NavigationReason::None, NavigationOutcome::Complete, NavigationReason::None);
			}
			for(std::uint32_t lane = 0U; lane < 3U; ++lane) {
				const float z = -0.50F + static_cast<float>(lane) * 0.50F;
				add("collider-stairs-within-step-limit", MakeStairScene(0.20F), Point(-2.3F, 0.20F, z), Point(0.8F, 1.0F, z),
					NavigationOutcome::Complete, NavigationReason::None, NavigationOutcome::Complete, NavigationReason::None);
			}
			for(std::uint32_t lane = 0U; lane < 3U; ++lane) {
				const float z = -0.50F + static_cast<float>(lane) * 0.50F;
				add("collider-stairs-over-step-limit", MakeStairScene(0.35F), Point(-2.3F, 0.35F, z), Point(0.8F, 1.75F, z),
					NavigationOutcome::Unreachable, NavigationReason::MissingSupport,
					NavigationOutcome::Unreachable, NavigationReason::MissingSupport);
			}
			for(std::uint32_t widthIndex = 0U; widthIndex < 3U; ++widthIndex) {
				const float gap = 0.20F + static_cast<float>(widthIndex) * 0.30F;
				add("unsupported-gap-never-becomes-a-step", MakeGapScene(gap), Point(-2.0F, 0.0F, 0.0F), Point(2.0F, 0.0F, 0.0F),
					NavigationOutcome::Unreachable, NavigationReason::MissingSupport,
					NavigationOutcome::Unreachable, NavigationReason::MissingSupport);
			}
			add("small-doorway-admits-small-profile", MakeDoorwayScene(0.35F, 1.30F), Point(-4.0F, 0.0F, 0.0F), Point(4.0F, 0.0F, 0.0F),
				NavigationOutcome::Complete, NavigationReason::None,
				NavigationOutcome::Unreachable, NavigationReason::InsufficientClearance);
			add("low-ceiling-rejects-large-profile", MakeDoorwayScene(1.20F, 1.30F), Point(-4.0F, 0.0F, 0.0F), Point(4.0F, 0.0F, 0.0F),
				NavigationOutcome::Complete, NavigationReason::None,
				NavigationOutcome::Unreachable, NavigationReason::InsufficientClearance);
			add("start-below-only-layer-is-invalid", MakeLayeredScene(false), Point(-2.0F, 1.50F, 0.0F), Point(2.0F, 0.0F, 0.0F),
				NavigationOutcome::InvalidEndpoint, NavigationReason::InvalidStart,
				NavigationOutcome::InvalidEndpoint, NavigationReason::InvalidStart);
			add("destination-above-only-layer-is-invalid", MakeLayeredScene(false), Point(-2.0F, 0.0F, 0.0F), Point(2.0F, 1.50F, 0.0F),
				NavigationOutcome::InvalidEndpoint, NavigationReason::InvalidDestination,
				NavigationOutcome::InvalidEndpoint, NavigationReason::InvalidDestination);
			add("destination-outside-mesh-and-collider-coverage-is-invalid", MakeFlatScene(), Point(-2.0F, 0.0F, 0.0F), Point(20.0F, 0.0F, 0.0F),
				NavigationOutcome::InvalidEndpoint, NavigationReason::InvalidDestination,
				NavigationOutcome::InvalidEndpoint, NavigationReason::InvalidDestination);

			return cases;
		}
	}

	void RegisterMeshGroundNavigationTests(TestRegistry& registry)
	{
		registry.Add("Navigation.MeshGround.TerrainFreeMatrixHasFiftyTwoProfileCheckedRequests", [](TestContext& context) {
			const NavigationProfile smallProfile = MakeSmallProfile();
			const NavigationProfile largeProfile = MakeLargeProfile();
			const std::vector<MeshGroundCase> cases = MakeMeshGroundMatrix();
			context.ExpectEqual(std::size_t(26U), cases.size(),
				"The terrain-free matrix must keep 26 distinct mesh/collider cases per reference profile.");

			std::size_t smallRequests = 0U;
			std::size_t largeRequests = 0U;
			for(const MeshGroundCase& testCase : cases) {
				const GroundRouteResult small = Evaluate(testCase.scene, smallProfile, testCase.start, testCase.destination);
				context.Expect(testCase.smallOutcome == small.outcome,
					std::string("Small-profile terrain-free outcome mismatch for ") + std::string(testCase.name));
				context.Expect(testCase.smallReason == small.reason,
					std::string("Small-profile terrain-free reason mismatch for ") + std::string(testCase.name));
				++smallRequests;

				const GroundRouteResult large = Evaluate(testCase.scene, largeProfile, testCase.start, testCase.destination);
				context.Expect(testCase.largeOutcome == large.outcome,
					std::string("Large-profile terrain-free outcome mismatch for ") + std::string(testCase.name));
				context.Expect(testCase.largeReason == large.reason,
					std::string("Large-profile terrain-free reason mismatch for ") + std::string(testCase.name));
				++largeRequests;
			}

			context.Expect(smallRequests >= 25U && largeRequests >= 25U,
				"The no-terrain matrix must execute at least 25 deterministic requests for each reference capsule profile.");
			context.Expect(smallRequests + largeRequests >= 50U,
				"The no-terrain mesh/collider matrix must execute at least 50 deterministic requests in total.");
		});
	}
}
