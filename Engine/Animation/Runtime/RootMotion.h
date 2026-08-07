// RootMotion.h - Pure root-track extraction and worker-local interval accumulation.

#pragma once

#include "Animation/Runtime/PoseBuffer.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

class ObjectManager;

namespace TerraEngine::Physics {
	class PhysicsSystem;
	struct PhysicsCommandResultSnapshot;
	struct IncrementalBodyStateSnapshot;
	struct PhysicsCharacterStatePacket;
}

namespace TerraEngine::Scene {
	class SceneMutationQueue;
	struct TransactionResult;
}

namespace TerraEngine::Animation {
	struct AnimationRootMotionPoint {
		AnimationFloat3 translation{};
		float yawRadians = 0.0F;
		std::int64_t loopIndex = 0;

		auto operator<=>(const AnimationRootMotionPoint&) const = default;
	};

	struct AnimationRootMotionExtractionRequest {
		AnimatorInstanceHandle animator;
		AnimationFixedTick sourceBeginTick = 0;
		AnimationFixedTick sourceEndTick = 0;
		AnimationRootMotionPoint begin;
		AnimationRootMotionPoint end;
		AnimationFloat3 accumulatedCycleTranslation{};
		float accumulatedCycleYawRadians = 0.0F;
		std::uint64_t sourceStateId = 0U;
		std::uint32_t layerIndex = 0U;
		AnimationGeneration generation = 0U;
		AnimatorRootMotionPolicy policy = AnimatorRootMotionPolicy::ExtractOnly;
	};

	// This function is intentionally pure. It creates a copied delta but never
	// applies it to ObjectManager, a character controller, or the physics world.
	TERRAENGINE_API AnimationStatus ExtractRootMotion(
		const AnimationRootMotionExtractionRequest& request,
		AnimationRootMotionDelta& output) noexcept;

	class TERRAENGINE_API AnimationRootMotionAccumulator {
	public:
		AnimationStatus Accumulate(const AnimationRootMotionDelta& delta) noexcept;
		[[nodiscard]] const std::optional<AnimationRootMotionDelta>& Peek() const noexcept { return accumulated_; }
		std::optional<AnimationRootMotionDelta> Consume() noexcept;
		void Reset() noexcept { accumulated_.reset(); }

	private:
		std::optional<AnimationRootMotionDelta> accumulated_;
	};

	// Runtime modes are separate from authored policy because a deterministic
	// network producer and a physics-body consumer are resolved capabilities, not
	// additional durable component values.
	enum class AnimationRootMotionApplicationMode : std::uint8_t {
		Ignore = 0U,
		ExtractOnly = 1U,
		ApplyToTransform = 2U,
		ApplyToCharacter = 3U,
		ApplyToPhysicsBody = 4U,
		DeterministicNetwork = 5U
	};

	enum class AnimationRootMotionDisposition : std::uint32_t {
		Ignored = 0U,
		Extracted = 1U,
		Applied = 2U,
		Adjusted = 3U,
		Rejected = 4U,
		Deferred = 5U
	};

	// Scene requests normally wait for SceneMutationQueue's declared flush. The
	// animation authority publisher may instead commit an already-preflighted
	// transform transaction at its own protected boundary so Physics observes the
	// authored automatic root motion in the same frame.
	enum class AnimationRootMotionSceneVisibility : std::uint8_t {
		DeferredQueue = 0U,
		ImmediateProtectedCommit = 1U
	};

	struct AnimationRootMotionApplicationRequest {
		AnimationRootMotionDelta delta;
		UUID ownerObjectId = EMPTY_UUID;
		std::uint32_t objectGeneration = 0U;
		std::uint32_t componentGeneration = 0U;
		// This transform is copied with the pose at capture. It converts animator-
		// local motion to world space without consulting worker-visible live state.
		AnimationTransform sourceObjectWorldTransform{};
		bool sourceObjectWorldTransformCaptured = false;
		AnimationRootMotionApplicationMode mode = AnimationRootMotionApplicationMode::ExtractOnly;
		std::uint32_t producerId = 0U;
		std::uint64_t producerSequence = 0U;
		std::uint64_t correlationId = 0U;
		std::uint64_t idempotencyKey = 0U;
		std::uint64_t publicationGeneration = 0U;
		float fixedDurationSeconds = 1.0F / 60.0F;
	};

	// Gameplay submits only copied identities. AnimationSystem resolves these
	// identities against the latest immutable publication and never exposes a pose
	// or component reference across the native/managed boundary.
	struct AnimationRootMotionDeferredApplicationRequest {
		AnimatorInstanceHandle animator;
		AnimationRootMotionApplicationMode mode = AnimationRootMotionApplicationMode::ExtractOnly;
		AnimationGeneration expectedPoseGeneration = 0U;
		std::uint64_t expectedPublicationGeneration = 0U;
		std::uint32_t producerId = 0U;
		std::uint64_t producerSequence = 0U;
		std::uint64_t correlationId = 0U;
		std::uint64_t idempotencyKey = 0U;
		float fixedDurationSeconds = 1.0F / 60.0F;
	};

