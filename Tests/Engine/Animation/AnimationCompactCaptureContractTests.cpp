// Supplemental source contracts follow compiled definitions; production stage tests prove behavior.
#include "TestHarness.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>

namespace TerraEngine::Tests {
	namespace {
		std::string StripSourceWhitespace(std::string text) {
			text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; }), text.end());
			return text;
		}

		std::string ReadAnimationSource(TestContext& context, std::string_view relative) {
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relative, std::ios::binary);
			context.Expect(input.is_open(), "Supplemental source contract must open its actual production file.");
			std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
			context.Expect(!input.bad() && !text.empty(), "Supplemental source contract requires nonempty readable production bytes.");
			return text;
		}

		// Bounds are actual adjacent compiled definitions in one file, never concatenated modules.
		std::string SourceRegion(TestContext& context, const std::string& source,
			std::string_view first, std::string_view next) {
			const auto begin = source.find(first);
			context.Expect(begin != std::string::npos, "The production definition must exist before selecting its region.");
			const auto end = source.find(next, begin + first.size());
			context.Expect(end != std::string::npos && end > begin, "The production region must have a real nonempty end bound.");
			return source.substr(begin, end - begin);
		}

		bool ContainsAll(const std::string& source, std::initializer_list<std::string_view> needles) {
			return std::all_of(needles.begin(), needles.end(), [&](auto needle) { return source.find(needle) != std::string::npos; });
		}

		std::size_t CountOccurrences(const std::string& source, std::string_view needle) {
			std::size_t count = 0U;
			for(auto position = source.find(needle); position != std::string::npos; position = source.find(needle, position + needle.size())) ++count;
			return count;
		}

		bool Ordered(const std::string& source, std::initializer_list<std::string_view> needles) {
			std::size_t position = 0U;
			for(auto needle : needles) {
				const auto found = source.find(needle, position);
				if(found == std::string::npos) return false;
				position = found + needle.size();
			}
			return true;
		}

		// Negative legacy checks inspect the complete coordinator/private-frame closure.
		bool ClosureContains(TestContext& context, std::string_view needle) {
			const auto directory = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Animation";
			std::size_t files = 0U;
			bool found = false;
			for(const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
				if(!entry.is_regular_file()) continue;
				const auto relative = entry.path().lexically_relative(directory);
				const auto extension = entry.path().extension().string();
				if(extension != ".cpp" && extension != ".h" && extension != ".hpp" && extension != ".inl"
					&& extension != ".hxx" && extension != ".cxx" && extension != ".cc") continue;
				const bool coordinator = relative.parent_path().empty() && entry.path().filename().string().starts_with("AnimationSystem");
				const bool frame = *relative.begin() == "Frame";
				if(!coordinator && !frame) continue;
				++files;
				const auto text = ReadAnimationSource(context, entry.path().lexically_relative(TERRAENGINE_SOURCE_DIR).generic_string());
				found = text.find(needle) != std::string::npos || found;
			}
			context.Expect(files >= 13U, "Legacy absence checks must select the complete extracted animation closure.");
			return found;
		}
	}

	void RegisterAnimationCompactCaptureContractTests(TestRegistry& registry) {
		registry.Add("Animation.CompactCapture.PreCopyAdmissionContract", [](TestContext& context) {
			const auto file = ReadAnimationSource(context, "Engine/Animation/Frame/AnimationFrameCapture.cpp");
			const auto source = SourceRegion(context, file, "AnimationSystem::Impl::CaptureRuntimeFrameInputs(", "AnimationSystem::Impl::CaptureQueuedFrameInputs(");
			const auto copy = SourceRegion(context, file, "AnimationSystem::Impl::CopyCompleteRuntimeInput(", "AnimationSystem::Impl::CaptureRuntimeFrameInputs(");
			const auto consumerProof = source.find("const AnimationResolvedConsumerRequirements compactConsumers");
			const auto modeProof = source.find("const bool compactModeProven");
			context.Expect(consumerProof != std::string::npos && modeProof != std::string::npos,
				"Both consumer and live-mode proof definitions must exist in capture.");
			const auto proofBegin = std::min(consumerProof, modeProof);
			const auto admission = source.find("const bool compactCaptureProven", proofBegin);
			const auto append = source.find("persistentReuse.candidateCompactGpuVisualCaptures.append", admission);
			context.Expect(proofBegin != std::string::npos && admission != std::string::npos
				&& append != std::string::npos && proofBegin < admission && admission < append,
				"Compact proof and SoA append must exist within the actual capture definition.");
			const auto proof = StripSourceWhitespace(source.substr(proofBegin, append - proofBegin));
			context.Expect(Ordered(source.substr(admission), {"candidateCompactGpuVisualCaptures.append", "candidateInputWorkIndices.push_back", "continue;", "CopyCompleteRuntimeInput("})
				&& source.find("candidateFrameWork.resize(frameInputs.capturedFrameInputs.size())") == std::string::npos,
				"Compact admission must continue after indexed SoA append before delegating complete input copying.");
			context.Expect(ContainsAll(copy, {"item.binding = record->binding", "item.candidateGraphState", "item.parameters = record->parameters"}),
				"The delegated complete copier must retain the broad binding, controller and parameter copies.");
			context.Expect(ContainsAll(proof, {"compactConsumerClosureProven", "frameInputs.frameConsumerReadView.Resolve", "compactConsumers.attribution.empty()", "compactRootMotionRequiresCpuAuthority", "RequiresCpuRootMotionAuthority", "residencyGeneration!=0U", "modeState.initialized", "modeState.effectiveMode", "==AnimationEffectiveExecutionMode::GpuVisual", "compactPolicyEligible", "gpu.rendererAvailable", "gpu.animationCapabilityAvailable", "gpu.resourcesResident", "gpu.evaluationHealthy", "std::isfinite(gpu.requiredBudget)", "gpu.requiredBudget<=gpu.availableBudget", "modeState.effectiveReason", "modeState.modeEnteredTick", "!modeState.promotionPending", "compactModeProven", "frameInputs.frameSceneGeneration!=0U", "gpuFastBinding->visual.renderIdentity.rendererGeneration!=0U"})
				&& proof.find("!liveBinding.clip->rootMotion.enabled") == std::string::npos
				&& proof.find("evaluationGenerations.rendererGeneration!=0U") == std::string::npos
				&& proof.find("evaluationGenerations.deterministicGeneration!=0U") == std::string::npos
				&& ContainsAll(source, {"FindCompactRuntimeState", "evaluationState.compactPreCopyCaptureCount.fetch_add"})
				&& !ClosureContains(context, "HasExactGpuPresentationFor("),
				"Compact admission must explicitly prove matching consumers, live GPU mode, resource/scene identity and observable pre-copy proof.");
			const auto diagnostics = ReadAnimationSource(context, "Engine/Animation/AnimationSystemDiagnostics.cpp");
			const auto planning = ReadAnimationSource(context, "Engine/Animation/Frame/AnimationFramePlanning.cpp");
			context.Expect(ContainsAll(diagnostics, {"counters.activeAnimators = frameInputs.capturedInputWorkIndices.size()", "counters.completeCapturedAnimators = frameStorage.frameWork.size()", "presentation->topology.generations.rendererGeneration"})
				&& diagnostics.find("compactGpuVisualCaptures.stageMasks[captured.storageIndex]") != std::string::npos,
				"Compiled diagnostics count direct captures, preserve compact stage masks and report accepted renderer identity.");
			context.Expect(CountOccurrences(planning, "StampGpuVisualFrameIdentity(") >= 4U
				&& CountOccurrences(planning, "hasAuthoritativeConsumer = false") >= 2U
				&& CountOccurrences(planning, "hasControllerOrTransition = false") >= 2U
				&& CountOccurrences(planning, "hasAdvancedFeature = false") >= 2U
				&& ContainsAll(planning, {"hasContinuitySource = previous.initialized", "visual.generations.meshGeneration = renderIdentity.meshHeaderVersion", "visual.generations.rendererGeneration", "visual.generations.sceneGeneration", "visual.generations.frameIdLow", "visual.generations.frameIdHigh", "visual.renderIdentity.sceneGeneration = frameInputs.frameSceneGeneration"}),
				"Compiled complete/compact planners share frame-identity stamping and retain protected exclusion/continuity facts.");
		});

		registry.Add("Animation.CompactCapture.CohortsPresentationAndContiguousPartitions", [](TestContext& context) {
			const auto types = ReadAnimationSource(context, "Engine/Animation/Frame/AnimationFrameRuntime.h");
			const auto capture = ReadAnimationSource(context, "Engine/Animation/Frame/AnimationFrameCapture.cpp");
			const auto planning = ReadAnimationSource(context, "Engine/Animation/Frame/AnimationFramePlanning.cpp");
			const auto compact = ReadAnimationSource(context, "Engine/Animation/Frame/AnimationCompactEvaluation.cpp");
			const auto presentation = ReadAnimationSource(context, "Engine/Animation/Frame/AnimationAuthorityPublication.cpp");
			context.Expect(ContainsAll(types, {"struct CompactGpuVisualCohort", "std::vector<std::size_t> cohortIndices", "std::vector<CompactGpuVisualCohort> cohorts", "cohortLookup.equal_range(fingerprint)", "cohorts[candidate->second].Matches(record)"}),
				"Compact captures must share immutable cohort facts without trusting hash collisions.");
			context.Expect(!ClosureContains(context, "presentationVisible") && capture.find("candidateCompactGpuVisualCaptures.append(") != std::string::npos
				&& types.find("std::vector<GpuAnimationVisualInput> gpuPresentationVisuals") != std::string::npos
				&& presentation.find("materializeCompactLowering(compact, compactIndex)") != std::string::npos,
				"Compact owners retain narrow visual lanes and materialize cold topology lowering in the compiled presentation helper.");
			context.Expect(StripSourceWhitespace(capture).find("||visibility->hasActiveView") != std::string::npos
				&& capture.find("visibility->Contains(input.descriptor.ownerObjectId)") == std::string::npos,
				"Active multi-view capture must fail open instead of suppressing newly visible compact owners.");
			context.Expect(compact.find("partition.itemBegin, partition.itemEnd") != std::string::npos
				&& !ClosureContains(context, "compactIndex += partitionCount"), "Compact workers must traverse contiguous partitions.");
			context.Expect(planning.find("const auto rangePlanningStart") != std::string::npos
				&& !ClosureContains(context, "const auto itemPlanningStart")
				&& ContainsAll(compact, {"bool cancellationObserved = false", "((compactIndex - boundedBegin) & 15U) == 0U"}),
				"Range telemetry and bounded compact cancellation must remain in their actual compiled worker helpers.");
			context.Expect(presentation.find("candidate.use_count() == 1U") != std::string::npos
				&& presentation.find("persistentReuse.gpuPresentationFramePool") != std::string::npos,
				"Presentation may recycle only retired immutable frame storage.");
		});

		registry.Add("Animation.CompactCapture.PublishesPartitionedScalarRootsAndRetainsWarmEnvelope", [](TestContext& context) {
			const auto capture = ReadAnimationSource(context, "Engine/Animation/Frame/AnimationFrameCapture.cpp");
			const auto planning = ReadAnimationSource(context, "Engine/Animation/Frame/AnimationFramePlanning.cpp");
			const auto runtime = ReadAnimationSource(context, "Engine/Animation/Frame/AnimationRuntimeRootPreparation.cpp");
			const auto types = ReadAnimationSource(context, "Engine/Animation/Frame/AnimationFrameRuntime.h");
			const auto publication = ReadAnimationSource(context, "Engine/Animation/Frame/AnimationAuthorityPublication.cpp");
			const auto sealing = ReadAnimationSource(context, "Engine/Animation/Frame/AnimationAuthoritySealing.cpp");
			const auto commit = SourceRegion(context, publication, "AnimationSystem::Impl::CommitPublishedEvaluationCacheEntries(", "AnimationSystem::Impl::ApplySealedSpecialistSidecars(");
			context.Expect(ContainsAll(capture, {"kMinimumParallelCompactCaptureRows = 8192U", "kCompactCaptureRowsPerTask = 1024U", "proveCompactRange(0U", "captureFutures", "consumerClosureProven"})
				&& capture.find("&& !frameInputs.frameHasConsumerDeclarations") == std::string::npos,
				"Large crowds prove disjoint compact admission before deterministic SoA assembly.");
			context.Expect(ContainsAll(planning, {"const bool stableGpuVisualProof", "executionDecision.nextState.continuity = continuity", "ResolveAnimationExecutionMode(request, previous)"}), "Stable continuity keeps the fail-closed execution-mode resolver fallback.");
			context.Expect(ContainsAll(types, {"struct CompactExecutionDecision", "std::vector<CompactExecutionDecision>"}), "Compact planning retains narrow mode/reason/state records.");
			context.Expect(ContainsAll(runtime, {"kMinimumParallelCompactRuntimeRows = 8192U", "Animation: Build Compact Runtime Root Serial", "evaluationState.availableWorkerCount <= 1U"}), "Only large compact populations need parallel root construction.");
			context.Expect(types.find("struct CompactRuntimeState") != std::string::npos
				&& sealing.find("nextCompactRuntimeRoot->at(handle.slotIndex)") != std::string::npos
				&& ContainsAll(runtime, {"bindingContentGeneration = current->contentGeneration", "CompactRuntimeState& state = nextCompact->at(handle.slotIndex)"})
				&& ContainsAll(capture, {"const bool retainedCompactAdmissionProof", "compactRuntimeState->bindingContentGeneration", "!compactAdmissionHasExternalWork"})
				&& publication.find("compactRuntimeStates.store(") != std::string::npos
				&& !ClosureContains(context, "specialistSidecars->compactRuntime.push_back")
				&& commit.find("record.playback = update.playback") == std::string::npos
				&& !ClosureContains(context, "record.playback = update.playback"),
				"Compact state and admission proof publish through generation-checked roots, not post-publication playback writes.");
			context.Expect(ContainsAll(types, {"class RuntimeRecordRoot", "kPageCapacity = 128U", "pages_[pageIndex].use_count() != 1U"})
				&& runtime.find("next->Set(handle.slotIndex, std::move(record))") != std::string::npos,
				"Sparse complete exceptions clone their owned runtime pages before root exchange.");
			context.Expect(ContainsAll(publication, {"const bool deferStableEnvelope", "if(!deferStableEnvelope)", "if(deferStableEnvelope)"}), "Warm topology avoids broad envelopes until its proof fails.");
		});

		registry.Add("Animation.BindingBatch.ReusesImmutableAuthoredDefaults", [](TestContext& context) {
			const auto source = ReadAnimationSource(context, "Engine/Animation/AnimationSystemBindings.cpp");
			const auto batch = SourceRegion(context, source, "AnimationSystem::BindAnimatorPlaybackBatch(", "AnimationSystem::RefreshAnimatorDirectClipStreaming(");
			const auto binding = SourceRegion(context, source, "AnimationSystem::BindAnimatorPlaybackLocked(", "AnimationSystem::BindAnimatorPlaybackBatch(");
			context.Expect(ContainsAll(batch, {"struct PreparedBinding", "ImmutableBindPreparation immutable", "BindAnimatorPlaybackLocked("})
				&& ContainsAll(binding, {"record->parameters.assign(initialParameters.begin()", "record->layerWeights.assign(initialLayerWeights.begin()"}),
				"Batch preparation delegates immutable defaults while the binding helper retains mutable per-animator copies.");
		});

		registry.Add("Animation.DirectClipRefresh.PreparesCohortsBeforeRuntimeLock", [](TestContext& context) {
			const auto source = ReadAnimationSource(context, "Engine/Animation/AnimationSystemBindings.cpp");
			const auto batch = SourceRegion(context, source, "AnimationSystem::RefreshAnimatorDirectClipStreamingBatch(", "AnimationSystem::UpdateEvaluationGenerations(");
			const auto refresh = SourceRegion(context, source, "AnimationSystem::RefreshAnimatorDirectClipStreamingLocked(", "AnimationSystem::RefreshAnimatorDirectClipStreamingBatch(");
			context.Expect(Ordered(batch, {"struct PreparedDirectClipReplacement", "prepared.playback.Configure", "lock.lock();"}), "Direct-clip cohort preparation must precede runtime ownership acquisition within its actual batch definition.");
			context.Expect(batch.find("RefreshAnimatorDirectClipStreamingLocked(") != std::string::npos
				&& ContainsAll(refresh, {"preparedReplacementEventStream", "replacementEventStream = *preparedReplacementEventStream", "replacementPlayback = *preparedReplacementPlayback"}),
				"The delegated refresh helper adopts independent cursors from a prepared cohort template.");
		});

		registry.Add("Animation.DirectClipRefresh.GpuOnlyPriorRecoversAfterAssetPublication", [](TestContext& context) {
			const auto source = ReadAnimationSource(context, "Engine/Animation/Frame/AnimationRuntimeRootPreparation.cpp");
			const auto prepare = SourceRegion(context, source, "AnimationSystem::Impl::PrepareRuntimeRootCandidate(", "AnimationSystem::Impl::PrepareCompleteRuntimeRecords(");
			const auto complete = SourceRegion(context, source, "AnimationSystem::Impl::PrepareCompleteRuntimeRecords(", "} // namespace TerraEngine::Animation");
			context.Expect(prepare.find("PrepareCompleteRuntimeRecords(") != std::string::npos, "Runtime root preparation must explicitly delegate complete-record construction.");
			context.Expect(Ordered(complete, {"const bool preservesPrior", "item.selectedPublication->poseGeneration", "record->playback = item.playback", "record->poseGeneration = preservesPrior"}), "Retained GPU-only rows keep their owner pose generation while playback state advances.");
			const auto begin = complete.find("const bool preservesPrior");
			const auto end = complete.find("record->playback = item.playback", begin);
			context.Expect(begin != std::string::npos && end != std::string::npos && end > begin, "Retained validation region requires real ordered bounds.");
			context.Expect(complete.substr(begin, end - begin).find("item.selectedPublication->snapshot == nullptr") == std::string::npos,
				"Retained GPU-only recovery must not require a CPU snapshot.");
		});
	}
}
