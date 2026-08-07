// AnimationAttachments.cpp - generation-safe pose copies for gameplay attachments.

#include "Animation/Runtime/AnimationAttachments.h"

#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/TransactionalMutation.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		constexpr std::size_t kMaximumPublishedAttachmentSnapshots = 4096U;

		AnimationTransform CombineTransform(
			const AnimationTransform& parent,
			const AnimationTransform& local) noexcept
		{
			const glm::quat parentRotation(
				parent.rotation.w, parent.rotation.x, parent.rotation.y, parent.rotation.z);
			const glm::vec3 parentScale(parent.scale.x, parent.scale.y, parent.scale.z);
			const glm::vec3 localTranslation(
				local.translation.x, local.translation.y, local.translation.z);
			const glm::vec3 translated = glm::vec3(parent.translation.x, parent.translation.y, parent.translation.z)
				+ parentRotation * (parentScale * localTranslation);
			const glm::quat localRotation(local.rotation.w, local.rotation.x, local.rotation.y, local.rotation.z);
			const glm::quat rotation = glm::normalize(parentRotation * localRotation);
			AnimationTransform result;
			result.translation = {translated.x, translated.y, translated.z};
			result.rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
			result.scale = {
				parent.scale.x * local.scale.x,
				parent.scale.y * local.scale.y,
				parent.scale.z * local.scale.z};
			return result;
		}

		bool HasNormalizedRotation(const AnimationTransform& transform) noexcept
		{
			return IsFinite(transform) && IsNormalized(transform.rotation);
		}

		const AnimationAttachmentTransformSnapshot* FindTransform(
			const std::vector<AnimationAttachmentTransformSnapshot>& transforms,
			std::uint64_t stableId) noexcept
		{
			const auto found = std::lower_bound(transforms.begin(), transforms.end(), stableId,
				[](const AnimationAttachmentTransformSnapshot& value, std::uint64_t id) {
					return value.stableId < id;
				});
			return found != transforms.end() && found->stableId == stableId ? &*found : nullptr;
		}

		bool SameSource(const AnimationBoneSocketSnapshot& snapshot,
			const AnimationAttachmentBinding& binding) noexcept
		{
			return binding.sourceAnimator == snapshot.animator
				&& binding.sourcePoseGeneration == snapshot.poseGeneration;
		}
	}

	AnimationAttachmentSnapshotPublisher& AnimationAttachmentSnapshotPublisher::Instance()
	{
		static AnimationAttachmentSnapshotPublisher publisher;
		return publisher;
	}

	std::size_t AnimationAttachmentSnapshotPublisher::AnimatorHandleHash::operator()(
		AnimatorInstanceHandle value) const noexcept
	{
		return (static_cast<std::size_t>(value.slotIndex) << 32U)
			^ static_cast<std::size_t>(value.generation);
	}

	bool AnimationAttachmentSnapshotPublisher::Publish(AnimationBoneSocketSnapshot snapshot)
	{
		if(!snapshot.animator.IsValid() || snapshot.poseGeneration == 0U
			|| snapshot.skeleton.generation == 0U) return false;
		try {
			std::scoped_lock lock(mutex_);
			const auto existing = std::find_if(snapshots_.begin(), snapshots_.end(), [&snapshot](const auto& value) {
				return value.animator == snapshot.animator;
			});
			if(existing != snapshots_.end()) {
				*existing = std::move(snapshot);
				return true;
			}
			if(snapshots_.size() >= kMaximumPublishedAttachmentSnapshots) snapshots_.erase(snapshots_.begin());
			snapshots_.push_back(std::move(snapshot));
			return true;
		} catch(const std::bad_alloc&) {
			return false;
		}
	}

	bool AnimationAttachmentSnapshotPublisher::CopySocketSnapshot(
		AnimatorInstanceHandle animator,
		AnimationSocketReadSnapshot& output) const
	{
		try {
			AnimationSocketReadSnapshot staged;
			std::scoped_lock lock(mutex_);
			publicationRequests_.insert(animator);
			const auto snapshot = std::find_if(snapshots_.begin(), snapshots_.end(), [animator](const auto& value) {
				return value.animator == animator;
			});
			if(snapshot == snapshots_.end()) return false;
			staged.animator = snapshot->animator;
			staged.poseGeneration = snapshot->poseGeneration;
			staged.sourceTick = snapshot->sourceTick;
			staged.publicationGeneration = snapshot->publicationGeneration;
			staged.skeleton = snapshot->skeleton;
			staged.sockets = snapshot->sockets;
			output = std::move(staged);
			return true;
		} catch(const std::bad_alloc&) {
			// A caller either receives one complete copied publication or retains its
			// prior snapshot; partially copied managed ABI output is never observable.
			return false;
		}
	}

	bool AnimationAttachmentSnapshotPublisher::CopySocketSnapshots(
		AnimatorInstanceHandle animator,
		std::vector<AnimationAttachmentTransformSnapshot>& output) const
	{
		AnimationSocketReadSnapshot snapshot;
		if(!CopySocketSnapshot(animator, snapshot)) return false;
		output = std::move(snapshot.sockets);
		return true;
	}

	bool AnimationAttachmentSnapshotPublisher::HasPublicationRequests() const
	{
		std::scoped_lock lock(mutex_);
		return !publicationRequests_.empty();
	}

	bool AnimationAttachmentSnapshotPublisher::IsPublicationRequested(
		AnimatorInstanceHandle animator) const
	{
		std::scoped_lock lock(mutex_);
		return publicationRequests_.contains(animator);
	}

	std::size_t AnimationAttachmentSnapshotPublisher::PublishedSocketCount(
		AnimatorInstanceHandle animator) const
	{
		std::scoped_lock lock(mutex_);
		publicationRequests_.insert(animator);
		const auto snapshot = std::find_if(snapshots_.begin(), snapshots_.end(), [animator](const auto& value) {
			return value.animator == animator;
		});
		return snapshot == snapshots_.end() ? 0U : snapshot->sockets.size();
	}

	void AnimationAttachmentSnapshotPublisher::RetireSnapshot(
		AnimatorInstanceHandle animator)
	{
		std::scoped_lock lock(mutex_);
		const auto snapshot = std::remove_if(snapshots_.begin(), snapshots_.end(), [animator](const auto& value) {
			return value.animator == animator;
		});
		snapshots_.erase(snapshot, snapshots_.end());
	}

	void AnimationAttachmentSnapshotPublisher::Retire(AnimatorInstanceHandle animator)
	{
		std::scoped_lock lock(mutex_);
		publicationRequests_.erase(animator);
		const auto snapshot = std::remove_if(snapshots_.begin(), snapshots_.end(), [animator](const auto& value) {
			return value.animator == animator;
		});
		snapshots_.erase(snapshot, snapshots_.end());
	}

	void AnimationAttachmentSnapshotPublisher::RetireMany(
		std::span<const AnimatorInstanceHandle> animators)
	{
		if(animators.empty()) return;
		std::scoped_lock lock(mutex_);
		for(const AnimatorInstanceHandle animator : animators) {
			publicationRequests_.erase(animator);
		}
		std::erase_if(snapshots_, [animators](const AnimationBoneSocketSnapshot& value) {
			return std::binary_search(
				animators.begin(), animators.end(), value.animator);
		});
	}

	void AnimationAttachmentSnapshotPublisher::Clear()
	{
		std::scoped_lock lock(mutex_);
		snapshots_.clear();
		publicationRequests_.clear();
	}

	AnimationStatus CaptureAnimationBoneSocketSnapshot(
		const AnimationBoneSocketSnapshotRequest& request,
		AnimationBoneSocketSnapshot& output) noexcept
	{
		if(!request.animator.IsValid() || request.sourceObjectId == EMPTY_UUID
			|| request.sourceObjectGeneration == 0U || request.sourceComponentGeneration == 0U
			|| !request.pose || !request.pose->IsSealed() || !request.skeleton
			|| !HasNormalizedRotation(request.sourceObjectWorldTransform)) {
			return AnimationStatus::InvalidArgument;
		}
		const AnimationPoseGeneration& poseGeneration = request.pose->Generation();
		if(poseGeneration.animator != request.animator || poseGeneration.poseGeneration == 0U
			|| request.sourceTick != poseGeneration.tick
			|| request.publicationGeneration == 0U
			|| poseGeneration.skeleton.assetId != request.skeleton->assetId
			|| poseGeneration.skeleton.generation == 0U
			|| request.pose->ModelTransforms().size() != request.skeleton->joints.size()) {
			return AnimationStatus::StaleHandle;
		}

		try {
			AnimationBoneSocketSnapshot staged;
			staged.animator = request.animator;
			staged.sourceObjectId = request.sourceObjectId;
			staged.sourceObjectGeneration = request.sourceObjectGeneration;
			staged.sourceComponentGeneration = request.sourceComponentGeneration;
			staged.poseGeneration = poseGeneration.poseGeneration;
			staged.sourceTick = request.sourceTick;
			staged.publicationGeneration = request.publicationGeneration;
			staged.skeleton = poseGeneration.skeleton;
			staged.sourceObjectWorldTransform = request.sourceObjectWorldTransform;
			staged.joints.reserve(request.skeleton->joints.size());
			for(std::size_t index = 0U; index < request.skeleton->joints.size(); ++index) {
				const SkeletonJoint& joint = request.skeleton->joints[index];
				const AnimationTransform model = request.pose->ModelTransforms()[index];
				if(joint.stableId == 0U || !HasNormalizedRotation(model)) return AnimationStatus::InvalidArgument;
				staged.joints.push_back({joint.stableId, 0U, model});
			}
			std::sort(staged.joints.begin(), staged.joints.end(), [](const auto& left, const auto& right) {
				return left.stableId < right.stableId;
			});
			for(std::size_t index = 1U; index < staged.joints.size(); ++index) {
				if(staged.joints[index - 1U].stableId == staged.joints[index].stableId) return AnimationStatus::InvalidArgument;
			}

			staged.sockets.reserve(request.skeleton->sockets.size());
			for(const SkeletonSocket& socket : request.skeleton->sockets) {
				const AnimationAttachmentTransformSnapshot* joint = FindTransform(staged.joints, socket.jointStableId);
				const AnimationTransform socketModel = joint == nullptr
					? AnimationTransform{} : CombineTransform(joint->modelTransform, socket.localTransform);
				if(socket.stableId == 0U || joint == nullptr || !HasNormalizedRotation(socket.localTransform)
					|| !HasNormalizedRotation(socketModel)) return AnimationStatus::InvalidArgument;
				staged.sockets.push_back({socket.stableId, socket.jointStableId, socketModel});
			}
			std::sort(staged.sockets.begin(), staged.sockets.end(), [](const auto& left, const auto& right) {
				return left.stableId < right.stableId;
			});
			for(std::size_t index = 1U; index < staged.sockets.size(); ++index) {
				if(staged.sockets[index - 1U].stableId == staged.sockets[index].stableId) return AnimationStatus::InvalidArgument;
			}
			output = std::move(staged);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationAttachmentEnqueueResult EnqueueAnimationAttachmentTransforms(
		const AnimationBoneSocketSnapshot& snapshot,
		std::span<const AnimationAttachmentBinding> bindings,
		ObjectManager& objects,
		TerraEngine::Scene::SceneMutationQueue& sceneMutations)
	{
		AnimationAttachmentEnqueueResult result;
		if(!snapshot.animator.IsValid() || snapshot.sourceObjectId == EMPTY_UUID
			|| snapshot.sourceObjectGeneration == 0U || snapshot.sourceComponentGeneration == 0U
			|| snapshot.poseGeneration == 0U || snapshot.publicationGeneration == 0U
			|| snapshot.skeleton.generation == 0U
			|| !HasNormalizedRotation(snapshot.sourceObjectWorldTransform) || bindings.empty()) {
			return result;
		}

		try {
			std::vector<TerraEngine::Scene::TransactionTransformUpdate> updates;
			updates.reserve(bindings.size());
			std::set<UUID> targets;
			std::uint64_t minimumBindingId = std::numeric_limits<std::uint64_t>::max();
			for(const AnimationAttachmentBinding& binding : bindings) {
				if(binding.bindingId == 0U || binding.targetObjectId == EMPTY_UUID
					|| binding.targetObjectGeneration == 0U || binding.targetComponentGeneration == 0U
					|| !SameSource(snapshot, binding) || !HasNormalizedRotation(binding.offset)
					|| (binding.jointStableId == 0U && binding.socketStableId == 0U)
					|| (binding.jointStableId != 0U && binding.socketStableId != 0U)
					|| !targets.insert(binding.targetObjectId).second) {
					return result;
				}
				const AnimationAttachmentTransformSnapshot* source = binding.socketStableId != 0U
					? FindTransform(snapshot.sockets, binding.socketStableId)
					: FindTransform(snapshot.joints, binding.jointStableId);
				const std::optional<Object> target = objects.getObject(binding.targetObjectId);
				if(source == nullptr || !target
					|| TerraEngine::CompressObjectGeneration(objects.getObjectLifetimeGeneration(*target))
						!= binding.targetObjectGeneration
					|| TerraEngine::CompressObjectGeneration(objects.getObjectComponentGeneration(*target))
						!= binding.targetComponentGeneration
					|| objects.GetComponent<Transform>(*target) == nullptr) {
					result.status = AnimationStatus::StaleHandle;
					return result;
				}
				// Socket/joint records are animator model-space for gameplay parity.
				// Convert only while creating the deferred world-space object update.
				const AnimationTransform resolved = CombineTransform(snapshot.sourceObjectWorldTransform,
					CombineTransform(source->modelTransform, binding.offset));
				if(!HasNormalizedRotation(resolved)) return result;
				updates.push_back({binding.targetObjectId,
					{resolved.translation.x, resolved.translation.y, resolved.translation.z},
					{resolved.rotation.w, resolved.rotation.x, resolved.rotation.y, resolved.rotation.z}});
				minimumBindingId = std::min(minimumBindingId, binding.bindingId);
			}
			std::sort(updates.begin(), updates.end(), [](const auto& left, const auto& right) {
				return left.objectId < right.objectId;
			});
			result.transactionId = "animation-attachment-" + std::to_string(snapshot.animator.slotIndex)
				+ "-" + std::to_string(snapshot.animator.generation)
				+ "-" + std::to_string(snapshot.poseGeneration)
				+ "-" + std::to_string(minimumBindingId);
			TerraEngine::Scene::TransactionalMutationBuilder builder(
				result.transactionId, TerraEngine::Scene::TransactionOrigin::System, "AnimationAttachment");
			builder.UpdateTransformBatch(objects, std::move(updates));
			TerraEngine::Scene::Transaction transaction = std::move(builder).Build();
			const TerraEngine::Scene::TransactionResult preflight =
				sceneMutations.PreflightTransaction(transaction, objects);
			if(preflight.status != TerraEngine::Scene::TransactionStatus::PreflightAccepted) {
				result.status = AnimationStatus::Rejected;
				return result;
			}
			sceneMutations.EnqueueTransaction(std::move(transaction));
			result.status = AnimationStatus::Queued;
			result.queuedTransformCount = bindings.size();
			return result;
		} catch(const std::bad_alloc&) {
			result.status = AnimationStatus::InternalError;
			return result;
		}
	}
}
