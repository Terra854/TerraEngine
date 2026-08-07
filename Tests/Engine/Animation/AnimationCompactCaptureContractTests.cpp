// AnimationCompactCaptureContractTests.cpp - Guards protected compact admission ordering.

#include "TestHarness.h"

#include <fstream>
#include <iterator>
#include <string>

namespace TerraEngine::Tests {
	namespace {
		std::string ReadAnimationSystemSource()
		{
			const std::string path = std::string(TERRAENGINE_SOURCE_DIR)
				+ "/Engine/Animation/AnimationSystem.cpp";
			std::ifstream source(path, std::ios::binary);
			return {std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
		}
	}

	void RegisterAnimationCompactCaptureContractTests(TestRegistry& registry)
	{
		registry.Add("Animation.CompactCapture.PreCopyAdmissionContract", [](TestContext& context) {
			const std::string source = ReadAnimationSystemSource();
			const auto countOccurrences = [&source](const std::string& needle) {
				std::size_t count = 0U;
				for(std::size_t position = source.find(needle); position != std::string::npos;
					position = source.find(needle, position + needle.size())) {
					++count;
				}
				return count;
			};
			const std::size_t admission = source.find("const bool compactCaptureProven");
			const std::size_t consumerProof = source.rfind(
				"const AnimationResolvedConsumerRequirements compactConsumers", admission);
			const std::size_t compactAppend = source.find("candidateCompactGpuVisualCaptures.append", admission);
			const std::size_t mappingAppend = source.find("candidateInputWorkIndices.push_back", admission);
			const std::size_t broadBindingCopy = source.find("item.binding = record->binding", admission);
			const std::size_t broadControllerCopy = source.find("item.candidateGraphState = record->graphState", admission);
			const std::size_t broadParameterCopy = source.find("item.parameters = record->parameters", admission);
			const std::string admissionProof = consumerProof != std::string::npos
				&& compactAppend != std::string::npos && compactAppend > consumerProof
				? source.substr(consumerProof, compactAppend - consumerProof)
				: std::string{};

			context.Expect(admission != std::string::npos && compactAppend > admission
				&& mappingAppend > compactAppend
				&& source.find("candidateFrameWork.resize(capturedFrameInputs.size())", admission)
					== std::string::npos,
				"Compact admission must append indexed SoA capture data without constructing a FrameWorkItem per input.");
			context.Expect(broadBindingCopy != std::string::npos
				&& broadControllerCopy != std::string::npos
				&& broadParameterCopy != std::string::npos
				&& broadBindingCopy > mappingAppend && broadControllerCopy > mappingAppend
				&& broadParameterCopy > mappingAppend,
				"Broad binding, controller, and parameter copies must occur only after compact admission continues.");
			context.Expect(admissionProof.find("compactConsumerClosureProven") != std::string::npos
				&& admissionProof.find("frameConsumerReadView.Resolve") != std::string::npos
				&& admissionProof.find("compactConsumers.attribution.empty()") != std::string::npos
				&& admissionProof.find("compactRootMotionRequiresCpuAuthority") != std::string::npos
				&& admissionProof.find("RequiresCpuRootMotionAuthority") != std::string::npos
				&& admissionProof.find("!liveBinding.clip->rootMotion.enabled") == std::string::npos
				&& source.find("hasContinuitySource = previous.initialized", admission) != std::string::npos
				&& admissionProof.find("HasExactGpuPresentationFor") == std::string::npos
				&& admissionProof.find("residencyGeneration != 0U") != std::string::npos
				&& admissionProof.find("evaluationGenerations.rendererGeneration != 0U")
					== std::string::npos
				&& admissionProof.find("evaluationGenerations.deterministicGeneration != 0U")
					== std::string::npos
				&& admissionProof.find("record->executionModeState.initialized")
					!= std::string::npos
				&& admissionProof.find("record->executionModeState.effectiveMode")
					!= std::string::npos
				&& admissionProof.find("== AnimationEffectiveExecutionMode::GpuVisual")
					!= std::string::npos
				&& admissionProof.find("compactPolicyEligible") != std::string::npos
				&& admissionProof.find("gpu.rendererAvailable") != std::string::npos
				&& admissionProof.find("gpu.animationCapabilityAvailable") != std::string::npos
				&& admissionProof.find("gpu.resourcesResident") != std::string::npos
				&& admissionProof.find("gpu.evaluationHealthy") != std::string::npos
				&& admissionProof.find("std::isfinite(gpu.requiredBudget)")
					!= std::string::npos
				&& admissionProof.find("gpu.requiredBudget <= gpu.availableBudget")
					!= std::string::npos
				&& admissionProof.find("modeState.effectiveReason") != std::string::npos
				&& admissionProof.find("modeState.modeEnteredTick") != std::string::npos
				&& admissionProof.find("!modeState.promotionPending") != std::string::npos
				&& admissionProof.find("compactModeProven") != std::string::npos
				&& admissionProof.find("frameSceneGeneration != 0U") != std::string::npos
				&& admissionProof.find("gpuFastBinding->visual.renderIdentity.rendererGeneration != 0U")
					!= std::string::npos
				&& source.find("compactPreCopyCaptureCount.fetch_add", admission) != std::string::npos,
				"Compact admission must resolve only matching consumers, prove every live GPU-mode input explicitly, retain the complete path through promotion or demotion, and preserve resource, scene, bind-pose continuity, and observable pre-copy proof.");
			context.Expect(source.find("counters.activeAnimators = capturedInputWorkIndices.size()")
				!= std::string::npos
				&& source.find("HasExactGpuPresentationFor(") == std::string::npos
				&& source.find("counters.completeCapturedAnimators = frameWork.size()")
				!= std::string::npos
				&& source.find("compactGpuVisualCaptures.stageMasks[captured.storageIndex]")
				!= std::string::npos
				&& source.find("presentation->topology.generations.rendererGeneration")
					!= std::string::npos,
				"Compact planning must tolerate pipelined immutable GPU envelopes while diagnostics count direct SoA captures, preserve their stage mask, and publish the accepted compact topology's renderer generation.");
			context.Expect(countOccurrences("StampGpuVisualFrameIdentity(") >= 4U
				&& countOccurrences("hasAuthoritativeConsumer = false") >= 2U
				&& countOccurrences("hasControllerOrTransition = false") >= 2U
				&& countOccurrences("hasAdvancedFeature = false") >= 2U
				&& source.find("visual.generations.meshGeneration = renderIdentity.meshHeaderVersion")
					!= std::string::npos
				&& source.find("visual.generations.rendererGeneration") != std::string::npos
				&& source.find("visual.generations.sceneGeneration") != std::string::npos
				&& source.find("visual.generations.frameIdLow") != std::string::npos
				&& source.find("visual.generations.frameIdHigh") != std::string::npos
				&& source.find("visual.renderIdentity.sceneGeneration = frameSceneGeneration")
					!= std::string::npos,
				"Complete and both compact planners must share one representable frame-identity stamp before GPU lowering, then copy the protected exclusion proof into the fail-closed compact input.");
		});

		registry.Add("Animation.CompactCapture.CohortsVisibilityAndContiguousPartitions", [](TestContext& context) {
			const std::string source = ReadAnimationSystemSource();
			context.Expect(source.find("struct CompactGpuVisualCohort") != std::string::npos
				&& source.find("std::vector<std::size_t> cohortIndices") != std::string::npos
				&& source.find("std::vector<CompactGpuVisualCohort> cohorts") != std::string::npos
				&& source.find("cohortLookup.equal_range(fingerprint)") != std::string::npos
				&& source.find("cohorts[candidate->second].Matches(record)") != std::string::npos,
				"Compact GPU captures must share immutable facts across both adjacent and interleaved cohort members without trusting a hash collision.");
			context.Expect(source.find("std::vector<std::uint8_t> presentationVisible")
					!= std::string::npos
				&& source.find("compact.presentationVisible[compactIndex] == 0U")
					!= std::string::npos,
				"Protected capture visibility must keep compact owners out of presentation when no output is active.");
			context.Expect(source.find("|| visibility->hasActiveView") != std::string::npos
				&& source.find("visibility->Contains(input.descriptor.ownerObjectId)")
					== std::string::npos,
				"An active multi-view frame must fail open so a newly queued capture cannot render compact owners in bind pose.");
			context.Expect(source.find("partition.itemBegin, partition.itemEnd")
					!= std::string::npos
				&& source.find("compactIndex += partitionCount") == std::string::npos,
				"Compact worker partitions must traverse contiguous ranges for cache locality.");
			context.Expect(source.find("const auto rangePlanningStart") != std::string::npos
				&& source.find("const auto itemPlanningStart") == std::string::npos
				&& source.find("bool cancellationObserved = false") != std::string::npos
				&& source.find("((compactIndex - boundedBegin) & 15U) == 0U")
					!= std::string::npos,
				"Animation telemetry must time worker ranges and compact cancellation must use a bounded latched poll instead of adding synchronization to every animator.");
			context.Expect(source.find("gpuPresentationFramePool") != std::string::npos
				&& source.find("candidate.use_count() == 1U") != std::string::npos,
				"GPU presentation publication must recycle retired immutable frame storage safely.");
		});

		registry.Add("Animation.BindingBatch.ReusesImmutableAuthoredDefaults", [](TestContext& context) {
			const std::string source = ReadAnimationSystemSource();
			context.Expect(source.find("struct PreparedBinding") != std::string::npos
				&& source.find("ImmutableBindPreparation immutable") != std::string::npos
				&& source.find("record->parameters.assign(initialParameters.begin()") != std::string::npos
				&& source.find("record->layerWeights.assign(initialLayerWeights.begin()") != std::string::npos,
				"A crowd binding wave must parse immutable parameter and layer defaults once while retaining mutable state per animator.");
		});

		registry.Add("Animation.DirectClipRefresh.PreparesCohortsBeforeRuntimeLock", [](TestContext& context) {
			const std::string source = ReadAnimationSystemSource();
			const std::size_t batch = source.find(
				"void AnimationSystem::RefreshAnimatorDirectClipStreamingBatch");
			const std::size_t preparation = source.find(
				"struct PreparedDirectClipReplacement", batch);
			const std::size_t preparationLoop = source.find(
				"prepared.playback.Configure", preparation);
			const std::size_t runtimeLock = source.find(
				"lock.lock();", preparation);
			context.Expect(batch != std::string::npos && preparation > batch
				&& preparationLoop > preparation && runtimeLock > preparationLoop,
				"Direct-clip cohort playback and event preparation must finish before protected runtime ownership is acquired.");
			context.Expect(source.find("preparedReplacementEventStream") != std::string::npos
				&& source.find("replacementEventStream = *preparedReplacementEventStream")
					!= std::string::npos
				&& source.find("replacementPlayback = *preparedReplacementPlayback")
					!= std::string::npos,
				"Every promoted owner must adopt independent cursors from one prepared cohort template.");
		});
	}
}
