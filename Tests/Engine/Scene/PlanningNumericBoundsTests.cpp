#include "TestHarness.h"
#include "Editor/AssetCompilers/VirtualGeometrySplineCurveBuilder.h"
#include "Scene/WorldStreamingSystem.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Editor;
		using namespace TerraEngine::Scene;
		std::array<VirtualGeometryCurveControlPoint, 2> NumericControls(float left, float right) {
			return {{{{left, 0.0F, 0.0F}, 0.25F, 1U, 11U}, {{right, 0.0F, 0.0F}, 0.25F, 2U, 13U}}};
		}
		struct NumericObservation {
			std::uint64_t splineConversions = 0U;
			std::uint64_t radiusConversions = 0U;
			std::uint64_t directoryLookups = 0U;
			VirtualGeometrySplinePlanningObserver Spline() {
				return {this, [](void* state, double estimate, std::uint32_t, std::uint32_t) {
					++static_cast<NumericObservation*>(state)->splineConversions;
					// Inspect the exact double input to the production integer cast.
					// A float observer would round UINT32_MAX upward to2^32.
					const bool nonfinite = !std::isfinite(estimate);
					if (nonfinite || estimate < 0.0F || static_cast<double>(estimate) > static_cast<double>((std::numeric_limits<std::uint32_t>::max)()))
						throw TestFailure("Spline production path reached an unsafe count conversion before clamping/rejection; observer stopped before the cast.");
				}};
			}
			RegionInterestPlanningObserver Streaming() {
				return {this, [](void* state, double radiusRegions) {
					++static_cast<NumericObservation*>(state)->radiusConversions;
					// int64 max rounds to2^63 as double. Require the exact upper bound
					// exclusively so no out-of-range conversion is ever executed here.
					if (!std::isfinite(radiusRegions) || radiusRegions < 0.0 || radiusRegions >= 0x1p63)
						throw TestFailure("Streaming production path reached an unsafe radius conversion before rejection; observer stopped before the cast.");
				}, [](void* state, const WorldRegionKey&) { ++static_cast<NumericObservation*>(state)->directoryLookups; }};
			}
		};
		void ExpectBoundedSpline(TestContext& context, const VirtualGeometrySplineCurveBuildResult& result, unsigned maximumSegments) {
			if (!result.IsComplete()) {
				context.Expect(result.fallbackRequired && !result.diagnostic.empty() && result.vertices.empty() && result.lineIndices.empty(), "A bounded rejection must explain failure and expose no partial candidate arrays.");
				return;
			}
			context.Expect(result.vertices.size() <= static_cast<std::size_t>(maximumSegments) + 1U
				&& result.lineIndices.size() <= static_cast<std::size_t>(maximumSegments) * 2U, "Generated spline output must obey the caller's existing segment budget.");
			for (const auto& vertex : result.vertices)
				for (const float value : vertex.position) context.Expect(std::isfinite(value), "Accepted spline output must contain finite coordinates.");
			for (const auto index : result.lineIndices) context.Expect(index < result.vertices.size(), "Every generated index must address actual candidate storage.");
		}
		void AddNumericRegions(TestContext& context, WorldRegionDirectory& directory) {
			for (const std::int64_t key : {0LL, 16LL}) {
				WorldRegionDirectoryEntry entry;
				entry.key = {key, 0, 0};
				entry.artifactPath = "Regions/NumericBounds.teregion";
				entry.contentHash.assign(64U, 'a');
				entry.revision = 1U;
				context.Expect(directory.Upsert(entry), "The real sparse directory must accept the bounded fixture keys.");
			}
		}
	}
	void RegisterPlanningNumericBoundsTests(TestRegistry& registry) {
		registry.Add("Engine.PlanningNumericBounds.SplineRepresentableEstimateClampsToDeclaredBudget", [](TestContext& context) {
			const auto controls = NumericControls(0.0F, 1024.0F);
			VirtualGeometrySplineCurveBuildRequest request;
			request.controlPoints = controls;
			request.maximumChordError = 1.0F / 1024.0F;
			request.maximumSegmentsPerSpan = 4U;
			request.maximumGeneratedSegments = 4U;
			request.sourceGeneration = 17U;
			NumericObservation observation;
			const auto observer = observation.Spline();
			const auto first = BuildVirtualGeometrySplineCurve(request, &observer);
			const auto second = BuildVirtualGeometrySplineCurve(request, &observer);
			context.Expect(observation.splineConversions > 0U, "The actual production builder must invoke its preconversion observation seam.");
			context.Expect(first.IsComplete() && first.vertices.size() == 5U && first.lineIndices.size() == 8U, "A representable estimate must clamp before generating only the declared four segments.");
			ExpectBoundedSpline(context, first, 4U);
			context.Expect(first.generationIdentity == second.generationIdentity && first.lineIndices == second.lineIndices, "Repeated actual builds must retain deterministic candidate identity and topology.");
		});
		registry.Add("Engine.PlanningNumericBounds.SplineTotalBudgetRejectsAtomically", [](TestContext& context) {
			const std::array<VirtualGeometryCurveControlPoint, 3> controls{{{{0, 0, 0}, 1, 1, 11}, {{8, 0, 0}, 1, 1, 13}, {{16, 0, 0}, 1, 1, 17}}};
			VirtualGeometrySplineCurveBuildRequest request;
			request.controlPoints = controls;
			request.maximumSegmentsPerSpan = 4U;
			request.maximumGeneratedSegments = 7U;
			request.sourceGeneration = 19U;
			const auto rejected = BuildVirtualGeometrySplineCurve(request);
			context.Expect(!rejected.IsComplete() && rejected.diagnostic == "spline-curve-segment-budget-exceeded" && rejected.generationIdentity == 0U
				&& rejected.vertices.empty() && rejected.lineIndices.empty(), "The existing aggregate budget must reject before candidate allocation/publication.");
			request.maximumGeneratedSegments = 8U;
			const auto accepted = BuildVirtualGeometrySplineCurve(request);
			context.Expect(accepted.IsComplete() && accepted.vertices.size() == 9U && accepted.lineIndices.size() == 16U, "The exact adjacent aggregate budget must remain supported.");
		});
		registry.Add("Engine.PlanningNumericBounds.SplineTinyChordStopsBeforeUnsafeNarrowing", [](TestContext& context) {
			const auto controls = NumericControls(0.0F, 1.0F);
			VirtualGeometrySplineCurveBuildRequest request;
			request.controlPoints = controls;
			request.maximumSegmentsPerSpan = request.maximumGeneratedSegments = 4U;
			request.sourceGeneration = 23U;
			NumericObservation observation;
			const auto observer = observation.Spline();
			(void)BuildVirtualGeometrySplineCurve(request, &observer);
			context.Expect(observation.splineConversions > 0U, "Verify the actual observer on safe input before attempting the guarded extreme request.");
			request.maximumChordError = (std::numeric_limits<float>::min)(); // Tiny normal value avoids denormal-mode dependence.
			// On old code the observer raises a safe regression failure BEFORE UB.
			ExpectBoundedSpline(context, BuildVirtualGeometrySplineCurve(request, &observer), 4U);
		});
		registry.Add("Engine.PlanningNumericBounds.SplineFiniteHugeEndpointsStayBounded", [](TestContext& context) {
			const auto controls = NumericControls(-(std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)());
			VirtualGeometrySplineCurveBuildRequest request;
			request.controlPoints = controls;
			request.maximumSegmentsPerSpan = request.maximumGeneratedSegments = 4U;
			request.sourceGeneration = 29U;
			NumericObservation observation;
			const auto observer = observation.Spline();
			const auto safeControls = NumericControls(0.0F, 1.0F);
			request.controlPoints = safeControls;
			(void)BuildVirtualGeometrySplineCurve(request, &observer);
			context.Expect(observation.splineConversions > 0U, "Verify the actual observer on safe input before attempting finite huge endpoints.");
			request.controlPoints = controls;
			ExpectBoundedSpline(context, BuildVirtualGeometrySplineCurve(request, &observer), 4U);
		});
		registry.Add("Engine.PlanningNumericBounds.SplineZeroLengthAndInvalidControls", [](TestContext& context) {
			const auto controls = NumericControls(0.0F, 0.0F);
			VirtualGeometrySplineCurveBuildRequest request;
			request.controlPoints = controls;
			request.sourceGeneration = 31U;
			const auto zero = BuildVirtualGeometrySplineCurve(request);
			context.Expect(zero.IsComplete() && zero.zeroLengthSpanCount == 1U && zero.vertices.size() == 2U, "The supported zero-length span must remain bounded and deterministic.");
			request.maximumChordError = (std::numeric_limits<float>::quiet_NaN)();
			const auto rejected = BuildVirtualGeometrySplineCurve(request);
			context.Expect(rejected.fallbackRequired && rejected.vertices.empty() && !rejected.diagnostic.empty(), "An invalid chord error must reject through the actual entry point.");
		});
		registry.Add("Engine.PlanningNumericBounds.StreamingSparseInterestWorkUsesExistingKeys", [](TestContext& context) {
			WorldRegionDirectory directory;
			AddNumericRegions(context, directory);
			ObserverInterest interest;
			interest.observerId = "review-sparse-radius";
			interest.radiusMetres = 17.0 * kWorldRegionEdgeMetres;
			NumericObservation observation;
			const auto observer = observation.Streaming();
			const auto plan = BuildDeterministicRegionInterestPlan({interest}, directory, {}, {2U, 2U}, 37U, &observer);
			context.Expect(observation.directoryLookups > 0U, "The actual planner must report its directory examinations through the observation seam.");
			// Old code performs exactly35^3 bounded lookups. This is a counted-work
			// failure, not a timing test, giant cubic scan, or fabricated bug model.
			context.Expect(observation.directoryLookups <= directory.Size(), "Sparse interest work must follow actual directory membership rather than examining every empty grid key.");
			context.Expect(plan.requested.size() == 2U && plan.requested.front() == WorldRegionKey{0, 0, 0}
				&& plan.requested.back() == WorldRegionKey{16, 0, 0}, "Bounded work must preserve the legitimate distant sparse key rather than silently clamp radius to an arbitrary product limit.");
		});
		registry.Add("Engine.PlanningNumericBounds.StreamingExtremeFiniteRadiusStopsBeforeUnsafeNarrowing", [](TestContext& context) {
			WorldRegionDirectory directory;
			AddNumericRegions(context, directory);
			ObserverInterest interest;
			interest.observerId = "review-extreme-radius";
			NumericObservation observation;
			const auto observer = observation.Streaming();
			(void)BuildDeterministicRegionInterestPlan({interest}, directory, {}, {2U, 2U}, 41U, &observer);
			context.Expect(observation.radiusConversions > 0U, "Verify the real preconversion observer on radius zero before the guarded extreme request.");
			observation.directoryLookups = 0U;
			interest.radiusMetres = 1.0e300;
			const auto plan = BuildDeterministicRegionInterestPlan({interest}, directory, {}, {2U, 2U}, 41U, &observer);
			context.Expect(plan.requested.empty(), "An unrepresentable finite radius must produce no expanded region requests.");
			context.Expect(observation.directoryLookups == 0U, "Rejected numeric input must not enter directory expansion.");
			context.Expect(plan.diagnostics.rejectedCount == 1U && plan.diagnostics.recentRejections.size() == 1U
				&& plan.diagnostics.recentRejections.front().interestIndex == 0U
				&& plan.diagnostics.recentRejections.front().reason == RegionInterestPlanningRejectionReason::RadiusUnrepresentable
				&& plan.diagnostics.directoryExaminations == 0U,
				"An unsafe finite radius must produce the copied typed rejection for its actual input ordinal, without examining the directory.");
		});
		registry.Add("Engine.PlanningNumericBounds.StreamingRadiusZeroUsesOneLookupInLargeDirectory", [](TestContext& context) {
			WorldRegionDirectory directory;
			// Public directory records are real planner input. Hundreds of unrelated
			// entries must not turn an ordinary radius-zero query into a full scan.
			for (std::int64_t index = 0; index < 512; ++index) {
				WorldRegionDirectoryEntry entry;
				entry.key = {index, 0, 0};
				entry.artifactPath = "Regions/RadiusZero.teregion";
				entry.contentHash.assign(64U, 'a');
				entry.revision = 1U;
				context.Expect(directory.Upsert(entry), "The public directory must accept each distinct radius-zero fixture key.");
			}
			context.Expect(directory.Size() == 512U, "The ordinary-query regression requires the complete large directory input.");
			ObserverInterest interest;
			interest.observerId = "review-radius-zero";
			NumericObservation observation;
			const auto observer = observation.Streaming();
			const auto plan = BuildDeterministicRegionInterestPlan({interest}, directory, {}, {2U, 2U}, 47U, &observer);
			context.Expect(plan.requested == std::vector<WorldRegionKey>{{0, 0, 0}}, "Radius zero must select only the actual center region.");
			context.Expect(observation.directoryLookups == 1U && plan.diagnostics.directoryExaminations == 1U
				&& plan.diagnostics.cubeInterests == 1U && plan.diagnostics.sparseInterests == 0U
				&& plan.diagnostics.rejectedCount == 0U,
				"Normal radius-zero planning must perform exactly one lookup, independently of unrelated directory size.");
		});
		registry.Add("Engine.PlanningNumericBounds.StreamingExtremeSignedKeysPreserveSparseDistance", [](TestContext& context) {
			const WorldRegionKey center{(std::numeric_limits<std::int64_t>::max)() - 7, 0, 0};
			const WorldRegionKey nearKey{(std::numeric_limits<std::int64_t>::max)() - 8, 0, 0};
			const WorldRegionKey farKey{(std::numeric_limits<std::int64_t>::min)(), 0, 0};
			ObserverInterest interest;
			interest.observerId = "review-signed-key-distance";
			interest.position = WorldRegionCanonicalOrigin(center);
			WorldRegionKey roundTrip{};
			// MAX-7 has a representable canonical cell multiplication. Check the
			// actual conversion first so a rejected position cannot fake an empty plan.
			context.Expect(WorldPositionToRegionKey(interest.position, roundTrip) == WorldCoordinateError::None && roundTrip == center,
				"The extreme-key fixture must enter the actual planner with its intended representable canonical center.");
			WorldRegionDirectory directory;
			for (const WorldRegionKey key : {nearKey, farKey}) {
				WorldRegionDirectoryEntry entry;
				entry.key = key;
				entry.artifactPath = "Regions/SignedDistance.teregion";
				entry.contentHash.assign(64U, 'a');
				entry.revision = 1U;
				context.Expect(directory.Upsert(entry), "The public sparse directory must retain both signed-boundary keys.");
			}
			// The adjacent representable double below2^63 is safe to narrow. The
			// opposite signed extreme is farther away, despite signed subtraction
			// wrapping to a deceptively small distance in an unchecked implementation.
			interest.radiusMetres = std::nextafter(0x1p63, 0.0) * kWorldRegionEdgeMetres;
			NumericObservation observation;
			const auto observer = observation.Streaming();
			const auto plan = BuildDeterministicRegionInterestPlan({interest}, directory, {}, {2U, 2U}, 53U, &observer);
			context.Expect(plan.requested == std::vector<WorldRegionKey>{nearKey}, "Exact sparse distance must retain the nearby key and exclude the far signed extreme.");
			context.Expect(observation.radiusConversions == 1U && observation.directoryLookups == 2U
				&& plan.diagnostics.directoryExaminations == 2U && plan.diagnostics.sparseInterests == 1U
				&& plan.diagnostics.cubeInterests == 0U && plan.diagnostics.rejectedCount == 0U,
				"A valid huge radius must inspect only actual sparse entries without overflow, cubic expansion, or silent rejection.");
		});
		registry.Add("Engine.PlanningNumericBounds.StreamingRejectionDiagnosticsRetainLatest64Ordinals", [](TestContext& context) {
			WorldRegionDirectory directory;
			AddNumericRegions(context, directory);
			constexpr std::size_t interestCount = 97U;
			const std::array reasons{RegionInterestPlanningRejectionReason::InvalidObserver,
				RegionInterestPlanningRejectionReason::InvalidRadius, RegionInterestPlanningRejectionReason::InvalidPosition,
				RegionInterestPlanningRejectionReason::RadiusUnrepresentable};
			std::vector<ObserverInterest> interests;
			interests.reserve(interestCount);
			for (std::size_t index = 0U; index < interestCount; ++index) {
				ObserverInterest interest;
				interest.observerId = "review-invalid-" + std::to_string(index);
				switch (index % reasons.size()) {
				case 0U: interest.observerId.clear(); break;
				case 1U: interest.radiusMetres = -1.0; break;
				case 2U: interest.position.offsetMetres.x = (std::numeric_limits<double>::quiet_NaN)(); break;
				default: interest.radiusMetres = 1.0e300; break;
				}
				interests.push_back(std::move(interest));
			}
			NumericObservation observation;
			const auto observer = observation.Streaming();
			const auto plan = BuildDeterministicRegionInterestPlan(interests, directory, {}, {2U, 2U}, 59U, &observer);
			context.Expect(plan.requested.empty() && plan.diagnostics.rejectedCount == interestCount
				&& plan.diagnostics.recentRejections.size() == 64U,
				"Every invalid interest must count while copied rejection storage remains capped at64.");
			// Each known-invalid input has an independent ordinal/reason oracle;
			// retained diagnostics must describe the latest64, not repeat the head.
			for (std::size_t index = 0U; index < 64U; ++index) {
				const std::size_t originalOrdinal = interestCount - 64U + index;
				context.Expect(plan.diagnostics.recentRejections[index].interestIndex == originalOrdinal
					&& plan.diagnostics.recentRejections[index].reason == reasons[originalOrdinal % reasons.size()],
					"Bounded copied diagnostics must preserve each retained input's exact ordinal and typed rejection reason.");
			}
			context.Expect(observation.radiusConversions == 0U && observation.directoryLookups == 0U
				&& plan.diagnostics.directoryExaminations == 0U && plan.diagnostics.cubeInterests == 0U
				&& plan.diagnostics.sparseInterests == 0U,
				"Rejected interests must not reach narrowing or any directory expansion strategy.");
		});
		registry.Add("Engine.PlanningNumericBounds.StreamingPriorityAndInvalidRadiusControls", [](TestContext& context) {
			WorldRegionDirectory directory;
			AddNumericRegions(context, directory);
			ObserverInterest ordinary;
			ordinary.observerId = "ordinary";
			ObserverInterest critical = ordinary;
			critical.observerId = "critical";
			critical.priority = ObserverInterestPriority::Critical;
			critical.position = WorldRegionCanonicalOrigin({16, 0, 0});
			ObserverInterest invalid = ordinary;
			invalid.observerId = "invalid";
			invalid.radiusMetres = (std::numeric_limits<double>::quiet_NaN)();
			const auto plan = BuildDeterministicRegionInterestPlan({ordinary, invalid, critical}, directory, {}, {1U, 1U}, 43U);
			context.Expect(plan.generation == 43U && plan.requested == std::vector<WorldRegionKey>{{16, 0, 0}}, "Priority and resident budget must retain their actual stable-key arbitration while invalid input is rejected.");
		});
	}
}
