#include "TestHarness.h"

#include "Physics/PhysicsSettings.h"
#include "Physics/PhysicsDiagnostics.h"

#include <array>
#include <cmath>
#include <limits>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;

	void RegisterPhysicsSettingsTests(TestRegistry& registry) {
		registry.Add("Engine.Physics.Settings.DefaultsUseSIUnitsAndBoundedStepping", [](TestContext& context) {
			const PhysicsSettings settings = PhysicsSettings::Defaults();
			context.Expect(settings.Validate().accepted, "Engine defaults should pass settings validation.");
			context.ExpectEqual(32U, settings.layers.size(), "Defaults must reserve exactly 32 stable layer slots.");
			context.ExpectEqual(9.81F, std::abs(settings.gravity.y), "Default gravity should be 9.81 m/s^2 in SI units.");
			context.Expect(settings.fixedTimeStep > 0.0F && settings.fixedTimeStep <= 1.0F / 30.0F,
				"The default fixed timestep must be positive and bounded for stable simulation.");
			context.Expect(settings.maxSubsteps > 0U, "A world must permit at least one fixed substep.");
			context.Expect(settings.maxAccumulatedTime >= settings.fixedTimeStep,
				"Accumulator clamp must hold at least one configured fixed step.");
		});

		registry.Add("Engine.Physics.Settings.LayersHaveStableIDsAndValidateMasksAndNames", [](TestContext& context) {
			PhysicsSettings settings = PhysicsSettings::Defaults();
			std::array<bool, 32> seen{};
			for(std::size_t index = 0; index < settings.layers.size(); ++index) {
				const CollisionLayer& layer = settings.layers[index];
				context.Expect(layer.id < 32U, "Collision layer IDs must fit the 32-layer mask.");
				context.ExpectEqual(index, static_cast<std::size_t>(layer.id),
					"Default layer IDs must remain stable when names or ordering are edited.");
				context.Expect(!seen[layer.id], "Collision layer IDs must be unique and stable.");
				seen[layer.id] = true;
				context.Expect(!layer.name.empty(), "Collision layer names must be nonempty.");
				context.Expect(PhysicsSettings::IsValidLayerMask(layer.collisionMask),
					"Every default collision mask must fit the fixed 32-bit layer contract.");
			}

			settings.layers[1].name = settings.layers[0].name;
			context.Expect(!settings.Validate().accepted, "Duplicate layer names must be rejected.");
			context.Expect(!PhysicsSettings::IsValidLayerMask(1ULL << 32U),
				"Masks wider than 32 bits must be rejected before narrowing.");
		});

		registry.Add("Engine.Physics.Settings.CalibratedQueryAndHistoryBounds", [](TestContext& context) {
			const PhysicsSettings settings = PhysicsSettings::Defaults();
			context.ExpectEqual(100000.0F, settings.maxCastDistance, "Cast distance must use the calibrated 100,000 m limit.");
			context.ExpectEqual(1000.0F, settings.maxShapeExtent, "Shape extent must use the calibrated 1,000 m limit.");
			context.ExpectEqual(64U, settings.maxHitsPerQuery, "Queries must cap results at 64 hits.");
			context.ExpectEqual(10000U, settings.maxRequestsPerBatch, "Batches must accept the calibrated 10,000-request limit.");
			context.ExpectEqual(2U, settings.retainedResultTicks, "Results must be retained for exactly two ticks.");
		});

		registry.Add("Engine.Physics.Settings.RejectNonfiniteNegativeAndOverBudgetValues", [](TestContext& context) {
			PhysicsSettings settings = PhysicsSettings::Defaults();
			settings.gravity.x = std::numeric_limits<float>::quiet_NaN();
			context.Expect(!settings.Validate().accepted, "Nonfinite gravity must be rejected.");
			settings = PhysicsSettings::Defaults();
			settings.fixedTimeStep = -0.01F;
			context.Expect(!settings.Validate().accepted, "Negative fixed timesteps must be rejected.");
			settings = PhysicsSettings::Defaults();
			settings.maxCastDistance = 100001.0F;
			context.Expect(!settings.Validate().accepted, "Over-budget cast distance must be rejected.");
			settings = PhysicsSettings::Defaults();
			settings.maxRequestsPerBatch = 10001U;
			context.Expect(!settings.Validate().accepted, "Over-budget query batches must be rejected.");
		});

		registry.Add("Engine.Physics.Settings.SolverQualityUsesProviderSafeBounds", [](TestContext& context) {
			PhysicsSettings settings = PhysicsSettings::Defaults();
			context.ExpectEqual(10U, settings.velocityIterations,
				"Existing projects must retain Jolt's ten-step velocity default.");
			context.ExpectEqual(2U, settings.positionIterations,
				"Existing projects must retain Jolt's two-step position default.");
			settings.velocityIterations = 2U;
			settings.positionIterations = 255U;
			context.Expect(settings.Validate().accepted,
				"The documented inclusive solver bounds must be accepted.");
			settings.velocityIterations = 1U;
			context.Expect(settings.Validate().code == PhysicsSettingsValidationCode::InvalidVelocityIterations,
				"Velocity iterations below Jolt's friction-safe minimum must be rejected explicitly.");
			settings = PhysicsSettings::Defaults();
			settings.positionIterations = 256U;
			context.Expect(settings.Validate().code == PhysicsSettingsValidationCode::InvalidPositionIterations,
				"Position iterations outside the provider's eight-bit range must be rejected explicitly.");
		});

		registry.Add("Engine.Physics.Settings.IncrementalPipelineDefaultsAndBounds", [](TestContext& context) {
			PhysicsSettings settings = PhysicsSettings::Defaults();
			context.ExpectEqual(1024U, settings.transformPublicationChunkSize,
				"Transform preparation should default to calibrated 1,024-record chunks.");
			context.ExpectEqual(1024U, settings.providerExtractionBatchSize,
				"Provider extraction should default to calibrated 1,024-record ranges.");
			context.ExpectEqual(512U, settings.parallelPreparationThreshold,
				"Small preparation ranges should remain inline below the calibrated threshold.");
			context.ExpectEqual(30U, settings.aggregateDiagnosticsIntervalFrames,
				"Nonessential aggregate diagnostics should default to a bounded 30-frame rate.");
			context.Expect(!settings.continuousCollisionEnabled,
				"Continuous collision must remain an explicit project opt-in.");

			settings.transformPublicationChunkSize = 63U;
			context.Expect(!settings.Validate().accepted,
				"Publication chunks below the supported reusable-storage range must be rejected.");
			settings = PhysicsSettings::Defaults();
			settings.parallelExtractionThreshold = 0U;
			context.Expect(!settings.Validate().accepted,
				"A zero parallel threshold would force dispatch for empty work and must be rejected.");
			settings = PhysicsSettings::Defaults();
			settings.aggregateDiagnosticsIntervalFrames = 601U;
			context.Expect(!settings.Validate().accepted,
				"Aggregate diagnostics intervals above the bounded history must be rejected.");
		});

		registry.Add("Engine.Physics.Settings.DiagnosticsCadenceNeverHidesCurrentFailure", [](TestContext& context) {
			context.Expect(ShouldRefreshPhysicsAggregateDiagnostics(30U, 30U, 0U, false),
				"The first bounded aggregate sample must publish immediately.");
			context.Expect(!ShouldRefreshPhysicsAggregateDiagnostics(31U, 30U, 30U, false),
				"Nonessential aggregate work should stay rate limited between configured intervals.");
			context.Expect(ShouldRefreshPhysicsAggregateDiagnostics(31U, 30U, 30U, true),
				"A current physics failure must bypass the aggregate rate limit.");
			context.Expect(ShouldRefreshPhysicsAggregateDiagnostics(60U, 30U, 30U, false),
				"The configured interval must refresh deterministic aggregate diagnostics.");
		});

		registry.Add("Engine.Physics.Settings.CollisionPairsAndContinuousMotionRequireMutualOptIn", [](TestContext& context) {
			context.Expect(PhysicsSettings::LayersCanCollide(1U, 1U << 2U, 2U, 1U << 1U),
				"A collision pair should pass only when both stable layer masks opt in.");
			context.Expect(!PhysicsSettings::LayersCanCollide(1U, 1U << 2U, 2U, 0U),
				"One-sided collision category policy must reject the provider contact.");
			context.Expect(PhysicsSettings::ResolveMotionQuality(false, PhysicsMotionQuality::LinearCast)
				== PhysicsMotionQuality::Discrete,
				"Per-body continuous collision must stay disabled until the project opts in.");
			context.Expect(PhysicsSettings::ResolveMotionQuality(true, PhysicsMotionQuality::LinearCast)
				== PhysicsMotionQuality::LinearCast,
				"Project opt-in should preserve the body's selective LinearCast request.");
		});
	}
}