	struct AnimationRootMotionApplicationReceipt {
		AnimationStatus status = AnimationStatus::InvalidArgument;
		AnimatorInstanceHandle animator;
		AnimationRootMotionApplicationMode mode = AnimationRootMotionApplicationMode::ExtractOnly;
		AnimationGeneration generation = 0U;
		std::uint32_t producerId = 0U;
		std::uint64_t producerSequence = 0U;
		std::uint64_t idempotencyKey = 0U;
		std::uint64_t publicationGeneration = 0U;
		AnimationFixedTick observableTick = 0;
		bool duplicate = false;
		bool idempotencyConflict = false;
		std::string sceneTransactionId;
		std::uint64_t physicsCommandId = 0U;
		std::uint64_t correlationId = 0U;
	};

	struct AnimationRootMotionApplicationResult {
		AnimatorInstanceHandle animator;
		AnimationStatus status = AnimationStatus::NotReady;
		AnimationRootMotionDisposition disposition = AnimationRootMotionDisposition::Rejected;
		AnimationGeneration generation = 0U;
		AnimationFixedTick sourceBeginTick = 0;
		AnimationFixedTick sourceEndTick = 0;
		AnimationFloat3 requestedTranslation{};
		AnimationFloat3 appliedTranslation{};
		float requestedYawRadians = 0.0F;
		float appliedYawRadians = 0.0F;
		std::uint64_t correlationId = 0U;
		std::uint64_t publicationGeneration = 0U;
		// Fields below extend the original copied-result prefix. The first fields
		// above retain their v1 meaning for append-only ABI compatibility.
		AnimationFloat3 localTranslation{};
		float localYawRadians = 0.0F;
		AnimationQuaternion requestedRotation{};
		AnimationQuaternion appliedRotation{};
		UUID ownerObjectId = EMPTY_UUID;
		std::uint32_t objectGeneration = 0U;
		std::uint32_t componentGeneration = 0U;
		std::uint64_t sourceStateId = 0U;
		std::int64_t accumulatedCycles = 0;
		std::uint32_t layerIndex = 0U;
		float layerContribution = 1.0F;
		AnimatorRootMotionPolicy authoredPolicy = AnimatorRootMotionPolicy::ExtractOnly;
		AnimationRootMotionApplicationMode applicationMode = AnimationRootMotionApplicationMode::ExtractOnly;
		std::uint32_t producerId = 0U;
		std::uint64_t producerSequence = 0U;
		std::uint64_t idempotencyKey = 0U;
		std::uint64_t physicsCommandId = 0U;
		bool duplicate = false;
		bool idempotencyConflict = false;

		auto operator<=>(const AnimationRootMotionApplicationResult&) const = default;
	};

	// This publisher owns only copied pending/result records. Scene and physics
	// protected boundaries acknowledge their own transaction/command IDs; no
	// worker ever observes or mutates their live storage through this class.
	class TERRAENGINE_API AnimationRootMotionResultPublisher {
	public:
		AnimationRootMotionResultPublisher();
		~AnimationRootMotionResultPublisher();

		static AnimationRootMotionResultPublisher& Instance();
		// Reserve is the once-only gate shared by authored and gameplay producers.
		// shouldRoute is true only for the first exact accepted request.
		AnimationRootMotionApplicationReceipt ReserveApplication(
			const AnimationRootMotionApplicationRequest& request,
			bool& shouldRoute);
		void UpdateApplicationReceipt(
			const AnimationRootMotionApplicationRequest& request,
			const AnimationRootMotionApplicationReceipt& receipt);
		// A deferred route uses this copied bit to carry an exact retry through to
		// the later terminal result without re-reserving or double-applying work.
		[[nodiscard]] bool ApplicationWasRetried(
			const AnimationRootMotionApplicationRequest& request) const noexcept;
		// Exception cleanup removes a pending scene/physics mapping by its exact
		// application identity and returns a physics command that must be cancelled.
		std::uint64_t AbandonPendingApplication(
			const AnimationRootMotionApplicationRequest& request) noexcept;
		void PublishImmediate(AnimationRootMotionApplicationResult result);
		bool RegisterSceneRequest(std::string transactionId, AnimationRootMotionApplicationResult result);
		bool RegisterPhysicsRequest(
			std::uint64_t commandId,
			AnimationRootMotionApplicationResult result,
			UUID objectId,
			std::uint32_t objectGeneration,
			std::uint32_t componentGeneration,
			AnimationFloat3 startPosition,
			AnimationFloat3 requestedEndPosition,
			AnimationQuaternion startRotation,
			AnimationQuaternion requestedEndRotation,
			bool character);
		void ObserveSceneTransactionResult(const TerraEngine::Scene::TransactionResult& result);
		void ObservePhysicsResults(
			const TerraEngine::Physics::PhysicsCommandResultSnapshot& results,
			const std::shared_ptr<const TerraEngine::Physics::IncrementalBodyStateSnapshot>& bodies,
			const std::shared_ptr<const TerraEngine::Physics::PhysicsCharacterStatePacket>& characters);
		void CopyForAnimator(
			AnimatorInstanceHandle animator,
			std::vector<AnimationRootMotionApplicationResult>& output) const;
		void Retire(AnimatorInstanceHandle animator);
		// A rebound crowd is removed from every receipt vector with one lock and one
		// index rebuild instead of repeating five vector scans per animator. The input
		// is sorted and unique so protected retirement performs no temporary allocation.
		void RetireMany(std::span<const AnimatorInstanceHandle> animators);
		void Clear();

