// AnimationExecutionModeTests.cpp - Pure Auto/Force resolver policy contracts.

#include "TestHarness.h"

#include "Animation/Runtime/AnimationExecutionMode.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Animation;

	namespace {
		AnimationExecutionModeRequest MakeEligibleRequest(AnimationFixedTick fixedTick = 0)
		{
			AnimationExecutionModeRequest request;
			request.fixedTick = fixedTick;
			request.requestedPolicy = AnimationExecutionPolicy::Auto;
			return request;
		}

		void ExpectModeAndReason(
			TestContext& context,
			const AnimationExecutionModeDecision& decision,
			AnimationEffectiveExecutionMode expectedMode,
			AnimationExecutionModeReason expectedReason,
			const char* message)
		{
			context.Expect(decision.effectiveMode == expectedMode, message);
			context.Expect(decision.reason == expectedReason,
				"The effective mode must expose the stable reason selected by the precedence table.");
		}

		AnimationExecutionModeState ResolveInitialState(const AnimationExecutionModeRequest& request)
		{
			return ResolveAnimationExecutionMode(request, {}).nextState;
		}

		AnimationModeContinuityToken MakeContinuityToken(
			AnimationFixedTick boundaryTick,
			std::uint64_t identityBase)
		{
			// Give every continuity field a distinct value so an omitted copy cannot
			// accidentally compare equal to the default-initialized destination.
			AnimationModeContinuityToken token;
			token.transitionBoundaryTick = boundaryTick;
			token.stateTimeTick = boundaryTick + 1;
			token.normalizedControllerTime = 0.375F;
			token.controllerState = identityBase + 1U;
			token.parameterSnapshot = identityBase + 2U;
			token.transition = identityBase + 3U;
			token.synchronizationState = identityBase + 4U;
			token.synchronizationPhaseTick = boundaryTick + 2;
			token.eventWatermark = identityBase + 5U;
			token.rootMotion = identityBase + 6U;
			token.motionMatch = identityBase + 7U;
			token.ragdollBlend = identityBase + 8U;
			token.pose = identityBase + 9U;
			return token;
		}
	}

	void RegisterAnimationExecutionModeTests(TestRegistry& registry)
	{
		registry.Add("Animation.ExecutionMode.EffectiveModesAndReasonLabelsAreStable", [](TestContext& context) {
			context.Expect(
				static_cast<std::uint8_t>(AnimationEffectiveExecutionMode::GpuVisual) == 0U
				&& static_cast<std::uint8_t>(AnimationEffectiveExecutionMode::HybridAuthoritative) == 1U
				&& static_cast<std::uint8_t>(AnimationEffectiveExecutionMode::CpuDeterministicFallback) == 2U,
				"The three effective modes need stable append-only numeric identities for diagnostics and future ABI packets.");
			context.Expect(ToString(AnimationEffectiveExecutionMode::GpuVisual) == "GpuVisual"
				&& ToString(AnimationEffectiveExecutionMode::HybridAuthoritative) == "HybridAuthoritative"
				&& ToString(AnimationEffectiveExecutionMode::CpuDeterministicFallback) == "CpuDeterministicFallback",
				"Effective-mode labels must remain stable and allocation-free.");
			context.Expect(ToString(AnimationExecutionModeReason::DeterministicRequired) == "DeterministicRequired"
				&& ToString(AnimationExecutionModeReason::ForceGpuUnsafe) == "ForceGpuUnsafe"
				&& ToString(AnimationExecutionModeReason::PromotionHysteresis) == "PromotionHysteresis",
				"Selection-reason labels are diagnostic contracts rather than localized display text.");
		});

		registry.Add("Animation.ExecutionMode.RootMotionPolicyRequiresAnAuthoredOrExplicitSource",
			[](TestContext& context) {
				context.Expect(!RequiresCpuRootMotionAuthority(
					AnimatorRootMotionPolicy::ExtractOnly, false, false, false, false),
					"An in-place clip must stay eligible for GPU Visual even when its authored policy would extract a real delta.");
				context.Expect(RequiresCpuRootMotionAuthority(
					AnimatorRootMotionPolicy::ApplyToObject, true, false, false, false)
					&& RequiresCpuRootMotionAuthority(
						AnimatorRootMotionPolicy::ExtractOnly, false, true, false, false)
					&& RequiresCpuRootMotionAuthority(
						AnimatorRootMotionPolicy::ExtractOnly, false, false, true, false),
					"Current, interrupted, and destination transition root tracks must retain CPU authority.");
				context.Expect(RequiresCpuRootMotionAuthority(
					AnimatorRootMotionPolicy::Ignore, false, false, false, true)
					&& !RequiresCpuRootMotionAuthority(
						AnimatorRootMotionPolicy::Ignore, true, true, true, false),
					"Explicit consumers override Ignore, while Ignore alone suppresses automatic extraction from authored tracks.");
			});

		registry.Add("Animation.ExecutionMode.ConsumerClosureSortsDeduplicatesAndIncludesAncestors", [](TestContext& context) {
			// Joint 0 is the root. Joint 3 depends on 2 -> 1 -> 0, while joint 5
			// depends on 4 -> 0. Repeated and unordered requests must still produce
			// one stable ascending closure for range-disjoint Hybrid evaluation.
			const std::array<std::int32_t, 6U> parents{-1, 0, 1, 2, 0, 4};
			const std::array<std::uint32_t, 4U> requested{5U, 3U, 5U, 1U};
			AnimationExecutionModeRequest request = MakeEligibleRequest();
			request.consumers.jointParents = parents;
			request.consumers.requestedJoints = requested;
			request.consumers.events = true;
			request.consumers.rootMotion = true;

			const AnimationExecutionModeDecision decision = ResolveAnimationExecutionMode(request, {});
			const std::vector<std::uint32_t> expected{0U, 1U, 2U, 3U, 4U, 5U};
			context.Expect(decision.consumerClosure.status == AnimationStatus::Ok,
				"A valid parent-before-child skeleton and bounded requests should resolve successfully.");
			context.Expect(decision.consumerClosure.joints == expected,
				"Hybrid joint closure must be sorted, deduplicated, and include every required ancestor.");
			context.Expect(decision.consumerClosure.events && decision.consumerClosure.rootMotion,
				"Non-joint authoritative outputs must remain explicit in the resolved subset.");
			context.Expect(decision.consumerClosure.HasAuthoritativeSubset(),
				"Joint and event/root requirements form a non-empty bounded authoritative subset.");
			ExpectModeAndReason(context, decision,
				AnimationEffectiveExecutionMode::HybridAuthoritative,
				AnimationExecutionModeReason::AuthoritativeSubsetRequired,
				"Auto must choose Hybrid when the consumer closure is non-empty.");
		});

		registry.Add("Animation.ExecutionMode.ConsumerClosurePreservesEveryAuthoritativeDeclaration", [](TestContext& context) {
			AnimationExecutionModeRequest request = MakeEligibleRequest();
			request.consumers.curves = true;
			request.consumers.morphs = true;
			request.consumers.events = true;
			request.consumers.rootMotion = true;
			request.consumers.contacts = true;
			request.consumers.proceduralResults = true;
			request.consumers.physics = true;
			request.consumers.attachments = true;
			request.consumers.networking = true;
			request.consumers.rollback = true;
			request.consumers.replay = true;
			request.consumers.recording = true;
			request.consumers.automation = true;
			request.consumers.diagnostics = true;
			request.consumers.completePose = true;

			const AnimationExecutionModeDecision decision = ResolveAnimationExecutionMode(request, {});
			const AnimationConsumerClosure& closure = decision.consumerClosure;
			context.Expect(closure.curves && closure.morphs && closure.events && closure.rootMotion
				&& closure.contacts && closure.proceduralResults && closure.physics && closure.attachments
				&& closure.networking && closure.rollback && closure.replay && closure.recording
				&& closure.automation && closure.diagnostics && closure.completePose,
				"The resolver must not drop any declared cross-system authority while computing the effective mode.");
			context.Expect(closure.HasAuthoritativeSubset() && closure.fingerprint != 0U,
				"The complete typed declaration must produce a non-empty, stable consumer identity.");
			ExpectModeAndReason(context, decision,
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::DeterministicRequired,
				"Replay authority must retain the behaviorally equivalent deterministic CPU path.");
		});

		registry.Add("Animation.ExecutionMode.EveryAuthoritativeConsumerKeepsForceGpuSafe", [](TestContext& context) {
			using ConsumerFlag = bool AnimationConsumerRequirements::*;
			struct FlagCase {
				ConsumerFlag flag = nullptr;
				AnimationExecutionModeReason expectedReason = AnimationExecutionModeReason::ForceGpuUnsafe;
			};

			// Test every declaration independently. Combining the flags would let an
			// omitted HasAuthoritativeSubset branch hide behind another true flag.
			const std::array<FlagCase, 15U> cases{{
				{&AnimationConsumerRequirements::curves, AnimationExecutionModeReason::ForceGpuUnsafe},
				{&AnimationConsumerRequirements::morphs, AnimationExecutionModeReason::ForceGpuUnsafe},
				{&AnimationConsumerRequirements::events, AnimationExecutionModeReason::ForceGpuUnsafe},
				{&AnimationConsumerRequirements::rootMotion, AnimationExecutionModeReason::ForceGpuUnsafe},
				{&AnimationConsumerRequirements::contacts, AnimationExecutionModeReason::ForceGpuUnsafe},
				{&AnimationConsumerRequirements::proceduralResults, AnimationExecutionModeReason::ForceGpuUnsafe},
				{&AnimationConsumerRequirements::physics, AnimationExecutionModeReason::ForceGpuUnsafe},
				{&AnimationConsumerRequirements::attachments, AnimationExecutionModeReason::ForceGpuUnsafe},
				{&AnimationConsumerRequirements::networking, AnimationExecutionModeReason::ForceGpuUnsafe},
				{&AnimationConsumerRequirements::rollback, AnimationExecutionModeReason::RollbackRequired},
				{&AnimationConsumerRequirements::replay, AnimationExecutionModeReason::DeterministicRequired},
				{&AnimationConsumerRequirements::recording, AnimationExecutionModeReason::ForceGpuUnsafe},
				{&AnimationConsumerRequirements::automation, AnimationExecutionModeReason::ForceGpuUnsafe},
				{&AnimationConsumerRequirements::diagnostics, AnimationExecutionModeReason::ForceGpuUnsafe},
				{&AnimationConsumerRequirements::completePose, AnimationExecutionModeReason::CompletePoseRequired}
			}};

			for(const FlagCase& flagCase : cases) {
				AnimationExecutionModeRequest request = MakeEligibleRequest();
				request.requestedPolicy = AnimationExecutionPolicy::ForceGpuVisual;
				request.consumers.*(flagCase.flag) = true;
				const AnimationExecutionModeDecision decision = ResolveAnimationExecutionMode(request, {});
				context.Expect(decision.effectiveMode == AnimationEffectiveExecutionMode::CpuDeterministicFallback
					&& decision.reason == flagCase.expectedReason,
					"Every individual authoritative declaration must make Force GPU fall back with its stable safety reason.");
			}

			const std::array<std::int32_t, 2U> parents{-1, 0};
			const std::array<std::uint32_t, 1U> requested{1U};
			AnimationExecutionModeRequest jointRequest = MakeEligibleRequest();
			jointRequest.requestedPolicy = AnimationExecutionPolicy::ForceGpuVisual;
			jointRequest.consumers.jointParents = parents;
			jointRequest.consumers.requestedJoints = requested;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(jointRequest, {}),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::ForceGpuUnsafe,
				"A requested joint and its ancestor closure must also make Force GPU unsafe.");
		});

		registry.Add("Animation.ExecutionMode.PrecedenceIsDeterministicAndForcePoliciesRemainSafe", [](TestContext& context) {
			AnimationExecutionModeRequest request = MakeEligibleRequest();
			request.deterministicRequired = true;
			request.rollbackOrResimulation = true;
			request.headless = true;
			request.validationReference = true;
			request.gpu.rendererAvailable = false;
			request.gpu.animationCapabilityAvailable = false;
			request.gpu.resourcesResident = false;
			request.gpu.evaluationHealthy = false;
			request.gpu.requiredBudget = 2.0;
			request.gpu.availableBudget = 1.0;
			request.requestedPolicy = AnimationExecutionPolicy::ForceCpuDeterministicFallback;
			request.consumers.completePose = true;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::DeterministicRequired,
				"Deterministic execution must have first precedence over every later fallback cause.");

			request.deterministicRequired = false;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::RollbackRequired,
				"Rollback/resimulation must outrank headless, validation, and renderer failures.");
			request.rollbackOrResimulation = false;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::HeadlessRequired,
				"Headless execution must outrank validation and GPU availability.");
			request.headless = false;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::ValidationRequired,
				"Reference validation must stay independent of renderer state.");
			request.validationReference = false;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::RendererUnavailable,
				"Renderer unavailability must outrank individual GPU feature and residency failures.");
			request.gpu.rendererAvailable = true;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::GpuCapabilityUnavailable,
				"Missing GPU animation capability must outrank residency and execution failure.");
			request.gpu.animationCapabilityAvailable = true;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::GpuResourcesUnavailable,
				"Incomplete compatible residency must outrank a latched GPU evaluation failure.");
			request.gpu.resourcesResident = true;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::GpuEvaluationFailure,
				"A latched GPU evaluation failure must downgrade immediately before considering budget or policy.");
			request.gpu.evaluationHealthy = true;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::GpuBudgetUnavailable,
				"An insufficient budget must outrank a manual CPU request for stable attribution.");
			request.gpu.availableBudget = 2.0;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::ForceCpuRequested,
				"Force CPU is honored after mandatory runtime fallbacks are attributed.");

			request.requestedPolicy = AnimationExecutionPolicy::Auto;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::CompletePoseRequired,
				"A complete CPU pose consumer cannot be represented by a Hybrid subset.");
			request.consumers.completePose = false;
			request.consumers.events = true;
			request.requestedPolicy = AnimationExecutionPolicy::ForceGpuVisual;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::ForceGpuUnsafe,
				"Force GPU must fall back to CPU rather than discard an authoritative event consumer.");

			request.consumers.events = false;
			request.requestedPolicy = AnimationExecutionPolicy::ForceHybridAuthoritative;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::ForceHybridMissingSubset,
				"Force Hybrid without a declared authoritative subset is unsafe and must fall back explicitly.");
			request.consumers.curves = true;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::HybridAuthoritative,
				AnimationExecutionModeReason::ForceHybridRequested,
				"Force Hybrid becomes legal once a bounded authoritative subset exists.");

			request.requestedPolicy = AnimationExecutionPolicy::Auto;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::HybridAuthoritative,
				AnimationExecutionModeReason::AuthoritativeSubsetRequired,
				"Auto must choose Hybrid for a non-empty curve closure.");
			request.consumers.curves = false;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::GpuVisual,
				AnimationExecutionModeReason::GpuVisualEligible,
				"Auto must choose GPU Visual when no CPU authority or fallback requirement exists.");
			request.requestedPolicy = AnimationExecutionPolicy::ForceGpuVisual;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::GpuVisual,
				AnimationExecutionModeReason::ForceGpuRequested,
				"A safe Force GPU request should remain visible in diagnostics.");
		});

		registry.Add("Animation.ExecutionMode.InvalidAndNonFiniteInputsFailSafeWithinBounds", [](TestContext& context) {
			AnimationExecutionModeRequest request = MakeEligibleRequest();
			request.gpu.requiredBudget = std::numeric_limits<double>::quiet_NaN();
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::GpuBudgetUnavailable,
				"NaN budget input must fail safely to CPU instead of comparing unordered values.");
			request.gpu.requiredBudget = 1.0;
			request.gpu.availableBudget = std::numeric_limits<double>::infinity();
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::GpuBudgetUnavailable,
				"Non-finite available budget must not silently make every GPU request eligible.");
			request.gpu.availableBudget = -1.0;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::GpuBudgetUnavailable,
				"Negative budgets are invalid and must choose the bounded safe path.");

			std::vector<std::int32_t> oversizedParents(kMaximumAnimationExecutionJoints + 1U, -1);
			request = MakeEligibleRequest();
			request.consumers.jointParents = oversizedParents;
			const AnimationExecutionModeDecision oversized = ResolveAnimationExecutionMode(request, {});
			context.Expect(oversized.consumerClosure.status == AnimationStatus::HardLimitExceeded,
				"Consumer input must be rejected before allocation when it exceeds the frozen skeleton bound.");
			ExpectModeAndReason(context, oversized,
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::InvalidConsumerClosure,
				"An oversized consumer closure must fail safely and explicitly.");

			const std::array<std::int32_t, 2U> cyclicParents{1, 0};
			const std::array<std::uint32_t, 1U> cyclicRequest{1U};
			request = MakeEligibleRequest();
			request.consumers.jointParents = cyclicParents;
			request.consumers.requestedJoints = cyclicRequest;
			const AnimationExecutionModeDecision cyclic = ResolveAnimationExecutionMode(request, {});
			context.Expect(cyclic.consumerClosure.status == AnimationStatus::InvalidArgument,
				"Ancestor cycles must be rejected without an unbounded walk.");
			context.Expect(cyclic.consumerClosure.joints.empty(),
				"A rejected closure must publish no partial joint subset.");

			const std::array<std::int32_t, 2U> invalidParents{-1, 7};
			request = MakeEligibleRequest();
			request.consumers.jointParents = invalidParents;
			request.consumers.requestedJoints = cyclicRequest;
			const AnimationExecutionModeDecision invalidParent = ResolveAnimationExecutionMode(request, {});
			context.Expect(invalidParent.consumerClosure.status == AnimationStatus::InvalidArgument
				&& invalidParent.consumerClosure.joints.empty(),
				"Out-of-range ancestors must reject the complete closure without publishing partial work.");

			request = MakeEligibleRequest();
			request.requestedPolicy = static_cast<AnimationExecutionPolicy>(255U);
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::InvalidRequestedPolicy,
				"Unknown durable policy values must fail closed with an explicit diagnostic reason.");

			request = MakeEligibleRequest(-1);
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, {}),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::InvalidRequestedPolicy,
				"A negative fixed-tick boundary is invalid and must never enter GPU evaluation.");
			const AnimationExecutionModeState validPriorState = ResolveInitialState(MakeEligibleRequest(8));
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, validPriorState),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::InvalidRequestedPolicy,
				"An invalid current tick must not be misreported as corruption of an otherwise valid prior state.");

			request = MakeEligibleRequest(8);
			request.stability.promotionHysteresisTicks = -1;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, validPriorState),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::InvalidRequestedPolicy,
				"Negative promotion hysteresis is an invalid current request rather than a usable timer.");
			request.stability.promotionHysteresisTicks = 0;
			request.stability.minimumResidenceTicks = -1;
			ExpectModeAndReason(context, ResolveAnimationExecutionMode(request, validPriorState),
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::InvalidRequestedPolicy,
				"Negative minimum residence must fail safely before mode promotion.");

			request = MakeEligibleRequest(8);
			AnimationExecutionModeState corruptState;
			corruptState.initialized = true;
			corruptState.effectiveMode = static_cast<AnimationEffectiveExecutionMode>(255U);
			corruptState.effectiveReason = AnimationExecutionModeReason::GpuVisualEligible;
			const AnimationExecutionModeDecision corrupt = ResolveAnimationExecutionMode(request, corruptState);
			ExpectModeAndReason(context, corrupt,
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationExecutionModeReason::InvalidPreviousState,
				"Corrupt persisted stability state must restart safely on CPU without inventing a transition source.");
			context.Expect(!corrupt.modeChanged && !corrupt.transition.occurred,
				"An invalid source record cannot authorize an externally visible mode transition.");
		});

		registry.Add("Animation.ExecutionMode.ConsumerClosureHonorsExactJointLimits", [](TestContext& context) {
			std::vector<std::int32_t> parents(kMaximumAnimationExecutionJoints, -1);
			std::vector<std::uint32_t> requested(kMaximumAnimationExecutionJoints, 0U);
			for(std::size_t index = 0U; index < requested.size(); ++index) {
				requested[index] = static_cast<std::uint32_t>(index);
			}

			AnimationExecutionModeRequest request = MakeEligibleRequest();
			request.consumers.jointParents = parents;
			request.consumers.requestedJoints = requested;
			const AnimationExecutionModeDecision exactLimit = ResolveAnimationExecutionMode(request, {});
			context.Expect(exactLimit.consumerClosure.status == AnimationStatus::Ok
				&& exactLimit.consumerClosure.joints.size() == kMaximumAnimationExecutionJoints
				&& exactLimit.consumerClosure.joints.front() == 0U
				&& exactLimit.consumerClosure.joints.back() == kMaximumAnimationExecutionJoints - 1U,
				"The exact frozen joint limit must remain valid without overflowing epoch or scratch bookkeeping.");

			std::vector<std::uint32_t> tooManyRequested(kMaximumAnimationExecutionJoints + 1U, 0U);
			request = MakeEligibleRequest();
			request.consumers.requestedJoints = tooManyRequested;
			const AnimationExecutionModeDecision tooMany = ResolveAnimationExecutionMode(request, {});
			context.Expect(tooMany.consumerClosure.status == AnimationStatus::HardLimitExceeded
				&& tooMany.consumerClosure.joints.empty(),
				"A requested-joint count above the hard limit must reject before temporary allocation.");

			const std::array<std::int32_t, 1U> oneParent{-1};
			const std::array<std::uint32_t, 1U> outOfRange{1U};
			request = MakeEligibleRequest();
			request.consumers.jointParents = oneParent;
			request.consumers.requestedJoints = outOfRange;
			const AnimationExecutionModeDecision invalidJoint = ResolveAnimationExecutionMode(request, {});
			context.Expect(invalidJoint.consumerClosure.status == AnimationStatus::InvalidArgument
				&& invalidJoint.consumerClosure.joints.empty(),
				"A requested joint outside the supplied hierarchy must reject the complete closure atomically.");
		});

		registry.Add("Animation.ExecutionMode.MalformedPromotionStateFailsSafe", [](TestContext& context) {
			AnimationExecutionModeState valid;
			valid.initialized = true;
			valid.effectiveMode = AnimationEffectiveExecutionMode::CpuDeterministicFallback;
			valid.effectiveReason = AnimationExecutionModeReason::DeterministicRequired;
			valid.modeEnteredTick = 2;

			std::array<AnimationExecutionModeState, 7U> malformed{};
			malformed.fill(valid);
			malformed[0].effectiveReason = AnimationExecutionModeReason::Uninitialized;
			malformed[1].modeEnteredTick = -1;
			malformed[2].modeEnteredTick = 11;
			malformed[3].promotionPending = true;
			malformed[3].promotionMode = AnimationEffectiveExecutionMode::CpuDeterministicFallback;
			malformed[3].promotionReason = AnimationExecutionModeReason::GpuVisualEligible;
			malformed[3].promotionSinceTick = 2;
			malformed[4].promotionPending = true;
			malformed[4].promotionMode = AnimationEffectiveExecutionMode::GpuVisual;
			malformed[4].promotionReason = AnimationExecutionModeReason::Uninitialized;
			malformed[4].promotionSinceTick = 2;
			malformed[5].promotionPending = true;
			malformed[5].promotionMode = AnimationEffectiveExecutionMode::GpuVisual;
			malformed[5].promotionReason = AnimationExecutionModeReason::GpuVisualEligible;
			malformed[5].promotionSinceTick = 1;
			malformed[6].promotionPending = true;
			malformed[6].promotionMode = AnimationEffectiveExecutionMode::GpuVisual;
			malformed[6].promotionReason = AnimationExecutionModeReason::AuthoritativeSubsetRequired;
			malformed[6].promotionSinceTick = 2;

			const AnimationExecutionModeRequest request = MakeEligibleRequest(10);
			for(const AnimationExecutionModeState& state : malformed) {
				const AnimationExecutionModeDecision decision = ResolveAnimationExecutionMode(request, state);
				context.Expect(decision.effectiveMode == AnimationEffectiveExecutionMode::CpuDeterministicFallback
					&& decision.reason == AnimationExecutionModeReason::InvalidPreviousState
					&& !decision.modeChanged && !decision.transition.occurred,
					"Every malformed persisted promotion record must restart safely without publishing a guessed transition.");
			}
		});

		registry.Add("Animation.ExecutionMode.HardDowngradesAreImmediateAndPromotionsUseFixedTicks", [](TestContext& context) {
			AnimationExecutionModeRequest request = MakeEligibleRequest(0);
			request.stability.promotionHysteresisTicks = 2;
			request.stability.minimumResidenceTicks = 5;
			AnimationExecutionModeDecision decision = ResolveAnimationExecutionMode(request, {});
			context.Expect(decision.effectiveMode == AnimationEffectiveExecutionMode::GpuVisual,
				"The first decision has no previous mode to protect and should initialize directly to the eligible target.");

			request.fixedTick = 1;
			request.deterministicRequired = true;
			request.continuity = MakeContinuityToken(request.fixedTick, 10U);
			decision = ResolveAnimationExecutionMode(request, decision.nextState);
			context.Expect(decision.modeChanged
				&& decision.effectiveMode == AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				"A correctness-driven GPU-to-CPU downgrade must ignore minimum residence and happen immediately.");
			context.Expect(decision.transition.occurred
				&& decision.transition.from == AnimationEffectiveExecutionMode::GpuVisual
				&& decision.transition.to == AnimationEffectiveExecutionMode::CpuDeterministicFallback
				&& decision.transition.continuity == request.continuity,
				"Mode changes must carry the complete fixed-tick continuity token into the target evaluator.");

			request.deterministicRequired = false;
			request.fixedTick = 2;
			decision = ResolveAnimationExecutionMode(request, decision.nextState);
			context.Expect(!decision.modeChanged
				&& decision.effectiveMode == AnimationEffectiveExecutionMode::CpuDeterministicFallback
				&& decision.reason == AnimationExecutionModeReason::PromotionHysteresis,
				"A CPU-to-GPU promotion must wait for a stable fixed-tick candidate.");

			request.fixedTick = 4;
			decision = ResolveAnimationExecutionMode(request, decision.nextState);
			context.Expect(!decision.modeChanged
				&& decision.reason == AnimationExecutionModeReason::MinimumResidence,
				"After hysteresis passes, the current mode's minimum residence must still be honored.");

			request.fixedTick = 6;
			request.continuity = MakeContinuityToken(request.fixedTick, 30U);
			decision = ResolveAnimationExecutionMode(request, decision.nextState);
			context.Expect(decision.modeChanged
				&& decision.effectiveMode == AnimationEffectiveExecutionMode::GpuVisual
				&& decision.reason == AnimationExecutionModeReason::GpuVisualEligible,
				"A stable promotion should occur once both fixed-tick gates pass.");
			context.Expect(decision.transition.continuity == request.continuity
				&& decision.nextState.continuity == request.continuity,
				"Promotion must transfer current state rather than the token captured when the candidate first appeared.");
			context.Expect(decision.transition.continuity.parameterSnapshot == 32U
				&& decision.transition.continuity.synchronizationState == 34U
				&& decision.transition.continuity.synchronizationPhaseTick == request.fixedTick + 2
				&& decision.transition.continuity.transitionBoundaryTick == request.fixedTick,
				"The transition must explicitly carry parameters, synchronization, and the safe fixed-tick boundary.");
		});

		registry.Add("Animation.ExecutionMode.FixedTickMaximumDoesNotOverflowStabilityGates", [](TestContext& context) {
			AnimationExecutionModeRequest request = MakeEligibleRequest(0);
			request.deterministicRequired = true;
			request.stability.promotionHysteresisTicks = std::numeric_limits<AnimationFixedTick>::max();
			request.stability.minimumResidenceTicks = std::numeric_limits<AnimationFixedTick>::max();
			AnimationExecutionModeDecision decision = ResolveAnimationExecutionMode(request, {});

			request.deterministicRequired = false;
			decision = ResolveAnimationExecutionMode(request, decision.nextState);
			context.Expect(!decision.modeChanged
				&& decision.reason == AnimationExecutionModeReason::PromotionHysteresis,
				"A maximum-duration promotion candidate must remain pending at its start tick.");

			request.fixedTick = std::numeric_limits<AnimationFixedTick>::max();
			request.continuity = MakeContinuityToken(42, 50U);
			request.continuity.transitionBoundaryTick = request.fixedTick;
			decision = ResolveAnimationExecutionMode(request, decision.nextState);
			context.Expect(decision.modeChanged
				&& decision.effectiveMode == AnimationEffectiveExecutionMode::GpuVisual
				&& decision.nextState.modeEnteredTick == request.fixedTick,
				"Unsigned elapsed-time arithmetic must pass both gates exactly at INT64_MAX without overflow.");
		});

		registry.Add("Animation.ExecutionMode.ConsumerArrivalDowngradesGpuToHybridWithoutOscillation", [](TestContext& context) {
			AnimationExecutionModeRequest request = MakeEligibleRequest(0);
			request.stability.promotionHysteresisTicks = 2;
			request.stability.minimumResidenceTicks = 4;
			AnimationExecutionModeDecision decision = ResolveAnimationExecutionMode(request, {});

			request.fixedTick = 1;
			request.consumers.events = true;
			decision = ResolveAnimationExecutionMode(request, decision.nextState);
			context.Expect(decision.modeChanged
				&& decision.effectiveMode == AnimationEffectiveExecutionMode::HybridAuthoritative,
				"A newly required authoritative subset must downgrade GPU Visual to Hybrid immediately.");

			request.fixedTick = 2;
			request.consumers.events = false;
			decision = ResolveAnimationExecutionMode(request, decision.nextState);
			context.Expect(!decision.modeChanged
				&& decision.effectiveMode == AnimationEffectiveExecutionMode::HybridAuthoritative,
				"A short-lived consumer disappearance must not oscillate the animator back to GPU Visual.");
			request.fixedTick = 6;
			decision = ResolveAnimationExecutionMode(request, decision.nextState);
			context.Expect(decision.modeChanged
				&& decision.effectiveMode == AnimationEffectiveExecutionMode::GpuVisual,
				"The animator may promote only after the stable candidate and residence windows both pass.");
		});

		registry.Add("Animation.ExecutionMode.ChangedPromotionSubsetRestartsHysteresis", [](TestContext& context) {
			AnimationExecutionModeRequest request = MakeEligibleRequest(0);
			request.stability.promotionHysteresisTicks = 2;
			request.stability.minimumResidenceTicks = 0;
			AnimationExecutionModeDecision decision = ResolveAnimationExecutionMode(request, {});

			request.fixedTick = 1;
			request.gpu.evaluationHealthy = false;
			decision = ResolveAnimationExecutionMode(request, decision.nextState);
			context.Expect(decision.effectiveMode == AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				"A GPU evaluation fault must establish the safe CPU residence immediately.");

			request.fixedTick = 2;
			request.gpu.evaluationHealthy = true;
			request.consumers.events = true;
			decision = ResolveAnimationExecutionMode(request, decision.nextState);
			context.Expect(decision.reason == AnimationExecutionModeReason::PromotionHysteresis,
				"The first Hybrid candidate starts its fixed-tick hysteresis window.");

			request.fixedTick = 3;
			request.consumers.events = false;
			request.consumers.rootMotion = true;
			decision = ResolveAnimationExecutionMode(request, decision.nextState);
			context.Expect(decision.reason == AnimationExecutionModeReason::PromotionHysteresis
				&& decision.nextState.promotionSinceTick == 3,
				"Changing the authoritative subset must restart hysteresis even when the broad target remains Hybrid.");

			request.fixedTick = 4;
			decision = ResolveAnimationExecutionMode(request, decision.nextState);
			context.Expect(!decision.modeChanged,
				"One fixed tick after a changed candidate must still retain the safe CPU mode.");
			request.fixedTick = 5;
			decision = ResolveAnimationExecutionMode(request, decision.nextState);
			context.Expect(decision.modeChanged
				&& decision.effectiveMode == AnimationEffectiveExecutionMode::HybridAuthoritative,
				"The new stable subset may promote exactly when its own hysteresis window completes.");
		});

		registry.Add("Animation.ExecutionMode.EmptyBatchPerformsNoDormantWork", [](TestContext& context) {
			std::vector<AnimationExecutionModeDecision> decisions;
			decisions.reserve(4U);
			decisions.push_back(ResolveAnimationExecutionMode(MakeEligibleRequest(), {}));
			const std::size_t capacityBefore = decisions.capacity();

			const AnimationExecutionModeBatchResult result = ResolveAnimationExecutionModes({}, decisions);
			context.Expect(result.status == AnimationStatus::Ok && result.resolvedCount == 0U,
				"A frame with zero active animators must report zero resolver work.");
			context.Expect(decisions.empty() && decisions.capacity() == capacityBefore,
				"The dormant batch path should clear stale results without allocating replacement storage.");
		});

		registry.Add("Animation.ExecutionMode.BatchPublishesCompleteDecisionsInInputOrder", [](TestContext& context) {
			std::array<AnimationExecutionModeBatchRequest, 2U> requests{};
			requests[0].request = MakeEligibleRequest();
			requests[1].request = MakeEligibleRequest();
			requests[1].request.consumers.events = true;
			std::vector<AnimationExecutionModeDecision> decisions;

			const AnimationExecutionModeBatchResult result = ResolveAnimationExecutionModes(requests, decisions);
			context.Expect(result.status == AnimationStatus::Ok && result.resolvedCount == requests.size(),
				"A bounded active batch must publish every resolved request atomically.");
			context.Expect(decisions.size() == requests.size()
				&& decisions[0].effectiveMode == AnimationEffectiveExecutionMode::GpuVisual
				&& decisions[1].effectiveMode == AnimationEffectiveExecutionMode::HybridAuthoritative,
				"Batch output order must match deterministic scheduler-owned input order.");
		});

		registry.Add("Animation.ExecutionMode.OversizedBatchRejectsWithoutClearingCallerOutput", [](TestContext& context) {
			// A rejected frame must not replace the last complete scheduler publication
			// with an empty list, because downstream consumers could observe that as a
			// valid frame containing zero animators.
			std::vector<AnimationExecutionModeDecision> decisions;
			decisions.push_back(ResolveAnimationExecutionMode(MakeEligibleRequest(4), {}));
			const std::vector<AnimationExecutionModeDecision> expected = decisions;
			std::vector<AnimationExecutionModeBatchRequest> oversized(
				kMaximumAnimationExecutionModeBatchSize + 1U);

			const AnimationExecutionModeBatchResult result = ResolveAnimationExecutionModes(oversized, decisions);
			context.Expect(result.status == AnimationStatus::HardLimitExceeded && result.resolvedCount == 0U,
				"A batch over the frozen limit must reject before per-animator resolution.");
			context.Expect(decisions == expected,
				"A hard-limit rejection must leave the caller's previous complete output intact atomically.");
		});

		registry.Add("Animation.ExecutionMode.PureResolverRepeatsWithoutWallClockState", [](TestContext& context) {
			AnimationExecutionModeRequest request = MakeEligibleRequest(77);
			request.consumers.events = true;
			request.continuity = MakeContinuityToken(71, 72U);
			const AnimationExecutionModeState previous = ResolveInitialState(MakeEligibleRequest(70));
			const AnimationExecutionModeDecision first = ResolveAnimationExecutionMode(request, previous);
			for(std::uint32_t iteration = 0U; iteration < 100U; ++iteration) {
				const AnimationExecutionModeDecision repeated = ResolveAnimationExecutionMode(request, previous);
				context.Expect(repeated == first,
					"Identical fixed-tick inputs and prior state must produce the same complete decision on every call.");
			}
		});
	}
}
