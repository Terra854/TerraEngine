// AnimationProcessingClassTests.cpp - Fail-closed compact classification contracts.

#include "TestHarness.h"

#include "Animation/Runtime/AnimationExecutionMode.h"
#include "Animation/AnimationSystem.h"

#include <array>
#include <cstdint>
#include <memory>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Animation;

	namespace {
		CompactGpuVisualInput MakeVisualInput()
		{
			CompactGpuVisualInput input;
			input.stableInstanceId = 7U;
			input.stableSubmissionOrdinal = 3U;
			input.playbackTick = 42;
			input.sourcePoseGeneration = 5U;
			input.generations = {1U, 2U, 3U, 4U, 5U, 6U, 7U};
			input.eligibilityReason = AnimationExecutionModeReason::GpuVisualEligible;
			input.gpuLoweringComplete = true;
			input.hasAuthoritativeConsumer = false;
			input.hasControllerOrTransition = false;
			input.hasAdvancedFeature = false;
			input.hasContinuitySource = true;
			return input;
		}
	}

	void RegisterAnimationProcessingClassTests(TestRegistry& registry)
	{
		registry.Add("Animation.ProcessingClass.VisualOnlyInputIsCompact", [](TestContext& context) {
			const CompactGpuVisualInput input = MakeVisualInput();
			context.Expect(ClassifyAnimationProcessing(input)
				== AnimationProcessingClass::CompactGpuVisual,
				"A complete visual-only input should use the compact frame record.");
		});

		registry.Add("Animation.ProcessingClass.AuthorityAndAdvancedRequirementsStayComplete",
			[](TestContext& context) {
				using Flag = bool CompactGpuVisualInput::*;
				const Flag exclusions[] = {
					&CompactGpuVisualInput::hasAuthoritativeConsumer,
					&CompactGpuVisualInput::hasControllerOrTransition,
					&CompactGpuVisualInput::hasAdvancedFeature};
				for(const Flag exclusion : exclusions) {
					CompactGpuVisualInput input = MakeVisualInput();
					input.*exclusion = true;
					context.Expect(ClassifyAnimationProcessing(input)
						== AnimationProcessingClass::Complete,
						"Any authoritative, controller, or advanced requirement must fail closed to complete processing.");
				}
			});

		registry.Add("Animation.ProcessingClass.AmbiguousOrIncompleteInputsFailClosedTable", [](TestContext& context) {
			using Mutator = void (*)(CompactGpuVisualInput&);
			const std::array<Mutator, 12U> exclusions{
				[](CompactGpuVisualInput& input) { input.gpuLoweringComplete = false; },
				[](CompactGpuVisualInput& input) { input.hasContinuitySource = false; },
				[](CompactGpuVisualInput& input) {
					input.eligibilityReason = AnimationExecutionModeReason::Uninitialized;
				},
				[](CompactGpuVisualInput& input) { input.sourcePoseGeneration = 0U; },
				[](CompactGpuVisualInput& input) { input.generations.rendererGeneration = 0U; },
				[](CompactGpuVisualInput& input) { input.generations.capabilityGeneration = 0U; },
				[](CompactGpuVisualInput& input) { input.generations.bindingGeneration = 0U; },
				[](CompactGpuVisualInput& input) { input.generations.resourceGeneration = 0U; },
				[](CompactGpuVisualInput& input) { input.generations.sceneGeneration = 0U; },
				[](CompactGpuVisualInput& input) { input.generations.authorityGeneration = 0U; },
				[](CompactGpuVisualInput& input) { input.generations.membershipGeneration = 0U; },
				[](CompactGpuVisualInput& input) { input.stableInstanceId = 0U; }};
			for(const Mutator exclusion : exclusions) {
				CompactGpuVisualInput input = MakeVisualInput();
				exclusion(input);
				context.Expect(ClassifyAnimationProcessing(input)
					== AnimationProcessingClass::Complete,
					"Any incomplete identity, capability, continuity, or eligibility proof must remain on the complete path.");
			}
		});

		registry.Add("Animation.ProcessingClass.ResultAndTransitionRequireExactGenerations",
			[](TestContext& context) {
				const CompactGpuVisualInput input = MakeVisualInput();
				CompactGpuVisualResult result;
				result.stableInstanceId = input.stableInstanceId;
				result.stableSubmissionOrdinal = input.stableSubmissionOrdinal;
				result.sourcePoseGeneration = input.sourcePoseGeneration;
				result.presentationPoseGeneration = 6U;
				result.generations = input.generations;
				result.reason = input.eligibilityReason;
				result.complete = true;
				context.Expect(result.IsPublishableFor(input),
					"A compact result may publish only for its exact captured input generation.");
				result.generations.rendererGeneration++;
				context.Expect(!result.IsPublishableFor(input),
					"A renderer-generation change must reject the whole compact result.");

				AnimationClassTransition transition;
				transition.occurred = true;
				transition.from = AnimationProcessingClass::Complete;
				transition.to = AnimationProcessingClass::CompactGpuVisual;
				transition.sourceGenerations = input.generations;
				transition.destinationGenerations = input.generations;
				transition.continuity.transitionBoundaryTick = input.playbackTick;
				context.Expect(transition.IsContinuous(),
					"A class transition needs an exact source/destination identity and boundary tick.");
				transition.destinationGenerations.membershipGeneration++;
				context.Expect(!transition.IsContinuous(),
					"Changing animator membership must reject a transition instead of mixing owner state.");
			});

		registry.Add("Animation.ProcessingClass.FastBindingCopiesRetainOneImmutableProof",
			[](TestContext& context) {
				auto payloads = std::make_shared<GpuAnimationResidentPayloads>();
				payloads->programWords.push_back(1U);
				payloads->packedClipBytes.push_back(1U);
				payloads->skeletonTables.push_back(1U);
				auto proof = std::make_shared<AnimatorPlaybackBinding::GpuFastBinding>();
				proof->programResourceIndex = 0U;
				proof->residentPayloads = std::move(payloads);
				proof->residentByteCount = sizeof(std::uint32_t) + 2U;
				AnimatorPlaybackBinding first;
				first.gpuFastBinding = proof;
				AnimatorPlaybackBinding second = first;
				context.Expect(first.gpuFastBinding->IsComplete(),
					"A complete compact binding must retain its immutable resident proof.");
				context.Expect(first.gpuFastBinding == second.gpuFastBinding,
					"Copied prefab bindings must share the immutable fast proof instead of duplicating payload ownership.");
			});
	}
}