	private:
		struct PendingScene;
		struct PendingPhysics;
		struct ApplicationRecord;
		struct AppliedMutationRecord;
		// Root-motion receipts use this exact producer identity for retry and
		// idempotency checks. Keeping a typed key avoids rescanning thousands of
		// retained records for every animated object at the protected boundary.
		struct ApplicationIdentity {
			std::uint32_t animatorSlot = 0U;
			std::uint32_t animatorGeneration = 0U;
			std::uint32_t producerId = 0U;
			std::uint64_t idempotencyKey = 0U;

			bool operator==(const ApplicationIdentity&) const noexcept = default;
		};
		struct ApplicationIdentityHash {
			std::size_t operator()(const ApplicationIdentity& identity) const noexcept;
		};
		struct ResultIdentity {
			ApplicationIdentity application;
			std::uint64_t correlationId = 0U;
			bool idempotencyConflict = false;

			bool operator==(const ResultIdentity&) const noexcept = default;
		};
		struct ResultIdentityHash {
			std::size_t operator()(const ResultIdentity& identity) const noexcept;
		};

		static constexpr std::size_t kRetainedRootMotionResults = 4096U;
		static constexpr std::size_t kRetainedRootMotionApplications = 4096U;
		[[nodiscard]] static ApplicationIdentity MakeApplicationIdentity(
			const AnimationRootMotionApplicationRequest& request) noexcept;
		[[nodiscard]] static ApplicationIdentity MakeApplicationIdentity(
			const AnimationRootMotionApplicationResult& result) noexcept;
		[[nodiscard]] static ResultIdentity MakeResultIdentity(
			const AnimationRootMotionApplicationResult& result) noexcept;
		[[nodiscard]] ApplicationRecord* FindApplicationLocked(
			const AnimationRootMotionApplicationRequest& request) noexcept;
		[[nodiscard]] ApplicationRecord* FindApplicationLocked(
			const AnimationRootMotionApplicationResult& result) noexcept;
		void RebuildLookupIndexesLocked();
		void RetainResultLocked(AnimationRootMotionApplicationResult result);
		void RememberAppliedMutationLocked(const ApplicationRecord& application) noexcept;

		mutable std::mutex mutex_;
		std::vector<PendingScene> pendingScene_;
		std::vector<PendingPhysics> pendingPhysics_;
		std::vector<ApplicationRecord> applications_;
		std::unordered_map<ApplicationIdentity, std::size_t, ApplicationIdentityHash>
			applicationIndex_;
		std::size_t applicationEvictionCursor_ = 0U;
		std::vector<AppliedMutationRecord> appliedMutations_;
		std::vector<AnimationRootMotionApplicationResult> results_;
		std::unordered_map<ResultIdentity, std::size_t, ResultIdentityHash> resultIndex_;
		std::size_t resultEvictionCursor_ = 0U;
	};

	// The caller invokes routing from a declared protected preparation boundary.
	// The function only snapshots live targets and enqueues typed deferred work;
	// actual visibility and final acknowledgement belong to the target boundary.
	TERRAENGINE_API AnimationRootMotionApplicationReceipt RouteRootMotionApplication(
		const AnimationRootMotionApplicationRequest& request,
		ObjectManager& objects,
		TerraEngine::Scene::SceneMutationQueue& sceneMutations,
		TerraEngine::Physics::PhysicsSystem& physics,
		AnimationRootMotionSceneVisibility sceneVisibility = AnimationRootMotionSceneVisibility::DeferredQueue,
		bool applicationReserved = false);
}
