// AnimationAttachmentTests.cpp - copied socket attachment and deferred mutation contracts.

#include "TestHarness.h"

#include "Animation/Runtime/AnimationAttachments.h"
#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"

#include <array>
#include <cmath>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		bool IsClose(float left, float right) { return std::abs(left - right) <= 0.0001F; }

		std::shared_ptr<const SkeletonAsset> MakeSkeleton() {
			auto skeleton = std::make_shared<SkeletonAsset>();
			skeleton->assetId.value[0] = 9U;
			skeleton->joints.push_back({101U, -1, {}, false, "Root", "root", "root"});
			skeleton->sockets.push_back({202U, 101U, {{1.0F, 0.0F, 0.0F}, {}, {1.0F, 1.0F, 1.0F}}, "Grip", "grip"});
			return skeleton;
		}

		std::shared_ptr<const PoseBuffer> MakePose(const SkeletonAsset& skeleton, float modelX) {
			auto pose = std::make_shared<PoseBuffer>();
			PoseBufferLayout layout;
			layout.jointCount = 1U;
			AnimationPoseGeneration generation;
			generation.animator = {8U, 2U};
			generation.poseGeneration = 5U;
			generation.skeleton = {skeleton.assetId, 3U};
			generation.programGeneration = 1U;
			generation.tick = 42;
			if(pose->BeginWrite(layout, generation) != AnimationStatus::Ok) return {};
			pose->MutableModelTransforms()[0].translation.x = modelX;
			if(pose->Seal() != AnimationStatus::Ok) return {};
			return pose;
		}

		AnimationBoneSocketSnapshot MakeSnapshot(float modelX) {
			const auto skeleton = MakeSkeleton();
			AnimationBoneSocketSnapshot snapshot;
			AnimationBoneSocketSnapshotRequest request;
			request.animator = {8U, 2U};
			request.sourceObjectId[0] = 7U;
			request.sourceObjectGeneration = 1U;
			request.sourceComponentGeneration = 1U;
			request.sourceTick = 42;
			request.publicationGeneration = 7U;
			request.sourceObjectWorldTransform.translation.x = 5.0F;
			request.pose = MakePose(*skeleton, modelX);
			request.skeleton = skeleton;
			(void)CaptureAnimationBoneSocketSnapshot(request, snapshot);
			return snapshot;
		}

		AnimationAttachmentBinding MakeBinding(const Object& object, ObjectManager& objects, std::uint64_t id = 1U) {
			AnimationAttachmentBinding binding;
			binding.bindingId = id;
			binding.targetObjectId = objects.getUUID(object);
			binding.targetObjectGeneration = TerraEngine::CompressObjectGeneration(objects.getObjectLifetimeGeneration(object));
			binding.targetComponentGeneration = TerraEngine::CompressObjectGeneration(objects.getObjectComponentGeneration(object));
			binding.sourceAnimator = {8U, 2U};
			binding.sourcePoseGeneration = 5U;
			binding.socketStableId = 202U;
			binding.offset.translation.x = 3.0F;
			return binding;
		}
	}

	void RegisterAnimationAttachmentTests(TestRegistry& registry) {
		registry.Add("Animation.Attachments.CopySocketHierarchyAndApplyOffsetOnlyAtFlush", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object target = objects.createObject("Attached Tool");
			if(objects.GetComponent<Transform>(target) == nullptr) objects.AddComponent<Transform>(target);
			const AnimationBoneSocketSnapshot snapshot = MakeSnapshot(10.0F);
			TerraEngine::Scene::SceneMutationQueue queue;
			const AnimationAttachmentBinding binding = MakeBinding(target, objects);
			const auto receipt = EnqueueAnimationAttachmentTransforms(snapshot, {&binding, 1U}, objects, queue);
			const Transform* before = objects.GetComponent<Transform>(target);
			const float beforeX = before == nullptr ? -1.0F : static_cast<float>(before->worldPose.position.offsetMetres.x);
			queue.Apply(objects, "AttachmentFlush");
			const Transform* after = objects.GetComponent<Transform>(target);
			context.Expect(receipt.status == AnimationStatus::Queued && before != nullptr && IsClose(beforeX, 0.0F)
				&& after != nullptr && IsClose(static_cast<float>(after->worldPose.position.offsetMetres.x), 19.0F),
				"A copied hierarchy+socket+offset transform must remain invisible until the protected flush.");
			objects.clear();
		});

		registry.Add("Animation.Attachments.SocketReadersReceiveCoherentModelSpaceSnapshots", [](TestContext& context) {
			AnimationAttachmentSnapshotPublisher::Instance().Clear();
			const AnimationBoneSocketSnapshot snapshot = MakeSnapshot(2.0F);
			context.Expect(AnimationAttachmentSnapshotPublisher::Instance().Publish(snapshot),
				"A protected publisher should retain a complete copied socket snapshot.");
			AnimationSocketReadSnapshot copiedSnapshot;
			const bool copied = AnimationAttachmentSnapshotPublisher::Instance().CopySocketSnapshot({8U, 2U}, copiedSnapshot);
			std::vector<AnimationAttachmentTransformSnapshot> sockets;
			const bool copiedCompatibility = AnimationAttachmentSnapshotPublisher::Instance().CopySocketSnapshots({8U, 2U}, sockets);
			AnimationAttachmentSnapshotPublisher::Instance().Retire({8U, 2U});
			context.Expect(copied && copiedCompatibility && copiedSnapshot.animator == AnimatorInstanceHandle{8U, 2U}
				&& copiedSnapshot.poseGeneration == 5U && copiedSnapshot.sourceTick == 42
				&& copiedSnapshot.publicationGeneration == 7U
				&& copiedSnapshot.skeleton.generation == 3U && copiedSnapshot.sockets.size() == 1U
				&& copiedSnapshot.sockets.front().stableId == 202U
				&& copiedSnapshot.sockets.front().parentBoneStableId == 101U
				&& IsClose(copiedSnapshot.sockets.front().modelTransform.translation.x, 3.0F)
				&& sockets == copiedSnapshot.sockets
				&& AnimationAttachmentSnapshotPublisher::Instance().PublishedSocketCount({8U, 2U}) == 0U,
				"Socket readers must receive one copied model-space publication with matching tick, pose generation, and parent bone identity.");
		});

		registry.Add("Animation.Attachments.MissingReadArmsDemandDrivenPublication", [](TestContext& context) {
			AnimationAttachmentSnapshotPublisher& publisher =
				AnimationAttachmentSnapshotPublisher::Instance();
			publisher.Clear();
			const AnimatorInstanceHandle animator{15U, 4U};
			AnimationSocketReadSnapshot missing;
			const bool copied = publisher.CopySocketSnapshot(animator, missing);
			context.Expect(!copied && publisher.HasPublicationRequests()
				&& publisher.IsPublicationRequested(animator),
				"A first missing coherent read must arm only that animator for the next protected publication.");
			publisher.Retire(animator);
			context.Expect(!publisher.HasPublicationRequests(),
				"Retiring the exact animator generation must also retire its publication request.");
		});

		registry.Add("Animation.Attachments.SocketReadersRejectRetiredHandleAfterRebind", [](TestContext& context) {
			AnimationAttachmentSnapshotPublisher::Instance().Clear();
			const AnimationBoneSocketSnapshot oldSnapshot = MakeSnapshot(2.0F);
			context.Expect(AnimationAttachmentSnapshotPublisher::Instance().Publish(oldSnapshot),
				"The first animator generation must be publishable for the stale-read regression.");
			AnimationAttachmentSnapshotPublisher::Instance().Retire({8U, 2U});
			AnimationBoneSocketSnapshot reboundSnapshot = MakeSnapshot(4.0F);
			reboundSnapshot.animator = {8U, 3U};
			reboundSnapshot.poseGeneration = 6U;
			reboundSnapshot.sourceTick = 43;
			reboundSnapshot.publicationGeneration = 8U;
			context.Expect(AnimationAttachmentSnapshotPublisher::Instance().Publish(reboundSnapshot),
				"A replacement animator generation must publish under its new exact handle.");
			AnimationSocketReadSnapshot stale;
			AnimationSocketReadSnapshot current;
			const bool staleCopied = AnimationAttachmentSnapshotPublisher::Instance().CopySocketSnapshot({8U, 2U}, stale);
			const bool currentCopied = AnimationAttachmentSnapshotPublisher::Instance().CopySocketSnapshot({8U, 3U}, current);
			context.Expect(!staleCopied && currentCopied && current.animator == AnimatorInstanceHandle{8U, 3U}
				&& current.poseGeneration == 6U && current.sourceTick == 43 && current.publicationGeneration == 8U
				&& current.sockets.size() == 1U && IsClose(current.sockets.front().modelTransform.translation.x, 5.0F),
				"Retiring a handle before rebind must make old socket reads fail rather than expose a newer generation through the old handle.");
			AnimationAttachmentSnapshotPublisher::Instance().Clear();
		});

		registry.Add("Animation.Attachments.RejectStaleReloadedRemovedAndReusedTargetsAtomically", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object first = objects.createObject("First Attachment");
			const Object second = objects.createObject("Second Attachment");
			if(objects.GetComponent<Transform>(first) == nullptr) objects.AddComponent<Transform>(first);
			if(objects.GetComponent<Transform>(second) == nullptr) objects.AddComponent<Transform>(second);
			const AnimationBoneSocketSnapshot snapshot = MakeSnapshot(0.0F);
			AnimationAttachmentBinding firstBinding = MakeBinding(first, objects, 1U);
			AnimationAttachmentBinding secondBinding = MakeBinding(second, objects, 2U);
			TerraEngine::Scene::SceneMutationQueue queue;
			const AnimationAttachmentBinding staleReload = [&firstBinding] {
				AnimationAttachmentBinding copy = firstBinding;
				copy.sourcePoseGeneration += 1U;
				return copy;
			}();
			const auto staleReceipt = EnqueueAnimationAttachmentTransforms(snapshot, {&staleReload, 1U}, objects, queue);
			const std::array bindings{firstBinding, secondBinding};
			const auto receipt = EnqueueAnimationAttachmentTransforms(snapshot, bindings, objects, queue);
			objects.removeObject(second);
			const Object reused = objects.createObject("Reused Attachment Slot");
			if(objects.GetComponent<Transform>(reused) == nullptr) objects.AddComponent<Transform>(reused);
			const auto result = queue.Apply(objects, "AttachmentStaleFlush");
			const Transform* firstTransform = objects.GetComponent<Transform>(first);
			const Transform* reusedTransform = objects.GetComponent<Transform>(reused);
			context.Expect(staleReceipt.status == AnimationStatus::InvalidArgument
				&& receipt.status == AnimationStatus::Queued && firstTransform != nullptr
				&& IsClose(static_cast<float>(firstTransform->worldPose.position.offsetMetres.x), 0.0F) && reusedTransform != nullptr
				&& IsClose(static_cast<float>(reusedTransform->worldPose.position.offsetMetres.x), 0.0F) && !result.transactionResults.empty()
				&& result.transactionResults.front().status != TerraEngine::Scene::TransactionStatus::Committed,
				"Reload mismatches, removed targets, and reused objects must reject the entire attachment batch without moving an unrelated object.");
			objects.clear();
		});
	}
}
