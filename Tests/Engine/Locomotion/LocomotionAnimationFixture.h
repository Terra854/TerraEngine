// LocomotionAnimationFixture.h - cohesive production-format assets for animation integration tests.
//
// Tests that exercise AnimationSystem's authored-component path need a real
// ResourceManager-backed controller dependency graph. This fixture compiles a
// tiny one-joint skeleton and three compatible clips with the production
// serializers, then removes its temporary files and resource registrations when
// the fixture leaves scope.
#pragma once

#include "../TestHarness.h"

#include "Animation/Assets/AnimationClipAsset.h"
#include "Animation/Assets/AnimationControllerAsset.h"
#include "Animation/Assets/RagdollProfileAsset.h"
#include "Animation/Assets/SkeletonAsset.h"
#include "Animation/Ragdoll/RagdollProfileValidator.h"
#include "AnimationClipCompiler.h"
#include "Components/AnimatorComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Resource/ResourceManager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	// This is intentionally a test-only builder, not a replacement asset pipeline.
	// Stable IDs make the generated graph reproducible; unique temporary directories
	// let independent test cases safely materialize the same immutable identities.
	class LocomotionAnimationFixture final {
	public:
		static constexpr std::uint64_t kSpeedParameter = 601U;
		static constexpr std::uint64_t kDirectionXParameter = 602U;
		static constexpr std::uint64_t kDirectionZParameter = 603U;
		static constexpr std::uint64_t kGroundedParameter = 604U;
		static constexpr std::uint64_t kMovingParameter = 605U;
		static constexpr std::uint64_t kStartMoveTrigger = 606U;
		static constexpr std::uint64_t kStartRootTrigger = 607U;

		LocomotionAnimationFixture(const LocomotionAnimationFixture&) = delete;
		LocomotionAnimationFixture& operator=(const LocomotionAnimationFixture&) = delete;

		~LocomotionAnimationFixture() { Cleanup(); }

		[[nodiscard]] static std::unique_ptr<LocomotionAnimationFixture> Create(TestContext& context)
		{
			auto fixture = std::unique_ptr<LocomotionAnimationFixture>(new LocomotionAnimationFixture());
			if(!fixture->Build(context)) return nullptr;
			return fixture;
		}

		[[nodiscard]] const Animation::SkeletonAsset& Skeleton() const noexcept { return skeleton_; }
		[[nodiscard]] const Animation::AnimationClipAsset& IdleClip() const noexcept { return idle_; }
		[[nodiscard]] const Animation::AnimationClipAsset& MoveClip() const noexcept { return move_; }
		[[nodiscard]] const Animation::AnimationClipAsset& RootClip() const noexcept { return root_; }
		[[nodiscard]] const Animation::AnimationControllerAsset& Controller() const noexcept { return controller_; }
		[[nodiscard]] const Animation::RagdollProfileAsset& RagdollProfile() const noexcept { return ragdollProfile_; }
		[[nodiscard]] UUID ControllerId() const noexcept { return controllerId_; }
		[[nodiscard]] UUID RagdollProfileId() const noexcept { return ragdollProfileId_; }
		[[nodiscard]] const std::filesystem::path& ControllerPath() const noexcept { return controllerPath_; }

		// Attach the cooked controller to a caller-owned object. The test remains
		// responsible for clearing ObjectManager before destroying its object graph.
		[[nodiscard]] AnimatorComponent* AttachAnimator(ObjectManager& objects, const Object& object) const
		{
			AnimatorComponent* component = objects.AddComponent<AnimatorComponent>(object);
			if(component != nullptr) {
				component->controllerAssetId = controllerId_;
				component->loopDefaultClip = true;
				objects.notifyObjectStructureChanged(object);
			}
			return component;
		}

	private:
		LocomotionAnimationFixture()
			: resources_(&ResourceManager::Instance())
		{}

		static Animation::AnimationGuid Guid(std::uint8_t seed)
		{
			Animation::AnimationGuid value;
			constexpr std::array<std::uint8_t, 13U> prefix{
				'T', 'E', 'R', 'R', 'A', '-', 'L', 'O', 'C', 'O', '0', '6', '0'};
			std::copy(prefix.begin(), prefix.end(), value.value.begin());
			value.value[13U] = seed;
			value.value[14U] = 0xA5U;
			value.value[15U] = 0x5AU;
			return value;
		}

		static Animation::AnimationMatrix4x4 Identity()
		{
			Animation::AnimationMatrix4x4 value{};
			value.values[0U] = value.values[5U] = value.values[10U] = value.values[15U] = 1.0F;
			return value;
		}

		static Animation::AnimationClipAsset CompileClip(
			TestContext& context,
			const Animation::SkeletonAsset& skeleton,
			std::uint8_t seed,
			bool rootMotion,
			float endX,
			float endYaw)
		{
			using namespace Animation;
			TerraEngine::AssetPipeline::AnimationClipCompileInput input;
			input.assetId = Guid(seed);
			input.skeletonAssetId = skeleton.assetId;
			input.compatibilityFingerprint = skeleton.compatibilityFingerprint;
			input.durationTicks = 8;
			input.sampleRate = 60U;
			input.segmentDurationTicks = 8;

			TerraEngine::AssetPipeline::AnimationSourceTrack translation;
			translation.stableTargetId = 1U;
			translation.kind = rootMotion ? AnimationClipTrackKind::RootTranslation : AnimationClipTrackKind::Translation;
			translation.interpolation = AnimationInterpolation::Linear;
			translation.keys = {{0, {0.0F, 0.0F, 0.0F}}, {8, {endX, 0.0F, 0.0F}}};
			input.tracks.push_back(translation);
			if(rootMotion) {
				TerraEngine::AssetPipeline::AnimationSourceTrack yaw;
				yaw.stableTargetId = 1U;
				yaw.kind = AnimationClipTrackKind::RootYaw;
				yaw.interpolation = AnimationInterpolation::Linear;
				TerraEngine::AssetPipeline::AnimationSourceKey begin;
				begin.tick = 0;
				begin.scalarValue = 0.0F;
				TerraEngine::AssetPipeline::AnimationSourceKey end;
				end.tick = 8;
				end.scalarValue = endYaw;
				yaw.keys = {begin, end};
				input.tracks.push_back(yaw);
			}

			AnimationClipAsset clip;
			TerraEngine::AssetPipeline::AnimationClipCompileReport report;
			const AnimationStatus status = TerraEngine::AssetPipeline::CompileAnimationClip(input, {}, clip, report);
			context.Expect(status == AnimationStatus::Ok && (!rootMotion || clip.rootMotion.enabled),
				"The locomotion fixture clip must compile through the production animation compiler.");
			return clip;
		}

		bool Write(std::string_view name, std::span<const std::uint8_t> bytes, std::filesystem::path& output)
		{
			output = directory_ / name;
			std::ofstream stream(output, std::ios::binary | std::ios::trunc);
			if(!stream) return false;
			stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			return stream.good();
		}

		bool Build(TestContext& context)
		{
			using namespace Animation;
			static std::atomic<std::uint64_t> nextDirectory{1U};
			const std::filesystem::path tempRoot = std::filesystem::temp_directory_path();
			for(std::size_t attempt = 0U; attempt < 4096U; ++attempt) {
				const std::uint64_t serial = nextDirectory.fetch_add(1U, std::memory_order_relaxed);
				const std::filesystem::path candidate = tempRoot
					/ ("TerraEngineLocomotionAnimationFixture-" + std::to_string(serial));
				std::error_code createError;
				if(std::filesystem::create_directory(candidate, createError)) {
					directory_ = candidate;
					directoryCreated_ = true;
					break;
				}
				// Existing names are expected when another process starts its serial
				// counter at the same value. Other filesystem failures are not retried.
				if(createError && createError != std::errc::file_exists) {
					context.Expect(false, "The locomotion fixture must exclusively create its temporary directory.");
					return false;
				}
			}
			if(!directoryCreated_) {
				context.Expect(false, "The locomotion fixture could not reserve a unique temporary directory.");
				return false;
			}

			skeleton_.assetId = Guid(60U);
			skeleton_.compatibilityFingerprint = 0x4c4f434f4d4f5433ULL;
			skeleton_.rootJointIndex = 0U;
			skeleton_.referencePoseHash = 0x060060060060060ULL;
			SkeletonJoint root;
			root.stableId = 1U;
			root.parentIndex = -1;
			root.displayName = "LocomotionRoot";
			root.normalizedSourcePath = "/root";
			root.semanticRole = "hips";
			skeleton_.joints.push_back(root);
			skeleton_.inverseBindMatrices.push_back(Identity());

			idle_ = CompileClip(context, skeleton_, 80U, false, 0.0F, 0.0F);
			move_ = CompileClip(context, skeleton_, 100U, false, 0.2F, 0.0F);
			root_ = CompileClip(context, skeleton_, 120U, true, 1.0F, 0.35F);

			controller_.assetId = Guid(140U);
			controller_.skeletonAssetId = skeleton_.assetId;
			controller_.compatibilityFingerprint = skeleton_.compatibilityFingerprint;
			controller_.parameters = {
				{kSpeedParameter, "Speed", AnimationControllerParameterType::Float, 0.0F},
				{kDirectionXParameter, "DirectionX", AnimationControllerParameterType::Float, 0.0F},
				{kDirectionZParameter, "DirectionZ", AnimationControllerParameterType::Float, 0.0F},
				{kGroundedParameter, "Grounded", AnimationControllerParameterType::Boolean, true},
				{kMovingParameter, "Moving", AnimationControllerParameterType::Boolean, false},
				// Current authored state-machine transitions accept trigger conditions;
				// these two explicit edges keep the five locomotion observations typed.
				{kStartMoveTrigger, "StartMove", AnimationControllerParameterType::Trigger, std::monostate{}},
				{kStartRootTrigger, "StartRootMotion", AnimationControllerParameterType::Trigger, std::monostate{}}};

			const std::array<std::pair<std::uint64_t, AnimationClipAsset*>, 3U> clips{{
				{10U, &idle_}, {20U, &move_}, {30U, &root_}}};
			std::array<std::filesystem::path, 3U> clipPaths{};
			for(std::size_t index = 0U; index < clips.size(); ++index) {
				std::vector<std::uint8_t> bytes;
				if(SerializeAnimationClipAsset(*clips[index].second, bytes) != AnimationStatus::Ok
					|| !Write(index == 0U ? "idle.teanim" : index == 1U ? "move.teanim" : "root.teanim", bytes, clipPaths[index])) {
					context.Expect(false, "The locomotion fixture must serialize each compatible clip to its isolated temporary directory.");
					return false;
				}
			}
			std::vector<std::uint8_t> skeletonBytes;
			if(SerializeSkeletonAsset(skeleton_, skeletonBytes) != AnimationStatus::Ok
				|| !Write("skeleton.teskel", skeletonBytes, skeletonPath_)) {
				context.Expect(false, "The locomotion fixture must serialize its skeleton through the production asset format.");
				return false;
			}
			const UUID skeletonId = resources_->LoadSkeleton(skeletonPath_, skeleton_.compatibilityFingerprint);
			if(skeletonId != UUID{}) registered_.push_back({skeletonId, AssetType::Skeleton});
			if(skeletonId != skeleton_.assetId.value) {
				context.Expect(false, "ResourceManager must preserve the fixture skeleton's deterministic identity.");
				return false;
			}
			for(std::size_t index = 0U; index < clips.size(); ++index) {
				const UUID clipId = resources_->LoadAnimationClip(clipPaths[index], skeleton_.compatibilityFingerprint);
				if(clipId != UUID{}) registered_.push_back({clipId, AssetType::AnimationClip});
				if(clipId != clips[index].second->assetId.value) {
					context.Expect(false, "ResourceManager must preserve each fixture clip's deterministic identity.");
					return false;
				}
				controller_.nodes.push_back({clips[index].first, AnimationControllerNodeKind::Clip, 0U,
					clips[index].second->assetId,
				resources_->GetAnimationAssetRuntimeGeneration(clipId, AssetType::AnimationClip),
					skeleton_.compatibilityFingerprint});
				if(controller_.nodes.back().dependencyGeneration == 0U) {
					context.Expect(false, "Each fixture controller clip dependency must have a published resource generation.");
					return false;
				}
			}
			controller_.states = {{101U, 10U, 0U, true, false, false},
				{102U, 20U, 0U, false, false, false}, {103U, 30U, 0U, false, false, false}};
			AnimationControllerTransition toMove;
			toMove.stableId = 201U;
			toMove.sourceStateId = 101U;
			toMove.destinationStateId = 102U;
			toMove.conditionParameterId = kStartMoveTrigger;
			toMove.durationTicks = 2;
			AnimationControllerTransition toRoot = toMove;
			toRoot.stableId = 202U;
			toRoot.destinationStateId = 103U;
			toRoot.conditionParameterId = kStartRootTrigger;
			controller_.transitions = {toMove, toRoot};
			controller_.layers.push_back({301U, 101U, {}, 0U, 0U, false, 1.0F});

			std::vector<std::uint8_t> controllerBytes;
			controllerPath_ = directory_ / "locomotion.teanimctrl";
			if(SerializeAnimationControllerAsset(controller_, controllerBytes) != AnimationStatus::Ok
				|| !Write("locomotion.teanimctrl", controllerBytes, controllerPath_)) {
				context.Expect(false, "The locomotion fixture controller must serialize through the production controller format.");
				return false;
			}
			controllerId_ = resources_->LoadAnimationAssetAsync(controllerPath_, AssetType::AnimationController).get();
			if(controllerId_ != UUID{}) registered_.push_back({controllerId_, AssetType::AnimationController});
			if(controllerId_ != controller_.assetId.value) {
				context.Expect(false, "ResourceManager must publish the fixture controller with its deterministic identity.");
				return false;
			}

			if(!BuildRagdollProfile(context)) return false;
			return true;
		}

		bool BuildRagdollProfile(TestContext& context)
		{
			using namespace Animation;
			ragdollProfile_.assetId = Guid(160U);
			ragdollProfile_.skeletonAssetId = skeleton_.assetId;
			ragdollProfile_.skeletonFingerprint = skeleton_.compatibilityFingerprint;
			ragdollProfile_.referencePoseHash = skeleton_.referencePoseHash;
			ragdollProfile_.physicsProviderFingerprint = 0x4a4f4c54U;
			ragdollProfile_.rootBodyId = 100U;
			ragdollProfile_.expectedTotalMassKilograms = 8.0F;
			ragdollProfile_.expectedMassToleranceFraction = 0.01F;
			ragdollProfile_.minimumBodyMassFraction = 0.9F;
			ragdollProfile_.maximumBodyMassFraction = 1.0F;
			ragdollProfile_.allowedCollisionLayerMask = 0x1U;

			RagdollColliderRecord collider;
			collider.colliderId = 1000U;
			collider.shape = RagdollColliderShape::Sphere;
			collider.dimensions = {0.25F, 0.0F, 0.0F};
			RagdollBodyRecord body;
			body.bodyId = 100U;
			body.stableJointId = 1U;
			body.colliders.push_back(collider);
			body.massKilograms = 8.0F;
			body.gravityFactor = 0.0F;
			body.collisionLayer = 0U;
			body.collisionMask = 0x1U;
			ragdollProfile_.bodies.push_back(std::move(body));
			ragdollProfile_.blends.push_back({1U, 1.0F, RagdollBoundaryPolicy::KinematicAnchor});

			RagdollRecoveryRecord recovery;
			recovery.candidateId = 700U;
			recovery.clipAssetId = root_.assetId;
			recovery.requiredContactMask = 0U;
			recovery.clipGeneration = resources_->GetAnimationAssetRuntimeGeneration(
				root_.assetId.value, AssetType::AnimationClip);
			recovery.clipSkeletonFingerprint = skeleton_.compatibilityFingerprint;
			recovery.requiredReferencePoseHash = skeleton_.referencePoseHash;
			recovery.rootJointStableId = 1U;
			recovery.posture = RagdollRecoveryPosture::Any;
			recovery.maximumPoseErrorMeters = 10.0F;
			recovery.maximumLinearSpeed = 100.0F;
			recovery.maximumAngularSpeed = 100.0F;
			recovery.blendDurationSeconds = 0.05F;
			recovery.clearanceRadiusMeters = 0.25F;
			recovery.clearanceHeightMeters = 1.0F;
			ragdollProfile_.recovery.push_back(std::move(recovery));

			const std::array<RagdollCollisionLayerRule, 1U> collisionLayers{{{0U, 0x1U, true}}};
			RagdollProfileValidationOptions options;
			options.collisionLayers = collisionLayers;
			const auto validation = ValidateRagdollProfile(ragdollProfile_, skeleton_, options);
			if(!validation.IsValid()) {
				context.Expect(false, "The fixture ragdoll profile and recovery candidate must pass production validation.");
				return false;
			}

			std::vector<std::uint8_t> profileBytes;
			ragdollProfilePath_ = directory_ / "locomotion.teragdoll";
			if(SerializeRagdollProfileAsset(ragdollProfile_, profileBytes) != AnimationStatus::Ok
				|| !Write("locomotion.teragdoll", profileBytes, ragdollProfilePath_)) {
				context.Expect(false, "The fixture ragdoll profile must serialize in the production asset format.");
				return false;
			}
			RagdollProfileAsset roundTrip;
			if(DeserializeRagdollProfileAsset(profileBytes,
				skeleton_.compatibilityFingerprint, roundTrip) != AnimationStatus::Ok
				|| roundTrip.assetId != ragdollProfile_.assetId
				|| roundTrip.bodies != ragdollProfile_.bodies
				|| roundTrip.recovery != ragdollProfile_.recovery) {
				context.Expect(false, "The serialized fixture ragdoll profile must pass a production decoder round trip.");
				return false;
			}
			ragdollProfileId_ = resources_->LoadAnimationAssetAsync(
				ragdollProfilePath_, AssetType::RagdollProfile).get();
			if(ragdollProfileId_ != UUID{}) registered_.push_back({ragdollProfileId_, AssetType::RagdollProfile});
			if(ragdollProfileId_ != ragdollProfile_.assetId.value) {
				context.Expect(false, "ResourceManager must publish the fixture ragdoll profile with its deterministic identity.");
				return false;
			}
			return true;
		}

		void Cleanup() noexcept
		{
			if(resources_ != nullptr) {
				for(auto it = registered_.rbegin(); it != registered_.rend(); ++it)
					resources_->DeleteResource(it->id, it->type);
			}
			std::error_code error;
			if(directoryCreated_)
				std::filesystem::remove_all(directory_, error);
		}

		struct RegisteredAsset { UUID id{}; AssetType type{}; };
		ResourceManager* resources_ = nullptr;
		std::filesystem::path directory_;
		std::filesystem::path skeletonPath_;
		std::filesystem::path controllerPath_;
		std::filesystem::path ragdollProfilePath_;
		bool directoryCreated_ = false;
		Animation::SkeletonAsset skeleton_;
		Animation::AnimationClipAsset idle_;
		Animation::AnimationClipAsset move_;
		Animation::AnimationClipAsset root_;
		Animation::AnimationControllerAsset controller_;
		Animation::RagdollProfileAsset ragdollProfile_;
		UUID controllerId_{};
		UUID ragdollProfileId_{};
		std::vector<RegisteredAsset> registered_;
	};
}
