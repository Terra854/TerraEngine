// Test-first contract tests for the source-geometry upright-capsule kernel.
//
// The runtime must implement the planned header directly. The kernel is independent
// of Recast/Detour: it validates physical support and clearance before a route
// can be presented as usable, including when provider quantization is coarse.

#include "../TestHarness.h"

#include "Navigation/Ground/GroundClearanceKernel.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace TerraEngine::Navigation;

		constexpr NavigationGeometryVector3 Point(float x, float y, float z)
		{
			return { x, y, z };
		}

		NavigationProfile MakeKernelProfile()
		{
			NavigationProfile profile = NavigationProfile::Defaults();
			profile.radiusMetres = 0.40;
			profile.heightMetres = 1.80;
			profile.stepHeightMetres = 0.25;
			profile.maxSlopeDegrees = 45.0;
			profile.clearanceMarginMetres = 0.01;
			profile.horizontalResolutionMetres = 0.10;
			profile.verticalResolutionMetres = 0.05;
			return profile;
		}

		void AddBox(GroundClearanceScene& scene, GroundGeometryContribution contribution,
			NavigationGeometryVector3 center, NavigationGeometryVector3 halfExtents)
		{
			NavigationGeometryPrimitive primitive{};
			primitive.kind = NavigationGeometryPrimitiveKind::Box;
			primitive.center = center;
			primitive.halfExtents = halfExtents;

			GroundClearanceSource source{};
			source.contribution = contribution;
			source.geometry.kind = NavigationGeometryPayloadKind::AnalyticPrimitives;
			source.geometry.primitives.push_back(primitive);
			scene.sources.push_back(std::move(source));
		}

		GroundClearanceScene MakeFlatSupport()
		{
			GroundClearanceScene scene{};
			AddBox(scene, GroundGeometryContribution::WalkableSupport, Point(0.0F, -0.10F, 0.0F), Point(6.0F, 0.10F, 4.0F));
			return scene;
		}

		GroundClearanceScene MakeRamp(float rise, float run)
		{
			GroundClearanceScene scene{};
			GroundClearanceSource source{};
			source.contribution = GroundGeometryContribution::WalkableSupport;
			source.geometry.kind = NavigationGeometryPayloadKind::TriangleMesh;
			source.geometry.vertices = {
				Point(-run * 0.5F, 0.0F, -1.0F), Point(run * 0.5F, rise, -1.0F),
				Point(-run * 0.5F, 0.0F, 1.0F), Point(run * 0.5F, rise, 1.0F)
			};
			source.geometry.triangles = {{ 0U, 1U, 2U }, { 2U, 1U, 3U }};
			scene.sources.push_back(std::move(source));
			return scene;
		}

		GroundClearanceResult Validate(const GroundClearanceScene& scene, NavigationGeometryVector3 start,
			NavigationGeometryVector3 destination, const NavigationProfile& profile = MakeKernelProfile(),
			std::uint32_t maximumRefinementSteps = 8U)
		{
			GroundCapsuleSegmentRequest request{};
			request.scene = scene;
			request.profile = profile;
			request.start = start;
			request.destination = destination;
			request.maximumRefinementSteps = maximumRefinementSteps;
			return ValidateGroundCapsuleSegment(request);
		}
	}

	void RegisterNavigationClearanceKernelTests(TestRegistry& registry)
	{
		registry.Add("Navigation.ClearanceKernel.ValidatesContinuousUprightCapsuleSupport", [](TestContext& context) {
			const GroundClearanceScene scene = MakeFlatSupport();
			const GroundClearanceResult result = Validate(scene, Point(-4.0F, 0.0F, 0.0F), Point(4.0F, 0.0F, 0.0F));
			context.Expect(result.accepted, "A capsule with continuous source support and open headroom must validate.");
			context.Expect(GroundClearanceStatus::Accepted == result.status,
				"An accepted segment must report a provider-neutral clearance status.");
			context.Expect(result.minimumSupportFraction >= 1.0,
				"Continuous validation must certify all swept samples, not only the two waypoint centers.");
		});

		registry.Add("Navigation.ClearanceKernel.RejectsMissingMidSegmentSupport", [](TestContext& context) {
			GroundClearanceScene scene{};
			// These slabs deliberately leave a 0.5 m opening around x=0. Both
			// endpoints are supported, so only a continuous swept check can reject.
			AddBox(scene, GroundGeometryContribution::WalkableSupport, Point(-2.0F, -0.10F, 0.0F), Point(1.75F, 0.10F, 2.0F));
			AddBox(scene, GroundGeometryContribution::WalkableSupport, Point(2.0F, -0.10F, 0.0F), Point(1.75F, 0.10F, 2.0F));
			const GroundClearanceResult result = Validate(scene, Point(-3.0F, 0.0F, 0.0F), Point(3.0F, 0.0F, 0.0F));
			context.Expect(!result.accepted, "A capsule segment crossing an unsupported gap must reject even when both endpoints stand on support.");
			context.Expect(GroundClearanceStatus::MissingSupport == result.status,
				"Unsupported swept motion must not be reclassified as a step or a complete route.");
		});

		registry.Add("Navigation.ClearanceKernel.RejectsLowCeilingAndCornerPenetration", [](TestContext& context) {
			GroundClearanceScene lowCeiling = MakeFlatSupport();
			AddBox(lowCeiling, GroundGeometryContribution::SolidObstruction, Point(0.0F, 1.45F, 0.0F), Point(2.0F, 0.10F, 2.0F));
			const GroundClearanceResult ceiling = Validate(lowCeiling, Point(-3.0F, 0.0F, 0.0F), Point(3.0F, 0.0F, 0.0F));
			context.Expect(GroundClearanceStatus::InsufficientClearance == ceiling.status,
				"The complete upright capsule height plus margin must fit under an overhead collider.");

			GroundClearanceScene corner = MakeFlatSupport();
			// The near corner intrudes into the 0.40 m radius plus 0.01 m margin;
			// a point-at-centre clearance check would still miss this side contact.
			AddBox(corner, GroundGeometryContribution::SolidObstruction, Point(0.0F, 0.70F, 0.50F), Point(0.20F, 0.70F, 0.20F));
			const GroundClearanceResult cornerResult = Validate(corner, Point(-3.0F, 0.0F, 0.0F), Point(3.0F, 0.0F, 0.0F));
			context.Expect(GroundClearanceStatus::CornerCollision == cornerResult.status,
				"Capsule side/corner contact must reject; waypoint-center-only checks are insufficient.");
		});

		registry.Add("Navigation.ClearanceKernel.EnforcesSlopeAndStepEnvelopes", [](TestContext& context) {
			const GroundClearanceResult walkableRamp = Validate(MakeRamp(2.0F, 4.0F), Point(-1.8F, 0.0F, 0.0F), Point(1.8F, 2.0F, 0.0F));
			context.Expect(GroundClearanceStatus::Accepted == walkableRamp.status,
				"A source-triangle ramp below the profile slope limit must remain usable.");

			const GroundClearanceResult steepRamp = Validate(MakeRamp(5.0F, 4.0F), Point(-1.8F, 0.0F, 0.0F), Point(1.8F, 5.0F, 0.0F));
			context.Expect(GroundClearanceStatus::ExcessiveSlope == steepRamp.status,
				"A source-triangle ramp over the profile slope limit must reject before query completion.");

			GroundClearanceScene highStep{};
			AddBox(highStep, GroundGeometryContribution::WalkableSupport, Point(-1.5F, -0.10F, 0.0F), Point(1.5F, 0.10F, 2.0F));
			AddBox(highStep, GroundGeometryContribution::WalkableSupport, Point(1.5F, 0.20F, 0.0F), Point(1.5F, 0.30F, 2.0F));
			const GroundClearanceResult step = Validate(highStep, Point(-2.0F, 0.0F, 0.0F), Point(2.0F, 0.50F, 0.0F));
			context.Expect(GroundClearanceStatus::ExcessiveStep == step.status,
				"A permitted step requires a bounded lift, advance, and supported landing; a taller ledge is not a gap bridge.");
		});

		registry.Add("Navigation.ClearanceKernel.RefinesConservativeQuantizationOrReportsResolutionLimit", [](TestContext& context) {
			GroundClearanceScene nearThreshold = MakeFlatSupport();
			// This ceiling is deliberately within one vertical voxel of the capsule
			// threshold. A coarse provider classification cannot decide it alone.
			AddBox(nearThreshold, GroundGeometryContribution::SolidObstruction, Point(0.0F, 1.815F, 0.0F), Point(2.0F, 0.005F, 2.0F));
			const GroundClearanceResult refined = Validate(nearThreshold, Point(-2.0F, 0.0F, 0.0F), Point(2.0F, 0.0F, 0.0F));
			context.Expect(refined.refinementSteps > 0U,
				"Near a quantization threshold, the kernel must run bounded source-geometry refinement.");
			context.Expect(refined.status == GroundClearanceStatus::Accepted || refined.status == GroundClearanceStatus::InsufficientClearance,
				"Refinement must resolve the physical source result without trusting a quantized provider label.");

			const GroundClearanceResult exhausted = Validate(nearThreshold, Point(-2.0F, 0.0F, 0.0F), Point(2.0F, 0.0F, 0.0F),
				MakeKernelProfile(), 0U);
			context.Expect(GroundClearanceStatus::ResolutionLimit == exhausted.status,
				"An unresolved near-threshold classification must report ResolutionLimit rather than invent reachability.");

			GroundClearanceScene malformed = MakeFlatSupport();
			malformed.sources.front().geometry.primitives.front().center.x = std::numeric_limits<float>::quiet_NaN();
			const GroundClearanceResult invalid = Validate(malformed, Point(-1.0F, 0.0F, 0.0F), Point(1.0F, 0.0F, 0.0F));
			context.Expect(GroundClearanceStatus::InvalidInput == invalid.status,
				"Non-finite source data must reject before collision or refinement work begins.");
		});
	}
}
