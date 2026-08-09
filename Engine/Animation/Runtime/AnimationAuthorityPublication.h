// AnimationAuthorityPublication.h - Sealed, immutable animation authority roots.
//
// Workers construct complete candidates.  The protected owner only compares
// generations and exchanges one shared root, keeping publication constant-time
// regardless of crowd size.

#pragma once

#include "Animation/Runtime/AnimationFrameSidecars.h"
#include "Animation/Runtime/AnimationExecutionMode.h"
#include "Animation/Runtime/AnimationMemory.h"
#include "Animation/Runtime/AnimationSnapshots.h"
#include "Animation/Assets/SkeletonAsset.h"

#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	inline constexpr std::size_t kAnimationAuthorityPageCapacity128 = 128U;
	inline constexpr std::size_t kAnimationAuthorityPageCapacity256 = 256U;

	enum class AnimationAuthorityPublicationOutcome : std::uint8_t {
		Published,
		RejectedInvalidCandidate,
		RejectedStalePredecessor,
		RejectedLifecycle,
		RejectedDuplicateSlot,
		RejectedMissingWorker,
		RejectedAllocation,
		RejectedHistoryReservation,
		RejectedSidecars,
		RejectedCancelled
	};

	struct AnimationSlotGenerationKey {
		std::uint32_t slot = kInvalidAnimatorInstanceSlot;
		AnimationGeneration lifetimeGeneration = 0U;
		std::uint64_t sessionGeneration = 0U;

		[[nodiscard]] constexpr bool IsValid() const noexcept
		{
			return slot != kInvalidAnimatorInstanceSlot && lifetimeGeneration != 0U
				&& sessionGeneration != 0U;
		}

		[[nodiscard]] constexpr AnimatorInstanceHandle Handle() const noexcept
		{
			return {slot, lifetimeGeneration};
		}

		auto operator<=>(const AnimationSlotGenerationKey&) const = default;
	};

	// One row contains only state every animator needs.  Rare variable-sized data
	// lives in AnimationFrameSidecars and a pose stays shared immutable storage.
	struct AnimationAuthorityRow {
		AnimationSlotGenerationKey key{};
		UUID ownerObjectId = EMPTY_UUID;
		AnimationGeneration objectGeneration = 0U;
		AnimationGeneration componentGeneration = 0U;
		AnimationGeneration sourceGeneration = 0U;
		AnimationGeneration poseGeneration = 0U;
		// The row carries the authority generation that selected it so internal
		// history and deferred-command validation never reopen mutable records.
		std::uint64_t authorityPublicationGeneration = 0U;
		AnimationFixedTick tick = 0;
		std::uint64_t stateId = 0U;
		std::uint64_t renderBindingGeneration = 0U;
		// CPU deformation consumers retain these immutable binding facts with the
		// pose; no renderer is allowed to reopen a mutable animator record.
		AnimationAssetHandle controller;
		AnimationAssetHandle clip;
		AnimationAssetHandle skeleton;
		std::shared_ptr<const SkeletonAsset> skeletonAsset;
		std::vector<std::uint64_t> jointStableIds;
		AnimationCompatibilityFingerprint skeletonCompatibilityFingerprint = 0U;
		std::uint64_t visualPoseContentIdLow = 0U;
		std::uint64_t visualPoseContentIdHigh = 0U;
		std::shared_ptr<const AnimationMemoryLease> poseMemoryReservation;
		AnimationTransform sourceObjectWorldTransform{};
		bool sourceObjectWorldTransformCaptured = false;
		AnimationExecutionPolicy requestedExecutionPolicy = AnimationExecutionPolicy::Auto;
		AnimationEffectiveExecutionMode effectiveExecutionMode =
			AnimationEffectiveExecutionMode::CpuDeterministicFallback;
		AnimatorRootMotionPolicy rootMotionPolicy = AnimatorRootMotionPolicy::Ignore;
		AnimatorUpdatePolicy updatePolicy = AnimatorUpdatePolicy::EveryFrame;
		AnimatorEventPolicy eventPolicy = AnimatorEventPolicy::Dispatch;
		AnimatorCullingPolicy cullingPolicy = AnimatorCullingPolicy::Automatic;
		AnimationStatus status = AnimationStatus::NotReady;
		std::shared_ptr<const AnimationSnapshot> snapshot;
		bool active = false;
		bool safeFallback = true;
		// Streaming diagnostics are immutable publication data.  They tell a
		// consumer why a safe fallback was selected without exposing residency.
		bool streamedClipPayloadsActive = false;

		[[nodiscard]] bool IsValid() const noexcept;
	};

	struct AnimationAuthorityPage {
		std::uint32_t firstSlot = 0U;
		std::uint32_t capacity = static_cast<std::uint32_t>(kAnimationAuthorityPageCapacity128);
		std::vector<AnimationAuthorityRow> rows;
		std::vector<std::uint64_t> changedMasks;
		// Compact workers validate their disjoint rows while constructing the page.
		// Once the page becomes const, this count is an immutable proof that sealing
		// need not traverse the same rows again. Compatibility/test pages leave zero
		// and continue through the complete validation path.
		std::size_t workerValidatedRowCount = 0U;

		[[nodiscard]] bool IsValid() const noexcept;
		[[nodiscard]] const AnimationAuthorityRow* Find(AnimationSlotGenerationKey key) const noexcept;
	};

	struct AnimationPublicationReceipt {
		AnimationAuthorityPublicationOutcome outcome = AnimationAuthorityPublicationOutcome::RejectedInvalidCandidate;
		std::uint64_t authorityGeneration = 0U;
		std::uint64_t expectedPreviousGeneration = 0U;
		std::uint64_t observedPreviousGeneration = 0U;
		std::uint32_t reusedPages = 0U;
		std::uint32_t changedPages = 0U;
		std::uint32_t evaluatedRows = 0U;
		std::uint32_t fallbackRows = 0U;
		std::uint32_t eventCount = 0U;
		std::uint32_t gpuCandidateCount = 0U;

		[[nodiscard]] bool Published() const noexcept
		{
			return outcome == AnimationAuthorityPublicationOutcome::Published;
		}
	};

	struct AnimationAuthorityFrame {
		std::uint64_t frameId = 0U;
		std::uint64_t sessionGeneration = 0U;
		std::uint64_t lifecycleGeneration = 0U;
		std::uint64_t authorityGeneration = 0U;
		std::shared_ptr<const std::vector<std::shared_ptr<const AnimationAuthorityPage>>> pages;
		std::shared_ptr<const AnimationFrameSidecars> sidecars;
		AnimationPublicationReceipt receipt{};

		[[nodiscard]] bool IsValid() const noexcept;
		[[nodiscard]] const AnimationAuthorityRow* Find(AnimationSlotGenerationKey key) const noexcept;
		[[nodiscard]] const AnimationAuthorityRow* Find(AnimatorInstanceHandle handle) const noexcept;
	};

	// A sealed candidate has already done every allocation, merge, ordering, and
	// history reservation.  No field is mutable once it reaches Publish().
	struct SealedAnimationFrameResult {
		std::uint64_t expectedPreviousGeneration = 0U;
		std::uint64_t candidateGeneration = 0U;
		std::shared_ptr<const AnimationAuthorityFrame> frame;
		bool historyReserved = false;
		bool workersComplete = false;
		bool cancelled = false;
		// SealAnimationFrameResult sets this only after it has validated every page,
		// row, and sidecar. The frame is const afterwards, so protected publication
		// can verify scalar ownership without rescanning thousands of immutable rows.
		bool sealedValidationComplete = false;
		AnimationPublicationReceipt receipt{};

		[[nodiscard]] bool IsValid() const noexcept;
	};

	struct AnimationAuthoritySealInput {
		std::uint64_t frameId = 0U;
		std::uint64_t sessionGeneration = 0U;
		std::uint64_t lifecycleGeneration = 0U;
		std::uint64_t expectedPreviousGeneration = 0U;
		std::uint64_t candidateGeneration = 0U;
		std::span<const std::shared_ptr<const AnimationAuthorityPage>> pages;
		std::shared_ptr<const AnimationFrameSidecars> sidecars;
		// Production workers can transfer sole ownership of an already-built sidecar
		// root. Tests and external callers may continue supplying the const fallback
		// above; sealing copies only that compatibility input. Keeping the mutable
		// owner separate avoids a crowd-sized deep copy before it becomes immutable.
		std::shared_ptr<AnimationFrameSidecars> ownedSidecars;
		// Sealing receives these precomputed structural facts from the worker.  They
		// make page reuse observable without requiring the protected publisher to
		// rescan a crowd-sized immutable root.
		std::uint32_t reusedPages = 0U;
		std::uint32_t changedPages = 0U;
		bool workersComplete = false;
		bool historyReserved = false;
		bool cancelled = false;
	};

	[[nodiscard]] TERRAENGINE_API SealedAnimationFrameResult SealAnimationFrameResult(
		const AnimationAuthoritySealInput& input) noexcept;

	// Failure injection is intentionally test-scoped and consumed during sealing;
	// it cannot alter a protected root exchange or production allocation policy.
	void TERRAENGINE_API SetAnimationAuthorityFailureInjectionForTesting(
		AnimationAuthorityPublicationOutcome outcome) noexcept;
	void TERRAENGINE_API ClearAnimationAuthorityFailureInjectionForTesting() noexcept;

	class TERRAENGINE_API AnimationAuthorityPublisher {
	public:
		[[nodiscard]] AnimationPublicationReceipt Publish(
			const SealedAnimationFrameResult& candidate,
			std::uint64_t expectedSessionGeneration,
			std::uint64_t expectedLifecycleGeneration) noexcept;
		[[nodiscard]] std::shared_ptr<const AnimationAuthorityFrame> Load() const noexcept;
		[[nodiscard]] AnimationPublicationReceipt LastReceipt() const noexcept;
		void Clear(std::uint64_t sessionGeneration, std::uint64_t lifecycleGeneration) noexcept;

	private:
		std::atomic<std::shared_ptr<const AnimationAuthorityFrame>> root_{};
		std::atomic<std::shared_ptr<const AnimationPublicationReceipt>> lastReceipt_{};
	};
}
