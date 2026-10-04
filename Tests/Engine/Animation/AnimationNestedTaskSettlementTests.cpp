// AnimationNestedTaskSettlementTests.cpp — Actual animation child ownership.
// These cases execute AnimationSystem workloads, not a substitute task guard.
#include "TestHarness.h"
#include "Animation/AnimationSystem.h"
#include "Animation/Diagnostics/AnimationDiagnostics.h"
#include "Animation/Graph/EvaluationProgram.h"
#include "AnimationClipCompiler.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Core/Object/ObjectManager.h"
#include "ThreadPool/ScopedTaskSettlement.h"
#include "Tests/Engine/Runtime/CorrectnessTestSupport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::AssetPipeline;
		using namespace TerraEngine::Tasks;
		using namespace TerraEngine::Tests::Correctness;
		// The real cooked clip/skeleton and GPU atlas layout below use the same
		// minimal checked records as existing integration and shader-layout tests.
		template<typename Value>
		void WriteFixtureValue(std::vector<std::uint8_t>& bytes, std::size_t offset, const Value& value) {
			if (offset > bytes.size() || sizeof(Value) > bytes.size() - offset) throw std::out_of_range("GPU fixture byte range");
			std::memcpy(bytes.data() + offset, &value, sizeof(Value));
		}
		AnimationGuid MakeIntegrationGuid(std::uint8_t seed)
		{
			AnimationGuid guid;
			for(std::size_t index = 0U; index < guid.value.size(); ++index) {
				guid.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return guid;
		}

		AnimationMatrix4x4 MakeIdentityMatrix()
		{
			AnimationMatrix4x4 matrix;
			matrix.values[0U] = 1.0F;
			matrix.values[5U] = 1.0F;
			matrix.values[10U] = 1.0F;
			matrix.values[15U] = 1.0F;
			return matrix;
		}

		std::shared_ptr<const SkeletonAsset> MakeIntegrationSkeleton()
		{
			auto skeleton = std::make_shared<SkeletonAsset>();
			skeleton->assetId = MakeIntegrationGuid(40U);
			skeleton->compatibilityFingerprint = 0x4387aabbccdd1122ULL;
			skeleton->rootJointIndex = 0U;
			SkeletonJoint root;
			root.stableId = 101U;
			root.parentIndex = -1;
			root.normalizedSourcePath = "/root";
			root.localBindTransform.rotation = {0.0F, 0.0F, 0.0F, 1.0F};
			root.localBindTransform.scale = {1.0F, 1.0F, 1.0F};
			skeleton->joints.push_back(root);
			skeleton->inverseBindMatrices.push_back(MakeIdentityMatrix());
			return skeleton;
		}

		std::shared_ptr<const AnimationClipAsset> MakeIntegrationClip(
			TestContext& context,
			const SkeletonAsset& skeleton,
			std::uint8_t assetSeed = 20U,
			float beginTranslationX = 0.0F,
			float endTranslationX = 1.0F)
		{
			AnimationClipCompileInput input;
			input.assetId = MakeIntegrationGuid(assetSeed);
			input.skeletonAssetId = skeleton.assetId;
			input.compatibilityFingerprint = skeleton.compatibilityFingerprint;
			// A nontrivial playback interval keeps successive frames observable;
			// a one-tick loop would continuously return to the same sample.
			input.durationTicks = 60;
			input.sampleRate = 60U;
			input.segmentDurationTicks = 60;

			AnimationSourceTrack translation;
			translation.stableTargetId = skeleton.joints[0U].stableId;
			translation.kind = AnimationClipTrackKind::Translation;
			translation.interpolation = AnimationInterpolation::Linear;
			AnimationSourceKey begin;
			begin.tick = 0;
			begin.vectorValue = {beginTranslationX, 0.0F, 0.0F};
			AnimationSourceKey end;
			end.tick = 60;
			end.vectorValue = {endTranslationX, 0.0F, 0.0F};
			translation.keys = {begin, end};
			input.tracks.push_back(std::move(translation));

			auto clip = std::make_shared<AnimationClipAsset>();
			AnimationClipCompileReport report;
			context.Expect(CompileAnimationClip(input, {}, *clip, report) == AnimationStatus::Ok,
				"The production integration fixture must compile one valid immutable clip.");
			return clip;
		}

		[[nodiscard]] std::shared_ptr<const GpuAnimationResidentPayloads>
		MakeAtlasResidentPayload(float constantTranslationX)
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



		AnimatorPlaybackBinding MakeNestedBinding(TestContext& context) {
			AnimatorPlaybackBinding binding;
			binding.skeleton = MakeIntegrationSkeleton();
			binding.clip = MakeIntegrationClip(context, *binding.skeleton, 20U, 1.0F, 1.0F);
			binding.residentSegments.assign(binding.clip->segments.size(), 1U);
			binding.jointStableIds = {101U}; binding.jointParents = {-1};
			binding.requestedExecutionPolicy = AnimationExecutionPolicy::ForceGpuVisual;
			binding.updatePolicy = AnimatorUpdatePolicy::EveryFrame;
			binding.eventPolicy = AnimatorEventPolicy::Suppress;
			binding.playback.loop = true; binding.playOnBind = true;
			binding.rootMotionPolicy = AnimatorRootMotionPolicy::Ignore;
			binding.evaluationGenerations.residencyGeneration = 1U;
			// The same checked direct-clip program shape is authored by the real
			// component coordinator. No controller transition or CPU consumer is added.
			auto program = std::make_shared<EvaluationProgram>();
			program->controllerAssetId = MakeIntegrationGuid(80U);
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
			binding.gpuResidentPayloads = MakeAtlasResidentPayload(1.0F);
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

		class NestedAnimationFixture {
		public:
			explicit NestedAnimationFixture(TestContext& context, bool mixed) : system(AnimationSystem::Instance()),
				priorHeadless(system.IsHeadlessExecutionRequired()), priorAuthored(system.IsAuthoredComponentPlaybackEnabled()),
				priorAdvancing(system.IsAuthoredComponentPlaybackAdvancing()) {
				try {
				ThreadPool::instance().shutdown(); ThreadPool::instance().initialize(3);
				system.Initialize(); system.SetHeadlessExecutionRequired(false);
				system.SetAuthoredComponentPlaybackEnabled(false);
				// This runtime fixture owns its clock instead of inheriting a paused
				// editor policy from a preceding singleton-based test.
				system.SetAuthoredComponentPlaybackAdvancing(true);
				const auto compact = MakeNestedBinding(context);
				auto cpu = compact; cpu.requestedExecutionPolicy = AnimationExecutionPolicy::ForceCpuDeterministicFallback;
				// 8193 gives three coarse publication chunks, so index-one admission
				// can fail after index-zero work has actually acquired queue ownership.
				const std::size_t count = 8193U + (mixed ? 1U : 0U);
				handles.reserve(count); std::vector<AnimatorPlaybackBindRequest> bindings;
				bindings.reserve(count); std::vector<AnimationStatus> statuses(count);
				for (std::size_t index = 0; index < count; ++index) {
					AnimatorInstanceDescriptor descriptor;
					const std::uint64_t identity = index + 1U;
					std::memcpy(descriptor.ownerObjectId.data(), &identity, sizeof(identity));
					descriptor.ownerObjectId[15U] = 0x62U; descriptor.componentGeneration = 1U;
					const auto handle = system.CreateAnimatorInstance(descriptor);
					context.Expect(handle.IsValid(), "Every real crowd instance must have a valid runtime handle.");
					handles.push_back(handle);
					bindings.push_back({handle, mixed && index == 8193U ? &cpu : &compact, nullptr, true});
				}
				system.BindAnimatorPlaybackBatch(bindings, statuses);
				context.Expect(std::all_of(statuses.begin(), statuses.end(), [](AnimationStatus value) { return value == AnimationStatus::Ok; }), "The protected production batch must bind the complete real crowd.");
				} catch (...) {
					// Construction can fail after acquiring handles, before a destructor
					// exists. Settle those owners without replacing the fixture failure.
					Cleanup();
					throw;
				}
			}
			~NestedAnimationFixture() { Cleanup(); }
			void Cleanup() noexcept {
				// Preserve the singleton's policy for following independently owned tests.
				try { SetNestedTaskHookForTesting({}); } catch (...) {}
				try { (void)system.InvalidateAnimatorInstances(handles); } catch (...) {}
				try { system.Shutdown(); } catch (...) {}
				try { system.SetHeadlessExecutionRequired(priorHeadless); } catch (...) {}
				try { system.SetAuthoredComponentPlaybackEnabled(priorAuthored); } catch (...) {}
				try { system.SetAuthoredComponentPlaybackAdvancing(priorAdvancing); } catch (...) {}
				try { ThreadPool::instance().shutdown(); } catch (...) {}
			}
			bool Frame(std::string* initiatingFailure = nullptr) {
				try {
					auto declarations = system.DeclareFrameWorkloads(1.0F / 60.0F);
					// Record the exception at its real workload boundary before the
					// compatibility scheduler converts it to a failed frame diagnostic.
					for (auto& declaration : declarations) if (declaration.work) {
						auto original = std::move(declaration.work);
						declaration.work = [original = std::move(original), initiatingFailure](TerraEngine::Scheduling::JobGraphContext& context) {
							try { original(context); }
							catch (const std::exception& error) { if (initiatingFailure && initiatingFailure->empty()) *initiatingFailure = error.what(); throw; }
						};
					}
					const auto frame = TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(std::move(declarations), "animation-nested-production");
					return frame.valid && frame.completed;
				} catch (const std::exception& error) {
					if (initiatingFailure && initiatingFailure->empty()) *initiatingFailure = error.what(); return false;
				}
			}
			AnimationSystem& system;
			bool priorHeadless;
			bool priorAuthored;
			bool priorAdvancing;
			std::vector<AnimatorInstanceHandle> handles;
		};

		void CheckActualGroup(TestContext& context, NestedTaskGroup group, bool mixed) {
			NestedAnimationFixture fixture(context, mixed);
			// Detailed rows copy the real policy reason; they neither request CPU
			// pose authority nor change the immutable binding's eligibility.
			auto diagnosticLease = AcquireAnimationDetailedDiagnosticsRequest();
			std::atomic_size_t observedChildren = 0;
			SetNestedTaskHookForTesting([&](NestedTaskCheckpoint point) { if (point.group == group && point.stage == NestedTaskStage::Child) ++observedChildren; });
			// Mode promotion and immutable binding proofs must come from completed
			// production frames; a manually forged compact-class flag cannot satisfy it.
			bool promoted = false;
			std::string promotionEvidence;
			for (int warm = 0; warm < 8; ++warm) {
				context.Expect(fixture.Frame(), "Warm production animation frames must complete before fault injection.");
				const auto published = fixture.system.LoadPublishedAuthorityFrame();
				const auto* row = published ? published->Find(fixture.handles.front()) : nullptr;
				// Compact publication rows keep default compatibility mode/status
				// fields. Their authoritative GPU admission lives in this same root's
				// validated sidecar, rather than in the broad CPU diagnostic row.
				promoted = row && row->active && !row->safeFallback && published->sidecars
					&& std::any_of(published->sidecars->gpuCandidates.begin(), published->sidecars->gpuCandidates.end(), [&](const auto& candidate) {
						return candidate.key.slot == fixture.handles.front().slotIndex
							&& candidate.key.lifetimeGeneration == fixture.handles.front().generation
							&& candidate.key.sessionGeneration == published->sessionGeneration
							&& candidate.status == AnimationStatus::Ok
							&& candidate.IsValidFor(published->authorityGeneration);
					});
				// Copy the actual accepted frame's policy evidence before fixture cleanup
				// retires it. A setup failure must identify its fallback, not hide it.
				promotionEvidence = " children=" + std::to_string(observedChildren.load())
					+ " root=" + std::to_string(bool(published)) + " row=" + std::to_string(bool(row));
				if(row) promotionEvidence += " mode=" + std::string(ToString(row->effectiveExecutionMode))
					+ " status=" + std::to_string(static_cast<unsigned>(row->status)) + " tick=" + std::to_string(row->tick);
				const auto integration = fixture.system.ReadProductionIntegrationSnapshot();
				promoted = promoted && integration.planStatus == AnimationStatus::Ok
					&& integration.rejectionReason == AnimationPlanRejectionReason::None;
				promotionEvidence += " planStatus=" + std::to_string(static_cast<unsigned>(integration.planStatus))
					+ " planRejection=" + std::to_string(static_cast<unsigned>(integration.rejectionReason))
					+ " items=" + std::to_string(integration.itemCount) + " scratch=" + std::to_string(integration.scratchBytes);
				if(const auto diagnostics = AcquireAnimationDiagnosticsSnapshot()) {
					for(std::size_t reason = 0; reason < diagnostics->counters.fallbackReasonCounts.size(); ++reason) {
						if(diagnostics->counters.fallbackReasonCounts[reason]) promotionEvidence += " "
							+ std::string(ToString(static_cast<AnimationFallbackReason>(reason))) + "="
							+ std::to_string(diagnostics->counters.fallbackReasonCounts[reason]);
					}
				}
				if(const auto detailed = AcquireAnimationDetailedDiagnosticsSnapshot()) {
					const auto first = std::find_if(detailed->animators.begin(), detailed->animators.end(), [&](const auto& record) {
						return record.instanceSlot == fixture.handles.front().slotIndex;
					});
					if(first != detailed->animators.end()) promotionEvidence += " selection=" + std::string(ToString(first->selectionReason));
				}
				if (promoted && observedChildren.load() >= 2 && warm >= 2) break;
			}
			SetNestedTaskHookForTesting({});
			context.Expect(promoted, "Completed production frames must publish successful compact GPU authority." + promotionEvidence);
			context.Expect(observedChildren.load() >= 2, "The requested production nested group must admit at least two real children.");
			const std::array stages{NestedTaskStage::Creation, NestedTaskStage::Submission, NestedTaskStage::Inline, NestedTaskStage::Child, NestedTaskStage::Result};
			for (const auto stage : stages) {
				// Compact runtime queues every chunk; it has no inline branch to fault.
				if (group == NestedTaskGroup::AnimationCompactRuntime && stage == NestedTaskStage::Inline) continue;
				const auto prior = fixture.system.LoadPublishedAuthorityFrame();
				const auto priorGeneration = fixture.system.AuthorityPublicationGeneration();
				ControlledGate held;
				const auto waitHeld = held.MakeWaiter(std::chrono::seconds(10));
				auto faultReached = std::make_shared<std::promise<void>>(); auto fault = faultReached->get_future();
				auto finished = std::make_shared<std::promise<void>>(); auto finishedFuture = finished->get_future();
				std::atomic_bool childReleased = false; std::atomic_bool blockedUntilRelease = false; std::atomic_bool faultSeen = false;
				const std::size_t heldIndex = stage == NestedTaskStage::Result ? 1U : 0U;
				const std::size_t faultIndex = stage == NestedTaskStage::Result ? 0U : 1U;
				const std::string reason = "actual animation nested failure " + std::to_string(static_cast<int>(group)) + "/" + std::to_string(static_cast<int>(stage));
				SetNestedTaskHookForTesting([&, waitHeld, faultReached, reason](NestedTaskCheckpoint point) {
					if (point.group != group) return;
					if (point.stage == NestedTaskStage::Child && point.childIndex == heldIndex) {
						if (waitHeld() != GateResult::Released) throw std::runtime_error("held production sibling did not release");
						childReleased.store(true); return;
					}
					if (point.stage == stage && (stage == NestedTaskStage::Inline || point.childIndex == faultIndex)) {
						if (!held.WaitForArrivals(1)) throw std::runtime_error("production sibling was not admitted");
						if (!faultSeen.exchange(true)) faultReached->set_value();
						throw std::runtime_error(reason);
					}
				});
				// This observer controls only the held child. The real workload executes
				// on its coordinator thread, leaving all three pool workers available.
				std::jthread release([&] {
					if (fault.wait_for(std::chrono::seconds(10)) == std::future_status::ready)
						blockedUntilRelease.store(finishedFuture.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
					held.Release();
				});
				std::string primary;
				const bool accepted = fixture.Frame(&primary); finished->set_value(); release.join();
				SetNestedTaskHookForTesting({});
				context.Expect(faultSeen.load() && !accepted && primary == reason, "Actual production failure must preserve the initiating exception and reject the frame.");
				context.Expect(blockedUntilRelease.load() && childReleased.load(), "Actual admitted sibling ownership must settle before the failing workload returns.");
				context.Expect(fixture.system.LoadPublishedAuthorityFrame() == prior && fixture.system.AuthorityPublicationGeneration() == priorGeneration,
					"A rejected real animation frame must not publish a partial authority root.");
				context.Expect(fixture.Frame() && fixture.system.AuthorityPublicationGeneration() > priorGeneration, "A later fault-free production frame must recover and publish normally.");
			}
		}
	}
	void RegisterAnimationNestedTaskSettlementTests(TestRegistry& registry) {
		registry.Add("Engine.NestedTaskSettlement.AnimationCompactCapture", [](TestContext& context) { CheckActualGroup(context, NestedTaskGroup::AnimationCompactCapture, false); });
		registry.Add("Engine.NestedTaskSettlement.AnimationHomogeneousRows", [](TestContext& context) { CheckActualGroup(context, NestedTaskGroup::AnimationHomogeneousRows, false); });
		registry.Add("Engine.NestedTaskSettlement.AnimationMixedRows", [](TestContext& context) { CheckActualGroup(context, NestedTaskGroup::AnimationMixedRows, true); });
		registry.Add("Engine.NestedTaskSettlement.AnimationCompactRuntime", [](TestContext& context) { CheckActualGroup(context, NestedTaskGroup::AnimationCompactRuntime, false); });
	}
}
