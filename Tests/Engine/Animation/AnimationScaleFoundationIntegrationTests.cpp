// AnimationScaleFoundationIntegrationTests.cpp - production ImplementationCheck-ImplementationCheck wiring checks.

#include "TestHarness.h"

#include "Animation/AnimationSystem.h"
#include "Animation/Assets/MotionDatabaseAsset.h"
#include "Animation/Assets/RetargetProfileAsset.h"
#include "Animation/Graph/EvaluationProgram.h"
#include "Animation/Retargeting/RetargetValidator.h"
#include "AnimationClipCompiler.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Scripting/ScriptGlue.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::AssetPipeline;

		AnimationGuid MakeIntegrationGuid(std::uint8_t seed)
		{
			AnimationGuid guid;
			for(std::size_t index = 0U; index < guid.value.size(); ++index) {
				guid.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return guid;
		}

		template<typename Record>
		Record MakeAdvancedRecord(NativeAnimationRecordKind kind)
		{
			Record record{};
			record.header = {
				static_cast<std::uint32_t>(sizeof(Record)),
				kNativeAnimationAbiVersion,
				static_cast<std::uint16_t>(kind)};
			return record;
		}

		NativeAnimationUuid MakeNativeUuid(const AnimationGuid& value) noexcept
		{
			NativeAnimationUuid result{};
			std::memcpy(&result.first, value.value.data(), sizeof(result.first));
			std::memcpy(&result.second,
				value.value.data() + sizeof(result.first), sizeof(result.second));
			return result;
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
			input.durationTicks = 1;
			input.sampleRate = 60U;
			input.segmentDurationTicks = 1;

			AnimationSourceTrack translation;
			translation.stableTargetId = skeleton.joints[0U].stableId;
			translation.kind = AnimationClipTrackKind::Translation;
			translation.interpolation = AnimationInterpolation::Linear;
			AnimationSourceKey begin;
			begin.tick = 0;
			begin.vectorValue = {beginTranslationX, 0.0F, 0.0F};
			AnimationSourceKey end;
			end.tick = 1;
			end.vectorValue = {endTranslationX, 0.0F, 0.0F};
			translation.keys = {begin, end};
			input.tracks.push_back(std::move(translation));

			auto clip = std::make_shared<AnimationClipAsset>();
			AnimationClipCompileReport report;
			context.Expect(CompileAnimationClip(input, {}, *clip, report) == AnimationStatus::Ok,
				"The production integration fixture must compile one valid immutable clip.");
			return clip;
		}

		AnimatorInstanceDescriptor MakeIntegrationDescriptor(std::uint8_t seed)
		{
			AnimatorInstanceDescriptor descriptor;
			descriptor.ownerObjectId[0U] = seed;
			descriptor.componentGeneration = 3U;
			return descriptor;
		}

		// Every integration case owns a live runtime handle. Automatic cleanup keeps a
		// failed expectation from contaminating later all-or-none publication tests.
		class IntegrationAnimatorGuard final {
		public:
			IntegrationAnimatorGuard(
				AnimationSystem& system,
				AnimatorInstanceHandle handle) noexcept
				: system_(system), handle_(handle)
			{
			}

			~IntegrationAnimatorGuard()
			{
				if(handle_.IsValid()) (void)system_.InvalidateAnimatorInstance(handle_);
			}

			IntegrationAnimatorGuard(const IntegrationAnimatorGuard&) = delete;
			IntegrationAnimatorGuard& operator=(const IntegrationAnimatorGuard&) = delete;

		private:
			AnimationSystem& system_;
			AnimatorInstanceHandle handle_;
		};

		AnimatorPlaybackBinding MakeIntegrationBinding(TestContext& context)
		{
			AnimatorPlaybackBinding binding;
			binding.skeleton = MakeIntegrationSkeleton();
			binding.clip = MakeIntegrationClip(context, *binding.skeleton);
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

		AnimatorPlaybackBinding MakeCrossfadeIntegrationBinding(TestContext& context)
		{
			AnimatorPlaybackBinding binding;
			binding.skeleton = MakeIntegrationSkeleton();
			auto idleClip = MakeIntegrationClip(
				context, *binding.skeleton, 24U, 0.0F, 0.0F);
			auto stopClip = MakeIntegrationClip(
				context, *binding.skeleton, 25U, 10.0F, 10.0F);
			auto reverseClip = MakeIntegrationClip(
				context, *binding.skeleton, 27U, -10.0F, -10.0F);
			binding.clip = idleClip;
			binding.clipGeneration = 5U;
			binding.residentSegments.assign(idleClip->segments.size(), 1U);

			auto program = std::make_shared<EvaluationProgram>();
			program->controllerAssetId = MakeIntegrationGuid(26U);
			program->compatibilityFingerprint =
				binding.skeleton->compatibilityFingerprint;
			program->states = {
				{100U, 0U, true, false, false, 5001U,
					EvaluationOpcode::SampleClip},
				{200U, 0U, false, false, false, 5002U,
					EvaluationOpcode::SampleClip},
				{300U, 0U, false, false, false, 5003U,
					EvaluationOpcode::SampleClip}};
			program->stableHash = ComputeEvaluationProgramStableHash(*program);
			binding.evaluationProgram = std::move(program);
			binding.controllerProgramRequired = true;
			binding.programGeneration = 4U;
			binding.stateId = 100U;
			binding.controllerClips = {
				{5001U, idleClip,
					std::vector<std::uint8_t>(idleClip->segments.size(), 1U), 5U},
				{5002U, stopClip,
					std::vector<std::uint8_t>(stopClip->segments.size(), 1U), 6U},
				{5003U, reverseClip,
					std::vector<std::uint8_t>(reverseClip->segments.size(), 1U), 7U}};
			binding.playback.loop = true;
			binding.playOnBind = true;
			binding.requestedExecutionPolicy =
				AnimationExecutionPolicy::ForceCpuDeterministicFallback;
			binding.rootMotionPolicy = AnimatorRootMotionPolicy::Ignore;
			binding.fallbackCompatibilityFingerprint =
				binding.skeleton->compatibilityFingerprint;
			for(const SkeletonJoint& joint : binding.skeleton->joints) {
				binding.jointStableIds.push_back(joint.stableId);
				binding.jointParents.push_back(joint.parentIndex);
			}
			return binding;
		}

		std::shared_ptr<MotionDatabaseAsset> MakeMotionIntegrationDatabase(
			AnimationCompatibilityFingerprint compatibility);

		AnimatorPlaybackBinding MakeMotionSelectionIntegrationBinding(TestContext& context)
		{
			AnimatorPlaybackBinding binding;
			binding.skeleton = MakeIntegrationSkeleton();
			auto firstClip = MakeIntegrationClip(
				context, *binding.skeleton, 21U, 0.0F, 1.0F);
			auto selectedClip = MakeIntegrationClip(
				context, *binding.skeleton, 22U, 8.0F, 9.0F);
			binding.clip = firstClip;
			binding.residentSegments.assign(firstClip->segments.size(), 1U);

			auto program = std::make_shared<EvaluationProgram>();
			program->controllerAssetId = MakeIntegrationGuid(23U);
			program->compatibilityFingerprint = binding.skeleton->compatibilityFingerprint;
			program->states = {
				{100U, 0U, true, false, false, 5001U, EvaluationOpcode::SampleClip},
				{200U, 0U, false, false, false, 5002U, EvaluationOpcode::SampleClip}};
			program->stableHash = ComputeEvaluationProgramStableHash(*program);
			binding.evaluationProgram = std::move(program);
			binding.programGeneration = 4U;
			binding.stateId = 100U;
			binding.controllerClips = {
				{5001U, firstClip, std::vector<std::uint8_t>(firstClip->segments.size(), 1U), 5U},
				{5002U, selectedClip, std::vector<std::uint8_t>(selectedClip->segments.size(), 1U), 6U}};

			auto database = MakeMotionIntegrationDatabase(
				binding.skeleton->compatibilityFingerprint);
			database->samples.front().sourceClipId = 5002U;
			database->samples.front().sourceTick = 1;
			database->featureSchemaFingerprint =
				ComputeMotionDatabaseSchemaFingerprint(*database);
			binding.motionDatabase = std::move(database);
			binding.motionDatabaseGeneration = 12U;
			binding.updatePolicy = AnimatorUpdatePolicy::Manual;
			binding.requestedExecutionPolicy =
				AnimationExecutionPolicy::ForceCpuDeterministicFallback;
			binding.fallbackCompatibilityFingerprint =
				binding.skeleton->compatibilityFingerprint;
			for(const SkeletonJoint& joint : binding.skeleton->joints) {
				binding.jointStableIds.push_back(joint.stableId);
				binding.jointParents.push_back(joint.parentIndex);
			}
			return binding;
		}

		std::shared_ptr<SkeletonAsset> MakeRetargetIntegrationSkeleton(
			std::uint8_t guidSeed,
			AnimationCompatibilityFingerprint compatibility,
			std::uint64_t referencePoseHash,
			std::uint64_t rootId,
			std::uint64_t hipsId,
			float height)
		{
			auto skeleton = std::make_shared<SkeletonAsset>();
			skeleton->assetId = MakeIntegrationGuid(guidSeed);
			skeleton->compatibilityFingerprint = compatibility;
			skeleton->referencePoseHash = referencePoseHash;
			skeleton->rootJointIndex = 0U;
			SkeletonJoint root;
			root.stableId = rootId;
			root.parentIndex = -1;
			root.displayName = "Root";
			root.normalizedSourcePath = "/root";
			root.semanticRole = "Root";
			root.deformsMesh = true;
			SkeletonJoint hips;
			hips.stableId = hipsId;
			hips.parentIndex = 0;
			hips.localBindTransform.translation = {0.0F, height, 0.0F};
			hips.displayName = "Hips";
			hips.normalizedSourcePath = "/root/hips";
			hips.semanticRole = "Hips";
			hips.deformsMesh = true;
			skeleton->joints = {root, hips};
			skeleton->inverseBindMatrices = {
				MakeIdentityMatrix(), MakeIdentityMatrix()};
			return skeleton;
		}

		std::shared_ptr<AnimationClipAsset> MakeRetargetIntegrationClip(
			TestContext& context,
			const SkeletonAsset& source)
		{
			AnimationClipCompileInput input;
			input.assetId = MakeIntegrationGuid(151U);
			input.skeletonAssetId = source.assetId;
			input.compatibilityFingerprint = source.compatibilityFingerprint;
			input.durationTicks = 60;
			input.sampleRate = 60U;
			input.segmentDurationTicks = 60;

			AnimationSourceTrack rootTranslation;
			rootTranslation.stableTargetId = source.joints[0U].stableId;
			rootTranslation.kind = AnimationClipTrackKind::Translation;
			rootTranslation.interpolation = AnimationInterpolation::Linear;
			AnimationSourceKey begin;
			begin.tick = 0;
			begin.vectorValue = {1.0F, 0.0F, 0.0F};
			AnimationSourceKey end;
			end.tick = 60;
			end.vectorValue = {2.0F, 0.0F, 0.0F};
			rootTranslation.keys = {begin, end};
			input.tracks.push_back(std::move(rootTranslation));

			auto clip = std::make_shared<AnimationClipAsset>();
			AnimationClipCompileReport report;
			context.Expect(CompileAnimationClip(input, {}, *clip, report)
				== AnimationStatus::Ok,
				"The production retarget fixture must compile source-rig motion through the real cooker.");
			return clip;
		}

		std::shared_ptr<RetargetProfileAsset> MakeRetargetIntegrationProfile(
			const SkeletonAsset& source,
			const SkeletonAsset& target)
		{
			auto profile = std::make_shared<RetargetProfileAsset>();
			profile->assetId = MakeIntegrationGuid(171U);
			profile->runtimeGeneration = 9U;
			profile->compatibilityFingerprint = 0x51726774696e7465ULL;
			profile->sourceSkeletonAssetId = source.assetId;
			profile->targetSkeletonAssetId = target.assetId;
			profile->sourceSkeletonFingerprint = source.compatibilityFingerprint;
			profile->targetSkeletonFingerprint = target.compatibilityFingerprint;
			profile->sourceReferencePoseHash = source.referencePoseHash;
			profile->targetReferencePoseHash = target.referencePoseHash;
			profile->semanticMappings = {
				{1U, 0U, 0U, 0U, source.joints[0U].stableId,
					target.joints[0U].stableId, "Root", "Root", true},
				{2U, 1U, 0U, 0U, source.joints[1U].stableId,
					target.joints[1U].stableId, "Hips", "Hips", true}};
			profile->proportion.sourceReferenceHeightMeters = 1.0F;
			profile->proportion.targetReferenceHeightMeters = 2.0F;
			profile->root.sourceRootJointId = source.joints[0U].stableId;
			profile->root.targetRootJointId = target.joints[0U].stableId;
			profile->quality.authoredCoverage = 1.0F;
			profile->quality.reviewReason = "Production integration fixture";
			return profile;
		}

		std::shared_ptr<MotionDatabaseAsset> MakeMotionIntegrationDatabase(
			AnimationCompatibilityFingerprint compatibility)
		{
			auto database = std::make_shared<MotionDatabaseAsset>();
			database->assetId = MakeIntegrationGuid(191U);
			database->runtimeGeneration = 12U;
			database->compatibilityFingerprint = compatibility;
			database->sourceFingerprint = 0x6d6f74696f6e6462ULL;
			database->quantizationScales.fill(1.0F);
			MotionDatabaseSample sample;
			sample.stableSampleId = 7001U;
			sample.sourceClipId = 5001U;
			sample.sourceTick = 10;
			database->samples.push_back(sample);
			database->featureSchemaFingerprint =
				ComputeMotionDatabaseSchemaFingerprint(*database);
			return database;
		}

		AnimatorPlaybackBinding MakeAdvancedIntegrationBinding(TestContext& context)
		{
			AnimatorPlaybackBinding binding;
			auto source = MakeRetargetIntegrationSkeleton(
				111U, 0x1111222233334444ULL, 0x1111000011110000ULL,
				1U, 2U, 1.0F);
			auto target = MakeRetargetIntegrationSkeleton(
				131U, 0x5555666677778888ULL, 0x2222000022220000ULL,
				101U, 102U, 2.0F);
			auto profile = MakeRetargetIntegrationProfile(*source, *target);
			RetargetValidationResult validation;
			RetargetCompiledProfile compiled;
			context.Expect(CompileRetargetProfile(
				*profile, *source, *target, validation, compiled) == AnimationStatus::Ok,
				"The minimal source/target mapping must satisfy the production retarget validator.");

			binding.clip = MakeRetargetIntegrationClip(context, *source);
			binding.skeleton = std::move(target);
			binding.retargetSourceSkeleton = std::move(source);
			binding.retargetProfile = std::move(profile);
			binding.motionDatabase = MakeMotionIntegrationDatabase(
				binding.skeleton->compatibilityFingerprint);
			binding.clipGeneration = 7U;
			binding.skeletonGeneration = 8U;
			binding.retargetProfileGeneration = 9U;
			binding.retargetSourceSkeletonGeneration = 10U;
			binding.motionDatabaseGeneration = 12U;
			binding.residentSegments.assign(binding.clip->segments.size(), 1U);
			for(const SkeletonJoint& joint : binding.skeleton->joints) {
				binding.jointStableIds.push_back(joint.stableId);
				binding.jointParents.push_back(joint.parentIndex);
			}
			binding.requestedExecutionPolicy =
				AnimationExecutionPolicy::ForceCpuDeterministicFallback;
			binding.rootMotionPolicy = AnimatorRootMotionPolicy::Ignore;
			binding.updatePolicy = AnimatorUpdatePolicy::Manual;
			binding.fallbackCompatibilityFingerprint =
				binding.skeleton->compatibilityFingerprint;
			return binding;
		}

		bool ExecuteIntegrationFrame(
			AnimationSystem& system,
			const char* frameId)
		{
			std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> workloads =
				system.DeclareFrameWorkloads(1.0F / 60.0F);
			const TerraEngine::Scheduling::FrameExecutionPlan plan =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(workloads), frameId);
			return plan.valid && plan.completed;
		}
	}

	void RegisterAnimationScaleFoundationIntegrationTests(TestRegistry& registry)
	{
		registry.Add("Animation.ScaleFoundation.ZeroChannelShellAdvancesAuthority", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
				MakeIntegrationDescriptor(90U));
			IntegrationAnimatorGuard cleanup(system, animator);
			const std::uint64_t priorAuthority = system.AuthorityPublicationGeneration();

			context.Expect(animator.IsValid()
				&& ExecuteIntegrationFrame(system, "animation-scale-zero-channel"),
				"A never-bound zero-channel animator must complete through the production scheduler.");
			const std::shared_ptr<const PublishedAnimatorPose> published =
				system.LoadPublishedAnimatorPose(animator);
			context.Expect(published && published->safePose && published->snapshot
				&& published->snapshot->pose
				&& published->snapshot->pose->LocalTransforms().empty()
				&& system.AuthorityPublicationGeneration() > priorAuthority,
				"The typed zero-channel safe pose must advance authority without fabricating skeleton channels.");
		});

		registry.Add("Animation.ScaleFoundation.ProductionPlanCacheAndMemoryAreConnected", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
				MakeIntegrationDescriptor(91U));
			IntegrationAnimatorGuard cleanup(system, animator);
			AnimatorPlaybackBinding binding = MakeIntegrationBinding(context);
			context.Expect(animator.IsValid()
				&& system.BindAnimatorPlayback(animator, binding) == AnimationStatus::Ok,
				"A valid manual CPU animator must enter the production scheduler path.");

			context.Expect(ExecuteIntegrationFrame(system, "animation-scale-first"),
				"The first production frame must evaluate and publish a complete pose.");
			const std::shared_ptr<const PublishedAnimatorPose> first =
				system.LoadPublishedAnimatorPose(animator);
			const AnimationProductionIntegrationSnapshot firstDiagnostics =
				system.ReadProductionIntegrationSnapshot();
			context.Expect(first && firstDiagnostics.planStatus == AnimationStatus::Ok
				&& firstDiagnostics.itemCount == 1U && firstDiagnostics.rangeCount == 1U
				&& firstDiagnostics.scratchBytes != 0U
				&& firstDiagnostics.deterministicPublicationValidated,
				"Production capture must validate one deterministic plan and its disjoint scratch range.");

			context.Expect(ExecuteIntegrationFrame(system, "animation-scale-reuse"),
				"An unchanged eligible manual animator must complete through cache reuse.");
			const std::shared_ptr<const PublishedAnimatorPose> second =
				system.LoadPublishedAnimatorPose(animator);
			const AnimationProductionIntegrationSnapshot reusedDiagnostics =
				system.ReadProductionIntegrationSnapshot();
			const std::size_t posePoolIndex = static_cast<std::size_t>(
				AnimationMemoryClass::PosePool);
			context.Expect(second == first && reusedDiagnostics.reusedItemCount == 1U
				&& reusedDiagnostics.cacheEntryCount == 1U
				&& reusedDiagnostics.memory.classes[posePoolIndex].bytesInUse != 0U
				&& reusedDiagnostics.memory.classes[posePoolIndex].highWaterBytes
					>= reusedDiagnostics.memory.classes[posePoolIndex].bytesInUse,
				"A truthful unchanged cache hit must retain the exact pose and its bounded lifetime lease.");
		});

		registry.Add("Animation.ScaleFoundation.ScriptCrossfadePublishesContinuousPose",
			[](TestContext& context) {
				AnimationSystem& system = AnimationSystem::Instance();
				system.Initialize();
				const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
					MakeIntegrationDescriptor(99U));
				IntegrationAnimatorGuard cleanup(system, animator);
				AnimatorPlaybackBinding binding =
					MakeCrossfadeIntegrationBinding(context);
				context.Expect(animator.IsValid()
					&& system.BindAnimatorPlayback(animator, std::move(binding))
						== AnimationStatus::Ok
					&& ExecuteIntegrationFrame(
						system, "animation-crossfade-source"),
					"The script-crossfade fixture must publish its source state before the command.");

				auto publishedX = [&system, animator]() {
					const std::shared_ptr<const PublishedAnimatorPose> publication =
						system.LoadPublishedAnimatorPose(animator);
					if(!publication || !publication->snapshot
						|| !publication->snapshot->pose
						|| publication->snapshot->pose->LocalTransforms().empty()) {
						return std::numeric_limits<float>::quiet_NaN();
					}
					return publication->snapshot->pose
						->LocalTransforms().front().translation.x;
				};
				context.Expect(std::abs(publishedX()) <= 0.01F,
					"The source clip must begin at its authored zero-translation pose.");

				NativeAnimationCommandRequest crossfade =
					MakeAdvancedRecord<NativeAnimationCommandRequest>(
						NativeAnimationRecordKind::CommandRequest);
				crossfade.animator = {
					animator.slotIndex,
					animator.generation};
				crossfade.command = NativeAnimationCommandKind::CrossFade;
				crossfade.stateId = 200U;
				crossfade.transitionSeconds =
					4.0F / static_cast<float>(kAnimationFixedTicksPerSecond);
				crossfade.producerId = 99U;
				crossfade.producerSequence = 1U;
				crossfade.correlationId = 9901U;
				crossfade.idempotencyKey = 99001U;
				crossfade.cancellationToken = 990001U;
				NativeAnimationCommandReceipt crossfadeReceipt =
					MakeAdvancedRecord<NativeAnimationCommandReceipt>(
						NativeAnimationRecordKind::CommandReceipt);
				context.Expect(Native_Animation_SubmitCommand(
						&crossfade, &crossfadeReceipt)
						== NativeAnimationStatus::Queued,
					"The same copied native boundary used by C# and exported builds must queue a typed four-tick crossfade.");

				std::array<float, 5U> samples{};
				std::array<float, 5U> progress{};
				std::array<std::uint64_t, 5U> states{};
				for(std::size_t frame = 0U; frame < samples.size(); ++frame) {
					const std::string frameName =
						"animation-script-crossfade-" + std::to_string(frame);
					context.Expect(ExecuteIntegrationFrame(
						system, frameName.c_str()),
						"Every transition tick must complete through the production scheduler.");
					samples[frame] = publishedX();
					const std::shared_ptr<const PublishedAnimatorPose> publication =
						system.LoadPublishedAnimatorPose(animator);
					if(publication && publication->snapshot) {
						progress[frame] =
							publication->snapshot->playback.transitionProgress;
						states[frame] =
							publication->snapshot->playback.currentStateId;
					}
				}
				context.Expect(std::abs(samples[0U] - 0.0F) <= 0.01F
					&& std::abs(samples[1U] - 2.5F) <= 0.02F
					&& std::abs(samples[2U] - 5.0F) <= 0.02F
					&& std::abs(samples[3U] - 7.5F) <= 0.02F
					&& std::abs(samples[4U] - 10.0F) <= 0.02F,
					std::string(
						"A scripted crossfade must publish the blended source/destination pose on every fixed tick instead of holding then snapping. samples=")
						+ std::to_string(samples[0U]) + ","
						+ std::to_string(samples[1U]) + ","
						+ std::to_string(samples[2U]) + ","
						+ std::to_string(samples[3U]) + ","
						+ std::to_string(samples[4U])
						+ " progress=" + std::to_string(progress[0U]) + ","
						+ std::to_string(progress[1U]) + ","
						+ std::to_string(progress[2U]) + ","
						+ std::to_string(progress[3U]) + ","
						+ std::to_string(progress[4U])
						+ " states=" + std::to_string(states[0U]) + ","
						+ std::to_string(states[1U]) + ","
						+ std::to_string(states[2U]) + ","
						+ std::to_string(states[3U]) + ","
						+ std::to_string(states[4U])
						+ " receipt="
						+ std::to_string(static_cast<std::uint32_t>(
							system.ReadCommandReceipt(
								crossfade.producerId,
								crossfade.producerSequence,
								crossfade.correlationId).status)));
				const std::shared_ptr<const PublishedAnimatorPose> completed =
					system.LoadPublishedAnimatorPose(animator);
				context.Expect(completed && completed->snapshot
					&& completed->snapshot->playback.currentStateId == 200U
					&& completed->snapshot->playback.previousStateId == 0U,
					"The final blended tick must promote the destination state atomically.");
			});

		registry.Add("Animation.ScaleFoundation.InterruptedCrossfadeCapturesPublishedPose",
			[](TestContext& context) {
				AnimationSystem& system = AnimationSystem::Instance();
				system.Initialize();
				const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
					MakeIntegrationDescriptor(100U));
				IntegrationAnimatorGuard cleanup(system, animator);
				AnimatorPlaybackBinding binding =
					MakeCrossfadeIntegrationBinding(context);
				context.Expect(animator.IsValid()
					&& system.BindAnimatorPlayback(animator, std::move(binding))
						== AnimationStatus::Ok
					&& ExecuteIntegrationFrame(
						system, "animation-interruption-source"),
					"The interruption fixture must publish its source pose.");

				auto submitCrossfade = [&system, animator](
					std::uint64_t stateId,
					std::uint64_t sequence) {
					AnimationCommand command;
					command.target = animator;
					command.kind = AnimationCommandKind::CrossFade;
					command.stateId = stateId;
					command.transitionTicks = 4;
					command.producerId = 100U;
					command.producerSequence = sequence;
					command.correlationId = 10000U + sequence;
					command.idempotencyKey = 100000U + sequence;
					command.cancellationToken = 1000000U + sequence;
					return system.SubmitCommand(command).status;
				};
				auto publishedX = [&system, animator]() {
					const std::shared_ptr<const PublishedAnimatorPose> publication =
						system.LoadPublishedAnimatorPose(animator);
					if(!publication || !publication->snapshot
						|| !publication->snapshot->pose
						|| publication->snapshot->pose->LocalTransforms().empty()) {
						return std::numeric_limits<float>::quiet_NaN();
					}
					return publication->snapshot->pose
						->LocalTransforms().front().translation.x;
				};

				context.Expect(submitCrossfade(200U, 1U)
						== AnimationStatus::Queued
					&& ExecuteIntegrationFrame(
						system, "animation-interruption-first-0")
					&& ExecuteIntegrationFrame(
						system, "animation-interruption-first-1")
					&& ExecuteIntegrationFrame(
						system, "animation-interruption-first-2")
					&& std::abs(publishedX() - 5.0F) <= 0.02F,
					"The first transition must reach its deterministic halfway pose.");
				context.Expect(submitCrossfade(300U, 2U)
						== AnimationStatus::Queued,
					"An active gameplay transition must accept a replacement destination.");

				std::array<float, 5U> samples{};
				for(std::size_t frame = 0U; frame < samples.size(); ++frame) {
					const std::string frameName =
						"animation-interruption-replacement-"
						+ std::to_string(frame);
					context.Expect(ExecuteIntegrationFrame(
						system, frameName.c_str()),
						"The replacement transition must complete through the production scheduler.");
					samples[frame] = publishedX();
				}
				context.Expect(std::abs(samples[0U] - 5.0F) <= 0.02F
					&& std::abs(samples[1U] - 1.25F) <= 0.02F
					&& std::abs(samples[2U] + 2.5F) <= 0.02F
					&& std::abs(samples[3U] + 6.25F) <= 0.02F
					&& std::abs(samples[4U] + 10.0F) <= 0.02F,
					"Replacing a crossfade must use the last complete blended pose as its immutable source, avoiding a jump back to either old endpoint.");
			});

		registry.Add("Animation.ScaleFoundation.LifecycleCancellationRejectsCapturedProductionWork", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
				MakeIntegrationDescriptor(92U));
			IntegrationAnimatorGuard cleanup(system, animator);
			AnimatorPlaybackBinding binding = MakeIntegrationBinding(context);
			(void)system.BindAnimatorPlayback(animator, binding);
			context.Expect(ExecuteIntegrationFrame(system, "animation-scale-cancel-baseline"),
				"The cancellation fixture must first establish one complete publication.");
			const std::shared_ptr<const PublishedAnimatorPose> prior =
				system.LoadPublishedAnimatorPose(animator);
			const std::uint64_t priorAuthority = system.AuthorityPublicationGeneration();
			std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> stale =
				system.DeclareFrameWorkloads(1.0F / 60.0F);

			context.Expect(system.NotifyLifecycleInvalidation(
				AnimationLifecycleBoundary::Scene) == AnimationStatus::Ok,
				"A protected scene change must advance the live CPU and GPU generation latches.");
			const TerraEngine::Scheduling::FrameExecutionPlan stalePlan =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(stale), "animation-scale-cancelled");
			const AnimationProductionIntegrationSnapshot diagnostics =
				system.ReadProductionIntegrationSnapshot();
			context.Expect(stalePlan.valid && stalePlan.completed
				&& diagnostics.cancellationRequested
				&& system.AuthorityPublicationGeneration() == priorAuthority
				&& system.LoadPublishedAnimatorPose(animator) == prior,
				"Cancelled captured work must leave the last complete authority generation untouched.");
		});

		registry.Add("Animation.ScaleFoundation.RecoverableFailureHoldsTypedPriorGeneration", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
				MakeIntegrationDescriptor(93U));
			IntegrationAnimatorGuard cleanup(system, animator);
			AnimatorPlaybackBinding binding = MakeIntegrationBinding(context);
			(void)system.BindAnimatorPlayback(animator, binding);
			context.Expect(ExecuteIntegrationFrame(system, "animation-scale-fallback-baseline"),
				"The fallback fixture must establish one typed last-known-good generation.");
			const std::shared_ptr<const PublishedAnimatorPose> prior =
				system.LoadPublishedAnimatorPose(animator);

			AnimatorPlaybackBinding missing = binding;
			missing.clip.reset();
			context.Expect(system.BindAnimatorPlayback(animator, std::move(missing))
				== AnimationStatus::MissingAsset,
				"A compatible missing clip must enter explicit recoverable fallback.");
			context.Expect(ExecuteIntegrationFrame(system, "animation-scale-hold-last"),
				"The failed generation must still complete through typed hold-last selection.");
			const AnimationProductionIntegrationSnapshot diagnostics =
				system.ReadProductionIntegrationSnapshot();
			context.Expect(system.LoadPublishedAnimatorPose(animator) == prior
				&& diagnostics.fallbackItemCount == 1U,
				"Recoverable failure must preserve the exact compatible prior pose without republishing root motion.");
		});

		registry.Add("Animation.ScaleFoundation.SkeletonLossHoldsExactCompatiblePriorPose", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
				MakeIntegrationDescriptor(94U));
			IntegrationAnimatorGuard cleanup(system, animator);
			AnimatorPlaybackBinding binding = MakeIntegrationBinding(context);
			(void)system.BindAnimatorPlayback(animator, binding);
			context.Expect(ExecuteIntegrationFrame(system, "animation-scale-skeleton-loss-baseline"),
				"The skeleton-loss fixture must first publish one compatible complete pose.");
			const std::shared_ptr<const PublishedAnimatorPose> prior =
				system.LoadPublishedAnimatorPose(animator);

			AnimatorPlaybackBinding missingSkeleton = binding;
			missingSkeleton.skeleton.reset();
			// BindAnimatorPlayback takes an owned binding value, so the successful
			// baseline bind cannot mutate this caller-side copy. A loss notification
			// must therefore retain the copied channel identities it needs to prove
			// that the previous pose is still skeleton-compatible.
			for(const SkeletonJoint& joint : binding.skeleton->joints) {
				missingSkeleton.jointStableIds.push_back(joint.stableId);
			}
			context.Expect(!missingSkeleton.jointStableIds.empty()
				&& missingSkeleton.fallbackCompatibilityFingerprint != 0U
				&& system.BindAnimatorPlayback(animator, std::move(missingSkeleton))
					== AnimationStatus::MissingAsset,
				"Skeleton loss must retain copied channel and compatibility identities for fallback validation.");
			context.Expect(ExecuteIntegrationFrame(system, "animation-scale-skeleton-loss-hold"),
				"A skeleton-loss generation must complete through compatible hold-last selection.");
			const AnimationProductionIntegrationSnapshot diagnostics =
				system.ReadProductionIntegrationSnapshot();
			context.Expect(prior && system.LoadPublishedAnimatorPose(animator) == prior
				&& diagnostics.fallbackItemCount == 1U,
				"Retained channels without a skeleton must not publish manufactured identity transforms before hold-last.");
		});

		registry.Add("Animation.ScaleFoundation.AdvancedRequestsReachProductionWorkers", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
				MakeIntegrationDescriptor(95U));
			IntegrationAnimatorGuard cleanup(system, animator);
			AnimatorPlaybackBinding binding = MakeAdvancedIntegrationBinding(context);
			const AnimationGuid profileId = binding.retargetProfile->assetId;
			const AnimationGuid databaseId = binding.motionDatabase->assetId;
			context.Expect(animator.IsValid()
				&& system.BindAnimatorPlayback(animator, std::move(binding))
					== AnimationStatus::Ok,
				"A source-rig clip and its target-rig profile must bind as one immutable production bundle.");

			NativeAnimationRetargetRequest activate =
				MakeAdvancedRecord<NativeAnimationRetargetRequest>(
					NativeAnimationRecordKind::RetargetRequest);
			activate.animator = {animator.slotIndex, animator.generation};
			activate.operation = NativeAnimationRetargetOperation::ActivateProfile;
			activate.profileAssetId = MakeNativeUuid(profileId);
			activate.profileGeneration = 9U;
			activate.correlationId = 9501U;

			NativeAnimationMotionMatchRequest setDatabase =
				MakeAdvancedRecord<NativeAnimationMotionMatchRequest>(
					NativeAnimationRecordKind::MotionMatchRequest);
			setDatabase.animator = {animator.slotIndex, animator.generation};
			setDatabase.operation = NativeAnimationMotionMatchOperation::SetDatabase;
			setDatabase.databaseAssetId = MakeNativeUuid(databaseId);
			setDatabase.databaseGeneration = 12U;
			setDatabase.featureCount = static_cast<std::uint32_t>(
				kMotionDatabaseFeatureCount);
			setDatabase.correlationId = 9502U;
			context.Expect(system.SubmitRetargetRequest(activate).status
					== NativeAnimationStatus::Queued
				&& system.SubmitMotionMatchRequest(setDatabase).status
					== NativeAnimationStatus::Queued
				&& ExecuteIntegrationFrame(system, "animation-advanced-activate"),
				"Profile and database activation must become visible only after one protected publication.");

			NativeAnimationMotionMatchRequest query = setDatabase;
			query.operation = NativeAnimationMotionMatchOperation::Query;
			query.queryId = 9503U;
			query.correlationId = 9503U;
			context.Expect(system.SubmitMotionMatchRequest(query).status
					== NativeAnimationStatus::Queued
				&& ExecuteIntegrationFrame(system, "animation-advanced-evaluate"),
				"The following worker frame must evaluate both the active retarget and motion query.");
			context.Expect(ExecuteIntegrationFrame(system, "animation-advanced-retain-query"),
				"A retained query on a direct-clip animator must remain diagnostic-only instead of poisoning a later frame that has no authored motion node.");

			const std::shared_ptr<const PublishedAnimatorPose> published =
				system.LoadPublishedAnimatorPose(animator);
			std::uint32_t retargetCount = 0U;
			std::uint32_t motionCount = 0U;
			(void)system.CopyRetargetSnapshots(animator, {}, retargetCount);
			(void)system.CopyMotionMatchSnapshots(animator, {}, motionCount);
			std::vector<NativeAnimationRetargetSnapshot> retarget(retargetCount);
			std::vector<NativeAnimationMotionMatchSnapshot> motion(motionCount);
			const AnimationStatus retargetCopy = system.CopyRetargetSnapshots(
				animator, retarget, retargetCount);
			const AnimationStatus motionCopy = system.CopyMotionMatchSnapshots(
				animator, motion, motionCount);
			const auto measuredRetarget = std::find_if(
				retarget.rbegin(), retarget.rend(), [&published](const auto& value) {
					return published && published->snapshot
						&& value.poseGeneration
							== published->snapshot->publicationGeneration;
				});
			const auto selectedMotion = std::find_if(
				motion.rbegin(), motion.rend(), [&query](const auto& value) {
					return value.queryId == query.queryId;
				});
			const auto local = published && published->snapshot
				&& published->snapshot->pose
				? published->snapshot->pose->LocalTransforms()
				: std::span<const AnimationTransform>{};
			context.Expect(retargetCopy == AnimationStatus::Ok
				&& motionCopy == AnimationStatus::Ok,
				"Advanced observations must remain readable through the bounded copy protocol.");
			context.Expect(published != nullptr,
				"Advanced worker evaluation must retain an immutable publication.");
			context.Expect(published->status == AnimationStatus::Ok,
				std::string("The active retarget and motion query must complete without selecting a safe-pose status; actual status=")
					+ std::to_string(static_cast<std::uint32_t>(published->status))
					+ ", retarget status="
					+ std::to_string(measuredRetarget == retarget.rend()
						? UINT32_MAX
						: static_cast<std::uint32_t>(measuredRetarget->status))
					+ ", latest retarget status="
					+ std::to_string(retarget.empty() ? UINT32_MAX
						: static_cast<std::uint32_t>(retarget.back().status)));
			context.Expect(local.size() == 2U,
				"The production publication must contain every target-skeleton joint.");
			context.Expect(local.size() == 2U && local.front().translation.x > 1.5F,
				"The active source-rig clip must be evaluated into a visibly scaled target-rig root transform.");
			context.Expect(measuredRetarget != retarget.rend()
				&& measuredRetarget->status == NativeAnimationStatus::Ok
				&& measuredRetarget->sourceSkeletonGeneration == 10U
				&& measuredRetarget->targetSkeletonGeneration == 8U,
				"The retarget observation must report evaluator-measured pose and skeleton generations.");
			context.Expect(selectedMotion != motion.rend()
				&& selectedMotion->status == NativeAnimationStatus::Ok
				&& selectedMotion->selectedSampleId == 7001U,
				"The worker-side motion query must publish the selected database sample rather than NotReady.");
		});

		registry.Add("Animation.ScaleFoundation.RetargetRebuildUsesPreEvaluationGeneration", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
				MakeIntegrationDescriptor(96U));
			IntegrationAnimatorGuard cleanup(system, animator);
			AnimatorPlaybackBinding binding = MakeAdvancedIntegrationBinding(context);
			// The regression requires this frame to replace the live publication. The
			// shared fixture is manual by default, so opt into ordinary frame advancement
			// instead of accidentally testing an unchanged-cache reuse path.
			binding.updatePolicy = AnimatorUpdatePolicy::EveryFrame;
			// The fixture normally opts into immutable unchanged-input reuse. Disable it
			// here so the protected boundary must replace the publication this request
			// targeted, even when the test host supplies a paused simulation clock.
			binding.evaluationGenerations.unchangedInputReuseEligible = false;
			const AnimationGuid profileId = binding.retargetProfile->assetId;
			const AnimationGeneration profileGeneration = binding.retargetProfileGeneration;
			NativeAnimationRetargetRequest activate =
				MakeAdvancedRecord<NativeAnimationRetargetRequest>(
					NativeAnimationRecordKind::RetargetRequest);
			activate.animator = {animator.slotIndex, animator.generation};
			activate.operation = NativeAnimationRetargetOperation::ActivateProfile;
			activate.profileAssetId = MakeNativeUuid(profileId);
			activate.profileGeneration = profileGeneration;
			activate.correlationId = 9600U;
			context.Expect(animator.IsValid()
				&& system.BindAnimatorPlayback(animator, std::move(binding))
					== AnimationStatus::Ok
				&& system.SubmitRetargetRequest(activate).status
					== NativeAnimationStatus::Queued
				&& ExecuteIntegrationFrame(system, "animation-retarget-rebuild-activate")
				&& ExecuteIntegrationFrame(system, "animation-retarget-rebuild-baseline"),
				"A retarget rebuild must start from an active profile's immutable baseline pose.");

			const std::shared_ptr<const PublishedAnimatorPose> baseline =
				system.LoadPublishedAnimatorPose(animator);
			context.Expect(baseline && baseline->snapshot,
				"The baseline publication must expose the generations a producer can target.");
			if(!baseline || !baseline->snapshot) return;

			NativeAnimationRetargetRequest rebuild =
				MakeAdvancedRecord<NativeAnimationRetargetRequest>(
					NativeAnimationRecordKind::RetargetRequest);
			rebuild.animator = {animator.slotIndex, animator.generation};
			rebuild.operation = NativeAnimationRetargetOperation::RequestRebuild;
			rebuild.profileAssetId = MakeNativeUuid(profileId);
			rebuild.profileGeneration = profileGeneration;
			rebuild.expectedPoseGeneration =
				baseline->snapshot->publicationGeneration;
			rebuild.expectedPublicationGeneration =
				baseline->authorityPublicationGeneration;
			rebuild.correlationId = 9601U;
			context.Expect(system.SubmitRetargetRequest(rebuild).status
					== NativeAnimationStatus::Queued
				&& ExecuteIntegrationFrame(system, "animation-retarget-rebuild"),
				"A rebuild targeting the publication captured before this frame must queue and execute.");

			const std::shared_ptr<const PublishedAnimatorPose> rebuilt =
				system.LoadPublishedAnimatorPose(animator);
			std::uint32_t retargetCount = 0U;
			(void)system.CopyRetargetSnapshots(animator, {}, retargetCount);
			std::vector<NativeAnimationRetargetSnapshot> retarget(retargetCount);
			const AnimationStatus retargetCopy = system.CopyRetargetSnapshots(
				animator, retarget, retargetCount);
			const auto successfulRebuild = std::find_if(
				retarget.rbegin(), retarget.rend(), [&rebuild](const auto& value) {
					return value.profileGeneration == rebuild.profileGeneration
						&& value.status == NativeAnimationStatus::Ok;
				});
			context.Expect(retargetCopy == AnimationStatus::Ok
				&& rebuilt && rebuilt->snapshot
				&& rebuilt->snapshot->publicationGeneration
					> baseline->snapshot->publicationGeneration
				&& successfulRebuild != retarget.rend()
				&& successfulRebuild->poseGeneration
					== rebuilt->snapshot->publicationGeneration,
				std::string("Completion must validate the pre-evaluation publication, then report the newly published pose. baseline pose=")
					+ std::to_string(baseline->snapshot->publicationGeneration)
					+ ", baseline authority="
					+ std::to_string(baseline->authorityPublicationGeneration)
					+ ", rebuilt pose="
					+ std::to_string(rebuilt && rebuilt->snapshot
						? rebuilt->snapshot->publicationGeneration : 0U)
					+ ", copy status=" + std::to_string(static_cast<std::uint32_t>(retargetCopy))
					+ ", latest retarget status="
					+ std::to_string(retarget.empty() ? UINT32_MAX
						: static_cast<std::uint32_t>(retarget.back().status)));

			// Reusing the older baseline after a successful rebuild is intentionally stale.
			// This proves the fix preserves optimistic-concurrency rejection rather than
			// merely removing generation validation from the completion boundary.
			NativeAnimationRetargetRequest staleRebuild = rebuild;
			staleRebuild.correlationId = 9602U;
			context.Expect(system.SubmitRetargetRequest(staleRebuild).status
					== NativeAnimationStatus::Queued
				&& ExecuteIntegrationFrame(system, "animation-retarget-rebuild-stale"),
				"A later rebuild request must still traverse the protected completion boundary.");

			retargetCount = 0U;
			(void)system.CopyRetargetSnapshots(animator, {}, retargetCount);
			retarget.assign(retargetCount, {});
			const AnimationStatus staleCopy = system.CopyRetargetSnapshots(
				animator, retarget, retargetCount);
			const auto staleResult = std::find_if(
				retarget.rbegin(), retarget.rend(), [&staleRebuild](const auto& value) {
					return value.profileGeneration == staleRebuild.profileGeneration
						&& value.status == NativeAnimationStatus::StaleHandle;
				});
			context.Expect(staleCopy == AnimationStatus::Ok
				&& staleResult != retarget.rend(),
				"A request whose expected generations predate this frame's input must remain stale.");
		});

		registry.Add("Animation.ScaleFoundation.MotionSelectionDrivesNextAuthoritativeFrame", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
				MakeIntegrationDescriptor(97U));
			IntegrationAnimatorGuard cleanup(system, animator);
			AnimatorPlaybackBinding binding = MakeMotionSelectionIntegrationBinding(context);
			const AnimationGuid databaseId = binding.motionDatabase->assetId;
			context.Expect(system.BindAnimatorPlayback(animator, std::move(binding))
					== AnimationStatus::Ok,
				"The motion-selection fixture must bind two controller clips and one compatible database.");

			NativeAnimationMotionMatchRequest setDatabase =
				MakeAdvancedRecord<NativeAnimationMotionMatchRequest>(
					NativeAnimationRecordKind::MotionMatchRequest);
			setDatabase.animator = {animator.slotIndex, animator.generation};
			setDatabase.operation = NativeAnimationMotionMatchOperation::SetDatabase;
			setDatabase.databaseAssetId = MakeNativeUuid(databaseId);
			setDatabase.databaseGeneration = 12U;
			setDatabase.featureCount = static_cast<std::uint32_t>(
				kMotionDatabaseFeatureCount);
			setDatabase.correlationId = 9701U;
			context.Expect(system.SubmitMotionMatchRequest(setDatabase).status
					== NativeAnimationStatus::Queued
				&& ExecuteIntegrationFrame(system, "animation-motion-activate"),
				"Database activation and controller entry initialization must publish atomically.");

			NativeAnimationMotionMatchRequest query = setDatabase;
			query.operation = NativeAnimationMotionMatchOperation::Query;
			query.queryId = 9702U;
			query.correlationId = 9702U;
			context.Expect(system.SubmitMotionMatchRequest(query).status
					== NativeAnimationStatus::Queued
				&& ExecuteIntegrationFrame(system, "animation-motion-query")
				&& ExecuteIntegrationFrame(system, "animation-motion-apply"),
				"The CPU-authoritative result must be retained and applied on the next worker frame without a current-frame wait.");

			const std::shared_ptr<const PublishedAnimatorPose> published =
				system.LoadPublishedAnimatorPose(animator);
			const auto local = published && published->snapshot
				&& published->snapshot->pose
				? published->snapshot->pose->LocalTransforms()
				: std::span<const AnimationTransform>{};
			context.Expect(published && published->status == AnimationStatus::Ok
				&& published->snapshot->playback.currentStateId == 200U
				&& local.size() == 1U && local.front().translation.x > 8.5F,
				"The retained selection must choose the matching clip state, seek its source tick, and feed the ordinary authoritative pose pipeline.");
		});

		registry.Add("Animation.ScaleFoundation.MotionMatchV2PublishesDetailsAndDrivesNextFrame", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
				MakeIntegrationDescriptor(98U));
			IntegrationAnimatorGuard cleanup(system, animator);
			AnimatorPlaybackBinding binding = MakeMotionSelectionIntegrationBinding(context);

			// Give the selected sample nonzero costs and add one sample that exercises
			// every hard-exclusion reason. The exact result proves each V2 field group
			// reached the deterministic scorer instead of being accepted then ignored.
			auto database = std::make_shared<MotionDatabaseAsset>(*binding.motionDatabase);
			MotionDatabaseSample& selected = database->samples.front();
			selected.features.values[0U] = 2;
			selected.tagMask = 1U;
			selected.contactMask = 1U;
			selected.phaseQuantized = 100U;
			selected.transitionFlags = 1U;
			selected.continuityGroup = 7U;
			MotionDatabaseSample excluded = selected;
			excluded.stableSampleId = 7002U;
			excluded.sourceClipId = 5001U;
			excluded.tagMask = 4U;
			excluded.contactMask = 0U;
			excluded.phaseQuantized = 300U;
			excluded.transitionFlags = 4U;
			excluded.exclusionFlags = 8U;
			database->samples.push_back(excluded);
			database->featureSchemaFingerprint =
				ComputeMotionDatabaseSchemaFingerprint(*database);
			const AnimationGuid databaseId = database->assetId;
			const std::uint64_t schemaFingerprint = database->featureSchemaFingerprint;
			binding.motionDatabase = std::move(database);
			context.Expect(system.BindAnimatorPlayback(animator, std::move(binding))
					== AnimationStatus::Ok,
				"The detailed-query fixture must bind a controller and immutable database together.");

			NativeAnimationMotionMatchQueryRequestV2 query =
				MakeAdvancedRecord<NativeAnimationMotionMatchQueryRequestV2>(
					NativeAnimationRecordKind::MotionMatchQueryRequestV2);
			query.animator = {animator.slotIndex, animator.generation};
			query.databaseAssetId = MakeNativeUuid(databaseId);
			query.databaseGeneration = 12U;
			query.featureCount = static_cast<std::uint32_t>(kMotionDatabaseFeatureCount);
			query.schemaFingerprint = schemaFingerprint;
			query.queryId = 9801U;
			query.correlationId = 9801U;
			query.queryFields = kNativeAnimationMotionMatchAllQueryFields;
			query.features[0U] = 5;
			std::fill(std::begin(query.featureWeights),
				std::end(query.featureWeights), static_cast<std::uint16_t>(1U));
			query.requiredTags = 1U;
			query.excludedTags = 4U;
			query.preferredTags = 8U;
			query.requiredContacts = 1U;
			query.preferredContacts = 3U;
			query.preferredPhase = 105U;
			query.maximumPhaseDistance = 10U;
			query.requiredTransitionFlags = 1U;
			query.excludedTransitionFlags = 4U;
			query.preferredTransitionFlags = 3U;
			query.preferredContinuityGroup = 9U;
			query.lastSampleId = 999U;
			query.lastSourceClipId = 5002U;
			query.excludedRegionFlags = 8U;
			query.phaseWeight = 13U;
			query.continuityPenalty = 17U;
			query.continuityBreakPenalty = 19U;
			query.sameClipPenalty = 29U;
			query.missingPreferredTagPenalty = 7U;
			query.contactMismatchPenalty = 11U;
			query.transitionMismatchPenalty = 23U;
			query.maximumCandidates = 1U;
			query.maximumSamplesToVisit = 2U;
			query.maximumDiagnosticExclusions = 1U;
			query.excludedSampleCount = 1U;
			query.excludedSampleIds[0U] = excluded.stableSampleId;

			const NativeAnimationMotionMatchQueryReceiptV2 receipt =
				system.SubmitMotionMatchQueryV2(query);
			context.Expect(receipt.status == NativeAnimationStatus::Queued
				&& receipt.validatedFields == query.queryFields
				&& receipt.scheduledFields == query.queryFields
				&& (receipt.flags & static_cast<std::uint32_t>(
					NativeAnimationMotionMatchQueryReceiptFlag::PayloadRetained)) != 0U
				&& ExecuteIntegrationFrame(system, "animation-motion-v2-query")
				&& ExecuteIntegrationFrame(system, "animation-motion-v2-apply"),
				"The complete copied V2 query must score on a worker and apply only on the next authoritative frame.");

			std::uint32_t resultCount = 0U;
			context.Expect(system.CopyMotionMatchResultsV2(animator, {}, resultCount)
					== AnimationStatus::BufferTooSmall && resultCount == 1U,
				"The V2 result history must expose the same atomic two-call copy protocol as V1.");
			std::vector<NativeAnimationMotionMatchResultV2> results(resultCount);
			const AnimationStatus copyStatus = system.CopyMotionMatchResultsV2(
				animator, results, resultCount);
			const NativeAnimationMotionMatchResultV2 result = results.empty()
				? NativeAnimationMotionMatchResultV2{} : results.back();
			const std::uint32_t expectedResultFields = (1U << 12U) - 1U;
			const std::uint32_t expectedFlags = static_cast<std::uint32_t>(
				NativeAnimationMotionMatchResultFlag::UsedFallback)
				| static_cast<std::uint32_t>(
					NativeAnimationMotionMatchResultFlag::UsedCpuFallback);
			context.Expect(copyStatus == AnimationStatus::Ok
				&& result.status == NativeAnimationStatus::Ok
				&& result.gpuStatus == NativeAnimationStatus::RendererUnavailable
				&& result.fallbackReason
					== NativeAnimationMotionMatchFallbackReason::GpuUnavailable
				&& result.selectionSource
					== NativeAnimationMotionMatchSelectionSource::CpuFullSearch
				&& result.populatedFields == expectedResultFields
				&& result.flags == expectedFlags,
				"CPU-authoritative V2 publication must report its populated fields, GPU absence, and fallback source truthfully.");
			context.Expect(result.selectedSampleId == 7001U
				&& result.selectedSourceClipId == 5002U
				&& result.selectedSourceTick == 1
				&& result.selectedClipGeneration == 6U
				&& result.featureCost == 3U
				&& result.featureBlockCosts[0U] == 3U
				&& result.phaseCost == 65U
				&& result.tagCost == 7U
				&& result.contactCost == 11U
				&& result.continuityCost == 36U
				&& result.transitionCost == 52U
				&& result.totalCost == 174U,
				"The detailed result must preserve the selected source and every exact integer cost group.");
			context.Expect(result.visitedSamples == 2U
				&& result.evaluatedSamples == 1U
				&& result.excludedSamples == 1U
				&& result.exclusionReasonMask == 0xffU
				&& result.gpuCandidateCount == 0U
				&& result.observationTick > 0
				&& result.publicationGeneration > receipt.publicationGeneration,
				"V2 publication must expose bounded candidate, exclusion, and protected-generation diagnostics.");

			const std::shared_ptr<const PublishedAnimatorPose> published =
				system.LoadPublishedAnimatorPose(animator);
			const auto local = published && published->snapshot
				&& published->snapshot->pose
				? published->snapshot->pose->LocalTransforms()
				: std::span<const AnimationTransform>{};
			context.Expect(published && published->status == AnimationStatus::Ok
				&& published->snapshot->playback.currentStateId == 200U
				&& local.size() == 1U && local.front().translation.x > 8.5F,
				"The V2 candidate must feed the ordinary next-frame controller and pose publication path.");
		});

		registry.Add("Animation.ScaleFoundation.DeterminismCaptureRestoresProductionPlayback", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
				MakeIntegrationDescriptor(96U));
			IntegrationAnimatorGuard cleanup(system, animator);
			AnimatorPlaybackBinding binding = MakeIntegrationBinding(context);
			context.Expect(system.BindAnimatorPlayback(animator, std::move(binding))
					== AnimationStatus::Ok
				&& ExecuteIntegrationFrame(system, "animation-rollback-baseline"),
				"The rollback fixture must establish a bound, fingerprinted production pose.");

			NativeAnimationDeterminismRequest capture =
				MakeAdvancedRecord<NativeAnimationDeterminismRequest>(
					NativeAnimationRecordKind::DeterminismRequest);
			capture.animator = {animator.slotIndex, animator.generation};
			capture.operation = NativeAnimationDeterminismOperation::Capture;
			capture.correlationId = 9601U;
			context.Expect(system.SubmitDeterminismRequest(capture).status
					== NativeAnimationStatus::Queued
				&& ExecuteIntegrationFrame(system, "animation-rollback-capture"),
				"Capture must store the just-published immutable state before returning success.");
			std::uint32_t captureCount = 0U;
			(void)system.CopyDeterminismSnapshots(animator, {}, captureCount);
			std::vector<NativeAnimationDeterminismSnapshot> snapshots(captureCount);
			(void)system.CopyDeterminismSnapshots(animator, snapshots, captureCount);
			const NativeAnimationDeterminismSnapshot captured = snapshots.empty()
				? NativeAnimationDeterminismSnapshot{} : snapshots.back();
			const AnimatorPlaybackStateSnapshot capturedPlayback =
				system.LoadAnimatorPlaybackState(animator);

			AnimationCommand seek;
			seek.target = animator;
			seek.kind = AnimationCommandKind::Seek;
			seek.playbackTick = 1;
			seek.producerId = 96U;
			seek.producerSequence = 1U;
			seek.correlationId = 9602U;
			context.Expect(system.SubmitCommand(seek).status == AnimationStatus::Queued
				&& ExecuteIntegrationFrame(system, "animation-rollback-diverge")
				&& system.LoadAnimatorPlaybackState(animator).absoluteTick
					!= capturedPlayback.absoluteTick,
				"A later protected command must move live playback away from the retained state.");

			NativeAnimationDeterminismRequest restore = capture;
			restore.operation = NativeAnimationDeterminismOperation::Restore;
			restore.sourceTick = captured.tick;
			restore.targetTick = captured.tick;
			restore.expectedChecksum = captured.poseChecksum;
			restore.correlationId = 9603U;
			context.Expect(captured.status == NativeAnimationStatus::Ok
				&& captured.poseChecksum != 0U
				&& system.SubmitDeterminismRequest(restore).status
					== NativeAnimationStatus::Queued
				&& ExecuteIntegrationFrame(system, "animation-rollback-restore"),
				"An in-window correction must restore only after exact fingerprint and generation validation.");
			std::uint32_t restoredCount = 0U;
			(void)system.CopyDeterminismSnapshots(animator, {}, restoredCount);
			snapshots.resize(restoredCount);
			const AnimationStatus restoredCopy = system.CopyDeterminismSnapshots(
				animator, snapshots, restoredCount);
			const NativeAnimationDeterminismSnapshot restored = snapshots.empty()
				? NativeAnimationDeterminismSnapshot{} : snapshots.back();
			const AnimatorPlaybackStateSnapshot restoredPlayback =
				system.LoadAnimatorPlaybackState(animator);
			context.Expect(restoredCopy == AnimationStatus::Ok
				&& restored.status == NativeAnimationStatus::Ok
				&& restored.poseChecksum == captured.poseChecksum
				&& restoredPlayback.absoluteTick == capturedPlayback.absoluteTick,
				"The first post-restore publication must reproduce the retained pose checksum and exact fixed-point playback cursor.");
		});
	}
}
