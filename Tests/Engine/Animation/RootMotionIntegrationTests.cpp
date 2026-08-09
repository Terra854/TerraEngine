// RootMotionIntegrationTests.cpp - deferred root-motion routing contracts.

#include "TestHarness.h"

#include "AnimationAuthorityTestHelpers.h"
#include "Animation/AnimationSystem.h"
#include "Animation/Runtime/RootMotion.h"
#include "AnimationClipCompiler.h"
#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "Scripting/ScriptGlue.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		AnimationRootMotionApplicationRequest MakeRequest(
			const Object& object,
			ObjectManager& objects,
			AnimationRootMotionApplicationMode mode,
			std::uint64_t correlation) {
			AnimationRootMotionApplicationRequest request;
			request.delta.animator = {17U, 3U};
			request.delta.sourceBeginTick = 10;
			request.delta.sourceEndTick = 11;
			request.delta.localTranslation = {2.0F, 0.0F, 0.0F};
			request.delta.worldTranslation = {2.0F, 0.0F, 0.0F};
			request.delta.localYawRadians = 0.25F;
			request.delta.worldYawRadians = 0.25F;
			request.delta.sourceStateId = 91U;
			request.delta.accumulatedCycles = 2;
			request.delta.layerIndex = 3U;
			request.delta.generation = 7U;
			request.delta.policy = AnimatorRootMotionPolicy::ExtractOnly;
			request.ownerObjectId = objects.getUUID(object);
			request.objectGeneration = TerraEngine::CompressObjectGeneration(
				objects.getObjectLifetimeGeneration(object));
			request.componentGeneration = TerraEngine::CompressObjectGeneration(
				objects.getObjectComponentGeneration(object));
			request.mode = mode;
			request.producerId = 11U;
			request.producerSequence = correlation;
			request.correlationId = correlation;
			request.idempotencyKey = correlation;
			request.publicationGeneration = 4U;
			return request;
		}

		bool IsClose(float left, float right) {
			return std::abs(left - right) <= 0.0001F;
		}

		const AnimationRootMotionApplicationResult* FindResult(
			const std::vector<AnimationRootMotionApplicationResult>& results,
			std::uint64_t correlation,
			bool idempotencyConflict = false) {
			const auto found = std::find_if(results.begin(), results.end(),
				[correlation, idempotencyConflict](const auto& value) {
					return value.correlationId == correlation
						&& value.idempotencyConflict == idempotencyConflict;
				});
			return found == results.end() ? nullptr : &(*found);
		}

		AnimationGuid RootMotionGuid(std::uint8_t seed) {
			AnimationGuid guid;
			for(std::size_t index = 0U; index < guid.value.size(); ++index) {
				guid.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return guid;
		}

		std::shared_ptr<const SkeletonAsset> MakeRootMotionSkeleton() {
			auto skeleton = std::make_shared<SkeletonAsset>();
			skeleton->assetId = RootMotionGuid(120U);
			skeleton->compatibilityFingerprint = 0xA11A11A11ULL;
			skeleton->rootJointIndex = 0U;
			SkeletonJoint joint;
			joint.stableId = 1U;
			joint.parentIndex = -1;
			joint.normalizedSourcePath = "/root";
			skeleton->joints.push_back(joint);
			SkeletonSocket socket;
			socket.stableId = 5U;
			socket.jointStableId = joint.stableId;
			socket.displayName = "Gameplay Socket";
			skeleton->sockets.push_back(socket);
			return skeleton;
		}

		std::shared_ptr<const AnimationClipAsset> MakeRootMotionClip(
			TestContext& context,
			const SkeletonAsset& skeleton) {
			using namespace TerraEngine::AssetPipeline;
			AnimationClipCompileInput input;
			input.assetId = RootMotionGuid(80U);
			input.skeletonAssetId = skeleton.assetId;
			input.compatibilityFingerprint = skeleton.compatibilityFingerprint;
			input.durationTicks = 4;
			input.sampleRate = 60U;
			input.segmentDurationTicks = 4;
			AnimationSourceTrack translation;
			translation.stableTargetId = 1U;
			translation.kind = AnimationClipTrackKind::RootTranslation;
			translation.interpolation = AnimationInterpolation::Linear;
			translation.keys = {
				{0, {0.0F, 0.0F, 0.0F}},
				{4, {4.0F, 0.0F, 0.0F}}};
			AnimationSourceTrack yaw;
			yaw.stableTargetId = 1U;
			yaw.kind = AnimationClipTrackKind::RootYaw;
			yaw.interpolation = AnimationInterpolation::Linear;
			AnimationSourceKey startYaw;
			startYaw.tick = 0;
			startYaw.scalarValue = 0.0F;
			AnimationSourceKey endYaw;
			endYaw.tick = 4;
			endYaw.scalarValue = 0.4F;
			yaw.keys = {startYaw, endYaw};
			input.tracks = {translation, yaw};
			auto clip = std::make_shared<AnimationClipAsset>();
			AnimationClipCompileReport report;
			context.Expect(CompileAnimationClip(input, {}, *clip, report) == AnimationStatus::Ok
				&& clip->rootMotion.enabled,
				"The scheduler fixture must compile a deterministic one-meter-per-tick root track.");
			return clip;
		}

		bool ExecuteRootMotionFrame(AnimationSystem& system, const char* frameId) {
			std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> workloads =
				system.DeclareFrameWorkloads(1.0F / 60.0F);
			const TerraEngine::Scheduling::FrameExecutionPlan plan =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(workloads), frameId);
			return plan.valid && plan.completed;
		}

		NativeAnimationCapabilityResult QueryNativeCapability(
			AnimatorInstanceHandle handle,
			NativeAnimationCapability capability,
			std::uint64_t stableId = 0U) {
			NativeAnimationCapabilityQuery query{};
			query.header = {
				static_cast<std::uint32_t>(sizeof(query)),
				kNativeAnimationAbiVersion,
				static_cast<std::uint16_t>(NativeAnimationRecordKind::CapabilityQuery)};
			query.animator = {handle.slotIndex, handle.generation};
			query.capability = capability;
			query.stableId = stableId;
			NativeAnimationCapabilityResult result{};
			result.header = {
				static_cast<std::uint32_t>(sizeof(result)),
				kNativeAnimationAbiVersion,
				static_cast<std::uint16_t>(NativeAnimationRecordKind::CapabilityResult)};
			(void)Native_Animation_QueryCapability(&query, &result);
			return result;
		}
	}

	void RegisterRootMotionIntegrationTests(TestRegistry& registry) {
		registry.Add("Animation.RootMotion.RoutesIgnoreExtractNetworkAndTransformAtBoundaries", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			AnimationRootMotionResultPublisher::Instance().Clear();
			const Object object = objects.createObject("Root Motion Target");
			if(objects.GetComponent<Transform>(object) == nullptr) objects.AddComponent<Transform>(object);
			TerraEngine::Scene::SceneMutationQueue queue;
			Physics::PhysicsSystem& physics = Physics::PhysicsSystem::Instance();

			for(const AnimationRootMotionApplicationMode mode : {
				AnimationRootMotionApplicationMode::Ignore,
				AnimationRootMotionApplicationMode::ExtractOnly,
				AnimationRootMotionApplicationMode::DeterministicNetwork}) {
				const AnimationRootMotionApplicationRequest request = MakeRequest(
					object, objects, mode, 100U + static_cast<std::uint64_t>(mode));
				const auto receipt = RouteRootMotionApplication(request, objects, queue, physics);
				context.Expect(receipt.status == AnimationStatus::Ok && queue.PendingTransactions().empty(),
					"Ignored, extracted, and deterministic-network root motion must publish copied results without a live scene write.");
			}

			const auto transformReceipt = RouteRootMotionApplication(
				MakeRequest(object, objects, AnimationRootMotionApplicationMode::ApplyToTransform, 200U),
				objects, queue, physics);
			const Transform* before = objects.GetComponent<Transform>(object);
			context.Expect(transformReceipt.status == AnimationStatus::Queued && before != nullptr
				&& IsClose(before->position.x, 0.0F) && queue.PendingTransactions().size() == 1U,
				"Transform application must stay invisible until its typed scene transaction flushes.");
			const auto flush = queue.Apply(objects, "RootMotionTestFlush");
			const Transform* after = objects.GetComponent<Transform>(object);
			std::vector<AnimationRootMotionApplicationResult> results;
			AnimationRootMotionResultPublisher::Instance().CopyForAnimator({17U, 3U}, results);
			const bool applied = std::any_of(results.begin(), results.end(), [](const auto& value) {
				return value.correlationId == 200U && value.disposition == AnimationRootMotionDisposition::Applied;
			});
			context.Expect(!flush.transactionResults.empty() && after != nullptr && IsClose(after->position.x, 2.0F) && applied,
				"The protected flush must apply the transaction and publish its generation-safe acknowledgement.");
			objects.clear();
		});

		registry.Add("Animation.RootMotion.CharacterAndPhysicsFailClosedUntilAuthoritativeStateExists", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			AnimationRootMotionResultPublisher::Instance().Clear();
			const Object object = objects.createObject("Unbound Physics Target");
			TerraEngine::Scene::SceneMutationQueue queue;
			Physics::PhysicsSystem& physics = Physics::PhysicsSystem::Instance();
			const auto character = RouteRootMotionApplication(MakeRequest(object, objects,
				AnimationRootMotionApplicationMode::ApplyToCharacter, 300U), objects, queue, physics);
			const auto body = RouteRootMotionApplication(MakeRequest(object, objects,
				AnimationRootMotionApplicationMode::ApplyToPhysicsBody, 301U), objects, queue, physics);
			std::vector<AnimationRootMotionApplicationResult> results;
			AnimationRootMotionResultPublisher::Instance().CopyForAnimator({17U, 3U}, results);
			const AnimationRootMotionApplicationResult* characterResult = FindResult(results, 300U);
			const AnimationRootMotionApplicationResult* bodyResult = FindResult(results, 301U);
			context.Expect((character.status == AnimationStatus::NotReady || character.status == AnimationStatus::StaleHandle)
				&& (body.status == AnimationStatus::NotReady || body.status == AnimationStatus::StaleHandle)
				&& queue.PendingTransactions().empty()
				&& characterResult != nullptr
				&& characterResult->disposition == AnimationRootMotionDisposition::Rejected
				&& bodyResult != nullptr
				&& bodyResult->disposition == AnimationRootMotionDisposition::Rejected,
				"Character and physics modes must reject safely until a matching authoritative state snapshot exists.");
			objects.clear();
		});

		registry.Add("Animation.RootMotion.ExactRetriesAreIdempotentAndChangedRetriesConflict", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			AnimationRootMotionResultPublisher::Instance().Clear();
			const Object object = objects.createObject("Idempotent Root Motion Target");
			if(objects.GetComponent<Transform>(object) == nullptr) objects.AddComponent<Transform>(object);
			TerraEngine::Scene::SceneMutationQueue queue;
			Physics::PhysicsSystem& physics = Physics::PhysicsSystem::Instance();
			const AnimationRootMotionApplicationRequest request = MakeRequest(
				object, objects, AnimationRootMotionApplicationMode::ApplyToTransform, 350U);
			const auto first = RouteRootMotionApplication(request, objects, queue, physics);
			const auto retry = RouteRootMotionApplication(request, objects, queue, physics);
			AnimationRootMotionApplicationRequest changed = request;
			changed.delta.localTranslation.x = 3.0F;
			changed.delta.worldTranslation.x = 3.0F;
			const auto conflict = RouteRootMotionApplication(changed, objects, queue, physics);

			context.Expect(first.status == AnimationStatus::Queued
				&& retry.status == AnimationStatus::Queued && retry.duplicate
				&& conflict.status == AnimationStatus::Rejected
				&& conflict.idempotencyConflict
				&& queue.PendingTransactions().size() == 1U,
				"An exact retry must reuse one accepted application while a changed payload with the same key rejects before a second mutation is queued.");
			(void)queue.Apply(objects, "RootMotionIdempotencyFlush");
			const Transform* transform = objects.GetComponent<Transform>(object);
			std::vector<AnimationRootMotionApplicationResult> results;
			AnimationRootMotionResultPublisher::Instance().CopyForAnimator({17U, 3U}, results);
			const AnimationRootMotionApplicationResult* applied = FindResult(results, 350U, false);
			const AnimationRootMotionApplicationResult* rejected = FindResult(results, 350U, true);
			context.Expect(transform != nullptr && IsClose(transform->position.x, 2.0F)
				&& applied != nullptr && applied->disposition == AnimationRootMotionDisposition::Applied
				&& applied->duplicate
				&& rejected != nullptr && rejected->disposition == AnimationRootMotionDisposition::Rejected,
				"The once-only ledger must preserve both the duplicate acknowledgement and the terminal conflicting retry without double application.");
			objects.clear();
		});

		registry.Add("Animation.RootMotion.ProtectedTransformCommitConvertsWorldDeltaIntoParentLocalSpace", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			AnimationRootMotionResultPublisher::Instance().Clear();
			const Object parent = objects.createObject("Scaled Root Motion Parent");
			const Object child = objects.createObject("Root Motion Child");
			if(objects.GetComponent<Transform>(parent) == nullptr) objects.AddComponent<Transform>(parent);
			if(objects.GetComponent<Transform>(child) == nullptr) objects.AddComponent<Transform>(child);
			Transform* parentTransform = objects.GetComponent<Transform>(parent);
			Transform* childTransform = objects.GetComponent<Transform>(child);
			parentTransform->position = {10.0F, 0.0F, 0.0F};
			parentTransform->scale = {2.0F, 1.0F, 1.0F};
			context.Expect(objects.setParent(child, parent),
				"The fixture must establish a parent before capturing the copied world transform.");
			// ObjectManager preserves world placement while reparenting. Author the
			// intended one-metre parent-local offset afterwards so the copied source
			// world transform is genuinely twelve metres under the scaled parent.
			// Reparenting also preserves world scale by authoring an inverse local
			// scale, so restore unit local scale before declaring a world scale of two.
			childTransform = objects.GetComponent<Transform>(child);
			childTransform->position = {1.0F, 0.0F, 0.0F};
			childTransform->scale = {1.0F, 1.0F, 1.0F};

			AnimationRootMotionApplicationRequest request = MakeRequest(
				child, objects, AnimationRootMotionApplicationMode::ApplyToTransform, 375U);
			request.delta.localTranslation = {1.0F, 0.0F, 0.0F};
			request.delta.worldTranslation = {};
			request.delta.localYawRadians = 0.0F;
			request.delta.worldYawRadians = 0.0F;
			request.sourceObjectWorldTransform.translation = {12.0F, 0.0F, 0.0F};
			request.sourceObjectWorldTransform.scale = {2.0F, 1.0F, 1.0F};
			request.sourceObjectWorldTransformCaptured = true;
			TerraEngine::Scene::SceneMutationQueue queue;
			const auto receipt = RouteRootMotionApplication(
				request,
				objects,
				queue,
				Physics::PhysicsSystem::Instance(),
				AnimationRootMotionSceneVisibility::ImmediateProtectedCommit);

			std::vector<AnimationRootMotionApplicationResult> results;
			AnimationRootMotionResultPublisher::Instance().CopyForAnimator({17U, 3U}, results);
			const AnimationRootMotionApplicationResult* applied = FindResult(results, 375U);
			childTransform = objects.GetComponent<Transform>(child);
			context.Expect(receipt.status == AnimationStatus::Ok
				&& queue.PendingTransactions().empty()
				&& childTransform != nullptr && IsClose(childTransform->position.x, 2.0F)
				&& applied != nullptr && applied->disposition == AnimationRootMotionDisposition::Applied
				&& IsClose(applied->requestedTranslation.x, 2.0F)
				&& applied->ownerObjectId == objects.getUUID(child)
				&& applied->sourceStateId == 91U && applied->accumulatedCycles == 2
				&& applied->layerIndex == 3U,
				"The animation publication boundary must commit one world-space root delta as the correct parent-local transform with complete copied metadata.");
			objects.clear();
		});

		registry.Add("Animation.RootMotion.TerminalHistoryRollsOverWithoutReapplyingCurrentMutation", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			AnimationRootMotionResultPublisher::Instance().Clear();
			const Object object = objects.createObject("Root Motion Rollover Target");
			if(objects.GetComponent<Transform>(object) == nullptr) objects.AddComponent<Transform>(object);
			TerraEngine::Scene::SceneMutationQueue queue;
			Physics::PhysicsSystem& physics = Physics::PhysicsSystem::Instance();
			const AnimationRootMotionApplicationRequest appliedRequest = MakeRequest(
				object, objects, AnimationRootMotionApplicationMode::ApplyToTransform, 900U);
			context.Expect(RouteRootMotionApplication(appliedRequest, objects, queue, physics).status
				== AnimationStatus::Queued,
				"The rollover fixture must begin with one accepted mutating application.");
			(void)queue.Apply(objects, "RootMotionRolloverInitialApply");

			AnimationStatus newestStatus = AnimationStatus::InternalError;
			constexpr std::uint64_t kTerminalFloodCount = 4096U;
			for(std::uint64_t index = 0U; index < kTerminalFloodCount; ++index) {
				AnimationRootMotionApplicationRequest terminal = MakeRequest(
					object,
					objects,
					AnimationRootMotionApplicationMode::ExtractOnly,
					10000U + index);
				newestStatus = RouteRootMotionApplication(terminal, objects, queue, physics).status;
			}
			const AnimationRootMotionApplicationReceipt replay = RouteRootMotionApplication(
				appliedRequest, objects, queue, physics);
			const Transform* transform = objects.GetComponent<Transform>(object);
			std::vector<AnimationRootMotionApplicationResult> results;
			AnimationRootMotionResultPublisher::Instance().CopyForAnimator({17U, 3U}, results);
			const AnimationRootMotionApplicationResult* newest = FindResult(
				results, 10000U + kTerminalFloodCount - 1U);
			context.Expect(newestStatus == AnimationStatus::Ok
				&& results.size() == 4096U && newest != nullptr
				&& replay.status == AnimationStatus::Rejected
				&& replay.idempotencyConflict
				&& queue.PendingTransactions().empty()
				&& transform != nullptr && IsClose(transform->position.x, 2.0F),
				"Terminal application identities must roll over at the bound while the latest-applied ledger prevents an evicted current-generation mutation from running twice.");
			AnimationRootMotionResultPublisher::Instance().Clear();
			objects.clear();
		});

		registry.Add("Animation.RootMotion.ConflictingPreparedRequestsAcknowledgeAppliedAndRejectedWithoutRollbackLeak", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			AnimationRootMotionResultPublisher::Instance().Clear();
			const Object object = objects.createObject("Conflict Target");
			if(objects.GetComponent<Transform>(object) == nullptr) objects.AddComponent<Transform>(object);
			TerraEngine::Scene::SceneMutationQueue queue;
			Physics::PhysicsSystem& physics = Physics::PhysicsSystem::Instance();
			const auto first = RouteRootMotionApplication(MakeRequest(object, objects,
				AnimationRootMotionApplicationMode::ApplyToTransform, 401U), objects, queue, physics);
			AnimationRootMotionApplicationRequest secondRequest = MakeRequest(object, objects,
				AnimationRootMotionApplicationMode::ApplyToTransform, 402U);
			secondRequest.delta.generation = 8U;
			secondRequest.publicationGeneration = 5U;
			const auto second = RouteRootMotionApplication(secondRequest, objects, queue, physics);
			queue.Apply(objects, "RootMotionFirstWins");
			const Transform* transform = objects.GetComponent<Transform>(object);
			std::vector<AnimationRootMotionApplicationResult> results;
			AnimationRootMotionResultPublisher::Instance().CopyForAnimator({17U, 3U}, results);
			const auto find = [&results](std::uint64_t correlation) {
				return std::find_if(results.begin(), results.end(), [correlation](const auto& result) {
					return result.correlationId == correlation;
				});
			};
			const auto firstResult = find(401U);
			const auto secondResult = find(402U);
			context.Expect(first.status == AnimationStatus::Queued && second.status == AnimationStatus::Queued
				&& transform != nullptr && IsClose(transform->position.x, 2.0F)
				&& firstResult != results.end() && firstResult->disposition == AnimationRootMotionDisposition::Applied
				&& secondResult != results.end() && secondResult->disposition == AnimationRootMotionDisposition::Rejected,
				"Generation conflicts must preserve first-writer ordering, reject the stale batch, and never leak a partial second motion.");
			objects.clear();
		});

		registry.Add("Animation.RootMotion.PhysicsAcknowledgementsReportAdjustedAuthoritativeMotion", [](TestContext& context) {
			AnimationRootMotionResultPublisher::Instance().Clear();
			AnimationRootMotionApplicationResult pending;
			pending.animator = {23U, 4U};
			pending.generation = 8U;
			pending.sourceBeginTick = 20;
			pending.sourceEndTick = 21;
			pending.requestedTranslation = {2.0F, 0.0F, 0.0F};
			pending.correlationId = 501U;
			TerraEngine::UUID owner{};
			owner[0] = 42U;
			context.Expect(AnimationRootMotionResultPublisher::Instance().RegisterPhysicsRequest(
				77U, pending, owner, 1U, 1U, {0.0F, 0.0F, 0.0F}, {2.0F, 0.0F, 0.0F},
				{}, {}, true),
				"The acknowledgement fixture must register one copied pending physics command.");
			Physics::PhysicsCommandResultSnapshot commands;
			commands.results = std::make_shared<const std::vector<Physics::PhysicsCommandResult>>(
				std::vector<Physics::PhysicsCommandResult>{{77U, Physics::PhysicsResultStatus::Success, 21U, {}}});
			auto states = std::make_shared<std::vector<Physics::PhysicsCharacterState>>();
			Physics::PhysicsCharacterState state;
			state.objectId = owner;
			state.objectGeneration = 1U;
			state.componentGeneration = 1U;
			state.pose.position = {1.0F, 0.0F, 0.0F};
			states->push_back(state);
			auto characters = std::make_shared<Physics::PhysicsCharacterStatePacket>();
			characters->states = states;
			AnimationRootMotionResultPublisher::Instance().ObservePhysicsResults(commands, {}, characters);
			std::vector<AnimationRootMotionApplicationResult> results;
			AnimationRootMotionResultPublisher::Instance().CopyForAnimator({23U, 4U}, results);
			context.Expect(results.size() == 1U && results.front().status == AnimationStatus::Ok
				&& results.front().disposition == AnimationRootMotionDisposition::Adjusted
				&& IsClose(results.front().appliedTranslation.x, 1.0F),
				"Physics acknowledgements must expose the adjusted authoritative displacement rather than echoing a requested target.");
		});

		registry.Add("Animation.RootMotion.AuthoredObjectPolicyPublishesAndCommitsBeforePhysics", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Shutdown();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			TerraEngine::Scene::SceneMutationQueue::Instance().Clear();
			system.Initialize();
			const Object object = objects.createObject("Automatic Root Motion Target");
			if(objects.GetComponent<Transform>(object) == nullptr) objects.AddComponent<Transform>(object);
			const auto skeleton = MakeRootMotionSkeleton();
			const auto clip = MakeRootMotionClip(context, *skeleton);
			AnimatorInstanceDescriptor descriptor;
			descriptor.ownerObjectId = objects.getUUID(object);
			descriptor.componentGeneration = TerraEngine::CompressObjectGeneration(
				objects.getObjectComponentGeneration(object));
			const AnimatorInstanceHandle handle = system.CreateAnimatorInstance(descriptor);
			AnimatorPlaybackBinding binding;
			binding.clip = clip;
			binding.skeleton = skeleton;
			binding.stateId = 44U;
			binding.rootMotionPolicy = AnimatorRootMotionPolicy::ApplyToObject;
			binding.requestedExecutionPolicy = AnimationExecutionPolicy::ForceCpuDeterministicFallback;
			binding.playback.timePolicy = AnimatorTimePolicy::Fixed;
			binding.playback.fixedStepTicks = 1;
			binding.playback.loop = true;
			context.Expect(handle.IsValid()
				&& system.BindAnimatorPlayback(handle, binding) == AnimationStatus::Ok,
				"The automatic-policy fixture must bind one generation-safe CPU-authoritative animator.");
			AnimationCommand play;
			play.target = handle;
			play.kind = AnimationCommandKind::Play;
			play.producerId = 90U;
			play.producerSequence = 1U;
			play.correlationId = 1U;
			play.idempotencyKey = 1U;
			context.Expect(system.SubmitCommand(play).status == AnimationStatus::Queued
				&& ExecuteRootMotionFrame(system, "automatic-root-motion-policy"),
				"The Play command and all four animation workloads must complete in one scheduler frame.");

			const Transform* transform = objects.GetComponent<Transform>(object);
			const PinnedAnimationAuthorityRow published =
				PinnedAnimationAuthorityRow::ForAnimator(system, handle);
			std::vector<AnimationRootMotionApplicationResult> results;
			AnimationRootMotionResultPublisher::Instance().CopyForAnimator(handle, results);
			const auto automatic = std::find_if(results.begin(), results.end(), [](const auto& result) {
				return result.applicationMode == AnimationRootMotionApplicationMode::ApplyToTransform;
			});
			const NativeAnimationCapabilityResult rootCapability = QueryNativeCapability(
				handle, NativeAnimationCapability::RootMotion);
			const NativeAnimationCapabilityResult boneCapability = QueryNativeCapability(
				handle, NativeAnimationCapability::BoneRead, 1U);
			const NativeAnimationCapabilityResult socketCapability = QueryNativeCapability(
				handle, NativeAnimationCapability::SocketRead, 5U);
			context.Expect(transform != nullptr && IsClose(transform->position.x, 1.0F)
				&& TerraEngine::Scene::SceneMutationQueue::Instance().PendingTransactions().empty()
				&& published && published->snapshot && published->snapshot->pose
				&& published->snapshot->pose->RootMotionDelta().has_value()
				&& automatic != results.end()
				&& automatic->status == AnimationStatus::Ok
				&& automatic->disposition == AnimationRootMotionDisposition::Applied
				&& automatic->authoredPolicy == AnimatorRootMotionPolicy::ApplyToObject
				&& automatic->sourceStateId == 44U
				&& automatic->objectGeneration == published->objectGeneration
				&& automatic->componentGeneration == published->componentGeneration
				&& automatic->publicationGeneration == published->authorityPublicationGeneration
				&& IsClose(automatic->localTranslation.x, 1.0F)
				&& IsClose(automatic->requestedTranslation.x, 1.0F)
				&& rootCapability.status == NativeAnimationStatus::Ok && rootCapability.supported == 1U
				&& boneCapability.status == NativeAnimationStatus::Ok && boneCapability.supported == 1U
				&& socketCapability.status == NativeAnimationStatus::Ok && socketCapability.supported == 1U,
				"ApplyToObject must force CPU root authority, commit before Physics, publish exact metadata, and report only genuinely ready managed capabilities.");

			std::array<NativeAnimationRootMotionResultDetails, 4U> details{};
			NativeAnimationBufferInfo info{};
			info.header = {
				static_cast<std::uint32_t>(sizeof(info)),
				kNativeAnimationAbiVersion,
				static_cast<std::uint16_t>(NativeAnimationRecordKind::BufferInfo)};
			const NativeAnimationAnimatorHandle nativeHandle{handle.slotIndex, handle.generation};
			const NativeAnimationStatus detailStatus = Native_Animation_CopyRootMotionResultDetails(
				&nativeHandle, details.data(), static_cast<std::uint32_t>(details.size()), &info);
			const auto nativeApplied = std::find_if(details.begin(), details.begin() + info.writtenCount,
				[](const NativeAnimationRootMotionResultDetails& value) {
					return value.applicationMode == NativeAnimationRootMotionApplicationMode::ApplyToTransform;
				});
			context.Expect(detailStatus == NativeAnimationStatus::Ok
				&& nativeApplied != details.begin() + info.writtenCount
				&& nativeApplied->header.size == sizeof(NativeAnimationRootMotionResultDetails)
				&& nativeApplied->authoredPolicy == static_cast<std::uint32_t>(AnimatorRootMotionPolicy::ApplyToObject)
				&& nativeApplied->sourceStateId == 44U
				&& IsClose(nativeApplied->localTranslation.x, 1.0F),
				"The additive details copy must preserve the exact native root result used by the managed gameplay surface.");
			(void)system.InvalidateAnimatorInstance(handle);
			system.Shutdown();
			TerraEngine::Scene::SceneMutationQueue::Instance().Clear();
			objects.clear();
			// Engine test suites share this singleton. Restore the normal active-empty
			// state so later stale-handle ABI tests observe StaleHandle, not Cancelled.
			system.Initialize();
		});

		registry.Add("Animation.RootMotion.WorkerSubmissionStaysCopiedDeferredAndOnceOnly", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Shutdown();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			TerraEngine::Scene::SceneMutationQueue::Instance().Clear();
			system.Initialize();
			const Object object = objects.createObject("Worker Root Motion Target");
			if(objects.GetComponent<Transform>(object) == nullptr) objects.AddComponent<Transform>(object);
			const auto skeleton = MakeRootMotionSkeleton();
			const auto clip = MakeRootMotionClip(context, *skeleton);
			AnimatorInstanceDescriptor descriptor;
			descriptor.ownerObjectId = objects.getUUID(object);
			descriptor.componentGeneration = TerraEngine::CompressObjectGeneration(
				objects.getObjectComponentGeneration(object));
			const AnimatorInstanceHandle handle = system.CreateAnimatorInstance(descriptor);
			AnimatorPlaybackBinding binding;
			binding.clip = clip;
			binding.skeleton = skeleton;
			binding.stateId = 55U;
			binding.rootMotionPolicy = AnimatorRootMotionPolicy::ExtractOnly;
			binding.requestedExecutionPolicy = AnimationExecutionPolicy::ForceCpuDeterministicFallback;
			binding.playback.timePolicy = AnimatorTimePolicy::Fixed;
			binding.playback.fixedStepTicks = 1;
			binding.playback.loop = true;
			context.Expect(handle.IsValid()
				&& system.BindAnimatorPlayback(handle, binding) == AnimationStatus::Ok,
				"The worker fixture must bind one copied root-motion publication.");
			AnimationCommand play;
			play.target = handle;
			play.kind = AnimationCommandKind::Play;
			play.producerId = 91U;
			play.producerSequence = 1U;
			play.correlationId = 1U;
			context.Expect(system.SubmitCommand(play).status == AnimationStatus::Queued
				&& ExecuteRootMotionFrame(system, "worker-root-motion-source"),
				"The source publication must become immutable before the worker submits its request.");
			const PinnedAnimationAuthorityRow published =
				PinnedAnimationAuthorityRow::ForAnimator(system, handle);
			const std::optional<AnimationRootMotionDelta> delta = published && published->snapshot
				&& published->snapshot->pose
				? published->snapshot->pose->RootMotionDelta()
				: std::optional<AnimationRootMotionDelta>{};
			AnimationRootMotionDeferredApplicationRequest request;
			request.animator = handle;
			request.mode = AnimationRootMotionApplicationMode::ApplyToTransform;
			request.expectedPoseGeneration = delta ? delta->generation : 0U;
			request.expectedPublicationGeneration = published
				? published->authorityPublicationGeneration : 0U;
			request.producerId = 0xC0DEU;
			request.producerSequence = 1U;
			request.correlationId = 600U;
			request.idempotencyKey = 600U;
			AnimationRootMotionApplicationReceipt workerReceipt;
			std::thread producer([&system, &request, &workerReceipt]() {
				// This call may run on a gameplay worker because it resolves only the
				// immutable publication and appends one bounded copied request.
				workerReceipt = system.SubmitRootMotionApplication(request);
			});
			producer.join();
			const AnimationRootMotionApplicationReceipt retry =
				system.SubmitRootMotionApplication(request);
			AnimationRootMotionDeferredApplicationRequest changed = request;
			changed.fixedDurationSeconds = 1.0F / 30.0F;
			const AnimationRootMotionApplicationReceipt conflict =
				system.SubmitRootMotionApplication(changed);
			const Transform* before = objects.GetComponent<Transform>(object);
			std::vector<AnimationRootMotionApplicationResult> deferredResults;
			AnimationRootMotionResultPublisher::Instance().CopyForAnimator(handle, deferredResults);
			const AnimationRootMotionApplicationResult* deferred = FindResult(deferredResults, 600U, false);
			const AnimationRootMotionApplicationResult* rejected = FindResult(deferredResults, 600U, true);
			context.Expect(delta.has_value()
				&& workerReceipt.status == AnimationStatus::Queued
				&& retry.status == AnimationStatus::Queued && retry.duplicate
				&& conflict.status == AnimationStatus::Rejected && conflict.idempotencyConflict
				&& before != nullptr && IsClose(before->position.x, 0.0F)
				&& TerraEngine::Scene::SceneMutationQueue::Instance().PendingTransactions().empty()
				&& deferred != nullptr && deferred->disposition == AnimationRootMotionDisposition::Deferred
				&& deferred->duplicate
				&& rejected != nullptr && rejected->disposition == AnimationRootMotionDisposition::Rejected,
				"Worker submission must remain invisible, expose its deferred state, reconcile exact retries, and reject a changed same-key payload atomically.");
			context.Expect(ExecuteRootMotionFrame(system, "worker-root-motion-drain"),
				"The next protected animation publication must drain the copied request before authored policy routing.");
			const Transform* after = objects.GetComponent<Transform>(object);
			std::vector<AnimationRootMotionApplicationResult> terminalResults;
			AnimationRootMotionResultPublisher::Instance().CopyForAnimator(handle, terminalResults);
			const AnimationRootMotionApplicationResult* applied = FindResult(terminalResults, 600U, false);
			context.Expect(after != nullptr && IsClose(after->position.x, 1.0F)
				&& applied != nullptr && applied->status == AnimationStatus::Ok
				&& applied->disposition == AnimationRootMotionDisposition::Applied
				&& applied->duplicate
				&& applied->producerId == request.producerId
				&& applied->producerSequence == request.producerSequence
				&& applied->idempotencyKey == request.idempotencyKey
				&& applied->publicationGeneration == request.expectedPublicationGeneration,
				"The protected drain must apply exactly the worker-copied source publication and carry its retry identity into the terminal result.");
			(void)system.InvalidateAnimatorInstance(handle);
			std::vector<AnimationRootMotionApplicationResult> retired;
			AnimationRootMotionResultPublisher::Instance().CopyForAnimator(handle, retired);
			context.Expect(retired.empty()
				&& system.SubmitRootMotionApplication(request).status == AnimationStatus::StaleHandle,
				"Animator invalidation must retire pending/results and reject the old worker handle before slot reuse.");
			system.Shutdown();
			TerraEngine::Scene::SceneMutationQueue::Instance().Clear();
			objects.clear();
			system.Initialize();
		});

		registry.Add("Animation.RootMotion.LegacyNativeCopyRetainsItsNinetySixByteStride", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			AnimatorInstanceDescriptor descriptor;
			descriptor.ownerObjectId[0] = 77U;
			descriptor.componentGeneration = 1U;
			const AnimatorInstanceHandle handle = system.CreateAnimatorInstance(descriptor);
			AnimationRootMotionResultPublisher::Instance().Clear();
			const NativeAnimationCapabilityResult boneBeforePose = QueryNativeCapability(
				handle, NativeAnimationCapability::BoneRead, 1U);
			const NativeAnimationCapabilityResult socketBeforePose = QueryNativeCapability(
				handle, NativeAnimationCapability::SocketRead, 5U);
			const NativeAnimationCapabilityResult rootBeforePose = QueryNativeCapability(
				handle, NativeAnimationCapability::RootMotion);
			context.Expect(boneBeforePose.status == NativeAnimationStatus::NotReady
				&& boneBeforePose.supported == 0U
				&& socketBeforePose.status == NativeAnimationStatus::NotReady
				&& socketBeforePose.supported == 0U
				&& rootBeforePose.status == NativeAnimationStatus::NotReady
				&& rootBeforePose.supported == 0U,
				"A live handle without a current CPU pose, attachment snapshot, or root delta must not advertise gameplay read support.");
			for(std::uint64_t index = 0U; index < 2U; ++index) {
				AnimationRootMotionApplicationResult result;
				result.animator = handle;
				result.status = AnimationStatus::Ok;
				result.disposition = AnimationRootMotionDisposition::Extracted;
				result.generation = 3U + static_cast<AnimationGeneration>(index);
				result.sourceBeginTick = static_cast<AnimationFixedTick>(index);
				result.sourceEndTick = static_cast<AnimationFixedTick>(index + 1U);
				result.correlationId = 700U + index;
				result.publicationGeneration = 9U;
				AnimationRootMotionResultPublisher::Instance().PublishImmediate(std::move(result));
			}

			struct LegacyCopyBuffer {
				std::array<NativeAnimationRootMotionResult, 2U> values{};
				std::array<std::uint8_t, 32U> canary{};
			};
			LegacyCopyBuffer output;
			output.canary.fill(0xA5U);
			NativeAnimationBufferInfo info{};
			info.header = {
				static_cast<std::uint32_t>(sizeof(info)),
				kNativeAnimationAbiVersion,
				static_cast<std::uint16_t>(NativeAnimationRecordKind::BufferInfo)};
			const NativeAnimationAnimatorHandle nativeHandle{handle.slotIndex, handle.generation};
			const NativeAnimationStatus status = Native_Animation_CopyRootMotionResults(
				&nativeHandle, output.values.data(), static_cast<std::uint32_t>(output.values.size()), &info);
			const bool canaryIntact = std::all_of(output.canary.begin(), output.canary.end(),
				[](std::uint8_t value) { return value == 0xA5U; });
			context.Expect(status == NativeAnimationStatus::Ok
				&& info.requiredCount == 2U && info.writtenCount == 2U
				&& output.values[0].header.size == sizeof(NativeAnimationRootMotionResult)
				&& output.values[0].correlationId == 700U
				&& output.values[1].correlationId == 701U
				&& canaryIntact,
				"The additive details API must not make the legacy no-stride copy function write beyond two exact 96-byte records.");
			(void)system.InvalidateAnimatorInstance(handle);
			system.Shutdown();
			ObjectManager::instance().clear();
			TerraEngine::Scene::SceneMutationQueue::Instance().Clear();
			system.Initialize();
		});
	}
}
