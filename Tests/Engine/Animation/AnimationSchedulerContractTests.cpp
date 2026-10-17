#include "TestHarness.h"

#include "Animation/AnimationPresentationSystem.h"
#include "Animation/AnimationSystem.h"
#include "Animation/Diagnostics/AnimationDiagnostics.h"
#include "Animation/Graph/EvaluationProgram.h"
#include "AnimationClipCompiler.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/JobGraph.h"
#include "Core/Scheduling/SystemScheduler.h"
#include "Core/SystemBase.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::AssetPipeline;
		using TerraEngine::Scheduling::FrameWorkloadDeclaration;
		using TerraEngine::Scheduling::SchedulerPhaseDefinition;
		using TerraEngine::Scheduling::SchedulerResourceDefinition;

		// This declaration-only system lets the test build the real animation
		// ordering graph without initializing windows, physics, or the renderer.
		class DeclarationOnlySystem final : public SystemsBase {
		public:
			explicit DeclarationOnlySystem(TerraEngine::Scheduling::SystemDeclaration declaration)
				: declaration_(std::move(declaration))
			{
			}

			void Initialize() override {}
			void Update(float) override {}
			void Shutdown() override {}

			std::string SystemName() override
			{
				return declaration_.name;
			}

			TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override
			{
				return declaration_;
			}

		private:
			TerraEngine::Scheduling::SystemDeclaration declaration_;
		};

		// The real AnimationSystem remains the sole owner of live instances. This
		// RAII fixture requests protected lifecycle changes and guarantees cleanup
		// even if a contract expectation throws midway through the test.
		class AnimatorMembershipGuard final {
		public:
			AnimatorMembershipGuard(TerraEngine::Animation::AnimationSystem& system, std::size_t count)
				: system_(system)
			{
				handles_.reserve(count);
				for(std::size_t index = 0U; index < count; ++index) {
					TerraEngine::Animation::AnimatorInstanceDescriptor descriptor;
					descriptor.ownerObjectId[0] = static_cast<std::uint8_t>((index % 255U) + 1U);
					descriptor.ownerObjectId[1] = static_cast<std::uint8_t>((index / 255U) + 1U);
					descriptor.componentGeneration = 1U;
					const TerraEngine::Animation::AnimatorInstanceHandle handle = system_.CreateAnimatorInstance(descriptor);
					if(handle.IsValid()) handles_.push_back(handle);
				}
			}

			~AnimatorMembershipGuard()
			{
				for(const TerraEngine::Animation::AnimatorInstanceHandle handle : handles_) {
					(void)system_.InvalidateAnimatorInstance(handle);
				}
			}

			[[nodiscard]] std::size_t Size() const noexcept { return handles_.size(); }
			[[nodiscard]] TerraEngine::Animation::AnimatorInstanceHandle At(std::size_t index) const { return handles_.at(index); }
			[[nodiscard]] TerraEngine::Animation::AnimatorInstanceHandle First() const noexcept
			{
				return handles_.empty() ? TerraEngine::Animation::AnimatorInstanceHandle{} : handles_.front();
			}

		private:
			TerraEngine::Animation::AnimationSystem& system_;
			std::vector<TerraEngine::Animation::AnimatorInstanceHandle> handles_;
		};

		// Match the checked one-joint CPU/GPU integration fixtures so these contracts
		// exercise native binding, lowering, and evaluation instead of fabricated rows.
		AnimationGuid MakeStageGuid(std::uint8_t seed)
		{
			AnimationGuid guid;
			for(std::size_t index = 0U; index < guid.value.size(); ++index) {
				guid.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return guid;
		}


		AnimationMatrix4x4 MakeStageIdentityMatrix()
		{
			AnimationMatrix4x4 matrix;
			matrix.values[0U] = 1.0F;
			matrix.values[5U] = 1.0F;
			matrix.values[10U] = 1.0F;
			matrix.values[15U] = 1.0F;
			return matrix;
		}

		std::shared_ptr<const SkeletonAsset> MakeStageSkeleton()
		{
			auto skeleton = std::make_shared<SkeletonAsset>();
			skeleton->assetId = MakeStageGuid(40U);
			skeleton->compatibilityFingerprint = 0x4387aabbccdd1122ULL;
			skeleton->rootJointIndex = 0U;
			SkeletonJoint root;
			root.stableId = 101U;
			root.parentIndex = -1;
			root.normalizedSourcePath = "/root";
			root.localBindTransform.rotation = {0.0F, 0.0F, 0.0F, 1.0F};
			root.localBindTransform.scale = {1.0F, 1.0F, 1.0F};
			skeleton->joints.push_back(root);
			skeleton->inverseBindMatrices.push_back(MakeStageIdentityMatrix());
			return skeleton;
		}

		std::shared_ptr<const AnimationClipAsset> MakeStageClip(
			TestContext& context,
			const SkeletonAsset& skeleton,
			std::uint8_t assetSeed = 20U,
			float beginTranslationX = 0.0F,
			float endTranslationX = 1.0F,
			AnimationFixedTick durationTicks = 1)
		{
			AnimationClipCompileInput input;
			input.assetId = MakeStageGuid(assetSeed);
			input.skeletonAssetId = skeleton.assetId;
			input.compatibilityFingerprint = skeleton.compatibilityFingerprint;
			input.durationTicks = durationTicks;
			input.sampleRate = 60U;
			input.segmentDurationTicks = durationTicks;

			AnimationSourceTrack translation;
			translation.stableTargetId = skeleton.joints[0U].stableId;
			translation.kind = AnimationClipTrackKind::Translation;
			translation.interpolation = AnimationInterpolation::Linear;
			AnimationSourceKey begin;
			begin.tick = 0;
			begin.vectorValue = {beginTranslationX, 0.0F, 0.0F};
			AnimationSourceKey end;
			end.tick = durationTicks;
			end.vectorValue = {endTranslationX, 0.0F, 0.0F};
			translation.keys = {begin, end};
			input.tracks.push_back(std::move(translation));

			auto clip = std::make_shared<AnimationClipAsset>();
			AnimationClipCompileReport report;
			context.Expect(CompileAnimationClip(input, {}, *clip, report) == AnimationStatus::Ok,
				"The production integration fixture must compile one valid immutable clip.");
			return clip;
		}


		AnimatorPlaybackBinding MakeStageCpuBinding(TestContext& context)
		{
			AnimatorPlaybackBinding binding;
			binding.skeleton = MakeStageSkeleton();
			binding.clip = MakeStageClip(context, *binding.skeleton);
			binding.updatePolicy = AnimatorUpdatePolicy::Manual;
			binding.requestedExecutionPolicy =
				AnimationExecutionPolicy::ForceCpuDeterministicFallback;
			binding.fallbackCompatibilityFingerprint =
				binding.skeleton->compatibilityFingerprint;
			AnimationEvaluationExternalGenerations& generations =
				binding.evaluationGenerations;
			generations.consumerGeneration = 11U;
			generations.morphGeneration = 12U;
			generations.graphGeneration = 13U;
			generations.viewGeneration = 14U;
			generations.assetGeneration = 15U;
			generations.residencyGeneration = 16U;
			generations.materialGeneration = 17U;
			generations.rendererGeneration = 18U;
			generations.physicsGeneration = 19U;
			generations.deterministicGeneration = 20U;
			generations.unchangedInputReuseEligible = true;
			return binding;
		}


		template<typename Value>
		void WriteFixtureValue(std::vector<std::uint8_t>& bytes, std::size_t offset, const Value& value) {
			if (offset > bytes.size() || sizeof(Value) > bytes.size() - offset) throw std::out_of_range("GPU fixture byte range");
			std::memcpy(bytes.data() + offset, &value, sizeof(Value));
		}
		[[nodiscard]] std::shared_ptr<const GpuAnimationResidentPayloads>
		MakeStageResidentPayload(float constantTranslationX)
		{
			GpuAnimationResidentPayloads payload{};

			// One ATD1 directory selects one valid constant track. All T/R/S lanes
			// may share it because atlas relocation is concerned with addresses,
			// while these cases exercise CPU planning and ownership, not shader execution.
			GpuAnimationTrackDirectoryHeader directory{};
			directory.entryCount = 1U;
			GpuAnimationTrackDirectoryEntry entry{};
			entry.startTick = 0U;
			entry.endTick = 60U;
			entry.trackHeaderOffsetBytes =
				sizeof(GpuAnimationTrackDirectoryHeader)
				+ sizeof(GpuAnimationTrackDirectoryEntry);
			AnimationCodecTrackHeaderV1 track{};
			track.encoding = AnimationCodecValueEncoding::Vector3Unorm16;
			track.flags = kAnimationCodecConstantTrack;
			track.byteSize = sizeof(AnimationCodecTrackHeaderV1);
			track.keyCount = 1U;
			track.segmentStartTick = 0;
			track.segmentEndTick = 60;
			track.constantVector.x = constantTranslationX;
			payload.packedClipBytes.resize(
				entry.trackHeaderOffsetBytes + sizeof(track), 0U);
			WriteFixtureValue(payload.packedClipBytes, 0U, directory);
			WriteFixtureValue(
				payload.packedClipBytes,
				sizeof(directory),
				entry);
			WriteFixtureValue(
				payload.packedClipBytes,
				entry.trackHeaderOffsetBytes,
				track);

			// The frozen program layout is a 64-byte directory followed by one
			// 16-byte joint record containing T/R/S locators and a reserved lane.
			payload.programWords.assign(20U, 0U);
			payload.programWords[16U] = 0U;
			payload.programWords[17U] = 0U;
			payload.programWords[18U] = 0U;

			// The one-joint skeleton contains a directory, parent table, explicit
			// per-joint depth, and one 64-byte palette/inverse-bind record.
			payload.skeletonTables.resize(112U, 0U);
			const std::uint32_t parentTableOffset = 16U;
			const std::uint32_t depthTableOffset = 32U;
			const std::uint32_t levelCount = 1U;
			const std::uint32_t paletteTableOffset = 48U;
			const std::int32_t rootParent = -1;
			WriteFixtureValue(payload.skeletonTables, 0U, parentTableOffset);
			WriteFixtureValue(payload.skeletonTables, 4U, depthTableOffset);
			WriteFixtureValue(payload.skeletonTables, 8U, levelCount);
			WriteFixtureValue(payload.skeletonTables, 12U, paletteTableOffset);
			WriteFixtureValue(payload.skeletonTables, parentTableOffset, rootParent);
			const std::uint32_t rootDepth = 0U;
			WriteFixtureValue(payload.skeletonTables, depthTableOffset, rootDepth);
			const std::uint32_t paletteJoint = 0U;
			WriteFixtureValue(payload.skeletonTables, paletteTableOffset, paletteJoint);
			const std::array<float, 12U> identityRows{
				1.0F, 0.0F, 0.0F, 0.0F,
				0.0F, 1.0F, 0.0F, 0.0F,
				0.0F, 0.0F, 1.0F, 0.0F};
			WriteFixtureValue(
				payload.skeletonTables,
				paletteTableOffset + 16U,
				identityRows);

			// Bounds always consume the morph resource, even when this atlas-only
			// fixture has no animated targets. A checked zero-count AMT1 table keeps
			// the validated request aligned with production deformable bindings.
			GpuAnimationMorphTableHeader morphHeader{};
			GpuAnimationMorphBoundsRecord morphBounds{};
			morphBounds.minimum = {-1.0F, -1.0F, -1.0F, 0.0F};
			morphBounds.maximum = {1.0F, 1.0F, 1.0F, 0.0F};
			payload.morphInputs.resize(
				sizeof(morphHeader) + sizeof(morphBounds), 0U);
			WriteFixtureValue(payload.morphInputs, 0U, morphHeader);
			WriteFixtureValue(
				payload.morphInputs,
				sizeof(morphHeader),
				morphBounds);
			return std::make_shared<const GpuAnimationResidentPayloads>(
				std::move(payload));
		}



		AnimatorPlaybackBinding MakeStageCompactBinding(TestContext& context) {
			AnimatorPlaybackBinding binding;
			binding.skeleton = MakeStageSkeleton();
			binding.clip = MakeStageClip(context, *binding.skeleton, 20U, 1.0F, 1.0F, 60);
			binding.residentSegments.assign(binding.clip->segments.size(), 1U);
			binding.jointStableIds = {101U}; binding.jointParents = {-1};
			binding.requestedExecutionPolicy = AnimationExecutionPolicy::ForceGpuVisual;
			binding.updatePolicy = AnimatorUpdatePolicy::EveryFrame;
			binding.eventPolicy = AnimatorEventPolicy::Suppress;
			binding.playback.loop = false; binding.playOnBind = false;
			binding.rootMotionPolicy = AnimatorRootMotionPolicy::Ignore;
			binding.evaluationGenerations.residencyGeneration = 1U;
			// The same checked direct-clip program shape is authored by the real
			// component coordinator. No controller transition or CPU consumer is added.
			auto program = std::make_shared<EvaluationProgram>();
			program->controllerAssetId = MakeStageGuid(80U);
			program->compatibilityFingerprint = binding.skeleton->compatibilityFingerprint;
			program->scratchPlan = {64U, 16U};
			EvaluationInstruction instruction;
			instruction.sourceStableId = 1U; instruction.opcode = EvaluationOpcode::SampleClip;
			instruction.dependencyAssetId = binding.clip->assetId;
			instruction.dependencyGeneration = 1U;
			instruction.dependencyFingerprint = binding.skeleton->compatibilityFingerprint;
			program->instructions.push_back(instruction);
			program->bytecode.push_back(static_cast<std::uint8_t>(EvaluationOpcode::SampleClip));
			program->states.push_back({1U, 0U, true, false, false, 1U, EvaluationOpcode::SampleClip});
			EvaluationLayer layer; layer.stableId = 2U; layer.rootStateId = 1U; layer.defaultWeight = 1.0F;
			program->layers.push_back(layer);
			program->stableHash = ComputeEvaluationProgramStableHash(*program);
			EvaluationDispatchState checked;
			context.Expect(DispatchEvaluationProgramCpu(*program, checked) == AnimationStatus::Ok && checked.dispatchedInstructionCount == 1U,
				"The real direct-clip evaluation program must pass production validation.");
			binding.evaluationProgram = program;
			binding.controllerClips.push_back({1U, binding.clip, binding.residentSegments, 1U});
			binding.stateId = 1U;
			binding.gpuResidentPayloads = MakeStageResidentPayload(1.0F);
			binding.gpuProgramResourceIndex = 0U;
			binding.gpuVisual.skeletonTopologyId = 1U;
			binding.gpuVisual.compatibilityFingerprint = binding.skeleton->compatibilityFingerprint;
			binding.gpuVisual.clipResourceIndex = 0U; binding.gpuVisual.skeletonResourceIndex = 0U;
			binding.gpuVisual.morphResourceIndex = 0U;
			binding.gpuVisual.jointCount = 1U; binding.gpuVisual.paletteEntryCount = 1U;
			binding.gpuVisual.palettePartitionCount = 1U; binding.gpuVisual.looping = true;
			binding.gpuVisual.generations.resourceGeneration = 1U;
			binding.gpuVisual.renderIdentity.meshId[0] = 1U;
			binding.gpuVisual.renderIdentity.meshHeaderVersion = 1U;
			binding.gpuVisual.renderIdentity.deformablePayloadGeneration = 1U;
			binding.gpuVisual.renderIdentity.rendererGeneration = 1U;
			binding.gpuVisual.renderIdentity.sceneGeneration = ObjectManager::instance().GetSceneGeneration();
			binding.gpuVisual.renderIdentity.skeletonCompatibilityFingerprint = binding.skeleton->compatibilityFingerprint;
			auto fast = std::make_shared<AnimatorPlaybackBinding::GpuFastBinding>();
			fast->programResourceIndex = 0U; fast->visual = binding.gpuVisual;
			fast->residentPayloads = binding.gpuResidentPayloads;
			fast->residentByteCount = fast->residentPayloads->programWords.size() * sizeof(std::uint32_t)
				+ fast->residentPayloads->packedClipBytes.size() + fast->residentPayloads->skeletonTables.size() + fast->residentPayloads->morphInputs.size();
			binding.gpuFastBinding = fast;
			binding.gpuExecution = {true, true, true, true, static_cast<double>(fast->residentByteCount), 1.0e9};
			// Use the checked default path, never the planner's prevalidated flags.
			GpuAnimationLoweringInput lowering;
			lowering.program = {program, program->stableHash, 0U}; lowering.visual = binding.gpuVisual;
			lowering.visual.stableInstanceId = 1U;
			lowering.visual.generations.instanceGeneration = 1U;
			lowering.visual.generations.componentGeneration = 1U;
			lowering.visual.generations.workGeneration = 1U;
			lowering.visual.generations.programGeneration = 1U;
			lowering.visual.generations.controllerGeneration = 1U;
			lowering.visual.generations.clipGeneration = 1U;
			lowering.visual.generations.skeletonGeneration = 1U;
			lowering.visual.generations.meshGeneration = 1U;
			lowering.visual.generations.rendererGeneration = 1U;
			lowering.visual.generations.sceneGeneration = static_cast<std::uint32_t>(ObjectManager::instance().GetSceneGeneration());
			lowering.visual.generations.frameIdLow = 1U;
			lowering.effectiveMode = AnimationEffectiveExecutionMode::GpuVisual;
			lowering.residentPayloads = binding.gpuResidentPayloads;
			GpuAnimationWorkRequest lowered;
			context.Expect(LowerGpuAnimationWorkRequest(lowering, lowered) == GpuAnimationLoweringStatus::Ok
				&& ValidateGpuAnimationResidentPayloads(lowered, *binding.gpuResidentPayloads),
				"The production GPU lowerer must accept the exact fixture program and resident bytes.");
			return binding;
		}


		// Run the real declarations rather than reimplementing animation stages.
		bool ExecuteStageFrame(AnimationSystem& system)
		{
			const auto plan = TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
				system.DeclareFrameWorkloads(0.0F), "animation-stage-contract");
			return plan.valid && plan.completed;
		}

		void InvokeStage(const FrameWorkloadDeclaration& stage, std::size_t count,
			TerraEngine::Scheduling::JobGraphContext& graphContext)
		{
			if(stage.rangeWork) stage.rangeWork({0U, 1U, 0U, count}, graphContext);
			else if(stage.work) stage.work(graphContext);
			else throw std::runtime_error("The production animation callback is missing.");
		}

		void CheckProductionWorkload(TestContext& context, std::size_t completeCount,
			std::size_t compactCount)
		{
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			AnimatorMembershipGuard membership(system, completeCount + compactCount);
			context.ExpectEqual(completeCount + compactCount, membership.Size(), "Every fixture owner must be admitted.");
			for(std::size_t index = 0; index < membership.Size(); ++index) {
				context.Expect(system.BindAnimatorPlayback(membership.At(index), index < completeCount
					? MakeStageCpuBinding(context) : MakeStageCompactBinding(context)) == AnimationStatus::Ok,
					"Checked CPU and resident GPU fixtures must bind through production admission.");
			}
			context.Expect(ExecuteStageFrame(system), "Initial binding admission must run all nine declared stages.");
			const auto admitted = system.LoadPublishedAuthorityFrame();
			for(std::size_t index = 0; index < membership.Size(); ++index) {
				const auto* row = admitted ? admitted->Find(membership.At(index)) : nullptr;
				context.Expect(row && row->effectiveExecutionMode == (index < completeCount
					? AnimationEffectiveExecutionMode::CpuDeterministicFallback : AnimationEffectiveExecutionMode::GpuVisual),
					"Initial complete admission must prove the actual selected CPU/GPU mode before compact capture.");
			}
			for(int frame = 0; frame < 2; ++frame) context.Expect(ExecuteStageFrame(system), "Compact warmup must execute the same nine declared stages.");
			const auto held = system.LoadPublishedAuthorityFrame();
			context.Expect(held && held->IsValid(), "The actual stages must publish a valid authority root.");
			context.ExpectEqual(completeCount, system.ReadProductionIntegrationSnapshot().itemCount,
				"Only complete owners may occupy the complete evaluation work plan after compact warmup.");
			for(std::size_t index = 0; index < membership.Size(); ++index) {
				const auto* row = held ? held->Find(membership.At(index)) : nullptr;
				if(index < completeCount) {
					context.Expect(row && row->effectiveExecutionMode == AnimationEffectiveExecutionMode::CpuDeterministicFallback
						&& row->snapshot && row->snapshot->pose,
						"Complete evaluation must preserve its selected CPU mode and sealed pose for readers.");
				} else {
					// The baseline compact writer leaves row mode/status metadata at
					// their defaults. Prove the actual compact output with its native
					// owner-qualified GPU sidecar, without claiming that metadata is correct.
					bool matchingGpuCandidate = false;
					if(held && held->sidecars) {
						const auto handle = membership.At(index);
						matchingGpuCandidate = std::any_of(held->sidecars->gpuCandidates.begin(), held->sidecars->gpuCandidates.end(),
							[&](const AnimationSpecialistRecord& candidate) {
								return candidate.key.slot == handle.slotIndex && candidate.key.lifetimeGeneration == handle.generation
									&& candidate.key.sessionGeneration == held->sessionGeneration
									&& candidate.IsValidFor(held->authorityGeneration) && candidate.kind == 1U
									&& candidate.status == AnimationStatus::Ok;
							});
					}
					context.Expect(row && !row->snapshot && !row->safeFallback && matchingGpuCandidate,
						"Each compact owner must publish a successful GPU sidecar in the exact authority/session without a CPU snapshot or fallback.");
				}
			}
			const auto* heldRow = held ? held->Find(membership.First()) : nullptr;
			const auto heldTick = heldRow ? heldRow->tick : -1;
			const auto heldPages = held ? held->pages : nullptr;
			const auto heldSidecars = held ? held->sidecars : nullptr;
			AnimationCommand command;
			command.target = membership.First(); command.kind = AnimationCommandKind::Seek;
			command.playbackTick = 1; command.producerId = 680U + completeCount * 10U + compactCount;
			command.producerSequence = 1; command.correlationId = command.producerId * 100U;
			context.Expect(system.SubmitCommand(command).status == AnimationStatus::Queued, "An exact seek must enter production command capture.");
			context.Expect(ExecuteStageFrame(system), "The command-bearing frame must commit through the same nine stages.");
			const auto published = system.LoadPublishedAuthorityFrame();
			const auto receipt = system.ReadCommandReceipt(command.producerId, command.producerSequence, command.correlationId);
			context.Expect(published && published != held && receipt.status == AnimationStatus::Ok
				&& receipt.publicationGeneration == published->authorityGeneration
				&& system.LoadAnimatorPlaybackState(command.target).absoluteTick == command.playbackTick,
				"The real runtime exchange and exact terminal receipt must name the same authority generation.");
			context.Expect(held && held->pages == heldPages && held->sidecars == heldSidecars
				&& held->Find(command.target) == heldRow && heldRow && heldRow->tick == heldTick,
				"A pinned reader must retain its exact immutable rows, pages, and sidecars across a successor exchange.");
		}

		void CheckGraphCancellationBoundary(TestContext& context, bool afterExchange)
		{
			using namespace TerraEngine::Scheduling;
			AnimationSystem& system = AnimationSystem::Instance(); system.Initialize();
			AnimatorMembershipGuard membership(system, 1U);
			context.Expect(system.BindAnimatorPlayback(membership.First(), MakeStageCpuBinding(context)) == AnimationStatus::Ok,
				"A fresh stopped CPU binding must admit a cache-eligible native evaluation.");
			const auto prior = system.LoadPublishedAuthorityFrame();
			const auto priorTick = system.LoadAnimatorPlaybackState(membership.First()).absoluteTick;
			const auto initialCache = system.ReadProductionIntegrationSnapshot().cacheEntryCount;
			AnimationCommand command;
			command.target = membership.First(); command.kind = AnimationCommandKind::Seek; command.playbackTick = 1;
			command.producerId = afterExchange ? 698U : 697U; command.producerSequence = 1; command.correlationId = command.producerId * 100U;
			context.Expect(system.SubmitCommand(command).status == AnimationStatus::Queued, "The cancellation fixture must queue an actual command.");
			auto stages = system.DeclareFrameWorkloads(0.0F);
			context.ExpectEqual(std::size_t{9U}, stages.size(), "The baseline must retain all nine production callbacks.");
			if(stages.size() != 9U) return;
			JobGraph graph("animation-cancellation-boundary", "Real animation callback cancellation");
			bool completedCallbacks = false;
			std::shared_ptr<const AnimationAuthorityFrame> committed;
			JobGraphJobDeclaration job;
			job.name = "InvokeActualAnimationStages"; job.cancellable = false;
			// A non-cancellable protected harness lets the actual callbacks observe
			// one real cancelled JobGraphContext. This tests callback obligations,
			// not the scheduler's separate cancelled-node admission policy.
			job.work = [&](JobGraphContext& graphContext) {
				for(std::size_t index = 0; index < 6U; ++index) InvokeStage(stages[index], 1U, graphContext);
				if(afterExchange) {
					InvokeStage(stages[6], 1U, graphContext);
					committed = system.LoadPublishedAuthorityFrame();
					context.Expect(committed && committed != prior, "Late cancellation must follow an observed real authority exchange.");
				}
				graph.RequestCancellation("exact authority boundary");
				context.Expect(graphContext.IsCancellationRequested(), "The remaining callbacks must receive actual graph cancellation.");
				if(!afterExchange) InvokeStage(stages[6], 1U, graphContext);
				InvokeStage(stages[7], 1U, graphContext);
				InvokeStage(stages[8], 1U, graphContext);
				completedCallbacks = true;
			};
			(void)graph.AddJob(std::move(job));
			const auto summary = graph.Execute();
			context.Expect(summary.valid && completedCallbacks && summary.cancelled,
				"The protected harness must finish both dependent callbacks under real cancellation.");
			const auto receipt = system.ReadCommandReceipt(command.producerId, command.producerSequence, command.correlationId);
			if(afterExchange) {
				context.Expect(committed && system.LoadPublishedAuthorityFrame() == committed
					&& receipt.status == AnimationStatus::Ok && receipt.publicationGeneration == committed->authorityGeneration
					&& system.LoadAnimatorPlaybackState(command.target).absoluteTick == command.playbackTick,
					"The cancelled finalizer must publish the committed ordinary receipt without revoking runtime progress.");
				context.Expect(system.ReadProductionIntegrationSnapshot().cacheEntryCount > initialCache,
					"The real dependent cache callback must complete committed cache work despite late graph cancellation.");
				context.Expect(ExecuteStageFrame(system), "A healthy successor must remain possible after cancelled specialist finalization.");
				const auto historical = system.ReadCommandReceipt(command.producerId, command.producerSequence, command.correlationId);
				context.Expect(historical.status == AnimationStatus::Ok && historical.publicationGeneration == receipt.publicationGeneration,
					"Subsequent capture must preserve the exact historical committed receipt.");
			} else {
				context.Expect(system.LoadPublishedAuthorityFrame() == prior && receipt.status == AnimationStatus::Cancelled
					&& receipt.publicationGeneration == 0U && system.LoadAnimatorPlaybackState(command.target).absoluteTick == priorTick,
					"Cancellation before exchange must settle the command without publishing any partial root or playback mutation.");
				context.ExpectEqual(initialCache, system.ReadProductionIntegrationSnapshot().cacheEntryCount,
					"An uncommitted candidate must not populate the published evaluation cache.");
			}
		}

		bool Contains(const std::vector<std::string>& values, const std::string& expected)
		{
			return std::find(values.begin(), values.end(), expected) != values.end();
		}

		const SchedulerPhaseDefinition* FindPhase(
			const std::vector<SchedulerPhaseDefinition>& phases,
			const std::string& name)
		{
			const auto found = std::find_if(phases.begin(), phases.end(), [&name](const SchedulerPhaseDefinition& phase) {
				return phase.name == name;
			});
			return found == phases.end() ? nullptr : &(*found);
		}

		const SchedulerResourceDefinition* FindResource(
			const std::vector<SchedulerResourceDefinition>& resources,
			const std::string& name)
		{
			const auto found = std::find_if(resources.begin(), resources.end(), [&name](const SchedulerResourceDefinition& resource) {
				return resource.name == name;
			});
			return found == resources.end() ? nullptr : &(*found);
		}

		const FrameWorkloadDeclaration* FindWorkload(
			const std::vector<FrameWorkloadDeclaration>& workloads,
			const std::string& name)
		{
			const auto found = std::find_if(workloads.begin(), workloads.end(), [&name](const FrameWorkloadDeclaration& workload) {
				return workload.workloadName == name;
			});
			return found == workloads.end() ? nullptr : &(*found);
		}

		TerraEngine::Scheduling::SystemDeclaration MakeDependencySystemDeclaration(
			std::string name,
			std::string phase)
		{
			TerraEngine::Scheduling::SystemDeclaration declaration;
			declaration.name = std::move(name);
			declaration.phase = std::move(phase);
			declaration.mainThread = true;
			declaration.profilingLabel = declaration.name;
			declaration.usesDefaultMetadata = false;
			return declaration;
		}

		void ExpectWorkerWorkload(
			TestContext& context,
			const FrameWorkloadDeclaration& workload,
			TerraEngine::Scheduling::FrameWorkloadPartitionPolicy expectedPartitionPolicy =
				TerraEngine::Scheduling::FrameWorkloadPartitionPolicy::DeterministicRanges)
		{
			context.Expect(workload.workerEligible, "Animation preparation must be worker eligible.");
			context.Expect(!workload.mainThreadRequired, "Worker-safe animation preparation must not require the main thread.");
			context.Expect(workload.phaseKind == TerraEngine::Scheduling::WorkerSystemPhaseKind::WorkerPrepare,
				"Worker animation work must identify itself as preparation.");
			context.Expect(workload.boundaryClassification == TerraEngine::Scheduling::HardBoundaryClassification::WorkerSafe,
				"Worker animation work must expose a worker-safe diagnostic classification.");
			context.Expect(workload.partitionPolicy == expectedPartitionPolicy,
				expectedPartitionPolicy == TerraEngine::Scheduling::FrameWorkloadPartitionPolicy::DeterministicRanges
					? "Animation worker work must use stable range partitions rather than per-animator jobs."
					: "The transactional presentation merge must remain one explicitly declared worker task.");
			context.Expect(workload.mutationPolicy == TerraEngine::Scheduling::FrameWorkloadMutationPolicy::None,
				"Worker animation work must only write owned prepared output, never live scene mutation state.");
			context.Expect(!workload.safeSubwork.empty(), "Worker animation work must explain its safe immutable-input work.");
			context.Expect(!workload.profilingLabel.empty(), "Every animation workload must have a visible profiling label.");
		}

		void ExpectProtectedWorkload(TestContext& context, const FrameWorkloadDeclaration& workload)
		{
			context.Expect(!workload.workerEligible, "Animation capture/publication must stay protected.");
			context.Expect(workload.mainThreadRequired, "Animation capture/publication must require the protected coordinator boundary.");
			context.Expect(workload.boundaryClassification == TerraEngine::Scheduling::HardBoundaryClassification::Protected,
				"Animation capture/publication must expose a protected diagnostic classification.");
			context.Expect(workload.hardBoundary, "Animation capture/publication must be visible as a hard boundary.");
			context.Expect(workload.mutationPolicy == TerraEngine::Scheduling::FrameWorkloadMutationPolicy::DeferredOnly,
				"Protected animation work must retain typed/deferred mutation discipline.");
			context.Expect(!workload.protectedReason.empty(), "Protected animation work must explain why it cannot run on workers.");
			context.Expect(!workload.profilingLabel.empty(), "Every protected animation boundary must have a profiling label.");
		}
	}

	void RegisterAnimationSchedulerContractTests(TestRegistry& registry)
	{
		registry.Add("Animation.Scheduler.LifecycleRetirementRejectsSealedCallbacks", [](TestContext& context) {
			using namespace TerraEngine::Scheduling;
			AnimationSystem& system = AnimationSystem::Instance(); system.Initialize();
			AnimatorMembershipGuard membership(system, 1U);
			context.Expect(system.BindAnimatorPlayback(membership.First(), MakeStageCpuBinding(context)) == AnimationStatus::Ok
				&& ExecuteStageFrame(system), "A real complete frame must establish the lifecycle fixture root.");
			const auto held = system.LoadPublishedAuthorityFrame();
			const auto* heldRow = held ? held->Find(membership.First()) : nullptr;
			// Authority rows use the captured frame tick; playback owns a separate
			// stopped clip cursor. Pin both identities rather than equating clocks.
			const auto heldFrameTick = heldRow ? heldRow->tick : -1;
			const auto heldSnapshot = heldRow ? heldRow->snapshot : nullptr;
			const auto heldPose = heldSnapshot ? heldSnapshot->pose : nullptr;
			context.Expect(heldRow && heldSnapshot && heldPose,
				"The lifecycle fixture must acquire an actual sealed CPU pose before retirement.");
			const auto priorTick = system.LoadAnimatorPlaybackState(membership.First()).absoluteTick;
			AnimationCommand command;
			command.target = membership.First(); command.kind = AnimationCommandKind::Seek; command.playbackTick = 1;
			command.producerId = 699U; command.producerSequence = 1U; command.correlationId = 69901U;
			context.Expect(system.SubmitCommand(command).status == AnimationStatus::Queued, "A real command must enter the candidate retired before exchange.");
			auto stages = system.DeclareFrameWorkloads(0.0F);
			context.ExpectEqual(std::size_t{9U}, stages.size(), "Lifecycle rejection must exercise the original nine callbacks.");
			if(stages.size() != 9U) return;
			JobGraph graph("animation-lifecycle-stage-contract");
			JobGraphJobDeclaration job; job.name = "RetireAfterActualSeal";
			job.work = [&](JobGraphContext& graphContext) {
				for(std::size_t index = 0; index < 6U; ++index) InvokeStage(stages[index], 1U, graphContext);
				context.Expect(system.NotifyLifecycleInvalidation(AnimationLifecycleBoundary::Renderer) == AnimationStatus::Ok,
					"Protected lifecycle invalidation must retire the candidate and fresh authority lookup.");
				for(std::size_t index = 6U; index < 9U; ++index) InvokeStage(stages[index], 1U, graphContext);
			};
			(void)graph.AddJob(std::move(job));
			const auto summary = graph.Execute();
			const auto receipt = system.ReadCommandReceipt(command.producerId, command.producerSequence, command.correlationId);
			context.Expect(summary.valid && summary.completed && !system.LoadPublishedAuthorityFrame()
				&& receipt.status == AnimationStatus::Cancelled && receipt.publicationGeneration == 0U
				&& system.LoadAnimatorPlaybackState(command.target).absoluteTick == priorTick,
				"Previously sealed callbacks must not resurrect a retired root or commit its queued playback changes.");
			context.Expect(held && held->IsValid() && held->Find(command.target) == heldRow
				&& heldRow && heldRow->snapshot == heldSnapshot && heldSnapshot->pose == heldPose
				&& heldRow->tick == heldFrameTick,
				"The reader pinned before lifecycle retirement must retain its exact sealed pose, row, and captured frame tick.");
		});

		registry.Add("Animation.Scheduler.ProductionCompleteStagesPreserveReaders", [](TestContext& context) { CheckProductionWorkload(context, 1U, 0U); });
		registry.Add("Animation.Scheduler.ProductionCompactStagesPreserveReaders", [](TestContext& context) { CheckProductionWorkload(context, 0U, 1U); });
		registry.Add("Animation.Scheduler.ProductionMixedStagesPreserveReaders", [](TestContext& context) { CheckProductionWorkload(context, 1U, 1U); });
		registry.Add("Animation.Scheduler.GraphCancellationBeforeAuthorityExchange", [](TestContext& context) { CheckGraphCancellationBoundary(context, false); });
		registry.Add("Animation.Scheduler.GraphCancellationAfterAuthorityExchange", [](TestContext& context) { CheckGraphCancellationBoundary(context, true); });

		registry.Add("Animation.Scheduler.PhaseOrderAndResourcesAreStable", [](TestContext& context) {
			const std::vector<SchedulerPhaseDefinition> phases = TerraEngine::Scheduling::DefaultSchedulerPhases();
			const SchedulerPhaseDefinition* scripting = FindPhase(phases, "Scripting");
			const SchedulerPhaseDefinition* animation = FindPhase(phases, "Animation");
			const SchedulerPhaseDefinition* physics = FindPhase(phases, "Physics");
			const SchedulerPhaseDefinition* presentation = FindPhase(phases, "AnimationPresentation");
			const SchedulerPhaseDefinition* ui = FindPhase(phases, "UI");
			const SchedulerPhaseDefinition* render = FindPhase(phases, "Render");
			context.Expect(scripting != nullptr && animation != nullptr && physics != nullptr
				&& presentation != nullptr && ui != nullptr && render != nullptr,
				"The complete animation phase chain must be declared.");
			if(scripting == nullptr || animation == nullptr || physics == nullptr
				|| presentation == nullptr || ui == nullptr || render == nullptr) {
				return;
			}
			context.Expect(scripting->order < animation->order && animation->order < physics->order
				&& physics->order < presentation->order && presentation->order < ui->order
				&& ui->order < render->order,
				"Animation authority must run after scripts and before physics; presentation must run after physics and before UI/render.");

			const std::vector<SchedulerResourceDefinition> resources = TerraEngine::Scheduling::DefaultSchedulerResources();
			const std::array<const char*, 19> requiredResources = {
				TerraEngine::Scheduling::kResourceAnimatorComponents,
				TerraEngine::Scheduling::kResourceAnimationCommandQueues,
				TerraEngine::Scheduling::kResourceAnimationFrameInputs,
				TerraEngine::Scheduling::kResourceAnimationEvaluationPrograms,
				TerraEngine::Scheduling::kResourceAnimationResidencyCapabilities,
				TerraEngine::Scheduling::kResourceAnimationExecutionModes,
				TerraEngine::Scheduling::kResourceAnimationConsumerMasks,
				TerraEngine::Scheduling::kResourceAnimationWorkPackets,
				TerraEngine::Scheduling::kResourceAnimationVisualPackets,
				TerraEngine::Scheduling::kResourceAnimationClipSegments,
				TerraEngine::Scheduling::kResourceAnimationRollbackState,
				TerraEngine::Scheduling::kResourceAnimationAuthoritativeOutputs,
				TerraEngine::Scheduling::kResourceAnimationRuntimeState,
				TerraEngine::Scheduling::kResourceAnimationPhysicsHandoffQueue,
				TerraEngine::Scheduling::kResourceAnimationManagedCallbackBatches,
				TerraEngine::Scheduling::kResourceAnimationRagdollMappings,
				TerraEngine::Scheduling::kResourceAnimationRecordingSamples,
				TerraEngine::Scheduling::kResourceAnimationPresentationPrepared,
				TerraEngine::Scheduling::kResourceAnimationPresentationPlan
			};
			for(const char* required : requiredResources) {
				const SchedulerResourceDefinition* resource = FindResource(resources, required);
				context.Expect(resource != nullptr, "Every animation handoff resource must be visible in scheduler diagnostics.");
				if(resource != nullptr) {
					context.Expect(!resource->description.empty(), "Animation scheduler resources must explain their ownership and visibility.");
				}
			}
		});

		registry.Add("Animation.Scheduler.SystemDeclarationsExposeProtectedHandoffs", [](TestContext& context) {
			static_assert(std::derived_from<TerraEngine::Animation::AnimationSystem, SystemsBase>);
			static_assert(std::derived_from<TerraEngine::Animation::AnimationPresentationSystem, SystemsBase>);

			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			TerraEngine::Animation::AnimationPresentationSystem& presentationSystem = TerraEngine::Animation::AnimationPresentationSystem::Instance();
			const TerraEngine::Scheduling::SystemDeclaration animation = animationSystem.SchedulerDeclaration();
			const TerraEngine::Scheduling::SystemDeclaration presentation = presentationSystem.SchedulerDeclaration();

			context.ExpectEqual(std::string("Animation"), animation.phase,
				"AnimationSystem must own the pre-physics Animation phase.");
			context.Expect(Contains(animation.after, "ScriptingSystem") && Contains(animation.before, "PhysicsSystem"),
				"AnimationSystem must be explicitly ordered between scripts and physics.");
			context.Expect(animation.requiresFlushedSceneMutations,
				"Animation capture must consume the last completed structural flush; script-submitted membership follows the documented next-frame rule.");
			context.Expect(animation.boundaryClassification == TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe
				&& animation.hardBoundary && !animation.safeSubwork.empty() && !animation.protectedCommit.empty(),
				"AnimationSystem must diagnose both worker planning and protected authority publication.");
			context.Expect(animation.omitWhenNoFrameWorkloads
				&& !animation.usesDefaultMetadata && animation.profilingLabel == "Animation",
				"AnimationSystem must expose stable non-default scheduling diagnostics.");

			context.ExpectEqual(std::string("AnimationPresentation"), presentation.phase,
				"AnimationPresentationSystem must own the post-physics presentation phase.");
			context.Expect(Contains(presentation.after, "PhysicsSystem")
				&& Contains(presentation.before, "UISystem") && Contains(presentation.before, "RenderSystem"),
				"Animation presentation must be explicitly ordered after physics and before UI/render.");
			context.Expect(!presentation.requiresFlushedSceneMutations,
				"Presentation must consume immutable physics snapshots without forcing an extra live-scene flush.");
			context.Expect(presentation.boundaryClassification == TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe
				&& presentation.hardBoundary && !presentation.safeSubwork.empty() && !presentation.protectedCommit.empty(),
				"AnimationPresentationSystem must diagnose worker preparation and protected plan publication.");
			context.Expect(presentation.omitWhenNoFrameWorkloads
				&& !presentation.usesDefaultMetadata && presentation.profilingLabel == "AnimationPresentation",
				"Animation presentation must expose stable non-default scheduling diagnostics.");

			DeclarationOnlySystem scripting(MakeDependencySystemDeclaration("ScriptingSystem", "Scripting"));
			DeclarationOnlySystem physics(MakeDependencySystemDeclaration("PhysicsSystem", "Physics"));
			DeclarationOnlySystem ui(MakeDependencySystemDeclaration("UISystem", "UI"));
			DeclarationOnlySystem render(MakeDependencySystemDeclaration("RenderSystem", "Render"));
			TerraEngine::Scheduling::SystemScheduler scheduler;
			scheduler.RegisterSystem(scripting);
			scheduler.RegisterSystem(animationSystem);
			scheduler.RegisterSystem(physics);
			scheduler.RegisterSystem(presentationSystem);
			scheduler.RegisterSystem(ui);
			scheduler.RegisterSystem(render);
			const TerraEngine::Scheduling::ScheduleSummary& summary = scheduler.BuildSchedule();
			context.Expect(summary.valid, "The animation authority/presentation declarations must form a valid scheduler graph.");
			context.Expect(summary.systems.size() >= 4U, "A valid animation fixture must contain the four authority/handoff systems.");
			if(summary.systems.size() >= 4U) {
				context.ExpectEqual(std::string("ScriptingSystem"), summary.systems[0], "Scripting must begin the animation contract fixture.");
				context.ExpectEqual(std::string("AnimationSystem"), summary.systems[1], "Animation authority must follow scripting.");
				context.ExpectEqual(std::string("PhysicsSystem"), summary.systems[2], "Physics must consume animation authority next.");
				context.ExpectEqual(std::string("AnimationPresentationSystem"), summary.systems[3], "Presentation must consume the current physics result.");
			}
		});

		registry.Add("Animation.Presentation.OwnerMappingUsesStableSlotGenerationIdentity", [](TestContext& context) {
			TerraEngine::Animation::GpuAnimationOwnerObjectMapping mapping{};
			mapping.ownerObjectId[0] = 71U;
			mapping.instanceSlot = 4U;
			mapping.instanceGeneration = 5U;
			mapping.componentGeneration = 6U;
			mapping.stableInstanceId = (static_cast<std::uint64_t>(mapping.instanceSlot) << 32U)
				| static_cast<std::uint64_t>(mapping.instanceGeneration);
			context.Expect(mapping.IsComplete(),
				"Presentation must preserve the established slot-high/generation-low stable identity for GPU-only object matching.");
			context.Expect(!mapping.HasPublishedPacketIndex(),
				"A producer mapping must remain unusable for direct render lookup until lowering publishes its exact packet index.");
			mapping.workPacketIndex = 19U;
			context.Expect(mapping.HasPublishedPacketIndex(),
				"A lowered owner mapping must expose its exact packet index without a renderer-side identity table.");

			mapping.stableInstanceId = (static_cast<std::uint64_t>(mapping.instanceGeneration) << 32U)
				| static_cast<std::uint64_t>(mapping.instanceSlot + 1U);
			context.Expect(!mapping.IsComplete(),
				"The former generation-high/slot-plus-one encoding must fail closed before RenderSystem can bind a GPU-only packet.");
		});

		registry.Add("Animation.Presentation.OwnerLookupFindsFirstExactMapping", [](TestContext& context) {
			TerraEngine::Animation::PublishedGpuAnimationPresentationPlan plan;
			for(std::uint32_t index = 0U; index < 32U; ++index) {
				TerraEngine::Animation::GpuAnimationOwnerObjectMapping mapping;
				mapping.ownerObjectId[0] = static_cast<std::uint8_t>(index + 1U);
				mapping.stableInstanceId = static_cast<std::uint64_t>(index + 1U);
				mapping.workPacketIndex = index;
				plan.ownerObjectMappings.push_back(mapping);
			}
			// A duplicate owner remains adjacent after canonical publication sorting.
			// The lookup must name the first record so RenderSystem can detect ambiguity.
			TerraEngine::Animation::GpuAnimationOwnerObjectMapping duplicate =
				plan.ownerObjectMappings[11U];
			duplicate.workPacketIndex = 99U;
			plan.ownerObjectMappings.insert(
				plan.ownerObjectMappings.begin() + 12U, duplicate);
			plan.BuildOwnerObjectLookup();

			const TerraEngine::Animation::GpuAnimationOwnerObjectMapping* found =
				plan.FindFirstOwnerObjectMapping(plan.ownerObjectMappings[11U].ownerObjectId);
			context.Expect(found == &plan.ownerObjectMappings[11U]
				&& found->workPacketIndex == 11U,
				"The immutable owner index must resolve the first exact sorted mapping.");
			TerraEngine::UUID missing{};
			missing[0] = 201U;
			context.Expect(plan.FindFirstOwnerObjectMapping(missing) == nullptr,
				"An absent owner must fail closed without falling through to another bucket.");
		});

		registry.Add("Animation.Binding.SharedArraysDetachOnlyOnMutation", [](TestContext& context) {
			TerraEngine::Animation::SharedCopyVector<std::uint64_t> original{
				11U, 22U, 33U};
			TerraEngine::Animation::SharedCopyVector<std::uint64_t> copied = original;
			context.Expect(original.data() == copied.data(),
				"A frame binding copy must retain the same immutable array storage.");

			copied.Mutable()[1U] = 99U;
			context.Expect(original.data() != copied.data(),
				"The first mutation must detach without changing an older frame snapshot.");
			context.ExpectEqual(static_cast<std::uint64_t>(22U), original[1U],
				"The original immutable binding array must preserve its captured value.");
			context.ExpectEqual(static_cast<std::uint64_t>(99U), copied[1U],
				"The detached writer must publish its replacement value independently.");
		});

		registry.Add("Animation.Scheduler.WorkloadsSeparateWorkerPreparationFromPublication", [](TestContext& context) {
			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			TerraEngine::Animation::AnimationPresentationSystem& presentationSystem = TerraEngine::Animation::AnimationPresentationSystem::Instance();
			AnimatorMembershipGuard membership(animationSystem, 256U);
			context.ExpectEqual(static_cast<std::size_t>(256U), membership.Size(),
				"The scheduler fixture must create every protected runtime membership record.");
			const std::vector<FrameWorkloadDeclaration> animationWorkloads = animationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
			const std::vector<FrameWorkloadDeclaration> presentationWorkloads = presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F);

			context.ExpectEqual(static_cast<std::size_t>(9U), animationWorkloads.size(),
				"AnimationSystem must expose capture, planning, finalization, sibling evaluation, sealing, constant-time publication, worker cache commit, and sparse finalization.");
			context.ExpectEqual(static_cast<std::size_t>(2U), presentationWorkloads.size(),
				"AnimationPresentationSystem must expose worker preparation and protected publication.");
			const FrameWorkloadDeclaration* capture = FindWorkload(animationWorkloads, "CaptureAnimationInputs");
			const FrameWorkloadDeclaration* plan = FindWorkload(animationWorkloads, "PlanAnimationEvaluation");
			const FrameWorkloadDeclaration* finalize = FindWorkload(animationWorkloads, "FinalizeAnimationWorkPlan");
			const FrameWorkloadDeclaration* evaluate = FindWorkload(animationWorkloads, "EvaluateCompleteAnimation");
			const FrameWorkloadDeclaration* evaluateCompact = FindWorkload(animationWorkloads, "EvaluateCompactGpuVisualAnimation");
			const FrameWorkloadDeclaration* seal = FindWorkload(animationWorkloads, "SealAnimationAuthority");
			const FrameWorkloadDeclaration* publish = FindWorkload(animationWorkloads, "PublishAnimationAuthority");
			const FrameWorkloadDeclaration* commitCache = FindWorkload(
				animationWorkloads, "CommitPublishedAnimationEvaluationCache");
			const FrameWorkloadDeclaration* finalizePublished = FindWorkload(
				animationWorkloads, "FinalizePublishedAnimationAuthority");
			const FrameWorkloadDeclaration* preparePresentation = FindWorkload(presentationWorkloads, "PrepareAnimationPresentation");
			const FrameWorkloadDeclaration* publishPresentation = FindWorkload(presentationWorkloads, "PublishAnimationPresentation");
			context.Expect(capture != nullptr && plan != nullptr && finalize != nullptr
				&& evaluate != nullptr && evaluateCompact != nullptr && seal != nullptr
				&& publish != nullptr && commitCache != nullptr
				&& finalizePublished != nullptr
				&& preparePresentation != nullptr && publishPresentation != nullptr,
				"Every frozen animation workload name must be declared.");
			if(capture == nullptr || plan == nullptr || finalize == nullptr
				|| evaluate == nullptr || evaluateCompact == nullptr || seal == nullptr
				|| publish == nullptr || commitCache == nullptr
				|| finalizePublished == nullptr
				|| preparePresentation == nullptr || publishPresentation == nullptr) {
				return;
			}
			for(const FrameWorkloadDeclaration& workload : animationWorkloads) {
				const std::size_t expectedItems = workload.workloadName == "FinalizeAnimationWorkPlan"
					|| workload.workloadName == "SealAnimationAuthority"
					? 1U : 256U;
				context.ExpectEqual(expectedItems, workload.estimatedWorkItemCount,
					"Per-animator stages must freeze membership while finalization remains one atomic plan build.");
			}

			ExpectProtectedWorkload(context, *capture);
			context.Expect(capture->phaseKind == TerraEngine::Scheduling::WorkerSystemPhaseKind::Snapshot,
				"Live component/command capture must be identified as a protected snapshot.");
			ExpectWorkerWorkload(context, *plan);
			context.Expect(plan->partitionWorkItemCount == 128U
				&& evaluate->partitionWorkItemCount == 128U
				&& evaluateCompact->partitionWorkItemCount == 128U,
				"Full-rate crowd planning and evaluation should use one practical worker-pool wave instead of dispatching 64-item microtasks.");
			ExpectWorkerWorkload(context, *finalize,
				TerraEngine::Scheduling::FrameWorkloadPartitionPolicy::Single);
			ExpectWorkerWorkload(context, *evaluate);
			ExpectWorkerWorkload(context, *evaluateCompact);
			ExpectWorkerWorkload(context, *seal,
				TerraEngine::Scheduling::FrameWorkloadPartitionPolicy::Single);
			ExpectProtectedWorkload(context, *publish);
			context.Expect(publish->phaseKind == TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit
				&& publish->sameFrameVisibilityRequired,
				"Authority publication must be a same-frame protected handoff to physics.");
			ExpectWorkerWorkload(context, *commitCache,
				TerraEngine::Scheduling::FrameWorkloadPartitionPolicy::Single);
			context.Expect(commitCache->sameFrameVisibilityRequired,
				"The accepted generation's cache commit must finish before sparse finalization consumes the token.");
			ExpectProtectedWorkload(context, *finalizePublished);
			context.Expect(finalizePublished->phaseKind
				== TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit
				&& finalizePublished->sameFrameVisibilityRequired,
				"The accepted token finalizer must remain ordered after the constant-time authority exchange.");

			ExpectWorkerWorkload(context, *preparePresentation,
				TerraEngine::Scheduling::FrameWorkloadPartitionPolicy::Single);
			ExpectProtectedWorkload(context, *publishPresentation);
			context.Expect(publishPresentation->phaseKind == TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit
				&& publishPresentation->sameFrameVisibilityRequired,
				"Presentation publication must be protected before UI/render consume the immutable plan.");

			context.Expect(Contains(capture->reads, TerraEngine::Scheduling::kResourceAnimatorComponents)
				&& Contains(capture->reads, TerraEngine::Scheduling::kResourceAnimationCommandQueues)
				&& Contains(capture->writes, TerraEngine::Scheduling::kResourceAnimationFrameInputs),
				"Capture must snapshot live animator inputs into the immutable frame input resource.");
			context.Expect(Contains(plan->reads, TerraEngine::Scheduling::kResourceAnimationFrameInputs)
				&& Contains(plan->writes, TerraEngine::Scheduling::kResourceAnimationWorkPackets)
				&& Contains(plan->writes, TerraEngine::Scheduling::kResourceAnimationVisualPackets),
				"Planning must consume immutable inputs and emit typed work/visual packets.");
			context.Expect(Contains(finalize->reads, TerraEngine::Scheduling::kResourceAnimationWorkPackets)
				&& Contains(finalize->writes, TerraEngine::Scheduling::kResourceAnimationWorkPackets),
				"Finalization must convert the complete planned packet set into one immutable scheduler plan.");
			context.Expect(Contains(evaluate->reads, TerraEngine::Scheduling::kResourceAnimationWorkPackets)
				&& Contains(evaluate->writes, TerraEngine::Scheduling::kResourceAnimationAuthoritativeOutputs),
				"Authoritative evaluation must write only prepared output owned by its frame workload.");
			context.Expect(Contains(seal->reads, TerraEngine::Scheduling::kResourceAnimationAuthoritativeOutputs)
				&& Contains(seal->writes, TerraEngine::Scheduling::kResourceAnimationAuthorityPages)
				&& Contains(seal->writes, TerraEngine::Scheduling::kResourceAnimationFrameSidecars)
				&& Contains(publish->reads, TerraEngine::Scheduling::kResourceAnimationAuthorityPages)
				&& Contains(publish->reads, TerraEngine::Scheduling::kResourceAnimationFrameSidecars)
				&& Contains(publish->writes, TerraEngine::Scheduling::kResourceAnimationAuthorityFrame)
				&& Contains(commitCache->reads, TerraEngine::Scheduling::kResourceAnimationAuthorityFrame)
				&& Contains(commitCache->writes, TerraEngine::Scheduling::kResourceAnimationEvaluationCache)
				&& Contains(finalizePublished->reads, TerraEngine::Scheduling::kResourceAnimationFrameSidecars)
				&& Contains(finalizePublished->writes, TerraEngine::Scheduling::kResourceAnimationPhysicsHandoffQueue)
				&& Contains(finalizePublished->writes, TerraEngine::Scheduling::kResourceAnimationManagedCallbackBatches),
				"Workers must seal typed authority pages and sidecars before a constant-time root exchange hands the accepted token to specialist finalization.");
			context.Expect(Contains(preparePresentation->reads, "PhysicsPublishedSnapshots")
				&& Contains(preparePresentation->writes, TerraEngine::Scheduling::kResourceAnimationPresentationPrepared),
				"Presentation preparation must consume immutable current-step physics snapshots.");
			context.Expect(Contains(publishPresentation->reads, TerraEngine::Scheduling::kResourceAnimationPresentationPrepared)
				&& Contains(publishPresentation->writes, TerraEngine::Scheduling::kResourceAnimationPresentationPlan),
				"Only the protected presentation boundary may publish the immutable render-facing plan.");

			context.Expect(!plan->explicitDependencies.empty() && capture->stableDescriptorId == plan->explicitDependencies.front(),
				"Planning must wait for the protected immutable input capture.");
			context.Expect(!finalize->explicitDependencies.empty()
				&& plan->stableDescriptorId == finalize->explicitDependencies.front(),
				"Single-plan finalization must wait for every planning range.");
			context.Expect(!evaluate->explicitDependencies.empty()
				&& finalize->stableDescriptorId == evaluate->explicitDependencies.front(),
				"Authoritative evaluation must wait for the immutable plan.");
			context.Expect(!seal->explicitDependencies.empty()
				&& !publish->explicitDependencies.empty()
				&& seal->stableDescriptorId == publish->explicitDependencies.front(),
				"Authority publication must wait for the sealed result of all worker evaluation.");
			context.Expect(!commitCache->explicitDependencies.empty()
				&& publish->stableDescriptorId == commitCache->explicitDependencies.front()
				&& !finalizePublished->explicitDependencies.empty()
				&& commitCache->stableDescriptorId
					== finalizePublished->explicitDependencies.front(),
				"Worker cache commit and sparse finalization must remain ordered after the accepted authority root.");
			context.Expect(!publishPresentation->explicitDependencies.empty()
				&& preparePresentation->stableDescriptorId == publishPresentation->explicitDependencies.front(),
				"Presentation publication must wait for worker preparation.");

		});

		registry.Add("Animation.Scheduler.CancelledPresentationPreparationDoesNotPoisonNextFrame", [](TestContext& context) {
			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			TerraEngine::Animation::AnimationPresentationSystem& presentationSystem = TerraEngine::Animation::AnimationPresentationSystem::Instance();
			AnimatorMembershipGuard membership(animationSystem, 32U);
			(void)animationSystem.DeclareFrameWorkloads(1.0F / 60.0F);

			std::vector<FrameWorkloadDeclaration> cancelledFrame = presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
			context.ExpectEqual(static_cast<std::size_t>(2U), cancelledFrame.size(),
				"The cancellation fixture requires prepare and publication descriptors.");
			if(cancelledFrame.size() != 2U) return;
			// Execute preparation alone to model cancellation before the protected
			// publication callback. Its private item count must not leak forward.
			std::vector<FrameWorkloadDeclaration> prepareOnly{cancelledFrame.front()};
			const TerraEngine::Scheduling::FrameExecutionPlan partial =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(prepareOnly), "animation-presentation-cancelled");
			context.Expect(partial.valid && partial.completed,
				"The fixture's worker preparation must complete before simulated cancellation.");

			const std::uint64_t generationBefore = presentationSystem.PresentationPublicationGeneration();
			std::vector<FrameWorkloadDeclaration> nextFrame = presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
			const TerraEngine::Scheduling::FrameExecutionPlan recovered =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(nextFrame), "animation-presentation-recovered");
			context.Expect(recovered.valid && recovered.completed,
				"The frame after cancelled preparation must execute normally.");
			context.ExpectEqual(generationBefore + 1U, presentationSystem.PresentationPublicationGeneration(),
				"A stale partial preparation count must not block the next protected publication.");
			context.ExpectEqual(static_cast<std::size_t>(32U), presentationSystem.PublishedAnimatorCount(),
				"Recovered publication must describe the complete current membership only.");
			const std::uint64_t recoveredGeneration = presentationSystem.PresentationPublicationGeneration();
			std::vector<FrameWorkloadDeclaration> latePublish{cancelledFrame.back()};
			// The dependency ran before cancellation, so invoke the retained protected
			// closure alone to model a late scheduler completion from that old frame.
			latePublish.front().explicitDependencies.clear();
			const TerraEngine::Scheduling::FrameExecutionPlan rejectedLatePublish =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(latePublish), "animation-presentation-cancelled-late-publish");
			context.Expect(rejectedLatePublish.valid && rejectedLatePublish.completed,
				"The scheduler may complete a retained callback, but its frame token must fail closed.");
			context.ExpectEqual(recoveredGeneration, presentationSystem.PresentationPublicationGeneration(),
				"A cancelled frame must not publish after a newer frame has replaced its declaration token.");
			context.ExpectEqual(static_cast<std::size_t>(32U), presentationSystem.PublishedAnimatorCount(),
				"Rejected late publication must preserve the newer complete presentation plan.");
		});

		registry.Add("Animation.Scheduler.RetainedTopologyRefreshesAnimationEpochEveryFrame", [](TestContext& context) {
			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			animationSystem.Initialize();
			{
				AnimatorMembershipGuard membership(animationSystem, 1U);
				TerraEngine::Scheduling::SystemScheduler scheduler;
				TerraEngine::Scheduling::SystemDeclaration declaration = animationSystem.SchedulerDeclaration();
				// Isolate callback rearming from the full engine dependency graph; this
				// fixture is specifically proving that one retained topology still asks
				// AnimationSystem for fresh frame-epoch closures on every execution.
				declaration.after.clear();
				declaration.before.clear();
				declaration.requiresFlushedSceneMutations = false;
				scheduler.RegisterSystem(animationSystem, std::move(declaration));

				std::uint64_t previousGeneration = animationSystem.AuthorityPublicationGeneration();
				for(std::uint32_t frame = 0U; frame < 3U; ++frame)
				{
					const TerraEngine::Scheduling::FrameExecutionPlan plan =
						scheduler.ExecuteFrameWorkloads(1.0F / 60.0F);
					const std::uint64_t currentGeneration = animationSystem.AuthorityPublicationGeneration();
					context.Expect(plan.valid && plan.completed,
						"A retained scheduler topology must complete each animation frame.");
					context.Expect(currentGeneration > previousGeneration,
						"Refreshing retained JobGraph callbacks must advance animation authority publication every frame.");
					previousGeneration = currentGeneration;
				}
			}
		});

		registry.Add("Animation.Scheduler.PresentationCapturesSameFrameSubmissionAndRejectsStaleGeneration", [](TestContext& context) {
			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			TerraEngine::Animation::AnimationPresentationSystem& presentationSystem = TerraEngine::Animation::AnimationPresentationSystem::Instance();
			// Isolate the singleton's monotonic submission stream so this regression
			// does not depend on an earlier test's submitted source generation.
			presentationSystem.Shutdown();
			presentationSystem.Initialize();
			AnimatorMembershipGuard membership(animationSystem, 1U);
			(void)animationSystem.DeclareFrameWorkloads(1.0F / 60.0F);

			std::vector<FrameWorkloadDeclaration> first = presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
			auto firstFrame = std::make_shared<TerraEngine::Animation::GpuAnimationPresentationFrame>();
			firstFrame->sourceGeneration = 1U;
			context.Expect(presentationSystem.SubmitGpuAnimationPresentationFrame(firstFrame) == TerraEngine::Animation::AnimationStatus::Ok,
				"A frame submitted after declarations but before worker preparation must be accepted.");
			const TerraEngine::Scheduling::FrameExecutionPlan firstPlan =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(std::move(first), "animation-presentation-same-frame");
			const auto firstPublication = presentationSystem.LoadPublishedGpuAnimationPresentationPlan();
			context.Expect(firstPlan.valid && firstPlan.completed && firstPublication
				&& firstPublication->sourceGeneration == 1U,
				"Presentation preparation must capture the same-frame producer handoff, not a declaration-time snapshot.");

			std::vector<FrameWorkloadDeclaration> second = presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
			auto replacementFrame = std::make_shared<TerraEngine::Animation::GpuAnimationPresentationFrame>();
			replacementFrame->sourceGeneration = 2U;
			auto staleFrame = std::make_shared<TerraEngine::Animation::GpuAnimationPresentationFrame>();
			staleFrame->sourceGeneration = 1U;
			context.Expect(presentationSystem.SubmitGpuAnimationPresentationFrame(replacementFrame) == TerraEngine::Animation::AnimationStatus::Ok
				&& presentationSystem.SubmitGpuAnimationPresentationFrame(staleFrame) == TerraEngine::Animation::AnimationStatus::StaleHandle,
				"A stale producer must not replace a newer immutable presentation source.");
			const TerraEngine::Scheduling::FrameExecutionPlan secondPlan =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(std::move(second), "animation-presentation-stale-source");
			const auto secondPublication = presentationSystem.LoadPublishedGpuAnimationPresentationPlan();
			context.Expect(secondPlan.valid && secondPlan.completed && secondPublication
				&& secondPublication->sourceGeneration == 2U,
				"Protected publication must validate and expose one captured source generation without mixing stale work.");
			presentationSystem.Shutdown();
			presentationSystem.Initialize();
		});

		registry.Add("Animation.Scheduler.ProtectedCaptureDrainsTypedCommandsBeforeWorkerPlanning", [](TestContext& context) {
			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			AnimatorMembershipGuard membership(animationSystem, 1U);
			TerraEngine::Animation::AnimationCommand command;
			command.target = membership.First();
			command.kind = TerraEngine::Animation::AnimationCommandKind::Stop;
			command.targetTick = 0;
			command.producerId = 7U;
			command.producerSequence = 1U;
			command.correlationId = 99U;
			const TerraEngine::Animation::AnimationCommandReceipt receipt = animationSystem.SubmitCommand(command);
			context.Expect(receipt.status == TerraEngine::Animation::AnimationStatus::Queued,
				"A valid typed command must enter the cross-producer queue before capture.");
			context.ExpectEqual(static_cast<std::size_t>(1U), animationSystem.PendingCommandCount(),
				"The command must remain queued until the protected capture boundary.");

			std::vector<FrameWorkloadDeclaration> workloads = animationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
			context.Expect(!workloads.empty(), "Active animation membership must expose its capture workload.");
			if(workloads.empty()) return;
			std::vector<FrameWorkloadDeclaration> captureOnly{workloads.front()};
			const TerraEngine::Scheduling::FrameExecutionPlan capturePlan =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(captureOnly), "animation-command-capture");
			context.Expect(capturePlan.valid && capturePlan.completed,
				"Protected input capture must complete without invoking worker evaluation.");
			context.ExpectEqual(static_cast<std::size_t>(0U), animationSystem.PendingCommandCount(),
				"Captured due commands must no longer remain in the producer queue.");
			context.ExpectEqual(static_cast<std::size_t>(1U), animationSystem.CapturedCommandCount(),
				"Worker planning must receive one copied command snapshot, not live queue access.");
		});

		registry.Add("Animation.Scheduler.CommandSubmissionCannotResurrectShutdownOwnership", [](TestContext& context) {
			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			animationSystem.Initialize();
			{
				AnimatorMembershipGuard membership(animationSystem, 1U);
				TerraEngine::Animation::AnimationCommand command;
				command.target = membership.First();
				command.kind = TerraEngine::Animation::AnimationCommandKind::Stop;
				command.targetTick = 0;
				command.producerId = 8U;
				command.producerSequence = 1U;
				command.correlationId = 100U;

				std::atomic_bool submit = false;
				TerraEngine::Animation::AnimationCommandReceipt racedReceipt;
				std::thread producer([&]() {
					while(!submit.load(std::memory_order_acquire)) std::this_thread::yield();
					racedReceipt = animationSystem.SubmitCommand(command);
				});
				submit.store(true, std::memory_order_release);
				animationSystem.Shutdown();
				producer.join();
				context.Expect(racedReceipt.status == TerraEngine::Animation::AnimationStatus::Queued
					|| racedReceipt.status == TerraEngine::Animation::AnimationStatus::Cancelled,
					"A command racing protected shutdown must finish atomically as queued-before-shutdown or cancelled-after-shutdown.");

				command.producerSequence = 2U;
				command.correlationId = 101U;
				const TerraEngine::Animation::AnimationCommandReceipt afterShutdown = animationSystem.SubmitCommand(command);
				context.Expect(afterShutdown.status == TerraEngine::Animation::AnimationStatus::Cancelled,
					"Cross-producer submission after shutdown must not recreate runtime owners.");
				context.ExpectEqual(static_cast<std::size_t>(0U), animationSystem.PendingCommandCount(),
					"A cancelled post-shutdown command must create no hidden queue storage.");
			}
			// Restore the process-wide singleton for later tests while keeping
			// allocation/lifetime mutation on the protected coordinator thread.
			animationSystem.Initialize();
		});

		registry.Add("Animation.Scheduler.RestartRetiresHandlesCommandsAndAuthorityGeneration", [](TestContext& context) {
			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			animationSystem.Initialize();

			TerraEngine::Animation::AnimatorInstanceDescriptor originalDescriptor;
			originalDescriptor.ownerObjectId[0] = 41U;
			originalDescriptor.componentGeneration = 1U;
			const TerraEngine::Animation::AnimatorInstanceHandle original =
				animationSystem.CreateAnimatorInstance(originalDescriptor);
			context.Expect(original.IsValid(), "The restart fixture requires one live original animator instance.");

			TerraEngine::Animation::AnimationCommand queuedBeforeShutdown;
			queuedBeforeShutdown.target = original;
			queuedBeforeShutdown.kind = TerraEngine::Animation::AnimationCommandKind::Stop;
			queuedBeforeShutdown.targetTick = 100;
			queuedBeforeShutdown.producerId = 19U;
			queuedBeforeShutdown.producerSequence = 1U;
			queuedBeforeShutdown.correlationId = 201U;
			context.Expect(animationSystem.SubmitCommand(queuedBeforeShutdown).status
				== TerraEngine::Animation::AnimationStatus::Queued,
				"A future command must be present so shutdown command cancellation is exercised.");
			TerraEngine::Animation::AnimationCommand capturedBeforeShutdown = queuedBeforeShutdown;
			capturedBeforeShutdown.targetTick = 0;
			capturedBeforeShutdown.producerSequence = 2U;
			capturedBeforeShutdown.correlationId = 202U;
			context.Expect(animationSystem.SubmitCommand(capturedBeforeShutdown).status
				== TerraEngine::Animation::AnimationStatus::Queued,
				"A due command must be present so shutdown copied-work clearing is exercised.");

			std::vector<FrameWorkloadDeclaration> originalFrame =
				animationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
			const TerraEngine::Scheduling::FrameExecutionPlan originalPlan =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(originalFrame), "animation-restart-original");
			context.Expect(originalPlan.valid && originalPlan.completed,
				"The original animator must publish a complete authority generation before shutdown.");
			context.ExpectEqual(static_cast<std::size_t>(1U), animationSystem.PendingCommandCount(),
				"The future command must still be pending immediately before shutdown.");
			context.ExpectEqual(static_cast<std::size_t>(1U), animationSystem.CapturedCommandCount(),
				"The due command must be copied into captured work immediately before shutdown.");
			const std::uint64_t generationBeforeShutdown = animationSystem.AuthorityPublicationGeneration();

			animationSystem.Shutdown();
			animationSystem.Shutdown();
			const std::uint64_t retiredGeneration = animationSystem.AuthorityPublicationGeneration();
			context.Expect(retiredGeneration > generationBeforeShutdown,
				"Shutdown must retire the last authority generation so a later session cannot alias it.");
			context.Expect(!animationSystem.IsAnimatorInstanceAlive(original),
				"Shutdown must invalidate the original handle even though the generation history is retained.");
			context.ExpectEqual(static_cast<std::size_t>(0U), animationSystem.PendingCommandCount(),
				"Shutdown must discard commands captured from the retired runtime session.");
			context.ExpectEqual(static_cast<std::size_t>(0U), animationSystem.CapturedCommandCount(),
				"Shutdown must discard copied command work before a later session starts.");

			animationSystem.Initialize();
			context.ExpectEqual(retiredGeneration, animationSystem.AuthorityPublicationGeneration(),
				"Initialize must preserve the retired authority generation instead of restarting it at zero.");
			TerraEngine::Animation::AnimatorInstanceDescriptor replacementDescriptor;
			replacementDescriptor.ownerObjectId[0] = 42U;
			replacementDescriptor.componentGeneration = 1U;
			const TerraEngine::Animation::AnimatorInstanceHandle replacement =
				animationSystem.CreateAnimatorInstance(replacementDescriptor);
			context.Expect(replacement.IsValid()
				&& replacement.slotIndex == original.slotIndex
				&& replacement.generation > original.generation,
				"Restart must preserve the slot's history and advance its generation for the replacement.");
			context.Expect(!animationSystem.IsAnimatorInstanceAlive(original)
				&& animationSystem.IsAnimatorInstanceAlive(replacement),
				"The stale original identity must remain dead while only its replacement is live.");

			TerraEngine::Animation::AnimationCommand staleAfterRestart = queuedBeforeShutdown;
			staleAfterRestart.targetTick = 0;
			staleAfterRestart.producerSequence = 3U;
			staleAfterRestart.correlationId = 203U;
			context.Expect(animationSystem.SubmitCommand(staleAfterRestart).status
				== TerraEngine::Animation::AnimationStatus::StaleHandle,
				"A command carrying the retired handle must not enter the replacement session's queue.");
			context.ExpectEqual(static_cast<std::size_t>(0U), animationSystem.PendingCommandCount(),
				"Neither pre-shutdown nor post-restart stale work may remain queued for the replacement.");
			context.Expect(animationSystem.IsAnimatorInstanceAlive(replacement),
				"Rejecting stale work must leave the replacement animator alive and unchanged.");

			std::vector<FrameWorkloadDeclaration> replacementFrame =
				animationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
			const TerraEngine::Scheduling::FrameExecutionPlan replacementPlan =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(replacementFrame), "animation-restart-replacement");
			context.Expect(replacementPlan.valid && replacementPlan.completed,
				"The replacement animator must publish normally after stale work is rejected.");
			context.Expect(animationSystem.AuthorityPublicationGeneration() > retiredGeneration,
				"The replacement publication must advance beyond the lifecycle retirement generation.");
			context.ExpectEqual(static_cast<std::size_t>(0U), animationSystem.CapturedCommandCount(),
				"The replacement frame must capture no command from the retired session or stale submission.");
			const std::uint64_t replacementGeneration = animationSystem.AuthorityPublicationGeneration();
			animationSystem.Initialize();
			context.ExpectEqual(replacementGeneration, animationSystem.AuthorityPublicationGeneration(),
				"Repeated Initialize while active must be idempotent and preserve the published identity.");
			context.ExpectEqual(static_cast<std::size_t>(1U), animationSystem.PublishedAnimatorCount(),
				"Idempotent Initialize must not erase a complete live publication under the same generation.");
			context.Expect(animationSystem.IsAnimatorInstanceAlive(replacement),
				"Idempotent Initialize must leave the active replacement instance untouched.");

			(void)animationSystem.InvalidateAnimatorInstance(replacement);
			animationSystem.Shutdown();
			// Restore the process-wide singleton for subsequent scheduler tests.
			animationSystem.Initialize();
		});

		registry.Add("Animation.Scheduler.PresentationLifecycleRetiresAndRejectsPostShutdownWork", [](TestContext& context) {
			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			TerraEngine::Animation::AnimationPresentationSystem& presentationSystem = TerraEngine::Animation::AnimationPresentationSystem::Instance();
			animationSystem.Initialize();
			presentationSystem.Initialize();
			{
				AnimatorMembershipGuard membership(animationSystem, 2U);
				std::vector<FrameWorkloadDeclaration> authority = animationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
				const TerraEngine::Scheduling::FrameExecutionPlan authorityPlan =
					TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
						std::move(authority), "animation-presentation-lifecycle-authority");
				context.Expect(authorityPlan.valid && authorityPlan.completed,
					"The lifecycle fixture must publish active authority membership before presentation work is declared.");

				std::vector<FrameWorkloadDeclaration> initialPresentation =
					presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
				const TerraEngine::Scheduling::FrameExecutionPlan initialPlan =
					TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
						std::move(initialPresentation), "animation-presentation-lifecycle-initial");
				context.Expect(initialPlan.valid && initialPlan.completed,
					"The lifecycle fixture must publish one complete render-facing plan before shutdown.");
				context.ExpectEqual(static_cast<std::size_t>(2U), presentationSystem.PublishedAnimatorCount(),
					"The complete presentation plan must expose the active membership count.");
				const std::uint64_t generationBeforeShutdown =
					presentationSystem.PresentationPublicationGeneration();
				// Bind a second frame without executing it. Its closures model scheduler
				// work retained across cancellation at the lifecycle boundary.
				std::vector<FrameWorkloadDeclaration> retiredPresentation =
					presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F);

				presentationSystem.Shutdown();
				presentationSystem.Shutdown();
				const std::uint64_t retiredGeneration =
					presentationSystem.PresentationPublicationGeneration();
				context.ExpectEqual(generationBeforeShutdown + 1U, retiredGeneration,
					"Repeated Shutdown must retire the presentation runtime exactly once.");
				context.ExpectEqual(static_cast<std::size_t>(0U), presentationSystem.PublishedAnimatorCount(),
					"Shutdown must clear the render-facing plan before ownership is released.");
				context.Expect(presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F).empty(),
					"A post-shutdown scheduler query must not recreate presentation ownership from active authority membership.");
				context.ExpectEqual(retiredGeneration, presentationSystem.PresentationPublicationGeneration(),
					"Rejecting post-shutdown work must not synthesize another publication identity.");

				presentationSystem.Initialize();
				context.ExpectEqual(retiredGeneration, presentationSystem.PresentationPublicationGeneration(),
					"Initialize must preserve the retired generation before the replacement plan publishes.");
				const TerraEngine::Scheduling::FrameExecutionPlan retiredPlan =
					TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
						std::move(retiredPresentation), "animation-presentation-lifecycle-retired");
				context.Expect(retiredPlan.valid && retiredPlan.completed,
					"The scheduler may finish cancelled descriptors, but their callbacks must fail closed.");
				context.ExpectEqual(static_cast<std::size_t>(0U), presentationSystem.PublishedAnimatorCount(),
					"A descriptor captured before shutdown must not publish into the replacement runtime session.");
				context.ExpectEqual(retiredGeneration, presentationSystem.PresentationPublicationGeneration(),
					"Rejected stale descriptors must not synthesize a presentation publication generation.");
				std::vector<FrameWorkloadDeclaration> replacementPresentation =
					presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
				const TerraEngine::Scheduling::FrameExecutionPlan replacementPlan =
					TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
						std::move(replacementPresentation), "animation-presentation-lifecycle-replacement");
				context.Expect(replacementPlan.valid && replacementPlan.completed,
					"Explicit Initialize must allow a new complete presentation plan to publish.");
				context.Expect(presentationSystem.PresentationPublicationGeneration() > retiredGeneration,
					"The replacement plan must advance beyond the retired runtime identity.");
				const std::uint64_t replacementGeneration =
					presentationSystem.PresentationPublicationGeneration();
				presentationSystem.Initialize();
				context.ExpectEqual(replacementGeneration, presentationSystem.PresentationPublicationGeneration(),
					"Repeated Initialize while active must preserve the complete published identity.");
				context.ExpectEqual(static_cast<std::size_t>(2U), presentationSystem.PublishedAnimatorCount(),
					"Idempotent Initialize must not erase a complete live presentation plan.");
			}
			presentationSystem.Shutdown();
			// Restore the process-wide singleton for subsequent scheduler tests.
			presentationSystem.Initialize();
		});

		registry.Add("Animation.Scheduler.ActiveToDormantRetiresPublishedGenerations", [](TestContext& context) {
			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			TerraEngine::Animation::AnimationPresentationSystem& presentationSystem = TerraEngine::Animation::AnimationPresentationSystem::Instance();
			{
				AnimatorMembershipGuard membership(animationSystem, 8U);
				std::vector<FrameWorkloadDeclaration> authority = animationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
				const TerraEngine::Scheduling::FrameExecutionPlan authorityPlan =
					TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
						std::move(authority), "animation-active-authority");
				context.Expect(authorityPlan.valid && authorityPlan.completed,
					"The active authority shell must publish one complete generation before retirement is tested.");

				std::vector<FrameWorkloadDeclaration> presentation = presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
				const TerraEngine::Scheduling::FrameExecutionPlan presentationPlan =
					TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
						std::move(presentation), "animation-active-presentation");
				context.Expect(presentationPlan.valid && presentationPlan.completed,
					"The active presentation shell must publish one complete render-facing generation.");
				context.ExpectEqual(static_cast<std::size_t>(8U), animationSystem.PublishedAnimatorCount(),
					"The active authority publication must expose the complete membership count.");
				context.ExpectEqual(static_cast<std::size_t>(8U), presentationSystem.PublishedAnimatorCount(),
					"The active presentation publication must expose the complete membership count.");
			}

			const std::uint64_t authorityGeneration = animationSystem.AuthorityPublicationGeneration();
			const std::uint64_t presentationGeneration = presentationSystem.PresentationPublicationGeneration();
			TerraEngine::Animation::ClearPublishedAnimationDiagnostics();
			context.Expect(animationSystem.DeclareFrameWorkloads(1.0F / 60.0F).empty(),
				"Transitioning to zero active animators must allocate no authority descriptors.");
			context.Expect(presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F).empty(),
				"Transitioning to zero active animators must allocate no presentation descriptors.");
			context.ExpectEqual(static_cast<std::size_t>(0U), animationSystem.PublishedAnimatorCount(),
				"Dormancy must retire the prior authoritative animator count.");
			context.ExpectEqual(static_cast<std::size_t>(0U), presentationSystem.PublishedAnimatorCount(),
				"Dormancy must retire the prior presentation animator count.");
			context.ExpectEqual(authorityGeneration + 1U, animationSystem.AuthorityPublicationGeneration(),
				"Authority retirement must advance its generation so stale readers fail closed.");
			context.ExpectEqual(presentationGeneration + 1U, presentationSystem.PresentationPublicationGeneration(),
				"Presentation retirement must advance its generation so stale render plans fail closed.");
			context.Expect(!TerraEngine::Animation::AcquireAnimationDiagnosticsSnapshot().has_value(),
				"Static-only projects must not enter global animation diagnostic collection.");
		});

		registry.Add("Animation.Scheduler.DormantSystemsDeclareNoUsefulJobs", [](TestContext& context) {
			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			TerraEngine::Animation::AnimationPresentationSystem& presentationSystem = TerraEngine::Animation::AnimationPresentationSystem::Instance();
			std::vector<FrameWorkloadDeclaration> workloads = animationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
			std::vector<FrameWorkloadDeclaration> presentation = presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
			context.Expect(workloads.empty() && presentation.empty(),
				"A zero-animator frame must return before allocating string/vector-bearing workload descriptors.");
			const std::uint64_t authorityGeneration = animationSystem.AuthorityPublicationGeneration();
			const std::uint64_t presentationGeneration = presentationSystem.PresentationPublicationGeneration();
			for(std::size_t frame = 0U; frame < 128U; ++frame) {
				context.Expect(animationSystem.DeclareFrameWorkloads(1.0F / 60.0F).empty()
					&& presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F).empty(),
					"Steady dormant declarations must remain empty without transition cleanup work.");
			}
			context.ExpectEqual(authorityGeneration, animationSystem.AuthorityPublicationGeneration(),
				"Steady dormancy must not repeatedly retire an already-empty authority publication.");
			context.ExpectEqual(presentationGeneration, presentationSystem.PresentationPublicationGeneration(),
				"Steady dormancy must not repeatedly retire an already-empty presentation publication.");

			const TerraEngine::Scheduling::FrameExecutionPlan plan = TerraEngine::Scheduling::ValidateFrameWorkloads(
				{}, "animation-dormant-contract");
			context.Expect(plan.valid, "Dormant animation descriptors must remain a valid visible scheduler contract.");
			context.Expect(plan.results.empty() && plan.workloads.empty(),
				"Dormant animation must create zero worker, inline, protected, or omitted frame jobs.");

			// Verify the scheduler honors the explicit zero-work contract instead of
			// wrapping SystemsBase::Update as a legacy protected workload.
			TerraEngine::Scheduling::SystemDeclaration animation = animationSystem.SchedulerDeclaration();
			animation.after.clear();
			animation.before.clear();
			animation.requiresFlushedSceneMutations = false;
			TerraEngine::Scheduling::SystemDeclaration presentationDeclaration = presentationSystem.SchedulerDeclaration();
			presentationDeclaration.after.clear();
			presentationDeclaration.before.clear();
			TerraEngine::Scheduling::SystemScheduler scheduler;
			scheduler.SetMutationFlushPoints({});
			scheduler.RegisterSystem(animationSystem, std::move(animation));
			scheduler.RegisterSystem(presentationSystem, std::move(presentationDeclaration));
			context.Expect(scheduler.BuildSchedule().valid, "The reduced dormant animation schedule must validate.");
			context.Expect(scheduler.BuildFrameWorkloadDeclarations(1.0F / 60.0F).empty(),
				"SystemScheduler must not synthesize compatibility Update jobs for an explicitly dormant converted system.");
		});
	}
}
