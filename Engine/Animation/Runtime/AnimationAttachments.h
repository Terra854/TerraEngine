// AnimationAttachments.h - copied bone/socket attachment snapshots and deferred writes.
//
// Attachments are consumers of an already-published pose.  They never borrow a
// PoseBuffer span or write Transform directly: the capture call copies the
// requested skeleton-space transforms, and the enqueue call emits one typed
// transaction that becomes visible only at the scene flush boundary.

#pragma once

#include "Animation/Assets/SkeletonAsset.h"
#include "Animation/Runtime/PoseBuffer.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

class ObjectManager;

namespace TerraEngine::Scene {
	class SceneMutationQueue;
}

namespace TerraEngine::Animation {
	// A copied record is keyed by the durable joint/socket identity rather than a
	// source-order index, so source reordering cannot attach gameplay objects to
	// the wrong bone after an asset reload.
	struct AnimationAttachmentTransformSnapshot {
		std::uint64_t stableId = 0U;
		// Sockets retain their durable parent identity so managed readers can
		// relate a socket to the same bone identity returned by bone reads.
		// Joint records use zero because they are their own parent-facing record.
		std::uint64_t parentBoneStableId = 0U;
		AnimationTransform modelTransform{};

		auto operator<=>(const AnimationAttachmentTransformSnapshot&) const = default;
	};

	struct AnimationBoneSocketSnapshot {
		AnimatorInstanceHandle animator;
		UUID sourceObjectId = EMPTY_UUID;
		std::uint32_t sourceObjectGeneration = 0U;
		std::uint32_t sourceComponentGeneration = 0U;
		AnimationGeneration poseGeneration = 0U;
		// The protected boundary captures this tick with the pose so socket reads
		// never pair transforms from one publication with another tick's metadata.
		AnimationFixedTick sourceTick = 0;
		std::uint64_t publicationGeneration = 0U;
		AnimationAssetHandle skeleton;
		// Pose transforms deliberately remain animator model-space. The copied
		// owner world transform is retained separately for deferred attachment
		// application at the later scene-mutation boundary.
		AnimationTransform sourceObjectWorldTransform{};
		std::vector<AnimationAttachmentTransformSnapshot> joints;
		std::vector<AnimationAttachmentTransformSnapshot> sockets;
	};

	// This is the one coherent read unit for an animator's copied socket data.
	// A consumer receives the tick, generation, skeleton identity, and all socket
	// transforms from a single publisher lock acquisition.
	struct AnimationSocketReadSnapshot {
		AnimatorInstanceHandle animator;
		AnimationGeneration poseGeneration = 0U;
		AnimationFixedTick sourceTick = 0;
		std::uint64_t publicationGeneration = 0U;
		AnimationAssetHandle skeleton;
		std::vector<AnimationAttachmentTransformSnapshot> sockets;
	};

	struct AnimationBoneSocketSnapshotRequest {
		AnimatorInstanceHandle animator;
		UUID sourceObjectId = EMPTY_UUID;
		std::uint32_t sourceObjectGeneration = 0U;
		std::uint32_t sourceComponentGeneration = 0U;
		AnimationFixedTick sourceTick = 0;
		std::uint64_t publicationGeneration = 0U;
		// This is a copied world transform captured by the protected animation
		// coordinator. Worker consumers must not look up the live source object.
		AnimationTransform sourceObjectWorldTransform{};
		std::shared_ptr<const PoseBuffer> pose;
		std::shared_ptr<const SkeletonAsset> skeleton;
	};

	// A binding supplies every generation needed to reject a stale target or a
	// previous animator publication. offset is applied after the bone/socket pose.
	struct AnimationAttachmentBinding {
		std::uint64_t bindingId = 0U;
		UUID targetObjectId = EMPTY_UUID;
		std::uint32_t targetObjectGeneration = 0U;
		std::uint32_t targetComponentGeneration = 0U;
		AnimatorInstanceHandle sourceAnimator;
		AnimationGeneration sourcePoseGeneration = 0U;
		std::uint64_t jointStableId = 0U;
		std::uint64_t socketStableId = 0U;
		AnimationTransform offset{};
	};

	struct AnimationAttachmentEnqueueResult {
		AnimationStatus status = AnimationStatus::InvalidArgument;
		std::string transactionId;
		std::size_t queuedTransformCount = 0U;
	};

	// Script, editor, and gameplay readers only need copied socket records. This
	// small publisher retains the latest immutable snapshot per exact animator
	// generation; it never exposes the live pose buffer that produced the copy.
	class TERRAENGINE_API AnimationAttachmentSnapshotPublisher {
	public:
		static AnimationAttachmentSnapshotPublisher& Instance();
		bool Publish(AnimationBoneSocketSnapshot snapshot);
		bool CopySocketSnapshot(
			AnimatorInstanceHandle animator,
			AnimationSocketReadSnapshot& output) const;
		bool CopySocketSnapshots(
			AnimatorInstanceHandle animator,
			std::vector<AnimationAttachmentTransformSnapshot>& output) const;
		[[nodiscard]] bool HasPublicationRequests() const;
		[[nodiscard]] bool IsPublicationRequested(
			AnimatorInstanceHandle animator) const;
		[[nodiscard]] std::size_t PublishedSocketCount(AnimatorInstanceHandle animator) const;
		void RetireSnapshot(AnimatorInstanceHandle animator);
		void Retire(AnimatorInstanceHandle animator);
		// Crowd lifecycle changes hold the publisher lock once and compact snapshots
		// once. The caller supplies sorted unique handles so membership checks remain
		// allocation-free while calling Retire repeatedly would rescan every snapshot.
		void RetireMany(std::span<const AnimatorInstanceHandle> animators);
		void Clear();

	private:
		struct AnimatorHandleHash {
			[[nodiscard]] std::size_t operator()(
				AnimatorInstanceHandle value) const noexcept;
		};

		mutable std::mutex mutex_;
		std::vector<AnimationBoneSocketSnapshot> snapshots_;
		// A failed coherent read is a subscription for the next protected frame.
		// Keeping requests separate from snapshots preserves the immutable read ABI
		// while ordinary animators avoid copying every bone when nobody consumes it.
		mutable std::unordered_set<AnimatorInstanceHandle, AnimatorHandleHash>
			publicationRequests_;
	};

	// Captures a standalone, immutable snapshot from a sealed CPU pose. A reload
	// cannot invalidate the snapshot because its skeleton and transforms are copied.
	TERRAENGINE_API AnimationStatus CaptureAnimationBoneSocketSnapshot(
		const AnimationBoneSocketSnapshotRequest& request,
		AnimationBoneSocketSnapshot& output) noexcept;

	// Resolves bindings deterministically and queues a single all-or-nothing typed
	// transform transaction. The caller must invoke this from a declared protected
	// preparation boundary; Apply() owns the later visibility boundary and rollback.
	TERRAENGINE_API AnimationAttachmentEnqueueResult EnqueueAnimationAttachmentTransforms(
		const AnimationBoneSocketSnapshot& snapshot,
		std::span<const AnimationAttachmentBinding> bindings,
		ObjectManager& objects,
		TerraEngine::Scene::SceneMutationQueue& sceneMutations);
}
