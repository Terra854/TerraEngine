// AnimatorComponentBindingTests.cpp - authored component/runtime ownership bridge.

#include "TestHarness.h"

#include "Animation/AnimationSystem.h"
#include "Animation/Assets/AnimationClipAsset.h"
#include "Animation/Assets/AnimationControllerAsset.h"
#include "Animation/Assets/MotionDatabaseAsset.h"
#include "Animation/Assets/SkeletonAsset.h"
#include "Animation/Runtime/AnimatorComponentBindingCoordinator.h"
#include "AnimationClipCompiler.h"
#include "Components/AnimatorComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Resource/ResourceManager.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::AssetPipeline;

		bool BindingNear(float left, float right)
		{
			return std::abs(left - right) <= 1.0e-4F;
		}

		AnimationGuid MakeBindingAssetGuid(std::uint8_t seed)
		{
			AnimationGuid guid;
			for(std::size_t index = 0U; index < guid.value.size(); ++index) {
				guid.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return guid;
		}

		AnimationMatrix4x4 MakeBindingIdentityMatrix()
		{
			AnimationMatrix4x4 matrix{};
			matrix.values[0U] = 1.0F;
			matrix.values[5U] = 1.0F;
			matrix.values[10U] = 1.0F;
			matrix.values[15U] = 1.0F;
			return matrix;
		}

		SkeletonAsset MakeBindingSkeleton()
		{
			SkeletonAsset skeleton;
			skeleton.assetId = MakeBindingAssetGuid(71U);
			skeleton.compatibilityFingerprint = 0x435f42494e44494eULL;
			skeleton.rootJointIndex = 0U;
			skeleton.referencePoseHash = 0x1122334455667788ULL;
			SkeletonJoint root;
			root.stableId = 1U;
			root.parentIndex = -1;
			root.deformsMesh = true;
			root.displayName = "Root";
			root.normalizedSourcePath = "/root";
			root.semanticRole = "hips";
			skeleton.joints.push_back(root);
			skeleton.inverseBindMatrices.push_back(MakeBindingIdentityMatrix());
			return skeleton;
		}

		AnimationClipAsset MakeBindingClip(const SkeletonAsset& skeleton)
		{
			AnimationClipAsset clip;
			clip.assetId = MakeBindingAssetGuid(103U);
			clip.skeletonAssetId = skeleton.assetId;
			clip.compatibilityFingerprint = skeleton.compatibilityFingerprint;
			clip.durationTicks = 60;
			clip.sampleRate = 60U;
			clip.compression.decoderVersion = 1U;
			// No tracks are needed for this ownership test. A valid empty clip asks
			// the evaluator to publish the complete skeleton reference pose.
			return clip;
		}

		AnimationClipAsset MakeConstantBindingClip(
			TestContext& context,
			const SkeletonAsset& skeleton,
			std::uint8_t assetSeed,
			float translationX)
		{
			AnimationClipCompileInput input;
			input.assetId = MakeBindingAssetGuid(assetSeed);
			input.skeletonAssetId = skeleton.assetId;
			input.compatibilityFingerprint = skeleton.compatibilityFingerprint;
			input.durationTicks = 60;
			input.sampleRate = 60U;
			input.segmentDurationTicks = 60;

			AnimationSourceTrack translation;
			translation.stableTargetId = skeleton.joints.front().stableId;
			translation.kind = AnimationClipTrackKind::Translation;
			translation.interpolation = AnimationInterpolation::Linear;
			translation.keys = {
				{0, {translationX, 0.0F, 0.0F}},
				{input.durationTicks, {translationX, 0.0F, 0.0F}}};
			input.tracks.push_back(std::move(translation));

			AnimationClipAsset clip;
			AnimationClipCompileReport report;
			context.Expect(CompileAnimationClip(input, {}, clip, report)
					== AnimationStatus::Ok,
				"The controller crossfade fixture must compile one constant production pose.");
			return clip;
		}

		AnimationClipAsset MakeStreamedBindingClip(
			TestContext& context,
			const SkeletonAsset& skeleton)
		{
			AnimationClipCompileInput input;
			input.assetId = MakeBindingAssetGuid(181U);
			input.skeletonAssetId = skeleton.assetId;
			input.compatibilityFingerprint = skeleton.compatibilityFingerprint;
			input.durationTicks = 60;
			input.sampleRate = 60U;
			input.segmentDurationTicks = 60;

			AnimationSourceTrack translation;
			translation.stableTargetId = skeleton.joints.front().stableId;
			translation.kind = AnimationClipTrackKind::Translation;
			translation.interpolation = AnimationInterpolation::Linear;
			AnimationSourceKey begin;
			begin.tick = 0;
			begin.vectorValue = {5.0F, 0.0F, 0.0F};
			AnimationSourceKey end = begin;
			end.tick = input.durationTicks;
			translation.keys = {begin, end};
			input.tracks.push_back(std::move(translation));

			AnimationClipAsset clip;
			AnimationClipCompileReport report;
			context.Expect(CompileAnimationClip(input, {}, clip, report) == AnimationStatus::Ok
				&& clip.segments.size() == 1U && !clip.streamData.empty(),
				"The range-streaming fixture must compile one non-empty production segment.");
			return clip;
		}

		AnimationClipAsset MakeStreamedBindingRootMotionClip(
			TestContext& context,
			const SkeletonAsset& skeleton)
		{
			AnimationClipCompileInput input;
			input.assetId = MakeBindingAssetGuid(191U);
			input.skeletonAssetId = skeleton.assetId;
			input.compatibilityFingerprint = skeleton.compatibilityFingerprint;
			input.durationTicks = 4;
			input.sampleRate = 60U;
			input.segmentDurationTicks = 4;

			AnimationSourceTrack translation;
			translation.stableTargetId = skeleton.joints.front().stableId;
			translation.kind = AnimationClipTrackKind::RootTranslation;
			translation.interpolation = AnimationInterpolation::Linear;
			translation.keys = {
				{0, {0.0F, 0.0F, 0.0F}},
				{4, {4.0F, 0.0F, 0.0F}}};

			AnimationSourceTrack yaw;
			yaw.stableTargetId = skeleton.joints.front().stableId;
			yaw.kind = AnimationClipTrackKind::RootYaw;
			yaw.interpolation = AnimationInterpolation::Linear;
			AnimationSourceKey beginYaw;
			beginYaw.tick = 0;
			beginYaw.scalarValue = 0.0F;
			AnimationSourceKey endYaw;
			endYaw.tick = 4;
			endYaw.scalarValue = 0.4F;
			yaw.keys = {beginYaw, endYaw};
			input.tracks = {translation, yaw};

			AnimationClipAsset clip;
			AnimationClipCompileReport report;
			context.Expect(CompileAnimationClip(input, {}, clip, report) == AnimationStatus::Ok
				&& clip.rootMotion.enabled && clip.segments.size() == 1U
				&& !clip.streamData.empty(),
				"The gameplay streaming fixture must compile one deterministic root-motion segment.");
			return clip;
		}

		MotionDatabaseAsset MakeStreamedBindingMotionDatabase(
			TestContext& context,
			std::uint8_t seed)
		{
			MotionDatabaseAsset database;
			database.assetId = MakeBindingAssetGuid(seed);
			database.compatibilityFingerprint = 0x4d4f54494f4e5354ULL;
			database.sourceFingerprint = 0x1020304050607080ULL + seed;
			database.quantizationScales.fill(1.0F);
			database.storageLayout.samplesPerBlock = 1U;
			for(std::uint64_t sampleIndex = 0U; sampleIndex < 2U; ++sampleIndex) {
				MotionDatabaseSample sample;
				sample.stableSampleId = sampleIndex + 1U;
				sample.sourceClipId = 100U + sampleIndex;
				sample.sourceTick = static_cast<AnimationFixedTick>(sampleIndex * 10U);
				sample.features.values[0U] = static_cast<std::int16_t>(sampleIndex + 1U);
				database.samples.push_back(sample);
			}
			context.Expect(RebuildMotionDatabaseStorageMetadata(database)
					== AnimationStatus::Ok,
				"The motion binding fixture must build two ordered one-sample blocks.");
			database.featureSchemaFingerprint =
				ComputeMotionDatabaseSchemaFingerprint(database);
			context.Expect(ValidateMotionDatabaseAsset(database) == AnimationStatus::Ok,
				"The motion binding fixture must satisfy the production database contract.");
			return database;
		}

		std::filesystem::path WriteBindingAsset(
			std::string_view fileName,
			std::span<const std::uint8_t> bytes)
		{
			const std::filesystem::path root = std::filesystem::temp_directory_path()
				/ "TerraEngineAnimatorComponentBindingTests";
			std::filesystem::create_directories(root);
			const std::filesystem::path path = root / fileName;
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			output.write(
				reinterpret_cast<const char*>(bytes.data()),
				static_cast<std::streamsize>(bytes.size()));
			return path;
		}

		bool ExecuteBindingFrame(AnimationSystem& system)
		{
			std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> workloads =
				system.DeclareFrameWorkloads(1.0F / 60.0F);
			const TerraEngine::Scheduling::FrameExecutionPlan plan =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(workloads), "animator-component-binding");
			return plan.valid && plan.completed;
		}
	}

	void RegisterAnimatorComponentBindingTests(TestRegistry& registry)
	{
		registry.Add("Animation.ComponentBinding.AuthoredComponentCreatesAndRetiresRuntimeInstance",
			[](TestContext& context) {
				ObjectManager& objects = ObjectManager::instance();
				ResourceManager& resources = ResourceManager::Instance();
				AnimationSystem& animation = AnimationSystem::Instance();
				objects.clear();
				animation.Initialize();

				const SkeletonAsset skeleton = MakeBindingSkeleton();
				const AnimationClipAsset clip = MakeBindingClip(skeleton);
				std::vector<std::uint8_t> skeletonBytes;
				std::vector<std::uint8_t> clipBytes;
				context.Expect(SerializeSkeletonAsset(skeleton, skeletonBytes) == AnimationStatus::Ok,
					"The binding fixture must serialize a valid source-independent skeleton.");
				context.Expect(SerializeAnimationClipAsset(clip, clipBytes) == AnimationStatus::Ok,
					"The binding fixture must serialize a valid source-independent reference-pose clip.");
				const std::filesystem::path skeletonPath =
					WriteBindingAsset("component-binding.teskel", skeletonBytes);
				const std::filesystem::path clipPath =
					WriteBindingAsset("component-binding.teanim", clipBytes);
				const UUID skeletonId = resources.LoadSkeleton(
					skeletonPath, skeleton.compatibilityFingerprint);
				const UUID clipId = resources.LoadAnimationClip(
					clipPath, skeleton.compatibilityFingerprint);
				context.Expect(skeletonId == skeleton.assetId.value && clipId == clip.assetId.value,
					"Runtime binding must resolve the same stable IDs emitted by the cooker.");

				const Object object = objects.createObject("Bound Animator");
				AnimatorComponent* component = objects.AddComponent<AnimatorComponent>(object);
				context.Expect(component != nullptr,
					"The fixture object must own an authored AnimatorComponent.");
				if(component != nullptr) {
					component->skeletonOverrideAssetId = skeletonId;
					component->defaultClipAssetId = clipId;
					component->defaultClipStartFrame = 12U;
					component->initialParametersJson = R"({"Speed":2.5})";
					objects.notifyObjectStructureChanged(object);
				}
				const UUID objectId = objects.getUUID(object);

				context.Expect(ExecuteBindingFrame(animation),
					"The production AnimationSystem declaration must create, bind, and evaluate the authored component.");
				const AnimatorInstanceHandle firstHandle = animation.ResolveAnimatorForObject(objectId);
				const std::shared_ptr<const PublishedAnimatorPose> published =
					animation.LoadPublishedAnimatorPoseForObject(objectId);
				const AnimatorPlaybackStateSnapshot initialPlayback =
					animation.LoadAnimatorPlaybackState(firstHandle);
				context.Expect(firstHandle.IsValid()
					&& published != nullptr && published->status == AnimationStatus::Ok
					&& published->snapshot && published->snapshot->pose
					&& published->snapshot->pose->LocalTransforms().size() == 1U
					&& published->snapshot->playback.parameters.size() == 1U
					&& published->snapshot->playback.parameters.front().type == AnimationParameterType::Float
					&& initialPlayback.runState == AnimatorPlaybackRunState::Playing
					&& initialPlayback.absoluteTick > 12,
					"A production-authored component must publish its copied initial parameters, complete immutable reference pose, and advance from its authored start frame.");

				bool completedLoopFrames = true;
				for(std::size_t frame = 0U; frame < 65U; ++frame) {
					if(!ExecuteBindingFrame(animation)) {
						completedLoopFrames = false;
						break;
					}
				}
				const AnimatorPlaybackStateSnapshot loopedPlayback =
					animation.LoadAnimatorPlaybackState(firstHandle);
				context.Expect(completedLoopFrames
					&& loopedPlayback.runState == AnimatorPlaybackRunState::Playing
					&& loopedPlayback.looping && loopedPlayback.loopCount >= 1,
					"A standalone authored default clip must loop instead of completing and freezing at its final pose.");

				component->loopDefaultClip = false;
				objects.notifyObjectStructureChanged(object);
				bool completedOneShotFrames = ExecuteBindingFrame(animation);
				const AnimatorInstanceHandle oneShotHandle =
					animation.ResolveAnimatorForObject(objectId);
				for(std::size_t frame = 0U; frame < 65U && completedOneShotFrames; ++frame) {
					completedOneShotFrames = ExecuteBindingFrame(animation);
				}
				const AnimatorPlaybackStateSnapshot oneShotPlayback =
					animation.LoadAnimatorPlaybackState(oneShotHandle);
				context.Expect(completedOneShotFrames
					&& oneShotHandle.IsValid() && oneShotHandle != firstHandle
					&& !animation.IsAnimatorInstanceAlive(firstHandle)
					&& oneShotPlayback.runState == AnimatorPlaybackRunState::Completed
					&& !oneShotPlayback.looping && oneShotPlayback.normalizedTime == 1.0F,
					"Disabling Loop Default Clip must produce an authored one-shot that completes at its final pose.");

				component->loopDefaultClip = true;
				objects.notifyObjectStructureChanged(object);
				context.Expect(ExecuteBindingFrame(animation),
					"Re-enabling looping must rebuild advancing direct-clip playback.");
				const AnimatorInstanceHandle activeHandle =
					animation.ResolveAnimatorForObject(objectId);
				context.Expect(activeHandle.IsValid() && activeHandle != oneShotHandle
					&& !animation.IsAnimatorInstanceAlive(oneShotHandle),
					"An authored component-generation change must retire the prior loop-policy instance.");
				const std::shared_ptr<const PublishedAnimatorPose> beforeReload =
					animation.LoadPublishedAnimatorPoseForObject(objectId);
				const AnimationGeneration skeletonBeforeReload =
					resources.GetAnimationAssetRuntimeGeneration(skeletonId, AssetType::Skeleton);
				const AnimationGeneration clipBeforeReload =
					resources.GetAnimationAssetRuntimeGeneration(clipId, AssetType::AnimationClip);
				const std::vector<std::uint8_t> corruptClipBytes{0x54U, 0x45U, 0x00U};
				(void)WriteBindingAsset("component-binding.teanim", corruptClipBytes);
				context.Expect(!resources.ReloadAnimationClip(clipId)
					&& resources.GetAnimationAssetRuntimeGeneration(
						clipId, AssetType::AnimationClip) == clipBeforeReload
					&& animation.LoadPublishedAnimatorPoseForObject(objectId) == beforeReload,
					"A malformed replacement must retain the last complete clip generation and animator publication.");
				(void)WriteBindingAsset("component-binding.teanim", clipBytes);
				context.Expect(resources.ReloadSkeleton(skeletonId) && resources.ReloadAnimationClip(clipId),
					"A same-GUID cooked reimport must publish fresh skeleton and clip generations.");
				const AnimationGeneration skeletonAfterReload =
					resources.GetAnimationAssetRuntimeGeneration(skeletonId, AssetType::Skeleton);
				const AnimationGeneration clipAfterReload =
					resources.GetAnimationAssetRuntimeGeneration(clipId, AssetType::AnimationClip);
				context.Expect(skeletonAfterReload > skeletonBeforeReload && clipAfterReload > clipBeforeReload,
					"Reload must advance cache generations monotonically rather than reusing a stable UUID generation.");
				context.Expect(ExecuteBindingFrame(animation),
					"The next protected frame must bind the coherent reloaded dependency pair.");
				const std::shared_ptr<const PublishedAnimatorPose> afterReload =
					animation.LoadPublishedAnimatorPoseForObject(objectId);
				const AnimatorPlaybackStateSnapshot reloadedPlayback =
					animation.LoadAnimatorPlaybackState(activeHandle);
				context.Expect(animation.ResolveAnimatorForObject(objectId) == activeHandle
					&& beforeReload && afterReload
					&& beforeReload->skeleton.generation == skeletonBeforeReload
					&& beforeReload->clip.generation == clipBeforeReload
					&& afterReload->skeleton.generation == skeletonAfterReload
					&& afterReload->clip.generation == clipAfterReload
					&& reloadedPlayback.runState == AnimatorPlaybackRunState::Playing
					&& reloadedPlayback.absoluteTick > 0,
					"Hot reload must retain the animator lifetime while atomically replacing its asset-generation bundle and restarting active playback.");

				resources.DeleteResource(clipId, AssetType::AnimationClip);
				resources.DeleteResource(skeletonId, AssetType::Skeleton);
				const AnimationGeneration clipTombstone =
					resources.GetAnimationAssetRuntimeGeneration(clipId, AssetType::AnimationClip);
				const AnimationGeneration skeletonTombstone =
					resources.GetAnimationAssetRuntimeGeneration(skeletonId, AssetType::Skeleton);
				context.Expect(clipTombstone > clipAfterReload && skeletonTombstone > skeletonAfterReload
					&& ExecuteBindingFrame(animation),
					"Deleting a cooked dependency must publish a tombstone before the next frame observes it.");
				const std::shared_ptr<const PublishedAnimatorPose> tombstoned =
					animation.LoadPublishedAnimatorPoseForObject(objectId);
				context.Expect(tombstoned && tombstoned != afterReload
					&& tombstoned->status == AnimationStatus::MissingAsset && tombstoned->safePose
					&& afterReload && afterReload->clip.generation < clipTombstone
					&& afterReload->skeleton.generation < skeletonTombstone,
					"The deleted publication must retire the last valid generation and expose an explicit safe-pose tombstone state.");
				context.Expect(resources.LoadSkeleton(skeletonPath, skeleton.compatibilityFingerprint) == skeletonId
					&& resources.LoadAnimationClip(clipPath, skeleton.compatibilityFingerprint) == clipId
					&& ExecuteBindingFrame(animation),
					"Reloading deleted same-GUID assets must restore a fresh, complete binding at the protected boundary.");
				const std::shared_ptr<const PublishedAnimatorPose> afterDeleteReload =
					animation.LoadPublishedAnimatorPoseForObject(objectId);
				context.Expect(afterDeleteReload && afterDeleteReload->status == AnimationStatus::Ok
					&& afterDeleteReload->skeleton.generation > skeletonTombstone
					&& afterDeleteReload->clip.generation > clipTombstone,
					"A reloaded same-GUID pair must not revive the deleted generation values.");

				animation.SetAuthoredComponentPlaybackEnabled(false);
				context.Expect(ExecuteBindingFrame(animation),
					"The edit domain must accept an empty authored-component animation frame.");
				const AnimatorInstanceHandle stoppedHandle =
					animation.ResolveAnimatorForObject(objectId);
				context.Expect(!stoppedHandle.IsValid()
					&& !animation.IsAnimatorInstanceAlive(activeHandle)
					&& animation.ScheduledAnimatorCount() == 0U,
					"Leaving Play Mode must retire authored runtime instances so stale deformation cannot survive in the editor.");

				animation.SetAuthoredComponentPlaybackEnabled(true);
				context.Expect(ExecuteBindingFrame(animation),
					"Re-entering the runtime domain must rebuild and autoplay the authored clip.");
				const AnimatorInstanceHandle resumedHandle =
					animation.ResolveAnimatorForObject(objectId);
				const AnimatorPlaybackStateSnapshot resumedPlayback =
					animation.LoadAnimatorPlaybackState(resumedHandle);
				context.Expect(resumedHandle.IsValid()
					&& resumedPlayback.runState == AnimatorPlaybackRunState::Playing
					&& resumedPlayback.absoluteTick > 0,
					"Entering Play Mode must create a fresh advancing instance instead of resuming a stale stopped pose.");

				animation.SetAuthoredComponentPlaybackAdvancing(false);
				context.Expect(ExecuteBindingFrame(animation)
					&& ExecuteBindingFrame(animation),
					"Edit Mode must continue publishing a reference pose while time is frozen.");
				const AnimatorInstanceHandle frozenHandle =
					animation.ResolveAnimatorForObject(objectId);
				const AnimatorPlaybackStateSnapshot frozenPlayback =
					animation.LoadAnimatorPlaybackState(frozenHandle);
				context.Expect(frozenHandle.IsValid()
					&& frozenHandle == resumedHandle
					&& animation.IsAnimatorInstanceAlive(resumedHandle)
					&& frozenPlayback.runState == AnimatorPlaybackRunState::Playing
					&& frozenPlayback.absoluteTick == 12,
					"Stopping must restart the retained binding at the authored start frame without discarding its complete controller and pose state.");

				component->defaultClipStartFrame = 24U;
				objects.notifyObjectStructureChanged(object);
				context.Expect(ExecuteBindingFrame(animation),
					"Editing Default Clip Start Frame must rebuild the frozen authored pose at the protected boundary.");
				const AnimatorInstanceHandle retimedHandle =
					animation.ResolveAnimatorForObject(objectId);
				const AnimatorPlaybackStateSnapshot retimedPlayback =
					animation.LoadAnimatorPlaybackState(retimedHandle);
				context.Expect(retimedHandle.IsValid()
					&& retimedHandle != frozenHandle
					&& !animation.IsAnimatorInstanceAlive(frozenHandle)
					&& retimedPlayback.absoluteTick == 24,
					"An Inspector start-frame edit must retire the old binding and publish the newly authored pose immediately.");
				animation.SetAuthoredComponentPlaybackAdvancing(true);

				component->playAutomatically = false;
				objects.notifyObjectStructureChanged(object);
				context.Expect(ExecuteBindingFrame(animation),
					"Disabling automatic playback must rebuild the component-owned runtime binding.");
				const AnimatorInstanceHandle manuallyStartedHandle =
					animation.ResolveAnimatorForObject(objectId);
				const AnimatorPlaybackStateSnapshot manuallyStartedPlayback =
					animation.LoadAnimatorPlaybackState(manuallyStartedHandle);
				context.Expect(manuallyStartedHandle.IsValid()
					&& manuallyStartedHandle != retimedHandle
					&& !animation.IsAnimatorInstanceAlive(retimedHandle)
					&& manuallyStartedPlayback.runState == AnimatorPlaybackRunState::Stopped
					&& manuallyStartedPlayback.absoluteTick == 24,
					"Play Automatically off must hold the configured start-frame pose until an explicit runtime command.");

				component->playAutomatically = true;
				objects.notifyObjectStructureChanged(object);
				context.Expect(ExecuteBindingFrame(animation),
					"Re-enabling automatic playback must rebuild and advance from the authored start frame.");
				const AnimatorInstanceHandle autoplayHandle =
					animation.ResolveAnimatorForObject(objectId);
				const AnimatorPlaybackStateSnapshot autoplayPlayback =
					animation.LoadAnimatorPlaybackState(autoplayHandle);
				context.Expect(autoplayHandle.IsValid()
					&& autoplayHandle != manuallyStartedHandle
					&& !animation.IsAnimatorInstanceAlive(manuallyStartedHandle)
					&& autoplayPlayback.runState == AnimatorPlaybackRunState::Playing
					&& autoplayPlayback.absoluteTick > 24,
					"Play Automatically on must resume normal component-owned startup playback.");

				objects.RemoveComponent<AnimatorComponent>(object);
				const std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> dormant =
					animation.DeclareFrameWorkloads(1.0F / 60.0F);
				context.Expect(dormant.empty() && !animation.IsAnimatorInstanceAlive(autoplayHandle)
					&& !animation.LoadPublishedAnimatorPoseForObject(objectId),
					"The next protected declaration must retire a removed component handle and its immutable publication.");

				animation.Shutdown();
				objects.clear();
				resources.DeleteResource(clipId, AssetType::AnimationClip);
				resources.DeleteResource(skeletonId, AssetType::Skeleton);
				std::error_code cleanupError;
				std::filesystem::remove(clipPath, cleanupError);
				cleanupError.clear();
				std::filesystem::remove(skeletonPath, cleanupError);
				// AnimationSystem is a process-wide singleton shared by every engine
				// test. Restore its normal active-empty state after this lifecycle
				// fixture so later scheduler tests can create their own memberships.
				animation.Initialize();
			});

		registry.Add("Animation.ComponentBinding.StreamsCookedClipSegmentsAtProtectedBoundary",
			[](TestContext& context) {
				const bool initializedPool = ThreadPool::instance().getThreadCount() == 0U;
				if(initializedPool) ThreadPool::instance().initialize(2U);
				ObjectManager& objects = ObjectManager::instance();
				ResourceManager& resources = ResourceManager::Instance();
				AnimationSystem& animation = AnimationSystem::Instance();
				objects.clear();
				animation.Initialize();

				const SkeletonAsset skeleton = MakeBindingSkeleton();
				const AnimationClipAsset clip = MakeStreamedBindingClip(context, skeleton);
				std::vector<std::uint8_t> skeletonBytes;
				std::vector<std::uint8_t> clipBytes;
				context.Expect(SerializeSkeletonAsset(skeleton, skeletonBytes) == AnimationStatus::Ok
					&& SerializeAnimationClipAsset(clip, clipBytes) == AnimationStatus::Ok,
					"The production range-streaming fixture must serialize valid cooked assets.");
				const std::filesystem::path skeletonPath =
					WriteBindingAsset("range-stream-binding.teskel", skeletonBytes);
				const std::filesystem::path clipPath =
					WriteBindingAsset("range-stream-binding.teanim", clipBytes);
				const UUID skeletonId = resources.LoadSkeleton(
					skeletonPath, skeleton.compatibilityFingerprint);
				const UUID clipId = resources.LoadAnimationClip(
					clipPath, skeleton.compatibilityFingerprint);
				const AnimationGeneration clipGeneration =
					resources.GetAnimationAssetRuntimeGeneration(clipId, AssetType::AnimationClip);
				const AnimationCookedRangeTable ranges =
					resources.DescribeAnimationClipSegmentRanges(clipId, clipGeneration);
				AnimationRangeReadResult physicalSegment;
				if(ranges.Succeeded() && ranges.blocks.size() == 1U) {
					physicalSegment = resources.ReadAnimationAssetRange(
						ranges.logicalPath, ranges.blocks[0U].byteOffset,
						ranges.blocks[0U].byteSize, ranges.blocks[0U].byteSize);
				}
				context.Expect(ranges.Succeeded() && ranges.blocks.size() == 1U
					&& physicalSegment.Succeeded()
					&& physicalSegment.bytes == clip.streamData,
					"ResourceManager must derive the real cooked DATA interval for the exact published clip generation.");

				const Object object = objects.createObject("Range Stream Animator");
				AnimatorComponent* component = objects.AddComponent<AnimatorComponent>(object);
				if(component != nullptr) {
					component->skeletonOverrideAssetId = skeletonId;
					component->defaultClipAssetId = clipId;
					// This case intentionally has no authoritative consumers so it can
					// continue proving the visual safe-pose path for a missing segment.
					component->rootMotionPolicy = AnimatorRootMotionPolicy::Ignore;
					component->eventPolicy = AnimatorEventPolicy::Suppress;
					objects.notifyObjectStructureChanged(object);
				}
				const UUID objectId = objects.getUUID(object);
				context.Expect(ExecuteBindingFrame(animation),
					"The first frame must queue its physical segment read without blocking evaluation.");
				const std::shared_ptr<const PublishedAnimatorPose> fallback =
					animation.LoadPublishedAnimatorPoseForObject(objectId);
				const auto fallbackModel = fallback && fallback->snapshot && fallback->snapshot->pose
					? fallback->snapshot->pose->ModelTransforms()
					: std::span<const AnimationTransform>{};
				context.Expect(component != nullptr && fallback && fallback->safePose
					&& fallbackModel.size() == 1U
					&& fallbackModel[0U].translation.x == 0.0F,
					"Worker-owned bytes must remain invisible until a later protected synchronization publishes the immutable residency snapshot.");

				std::shared_ptr<const PublishedAnimatorPose> resident;
				std::size_t publicationFrames = 0U;
				for(; publicationFrames < 128U; ++publicationFrames) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					if(!ExecuteBindingFrame(animation)) break;
					resident = animation.LoadPublishedAnimatorPoseForObject(objectId);
					const auto model = resident && resident->snapshot && resident->snapshot->pose
						? resident->snapshot->pose->ModelTransforms()
						: std::span<const AnimationTransform>{};
					if(resident && resident->status == AnimationStatus::Ok
						&& model.size() == 1U && model[0U].translation.x == 5.0F) break;
				}
				context.Expect(publicationFrames < 128U && resident
					&& resident->status == AnimationStatus::Ok,
					"A complete matching range must become visible on a later protected frame and drive the production evaluator.");

				objects.RemoveComponent<AnimatorComponent>(object);
				(void)animation.DeclareFrameWorkloads(1.0F / 60.0F);
				animation.Shutdown();
				objects.clear();
				resources.DeleteResource(clipId, AssetType::AnimationClip);
				resources.DeleteResource(skeletonId, AssetType::Skeleton);
				std::error_code cleanupError;
				std::filesystem::remove(clipPath, cleanupError);
				cleanupError.clear();
				std::filesystem::remove(skeletonPath, cleanupError);
				animation.Initialize();
				if(initializedPool) ThreadPool::instance().shutdown();
			});

		registry.Add("Animation.ComponentBinding.GameplayCriticalClipUsesDataUntilAtomicStreamPromotion",
			[](TestContext& context) {
				const bool initializedPool = ThreadPool::instance().getThreadCount() == 0U;
				if(initializedPool) ThreadPool::instance().initialize(1U);
				ObjectManager& objects = ObjectManager::instance();
				ResourceManager& resources = ResourceManager::Instance();
				AnimationSystem& animation = AnimationSystem::Instance();
				objects.clear();
				animation.Initialize();

				const SkeletonAsset skeleton = MakeBindingSkeleton();
				const AnimationClipAsset clip =
					MakeStreamedBindingRootMotionClip(context, skeleton);
				std::vector<std::uint8_t> skeletonBytes;
				std::vector<std::uint8_t> clipBytes;
				context.Expect(SerializeSkeletonAsset(skeleton, skeletonBytes) == AnimationStatus::Ok
					&& SerializeAnimationClipAsset(clip, clipBytes) == AnimationStatus::Ok,
					"The gameplay-critical fixture must serialize complete DATA and one exact streamed segment.");
				const std::filesystem::path skeletonPath =
					WriteBindingAsset("gameplay-stream-binding.teskel", skeletonBytes);
				const std::filesystem::path clipPath =
					WriteBindingAsset("gameplay-stream-binding.teanim", clipBytes);
				const UUID skeletonId = resources.LoadSkeleton(
					skeletonPath, skeleton.compatibilityFingerprint);
				const UUID clipId = resources.LoadAnimationClip(
					clipPath, skeleton.compatibilityFingerprint);

				// Occupy the only worker before synchronization. The first two frames
				// must therefore evaluate from complete DATA while the exact segment
				// request remains queued but physically unread.
				std::promise<void> blockerStarted;
				std::future<void> blockerStartedFuture = blockerStarted.get_future();
				std::promise<void> releaseBlocker;
				const std::shared_future<void> releaseFuture =
					releaseBlocker.get_future().share();
				auto blocker = ThreadPool::instance().createTask(
					[&blockerStarted, releaseFuture]() {
						blockerStarted.set_value();
						releaseFuture.wait();
					});
				std::future<void> blockerCompletion = blocker->getFuture();
				ThreadPool::instance().enqueue(blocker, 1U);
				const bool blockerRunning = blockerStartedFuture.wait_for(
					std::chrono::seconds(3)) == std::future_status::ready;

				const Object object = objects.createObject("Gameplay Stream Animator");
				AnimatorComponent* component = objects.AddComponent<AnimatorComponent>(object);
				if(component != nullptr) {
					component->skeletonOverrideAssetId = skeletonId;
					component->defaultClipAssetId = clipId;
					component->rootMotionPolicy = AnimatorRootMotionPolicy::ExtractOnly;
					component->timePolicy = AnimatorTimePolicy::Fixed;
					objects.notifyObjectStructureChanged(object);
				}
				const UUID objectId = objects.getUUID(object);
				context.Expect(blockerRunning && ExecuteBindingFrame(animation),
					"The protected bind must queue streaming without waiting for its blocked worker.");
				const std::shared_ptr<const PublishedAnimatorPose> initial =
					animation.LoadPublishedAnimatorPoseForObject(objectId);
				const bool completeDataVisible = component != nullptr && initial
					&& initial->status == AnimationStatus::Ok && !initial->safePose
					&& !initial->streamedClipPayloadsActive;

				AnimationCommand playFromData;
				if(initial) playFromData.target = initial->animator;
				playFromData.kind = AnimationCommandKind::Play;
				playFromData.producerId = 501U;
				playFromData.producerSequence = 1U;
				playFromData.correlationId = 1U;
				playFromData.idempotencyKey = 1U;
				const AnimationCommandReceipt dataPlayReceipt =
					animation.SubmitCommand(playFromData);
				context.Expect(ExecuteBindingFrame(animation),
					"Gameplay authority must keep evaluating while segment IO is blocked.");
				const std::shared_ptr<const PublishedAnimatorPose> dataPose =
					animation.LoadPublishedAnimatorPoseForObject(objectId);
				const std::optional<AnimationRootMotionDelta> dataRoot =
					dataPose && dataPose->snapshot && dataPose->snapshot->pose
					? dataPose->snapshot->pose->RootMotionDelta()
					: std::optional<AnimationRootMotionDelta>{};

				releaseBlocker.set_value();
				blockerCompletion.wait();
				std::shared_ptr<const PublishedAnimatorPose> streamedPose;
				std::size_t publicationFrames = 0U;
				for(; publicationFrames < 128U; ++publicationFrames) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					if(!ExecuteBindingFrame(animation)) break;
					streamedPose = animation.LoadPublishedAnimatorPoseForObject(objectId);
					if(streamedPose && streamedPose->streamedClipPayloadsActive) break;
				}

				AnimationCommand playFromStream = playFromData;
				if(streamedPose) playFromStream.target = streamedPose->animator;
				playFromStream.producerSequence = 2U;
				playFromStream.correlationId = 2U;
				playFromStream.idempotencyKey = 2U;
				const AnimationCommandReceipt streamPlayReceipt =
					animation.SubmitCommand(playFromStream);
				context.Expect(ExecuteBindingFrame(animation),
					"The promoted immutable segment must remain usable by the next fixed tick.");
				const std::shared_ptr<const PublishedAnimatorPose> promoted =
					animation.LoadPublishedAnimatorPoseForObject(objectId);
				const std::optional<AnimationRootMotionDelta> streamRoot =
					promoted && promoted->snapshot && promoted->snapshot->pose
					? promoted->snapshot->pose->RootMotionDelta()
					: std::optional<AnimationRootMotionDelta>{};

				context.Expect(completeDataVisible
					&& dataPlayReceipt.status == AnimationStatus::Queued
					&& dataPose && dataPose->status == AnimationStatus::Ok
					&& !dataPose->safePose && !dataPose->streamedClipPayloadsActive
					&& dataRoot.has_value()
					&& BindingNear(dataRoot->localTranslation.x, 1.0F)
					&& BindingNear(dataRoot->localYawRadians, 0.1F)
					&& publicationFrames < 128U && streamedPose
					&& streamedPose->streamedClipPayloadsActive
					&& streamPlayReceipt.status == AnimationStatus::Queued
					&& promoted && promoted->status == AnimationStatus::Ok
					&& !promoted->safePose && promoted->streamedClipPayloadsActive
					&& streamRoot.has_value()
					&& BindingNear(streamRoot->localTranslation.x,
						dataRoot ? dataRoot->localTranslation.x : 0.0F)
					&& BindingNear(streamRoot->localYawRadians,
						dataRoot ? dataRoot->localYawRadians : 0.0F),
					"Gameplay-critical playback must use complete DATA before publication and atomically promote to identical pinned-segment root motion afterward.");

				objects.RemoveComponent<AnimatorComponent>(object);
				(void)animation.DeclareFrameWorkloads(1.0F / 60.0F);
				animation.Shutdown();
				objects.clear();
				resources.DeleteResource(clipId, AssetType::AnimationClip);
				resources.DeleteResource(skeletonId, AssetType::Skeleton);
				std::error_code cleanupError;
				std::filesystem::remove(clipPath, cleanupError);
				cleanupError.clear();
				std::filesystem::remove(skeletonPath, cleanupError);
				animation.Initialize();
				if(initializedPool) ThreadPool::instance().shutdown();
			});

		registry.Add("Animation.ComponentBinding.MotionDatabasePromotesAfterEveryStrmBlockPublishes",
			[](TestContext& context) {
				const bool initializedPool = ThreadPool::instance().getThreadCount() == 0U;
				if(initializedPool) ThreadPool::instance().initialize(1U);
				ObjectManager& objects = ObjectManager::instance();
				ResourceManager& resources = ResourceManager::Instance();
				AnimationSystem& animation = AnimationSystem::Instance();
				objects.clear();
				animation.Initialize();

				const MotionDatabaseAsset database =
					MakeStreamedBindingMotionDatabase(context, 201U);
				std::vector<std::uint8_t> databaseBytes;
				context.Expect(SerializeMotionDatabaseAsset(database, databaseBytes)
						== AnimationStatus::Ok,
					"The promotion fixture must emit a checked DATA plus STRM database.");
				const std::filesystem::path databasePath =
					WriteBindingAsset("motion-promote.temotiondb", databaseBytes);
				const UUID databaseId = resources.LoadAnimationAssetAsync(
					databasePath, AssetType::MotionDatabase).get();
				const AnimationGeneration generation =
					resources.GetAnimationAssetRuntimeGeneration(
						databaseId, AssetType::MotionDatabase);
				const AnimationCookedRangeTable ranges =
					resources.DescribeMotionDatabaseBlockRanges(databaseId, generation);

				const Object object = objects.createObject("Motion Stream Animator");
				AnimatorComponent* component = objects.AddComponent<AnimatorComponent>(object);
				if(component != nullptr) {
					component->motionDatabaseOverrideAssetId = databaseId;
					objects.notifyObjectStructureChanged(object);
				}
				AnimatorComponentBindingCoordinator coordinator;
				AnimatorComponentSynchronizationSummary summary =
					coordinator.Synchronize(animation, objects, resources);
				const bool beganWithCompleteFallback = component != nullptr
					&& databaseId == database.assetId.value && ranges.Succeeded()
					&& ranges.blocks.size() == database.residentBlocks.size()
					&& summary.fallbackMotionDatabaseCount == 1U
					&& summary.streamedMotionDatabaseCount == 0U
					&& summary.requestedStreamPayloadCount > 0U
					&& summary.retainedStreamPayloadCount > 0U;

				std::size_t attempts = 0U;
				for(; attempts < 128U && summary.streamedMotionDatabaseCount == 0U;
					++attempts) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					summary = coordinator.Synchronize(animation, objects, resources);
				}
				context.Expect(beganWithCompleteFallback
					&& attempts < 128U
					&& summary.streamedMotionDatabaseCount == 1U
					&& summary.fallbackMotionDatabaseCount == 0U
					&& summary.residentStreamPayloadCount
						== database.residentBlocks.size()
					&& summary.retainedStreamPayloadCount
						== database.residentBlocks.size(),
					"The binding must retain complete DATA until every exact block is resident, then atomically publish the all-block decoded candidate.");

				coordinator.InvalidateAll(animation);
				animation.Shutdown();
				objects.clear();
				resources.DeleteResource(databaseId, AssetType::MotionDatabase);
				std::error_code cleanupError;
				std::filesystem::remove(databasePath, cleanupError);
				animation.Initialize();
				if(initializedPool) ThreadPool::instance().shutdown();
			});

		registry.Add("Animation.ComponentBinding.CorruptMotionStrmKeepsCompleteDataFallback",
			[](TestContext& context) {
				const bool initializedPool = ThreadPool::instance().getThreadCount() == 0U;
				if(initializedPool) ThreadPool::instance().initialize(1U);
				ObjectManager& objects = ObjectManager::instance();
				ResourceManager& resources = ResourceManager::Instance();
				AnimationSystem& animation = AnimationSystem::Instance();
				objects.clear();
				animation.Initialize();

				const MotionDatabaseAsset database =
					MakeStreamedBindingMotionDatabase(context, 211U);
				std::vector<std::uint8_t> databaseBytes;
				context.Expect(SerializeMotionDatabaseAsset(database, databaseBytes)
						== AnimationStatus::Ok,
					"The corruption fixture must begin as a valid current database.");
				const std::filesystem::path databasePath =
					WriteBindingAsset("motion-corrupt.temotiondb", databaseBytes);
				const UUID databaseId = resources.LoadAnimationAssetAsync(
					databasePath, AssetType::MotionDatabase).get();
				const AnimationGeneration generation =
					resources.GetAnimationAssetRuntimeGeneration(
						databaseId, AssetType::MotionDatabase);
				const AnimationCookedRangeTable ranges =
					resources.DescribeMotionDatabaseBlockRanges(databaseId, generation);
				if(ranges.Succeeded() && !ranges.blocks.empty()
					&& ranges.blocks.front().byteOffset < databaseBytes.size()) {
					databaseBytes[static_cast<std::size_t>(
						ranges.blocks.front().byteOffset)] ^= 0xffU;
					(void)WriteBindingAsset("motion-corrupt.temotiondb", databaseBytes);
				}

				const Object object = objects.createObject("Corrupt Motion Stream Animator");
				AnimatorComponent* component = objects.AddComponent<AnimatorComponent>(object);
				if(component != nullptr) {
					component->motionDatabaseOverrideAssetId = databaseId;
					objects.notifyObjectStructureChanged(object);
				}
				AnimatorComponentBindingCoordinator coordinator;
				AnimatorComponentSynchronizationSummary summary =
					coordinator.Synchronize(animation, objects, resources);
				for(std::size_t attempt = 0U;
					attempt < 128U && (summary.requestedStreamPayloadCount != 0U
						|| summary.failedStreamPayloadCount == 0U); ++attempt) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					summary = coordinator.Synchronize(animation, objects, resources);
				}
				context.Expect(component != nullptr && ranges.Succeeded()
					&& summary.failedStreamPayloadCount > 0U
					&& summary.streamedMotionDatabaseCount == 0U
					&& summary.fallbackMotionDatabaseCount == 1U,
					"A same-size corrupt physical block must fail raw-byte validation and leave the complete DATA database authoritative.");

				coordinator.InvalidateAll(animation);
				animation.Shutdown();
				objects.clear();
				resources.DeleteResource(databaseId, AssetType::MotionDatabase);
				std::error_code cleanupError;
				std::filesystem::remove(databasePath, cleanupError);
				animation.Initialize();
				if(initializedPool) ThreadPool::instance().shutdown();
			});

		registry.Add("Animation.ComponentBinding.RemovalCancelsAndReleasesPendingMotionBlocks",
			[](TestContext& context) {
				const bool initializedPool = ThreadPool::instance().getThreadCount() == 0U;
				if(initializedPool) ThreadPool::instance().initialize(1U);
				ObjectManager& objects = ObjectManager::instance();
				ResourceManager& resources = ResourceManager::Instance();
				AnimationSystem& animation = AnimationSystem::Instance();
				objects.clear();
				animation.Initialize();

				const MotionDatabaseAsset database =
					MakeStreamedBindingMotionDatabase(context, 221U);
				std::vector<std::uint8_t> databaseBytes;
				context.Expect(SerializeMotionDatabaseAsset(database, databaseBytes)
						== AnimationStatus::Ok,
					"The cancellation fixture must serialize exact physical STRM blocks.");
				const std::filesystem::path databasePath =
					WriteBindingAsset("motion-cancel.temotiondb", databaseBytes);
				const UUID databaseId = resources.LoadAnimationAssetAsync(
					databasePath, AssetType::MotionDatabase).get();

				std::promise<void> blockerStarted;
				std::future<void> blockerStartedFuture = blockerStarted.get_future();
				std::promise<void> releaseBlocker;
				const std::shared_future<void> releaseFuture =
					releaseBlocker.get_future().share();
				auto blocker = ThreadPool::instance().createTask(
					[&blockerStarted, releaseFuture]() {
						blockerStarted.set_value();
						releaseFuture.wait();
					});
				std::future<void> blockerCompletion = blocker->getFuture();
				ThreadPool::instance().enqueue(blocker, 1U);
				const bool blockerRunning = blockerStartedFuture.wait_for(
					std::chrono::seconds(3)) == std::future_status::ready;

				const Object object = objects.createObject("Cancelled Motion Stream Animator");
				AnimatorComponent* component = objects.AddComponent<AnimatorComponent>(object);
				if(component != nullptr) {
					component->motionDatabaseOverrideAssetId = databaseId;
					objects.notifyObjectStructureChanged(object);
				}
				const bool componentCreated = component != nullptr;
				AnimatorComponentBindingCoordinator coordinator;
				const AnimatorComponentSynchronizationSummary queued =
					coordinator.Synchronize(animation, objects, resources);
				objects.RemoveComponent<AnimatorComponent>(object);
				const AnimatorComponentSynchronizationSummary removed =
					coordinator.Synchronize(animation, objects, resources);
				releaseBlocker.set_value();
				blockerCompletion.wait();
				const AnimatorComponentSynchronizationSummary afterLateWorkers =
					coordinator.Synchronize(animation, objects, resources);

				context.Expect(blockerRunning && componentCreated
					&& queued.requestedStreamPayloadCount
						== database.residentBlocks.size()
					&& queued.retainedStreamPayloadCount
						== database.residentBlocks.size()
					&& removed.runtimeActiveCount == 0U
					&& removed.cancelledStreamPayloadCount
						== database.residentBlocks.size()
					&& removed.retainedStreamPayloadCount == 0U
					&& afterLateWorkers.residentStreamPayloadCount == 0U
					&& afterLateWorkers.streamedMotionDatabaseCount == 0U,
					"Component removal must release every retain, cancel queued block IO, and reject late workers without publishing a database candidate.");

				coordinator.InvalidateAll(animation);
				animation.Shutdown();
				objects.clear();
				resources.DeleteResource(databaseId, AssetType::MotionDatabase);
				std::error_code cleanupError;
				std::filesystem::remove(databasePath, cleanupError);
				animation.Initialize();
				if(initializedPool) ThreadPool::instance().shutdown();
			});

		registry.Add("Animation.ComponentBinding.RegisteredDependenciesStreamAndReactivate",
			[](TestContext& context) {
				const bool initializedPool = ThreadPool::instance().getThreadCount() == 0U;
				if(initializedPool) ThreadPool::instance().initialize(2U);
				ObjectManager& objects = ObjectManager::instance();
				ResourceManager& resources = ResourceManager::Instance();
				AnimationSystem& animation = AnimationSystem::Instance();
				objects.clear();
				animation.Initialize();

				const TerraEngine::Resource::StreamingPolicy priorStreamingPolicy =
					resources.GetStreamingPolicy();
				TerraEngine::Resource::StreamingPolicy boundedStreamingPolicy =
					priorStreamingPolicy;
				// One new load per protected frame makes the safe-pose interval
				// deterministic and proves the coordinator retries a dependency set
				// instead of blocking the main thread until every file is resident.
				boundedStreamingPolicy.maxLoadsPerUpdate = 1U;
				resources.SetStreamingPolicy(boundedStreamingPolicy);

				SkeletonAsset skeleton = MakeBindingSkeleton();
				skeleton.assetId = MakeBindingAssetGuid(181U);
				AnimationClipAsset clip = MakeBindingClip(skeleton);
				clip.assetId = MakeBindingAssetGuid(201U);
				AnimationControllerAsset controller;
				controller.assetId = MakeBindingAssetGuid(221U);
				controller.skeletonAssetId = skeleton.assetId;
				controller.compatibilityFingerprint = skeleton.compatibilityFingerprint;
				controller.nodes.push_back({10U, AnimationControllerNodeKind::Clip, 0U,
					clip.assetId, 1U, skeleton.compatibilityFingerprint});
				controller.states.push_back({100U, 10U, 0U, true, false, false});
				controller.layers.push_back({400U, 100U, {}, 0U, 0U, false, 1.0F});

				std::vector<std::uint8_t> skeletonBytes;
				std::vector<std::uint8_t> clipBytes;
				std::vector<std::uint8_t> controllerBytes;
				context.Expect(SerializeSkeletonAsset(skeleton, skeletonBytes) == AnimationStatus::Ok
					&& SerializeAnimationClipAsset(clip, clipBytes) == AnimationStatus::Ok
					&& SerializeAnimationControllerAsset(controller, controllerBytes)
						== AnimationStatus::Ok,
					"The automatic-streaming fixture must produce three valid cooked dependencies.");
				const std::filesystem::path skeletonPath =
					WriteBindingAsset("streamed-controller.teskel", skeletonBytes);
				const std::filesystem::path clipPath =
					WriteBindingAsset("streamed-controller.teanim", clipBytes);
				const std::filesystem::path controllerPath =
					WriteBindingAsset("streamed-controller.teanimctrl", controllerBytes);
				resources.RegisterAssetPath(
					skeleton.assetId.value, skeletonPath, AssetType::Skeleton);
				resources.RegisterAssetPath(
					clip.assetId.value, clipPath, AssetType::AnimationClip);
				resources.RegisterAssetPath(
					controller.assetId.value, controllerPath, AssetType::AnimationController);
				context.Expect(!resources.IsAnimationAssetPublished(
						skeleton.assetId.value, AssetType::Skeleton)
					&& !resources.IsAnimationAssetPublished(
						clip.assetId.value, AssetType::AnimationClip)
					&& !resources.IsAnimationAssetPublished(
						controller.assetId.value, AssetType::AnimationController),
					"Path registration must not synchronously populate an animation cache.");

				const Object object = objects.createObject("Streamed Controller Animator");
				AnimatorComponent* component = objects.AddComponent<AnimatorComponent>(object);
				if(component != nullptr) {
					component->controllerAssetId = controller.assetId.value;
					objects.notifyObjectStructureChanged(object);
				}
				const UUID objectId = objects.getUUID(object);
				context.Expect(ExecuteBindingFrame(animation),
					"The first protected frame must schedule the registered controller without waiting for IO.");
				const AnimatorInstanceHandle handle = animation.ResolveAnimatorForObject(objectId);
				const std::shared_ptr<const PublishedAnimatorPose> pending =
					animation.LoadPublishedAnimatorPoseForObject(objectId);
				context.Expect(component != nullptr && handle.IsValid() && pending
					&& pending->status == AnimationStatus::MissingAsset && pending->safePose,
					"An unloaded dependency graph must remain an explicit safe pose while its first request is pending.");

				const auto hasAnimatorNeed = [&](const UUID& assetId) {
					const auto record = resources.GetStreamableAsset(assetId);
					return record && std::any_of(record->needSources.begin(), record->needSources.end(),
						[&](const TerraEngine::Resource::StreamingNeed& need) {
							return need.objectId == objectId
								&& need.needType == TerraEngine::Resource::StreamingNeedType::RuntimeExplicit;
						});
				};
				context.Expect(hasAnimatorNeed(controller.assetId.value),
					"The authored controller must publish an object-owned active runtime need immediately.");

				std::shared_ptr<const PublishedAnimatorPose> resident;
				std::size_t activationFrames = 0U;
				for(; activationFrames < 256U; ++activationFrames) {
					if(!ExecuteBindingFrame(animation)) break;
					resident = animation.LoadPublishedAnimatorPoseForObject(objectId);
					if(resident && resident->status == AnimationStatus::Ok
						&& resident->clip.assetId == clip.assetId) break;
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				context.Expect(activationFrames < 256U && resident
					&& resident->status == AnimationStatus::Ok
					&& resources.IsAnimationAssetPublished(
						skeleton.assetId.value, AssetType::Skeleton)
					&& resources.IsAnimationAssetPublished(
						clip.assetId.value, AssetType::AnimationClip)
					&& hasAnimatorNeed(skeleton.assetId.value)
					&& hasAnimatorNeed(clip.assetId.value),
					"Controller-discovered skeleton and clip dependencies must load on bounded later frames and retain active needs.");

				const AnimationGeneration skeletonBeforeEviction =
					resources.GetAnimationAssetRuntimeGeneration(
						skeleton.assetId.value, AssetType::Skeleton);
				const AnimationGeneration clipBeforeEviction =
					resources.GetAnimationAssetRuntimeGeneration(
						clip.assetId.value, AssetType::AnimationClip);
				context.Expect(resources.ReleaseStreamableAssetForRuntime(
						skeleton.assetId.value, AssetType::Skeleton, 10000U)
					&& resources.ReleaseStreamableAssetForRuntime(
						clip.assetId.value, AssetType::AnimationClip, 10000U)
					&& resources.GetAnimationAssetRuntimeGeneration(
						skeleton.assetId.value, AssetType::Skeleton) > skeletonBeforeEviction
					&& resources.GetAnimationAssetRuntimeGeneration(
						clip.assetId.value, AssetType::AnimationClip) > clipBeforeEviction,
					"Explicit eviction must publish generation tombstones while retaining registered reload paths.");
				context.Expect(ExecuteBindingFrame(animation),
					"The next protected frame must observe eviction and begin bounded reactivation.");
				const std::shared_ptr<const PublishedAnimatorPose> evicted =
					animation.LoadPublishedAnimatorPoseForObject(objectId);
				context.Expect(evicted && evicted->status == AnimationStatus::MissingAsset
					&& evicted->safePose,
					"At least one dependency must remain unavailable under the one-request budget, preserving safe-pose authority.");

				std::shared_ptr<const PublishedAnimatorPose> reactivated;
				std::size_t reactivationFrames = 0U;
				for(; reactivationFrames < 256U; ++reactivationFrames) {
					if(!ExecuteBindingFrame(animation)) break;
					reactivated = animation.LoadPublishedAnimatorPoseForObject(objectId);
					if(reactivated && reactivated->status == AnimationStatus::Ok) break;
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				context.Expect(reactivationFrames < 256U && reactivated
					&& reactivated->status == AnimationStatus::Ok
					&& animation.ResolveAnimatorForObject(objectId) == handle
					&& reactivated->skeleton.generation
						== resources.GetAnimationAssetRuntimeGeneration(
							skeleton.assetId.value, AssetType::Skeleton)
					&& reactivated->clip.generation
						== resources.GetAnimationAssetRuntimeGeneration(
							clip.assetId.value, AssetType::AnimationClip),
					"An active animator must automatically reload evicted dependencies without replacing its generation-safe handle.");

				objects.RemoveComponent<AnimatorComponent>(object);
				(void)animation.DeclareFrameWorkloads(1.0F / 60.0F);
				context.Expect(!hasAnimatorNeed(controller.assetId.value)
					&& !hasAnimatorNeed(skeleton.assetId.value)
					&& !hasAnimatorNeed(clip.assetId.value),
					"Removing the component must release only that object's active dependency needs.");

				animation.Shutdown();
				objects.clear();
				resources.SetStreamingPolicy(priorStreamingPolicy);
				resources.DeleteResource(
					controller.assetId.value, AssetType::AnimationController);
				resources.DeleteResource(clip.assetId.value, AssetType::AnimationClip);
				resources.DeleteResource(skeleton.assetId.value, AssetType::Skeleton);
				std::error_code cleanupError;
				for(const std::filesystem::path& path
					: {controllerPath, clipPath, skeletonPath}) {
					std::filesystem::remove(path, cleanupError);
					cleanupError.clear();
				}
				animation.Initialize();
				if(initializedPool) ThreadPool::instance().shutdown();
			});

		registry.Add("Animation.ComponentBinding.ControllerCompilesAndTransitionsProductionClip",
			[](TestContext& context) {
				const bool initializedPool = ThreadPool::instance().getThreadCount() == 0U;
				if(initializedPool) ThreadPool::instance().initialize(2U);
				ObjectManager& objects = ObjectManager::instance();
				ResourceManager& resources = ResourceManager::Instance();
				AnimationSystem& animation = AnimationSystem::Instance();
				objects.clear();
				animation.Initialize();

				const SkeletonAsset skeleton = MakeBindingSkeleton();
				AnimationClipAsset idle = MakeConstantBindingClip(
					context, skeleton, 121U, 0.0F);
				AnimationClipAsset action = MakeConstantBindingClip(
					context, skeleton, 141U, 10.0F);
				std::vector<std::uint8_t> skeletonBytes;
				std::vector<std::uint8_t> idleBytes;
				std::vector<std::uint8_t> actionBytes;
				context.Expect(SerializeSkeletonAsset(skeleton, skeletonBytes) == AnimationStatus::Ok
					&& SerializeAnimationClipAsset(idle, idleBytes) == AnimationStatus::Ok
					&& SerializeAnimationClipAsset(action, actionBytes) == AnimationStatus::Ok,
					"The controller production fixture must serialize its complete dependency set.");
				const std::filesystem::path skeletonPath =
					WriteBindingAsset("controller-binding.teskel", skeletonBytes);
				const std::filesystem::path idlePath =
					WriteBindingAsset("controller-idle.teanim", idleBytes);
				const std::filesystem::path actionPath =
					WriteBindingAsset("controller-action.teanim", actionBytes);
				const UUID skeletonId = resources.LoadSkeleton(
					skeletonPath, skeleton.compatibilityFingerprint);
				const UUID idleId = resources.LoadAnimationClip(
					idlePath, skeleton.compatibilityFingerprint);
				const UUID actionId = resources.LoadAnimationClip(
					actionPath, skeleton.compatibilityFingerprint);

				AnimationControllerAsset controller;
				controller.assetId = MakeBindingAssetGuid(161U);
				controller.skeletonAssetId = skeleton.assetId;
				controller.compatibilityFingerprint = skeleton.compatibilityFingerprint;
				controller.parameters.push_back({
					77U, "PlayAction", AnimationControllerParameterType::Trigger, std::monostate{}});
				controller.nodes.push_back({10U, AnimationControllerNodeKind::Clip, 0U,
					idle.assetId, resources.GetAnimationAssetRuntimeGeneration(
						idleId, AssetType::AnimationClip), skeleton.compatibilityFingerprint});
				controller.nodes.push_back({20U, AnimationControllerNodeKind::Clip, 0U,
					action.assetId, resources.GetAnimationAssetRuntimeGeneration(
						actionId, AssetType::AnimationClip), skeleton.compatibilityFingerprint});
				controller.states.push_back({100U, 10U, 0U, true, false, false});
				controller.states.push_back({200U, 20U, 0U, false, false, false});
				AnimationControllerTransition transition;
				transition.stableId = 300U;
				transition.sourceStateId = 100U;
				transition.destinationStateId = 200U;
				transition.conditionParameterId = 77U;
				transition.durationTicks = 4;
				controller.transitions.push_back(transition);
				controller.layers.push_back({400U, 100U, {}, 0U, 0U, false, 1.0F});
				std::vector<std::uint8_t> controllerBytes;
				context.Expect(SerializeAnimationControllerAsset(controller, controllerBytes)
					== AnimationStatus::Ok,
					"The authored controller must pass the same checked format used by ResourceManager.");
				const std::filesystem::path controllerPath =
					WriteBindingAsset("controller-binding.teanimctrl", controllerBytes);
				const UUID controllerId = resources.LoadAnimationAssetAsync(
					controllerPath, AssetType::AnimationController).get();
				context.Expect(controllerId == controller.assetId.value,
					"The opaque controller loader must retain the cooked durable asset identity.");

				const Object object = objects.createObject("Controller Animator");
				AnimatorComponent* component = objects.AddComponent<AnimatorComponent>(object);
				if(component != nullptr) {
					component->controllerAssetId = controllerId;
					component->defaultClipStartFrame = 12U;
					component->loopDefaultClip = true;
					objects.notifyObjectStructureChanged(object);
				}
				const UUID objectId = objects.getUUID(object);
				// Freeze authored time while the controller is first bound. This
				// makes the exact entry-frame contract observable without a scheduler
				// advance racing the assertion on the same frame.
				animation.SetAuthoredComponentPlaybackAdvancing(false);
				std::shared_ptr<const PublishedAnimatorPose> idlePublication;
				for(std::size_t attempt = 0U; attempt < 32U; ++attempt) {
					if(!ExecuteBindingFrame(animation)) break;
					idlePublication = animation.LoadPublishedAnimatorPoseForObject(objectId);
					if(idlePublication && idlePublication->status == AnimationStatus::Ok
						&& idlePublication->clip.assetId == idle.assetId) break;
					std::this_thread::yield();
				}
				AnimatorInstanceHandle handle = animation.ResolveAnimatorForObject(objectId);
				AnimatorPlaybackStateSnapshot playback =
					animation.LoadAnimatorPlaybackState(handle);
				context.Expect(component != nullptr && handle.IsValid() && idlePublication
					&& idlePublication->status == AnimationStatus::Ok
					&& idlePublication->controller.assetId.value == controllerId
					&& idlePublication->clip.assetId == idle.assetId
					&& idlePublication->snapshot
					&& idlePublication->snapshot->playback.currentStateId == 100U
					&& playback.absoluteTick == 12
					&& playback.looping,
					"The production coordinator must compile the controller asynchronously, resolve its skeleton, publish its entry clip, and honor the authored start frame and loop policy.");

				// Entering Play Mode used to destroy the component-owned handle and
				// its already compiled controller. The replacement then exposed a
				// reference/frame-zero pose for several asynchronous compile frames.
				animation.SetAuthoredComponentPlaybackAdvancing(true);
				const std::shared_ptr<const PublishedAnimatorPose> playBoundaryPose =
					animation.LoadPublishedAnimatorPoseForObject(objectId);
				context.Expect(animation.ResolveAnimatorForObject(objectId) == handle
					&& playBoundaryPose == idlePublication,
					"Changing the edit/play clock domain must retain the last complete controller pose until the protected replacement frame is ready.");
				context.Expect(ExecuteBindingFrame(animation),
					"The first Play Mode frame must restart the already compiled controller without a dependency gap.");
				const std::shared_ptr<const PublishedAnimatorPose> firstPlayingPose =
					animation.LoadPublishedAnimatorPoseForObject(objectId);
				const AnimatorPlaybackStateSnapshot firstPlayingPlayback =
					animation.LoadAnimatorPlaybackState(handle);
				context.Expect(animation.ResolveAnimatorForObject(objectId) == handle
					&& firstPlayingPose
					&& firstPlayingPose->status == AnimationStatus::Ok
					&& firstPlayingPose->clip.assetId == idle.assetId
					&& firstPlayingPlayback.absoluteTick > 12,
					"The first advancing frame must begin from the authored controller frame without publishing frame zero or a NotReady safe pose.");

				animation.SetAuthoredComponentPlaybackAdvancing(false);
				const std::shared_ptr<const PublishedAnimatorPose> stopBoundaryPose =
					animation.LoadPublishedAnimatorPoseForObject(objectId);
				context.Expect(animation.ResolveAnimatorForObject(objectId) == handle
					&& stopBoundaryPose == firstPlayingPose
					&& ExecuteBindingFrame(animation),
					"Stopping must retain the last complete pose until the authored start pose replaces it at the next protected frame.");
				const std::shared_ptr<const PublishedAnimatorPose> firstStoppedPose =
					animation.LoadPublishedAnimatorPoseForObject(objectId);
				const AnimatorPlaybackStateSnapshot firstStoppedPlayback =
					animation.LoadAnimatorPlaybackState(handle);
				context.Expect(animation.ResolveAnimatorForObject(objectId) == handle
					&& firstStoppedPose
					&& firstStoppedPose->status == AnimationStatus::Ok
					&& firstStoppedPose->clip.assetId == idle.assetId
					&& firstStoppedPlayback.absoluteTick == 12,
					"Returning to Edit Mode must atomically publish the authored controller frame without an intermediate frame-zero pose.");

				// The editor atomically restores its authored scene graph immediately
				// after switching back to the stopped clock domain. That restoration
				// gives the component a new durable generation even when its UUID and
				// controller remain unchanged. The replacement handle must reuse the
				// already compiled controller so its first frame is complete.
				objects.notifyObjectStructureChanged(object);
				context.Expect(ExecuteBindingFrame(animation),
					"The first frame after an authored scene restoration must evaluate without waiting for another controller compilation.");
				const AnimatorInstanceHandle restoredHandle =
					animation.ResolveAnimatorForObject(objectId);
				const std::shared_ptr<const PublishedAnimatorPose> restoredPose =
					animation.LoadPublishedAnimatorPoseForObject(objectId);
				const AnimatorPlaybackStateSnapshot restoredPlayback =
					animation.LoadAnimatorPlaybackState(restoredHandle);
				context.Expect(restoredHandle.IsValid()
					&& restoredHandle != handle
					&& restoredPose
					&& restoredPose->status == AnimationStatus::Ok
					&& restoredPose->clip.assetId == idle.assetId
					&& restoredPlayback.absoluteTick == 12,
					"An authored scene restore must publish the configured controller start frame immediately instead of a transient NotReady/reference pose.");
				handle = restoredHandle;

				component->defaultClipStartFrame = 24U;
				context.Expect(ExecuteBindingFrame(animation),
					"Editing the start frame must rebuild a controller-backed binding at the protected boundary.");
				const AnimatorInstanceHandle retimedHandle =
					animation.ResolveAnimatorForObject(objectId);
				const AnimatorPlaybackStateSnapshot retimedPlayback =
					animation.LoadAnimatorPlaybackState(retimedHandle);
				context.Expect(retimedHandle.IsValid(),
					"A controller-backed Inspector edit must retain a valid runtime binding.");
				context.Expect(retimedHandle == handle
					&& animation.IsAnimatorInstanceAlive(handle),
					"A reflected Inspector edit must replace immutable playback state without invalidating the component's generation-safe handle.");
				context.ExpectEqual<AnimationFixedTick>(24, retimedPlayback.absoluteTick,
					"A controller-backed Inspector edit must seek the replacement binding to the newly authored entry frame.");
				context.Expect(retimedPlayback.looping,
					"A controller-backed Inspector edit must preserve the authored loop choice.");

				handle = retimedHandle;
				animation.SetAuthoredComponentPlaybackAdvancing(true);
				bool completedLoopFrames = true;
				for(std::size_t frame = 0U; frame < 128U; ++frame) {
					if(!ExecuteBindingFrame(animation)) {
						completedLoopFrames = false;
						break;
					}
					handle = animation.ResolveAnimatorForObject(objectId);
					playback = animation.LoadAnimatorPlaybackState(handle);
					if(handle.IsValid()
						&& playback.runState == AnimatorPlaybackRunState::Playing
						&& playback.looping && playback.loopCount >= 1) {
						break;
					}
				}
				context.Expect(completedLoopFrames
					&& handle.IsValid()
					&& playback.runState == AnimatorPlaybackRunState::Playing
					&& playback.looping && playback.loopCount >= 1,
					"A controller entry clip must continue playing across its end when Loop Default Clip is enabled.");

				AnimationCommand trigger;
				trigger.target = handle;
				trigger.kind = AnimationCommandKind::FireTrigger;
				trigger.parameterType = AnimationParameterType::Trigger;
				trigger.parameterId = 77U;
				trigger.producerId = 19U;
				trigger.producerSequence = 1U;
				trigger.correlationId = 1U;
				trigger.targetTick = 0;
				context.Expect(animation.SubmitCommand(trigger).status == AnimationStatus::Queued,
					"A generation-safe trigger must enter the live controller frame through the deferred command queue.");
				std::array<float, 5U> transitionSamples{};
				for(std::size_t frame = 0U;
					frame < transitionSamples.size(); ++frame) {
					context.Expect(ExecuteBindingFrame(animation),
						"Every authored transition tick must publish through the production coordinator.");
					const std::shared_ptr<const PublishedAnimatorPose> transitionPose =
						animation.LoadPublishedAnimatorPoseForObject(objectId);
					transitionSamples[frame] = transitionPose
						&& transitionPose->snapshot
						&& transitionPose->snapshot->pose
						&& !transitionPose->snapshot->pose
							->LocalTransforms().empty()
						? transitionPose->snapshot->pose
							->LocalTransforms().front().translation.x
						: std::numeric_limits<float>::quiet_NaN();
				}
				const std::shared_ptr<const PublishedAnimatorPose> actionPublication =
					animation.LoadPublishedAnimatorPoseForObject(objectId);
				context.Expect(actionPublication && actionPublication->status == AnimationStatus::Ok
					&& actionPublication->clip.assetId == action.assetId
					&& actionPublication->snapshot
					&& actionPublication->snapshot->playback.currentStateId == 200U,
					"The worker-owned state-machine step must atomically promote the destination clip and observable state.");
				context.Expect(BindingNear(transitionSamples[0U], 0.0F)
					&& BindingNear(transitionSamples[1U], 2.5F)
					&& BindingNear(transitionSamples[2U], 5.0F)
					&& BindingNear(transitionSamples[3U], 7.5F)
					&& BindingNear(transitionSamples[4U], 10.0F),
					"An authored trigger transition must publish every blended pose instead of delaying the destination until one snapping frame.");

				animation.Shutdown();
				objects.clear();
				resources.DeleteResource(controllerId, AssetType::AnimationController);
				resources.DeleteResource(actionId, AssetType::AnimationClip);
				resources.DeleteResource(idleId, AssetType::AnimationClip);
				resources.DeleteResource(skeletonId, AssetType::Skeleton);
				std::error_code cleanupError;
				for(const std::filesystem::path& path
					: {controllerPath, actionPath, idlePath, skeletonPath}) {
					std::filesystem::remove(path, cleanupError);
					cleanupError.clear();
				}
				animation.Initialize();
				if(initializedPool) ThreadPool::instance().shutdown();
			});

		registry.Add("Animation.ComponentBinding.ProceduralGoalChangesPublishedPoseAndResult",
			[](TestContext& context) {
				ObjectManager& objects = ObjectManager::instance();
				ResourceManager& resources = ResourceManager::Instance();
				AnimationSystem& animation = AnimationSystem::Instance();
				objects.clear();
				animation.Initialize();

				const SkeletonAsset skeleton = MakeBindingSkeleton();
				const AnimationClipAsset clip = MakeBindingClip(skeleton);
				std::vector<std::uint8_t> skeletonBytes;
				std::vector<std::uint8_t> clipBytes;
				(void)SerializeSkeletonAsset(skeleton, skeletonBytes);
				(void)SerializeAnimationClipAsset(clip, clipBytes);
				const std::filesystem::path skeletonPath =
					WriteBindingAsset("procedural-binding.teskel", skeletonBytes);
				const std::filesystem::path clipPath =
					WriteBindingAsset("procedural-binding.teanim", clipBytes);
				const UUID skeletonId = resources.LoadSkeleton(
					skeletonPath, skeleton.compatibilityFingerprint);
				const UUID clipId = resources.LoadAnimationClip(
					clipPath, skeleton.compatibilityFingerprint);
				const Object object = objects.createObject("Procedural Animator");
				AnimatorComponent* component = objects.AddComponent<AnimatorComponent>(object);
				if(component != nullptr) {
					component->skeletonOverrideAssetId = skeletonId;
					component->defaultClipAssetId = clipId;
					objects.notifyObjectStructureChanged(object);
				}
				const UUID objectId = objects.getUUID(object);
				context.Expect(ExecuteBindingFrame(animation),
					"The procedural production fixture must publish an initial immutable pose.");
				const AnimatorInstanceHandle handle = animation.ResolveAnimatorForObject(objectId);
				const std::shared_ptr<const PublishedAnimatorPose> before =
					animation.LoadPublishedAnimatorPose(handle);

				NativeAnimationProceduralGoalCommand goal{};
				goal.header = {static_cast<std::uint32_t>(sizeof(goal)),
					kNativeAnimationAbiVersion,
					static_cast<std::uint16_t>(NativeAnimationRecordKind::ProceduralGoalCommand)};
				goal.animator = {handle.slotIndex, handle.generation};
				goal.command = NativeAnimationProceduralCommandKind::SetGoal;
				goal.goalKind = NativeAnimationProceduralGoalKind::PositionAndRotation;
				goal.coordinateSpace = NativeAnimationProceduralCoordinateSpace::Model;
				goal.goalId = 9001U;
				goal.targetJointStableId = 1U;
				goal.goalTransform.translation = {2.0F, 3.0F, 4.0F};
				goal.goalTransform.rotation = {0.0F, 0.0F, 0.0F, 1.0F};
				goal.goalTransform.scale = {1.0F, 1.0F, 1.0F};
				goal.poleVector = {0.0F, 1.0F, 0.0F};
				goal.positionWeight = 1.0F;
				goal.rotationWeight = 1.0F;
				goal.maximumStretchScale = 1.0F;
				goal.flags = NativeAnimationProceduralGoalFlag::Authoritative;
				goal.producerId = 29U;
				goal.producerSequence = 1U;
				goal.correlationId = 1U;
				goal.idempotencyKey = 1U;
				goal.cancellationToken = 1U;
				goal.targetTick = 0;
				if(before && before->snapshot) {
					goal.expectedPoseGeneration = before->snapshot->publicationGeneration;
					goal.expectedPublicationGeneration = before->authorityPublicationGeneration;
				}
				const NativeAnimationProceduralGoalReceipt queued =
					animation.SubmitProceduralGoal(goal);
				context.Expect(queued.status == NativeAnimationStatus::Queued
					&& ExecuteBindingFrame(animation) && ExecuteBindingFrame(animation),
					"A validated gameplay goal must activate only after its protected receipt boundary and evaluate on the following frame.");
				const std::shared_ptr<const PublishedAnimatorPose> after =
					animation.LoadPublishedAnimatorPose(handle);
				std::uint32_t requiredCount = 0U;
				const AnimationStatus countStatus = animation.CopyProceduralResults(
					handle, {}, requiredCount);
				std::vector<NativeAnimationProceduralResultSnapshot> results(requiredCount);
				const AnimationStatus copyStatus = animation.CopyProceduralResults(
					handle, results, requiredCount);
				const auto model = after && after->snapshot && after->snapshot->pose
					? after->snapshot->pose->ModelTransforms()
					: std::span<const AnimationTransform>{};
				context.Expect(after && after->status == AnimationStatus::Ok
					&& model.size() == 1U
					&& model[0U].translation == AnimationFloat3{2.0F, 3.0F, 4.0F}
					&& countStatus == AnimationStatus::BufferTooSmall
					&& copyStatus == AnimationStatus::Ok && results.size() == 1U
					&& results[0U].goalId == goal.goalId
					&& results[0U].poseGeneration == after->snapshot->publicationGeneration,
					"The live CPU-authoritative procedural stage must change the published pose and expose a generation-matched copied result.");

				animation.Shutdown();
				objects.clear();
				resources.DeleteResource(clipId, AssetType::AnimationClip);
				resources.DeleteResource(skeletonId, AssetType::Skeleton);
				std::error_code cleanupError;
				std::filesystem::remove(clipPath, cleanupError);
				cleanupError.clear();
				std::filesystem::remove(skeletonPath, cleanupError);
				animation.Initialize();
			});
	}
}
