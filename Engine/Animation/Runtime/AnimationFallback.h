// AnimationFallback.h - Explicit safe-pose selection and atomic publication.
//
// Selection is a pure value decision suitable for worker preparation. A caller
// publishes only a complete immutable candidate at its protected boundary; a
// rejected or failed candidate leaves the prior shared publication untouched.

#pragma once

#include "Animation/Runtime/AnimatorInstanceStore.h"
#include "Core/Export.h"

#include <atomic>
#include <compare>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

namespace TerraEngine::Animation {
	// Policy is runtime intent, not a renderer API. Every degraded selection is
	// returned with a reason so fallback can never be silent in diagnostics.
	enum class AnimationFallbackPolicy : std::uint8_t {
		Disabled = 0U,
		BindPose = 1U,
		HoldLastCompatible = 2U,
		CpuThenHoldLast = 3U,
		CpuThenBindPose = 4U
	};

	enum class AnimationFallbackReason : std::uint8_t {
		None = 0U,
		MissingAsset = 1U,
		IncompatibleAsset = 2U,
		AllocationFailure = 3U,
		BudgetPressure = 4U,
		Cancelled = 5U,
		StaleGeneration = 6U,
		RendererUnavailable = 7U,
		RendererLost = 8U,
		DeviceLost = 9U,
		PhysicsUnavailable = 10U,
		ValidationFailure = 11U,
		InternalFailure = 12U
	};

	enum class AnimationFallbackSource : std::uint8_t {
		None = 0U,
		Current = 1U,
		CpuDeterministic = 2U,
		HeldPriorGeneration = 3U,
		BindPose = 4U,
		Disabled = 5U
	};

	enum class AnimationFallbackDecisionStatus : std::uint8_t {
		Ready = 0U,
		Degraded = 1U,
		Unavailable = 2U,
		InvalidArgument = 3U
	};

	// Lifecycle fields must match for hold-last. contentGeneration may differ so a
	// failed compatible reload can retain the last-known-good generation. Renderer
	// and physics generations remain exact for new current/CPU publications but do
	// not invalidate a compatible immutable visual pose by themselves.
	struct AnimationFallbackGenerationKey {
		AnimatorInstanceHandle animator;
		AnimationGeneration componentGeneration = 0U;
		// Scene, renderer, and physics owners publish 64-bit process-lifetime
		// generations. Keeping their native width avoids accepting a stale result
		// after a long-running editor session crosses a 32-bit boundary.
		std::uint64_t sceneGeneration = 0U;
		AnimationGeneration contentGeneration = 0U;
		std::uint64_t rendererGeneration = 0U;
		std::uint64_t physicsWorldGeneration = 0U;
		AnimationGeneration publicationGeneration = 0U;
		AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;

		[[nodiscard]] bool IsValid() const noexcept;
		[[nodiscard]] bool HasSameLifecycle(
			const AnimationFallbackGenerationKey& other) const noexcept;
		[[nodiscard]] bool IsHoldCompatibleWith(
			const AnimationFallbackGenerationKey& expected) const noexcept;
		auto operator<=>(const AnimationFallbackGenerationKey&) const = default;
	};

	struct AnimationFallbackAvailability {
		AnimationFallbackGenerationKey generations{};
		bool available = false;
		bool complete = false;
		bool finite = false;
	};

	struct AnimationFallbackRequest {
		AnimationFallbackPolicy policy = AnimationFallbackPolicy::CpuThenHoldLast;
		AnimationFallbackReason reason = AnimationFallbackReason::None;
		AnimationFallbackGenerationKey expectedGenerations{};
		AnimationFallbackAvailability current{};
		AnimationFallbackAvailability cpuFallback{};
		AnimationFallbackAvailability prior{};
		bool bindPoseAvailable = false;
	};

	struct AnimationFallbackDecision {
		AnimationFallbackDecisionStatus status = AnimationFallbackDecisionStatus::InvalidArgument;
		AnimationFallbackSource source = AnimationFallbackSource::None;
		AnimationFallbackReason reason = AnimationFallbackReason::None;
		AnimationFallbackGenerationKey selectedGenerations{};
		bool preservesPriorGeneration = false;
		bool publishesNewGeneration = false;
	};

	[[nodiscard]] TERRAENGINE_API AnimationFallbackDecision SelectAnimationFallback(
		const AnimationFallbackRequest& request) noexcept;

	enum class AnimationFallbackPublishStatus : std::uint8_t {
		Published = 0U,
		AlreadyPublished = 1U,
		RejectedInvalid = 2U,
		RejectedUnexpectedGeneration = 3U,
		RejectedStale = 4U
	};

	// Payload stays strongly typed while the runtime policy remains independent of
	// PoseBuffer, renderer packets, or future network snapshot implementations.
	template<typename Payload>
	struct AnimationFallbackPublication {
		AnimationFallbackGenerationKey generations{};
		AnimationFallbackSource source = AnimationFallbackSource::None;
		std::shared_ptr<const Payload> payload;
		bool complete = false;
		bool finite = false;

		[[nodiscard]] bool IsUsable() const noexcept {
			return generations.IsValid() && payload != nullptr && complete && finite;
		}
	};

	// The expected key must be supplied from live protected-boundary state, not
	// copied from the worker candidate. This is the generation latch that rejects
	// a late result after object reuse, scene reload, or another lifecycle change.
	template<typename Payload>
	class AnimationFallbackPublicationLatch {
	public:
		using Publication = AnimationFallbackPublication<Payload>;

		// Protected batch publishers call this for every candidate before mutating
		// any live runtime record. The result is advisory until TryPublish performs
		// the atomic swap, but a single protected owner makes it a complete preflight.
		[[nodiscard]] AnimationFallbackPublishStatus ValidatePublication(
			const std::shared_ptr<const Publication>& candidate,
			const AnimationFallbackGenerationKey& expected) const noexcept
		{
			if(!candidate || !candidate->IsUsable() || !expected.IsValid()
				|| (candidate->source != AnimationFallbackSource::Current
					&& candidate->source != AnimationFallbackSource::CpuDeterministic
					&& candidate->source != AnimationFallbackSource::BindPose)) {
				return AnimationFallbackPublishStatus::RejectedInvalid;
			}
			if(candidate->generations != expected) {
				return AnimationFallbackPublishStatus::RejectedUnexpectedGeneration;
			}
			const std::shared_ptr<const Publication> prior =
				published_.load(std::memory_order_acquire);
			if(prior == candidate) return AnimationFallbackPublishStatus::AlreadyPublished;
			if(prior && prior->generations.HasSameLifecycle(candidate->generations)
				&& prior->generations.publicationGeneration
					>= candidate->generations.publicationGeneration) {
				return AnimationFallbackPublishStatus::RejectedStale;
			}
			return AnimationFallbackPublishStatus::Published;
		}

		[[nodiscard]] AnimationFallbackPublishStatus TryPublish(
			std::shared_ptr<const Publication> candidate,
			const AnimationFallbackGenerationKey& expected) noexcept
		{
			const AnimationFallbackPublishStatus validation =
				ValidatePublication(candidate, expected);
			if(validation != AnimationFallbackPublishStatus::Published) return validation;

			std::shared_ptr<const Publication> prior = published_.load(std::memory_order_acquire);
			for(;;) {
				if(prior == candidate) {
					return AnimationFallbackPublishStatus::AlreadyPublished;
				}
				if(prior && prior->generations.HasSameLifecycle(candidate->generations)
					&& prior->generations.publicationGeneration
						>= candidate->generations.publicationGeneration) {
					return AnimationFallbackPublishStatus::RejectedStale;
				}
				if(published_.compare_exchange_weak(
					prior,
					candidate,
					std::memory_order_release,
					std::memory_order_acquire)) {
					return AnimationFallbackPublishStatus::Published;
				}
			}
		}

		[[nodiscard]] std::shared_ptr<const Publication> Load() const noexcept {
			return published_.load(std::memory_order_acquire);
		}

		// Lifecycle owners clear at the same protected boundary that invalidates
		// cancellation generations. Worker code never calls this method.
		void Clear() noexcept {
			published_.store(nullptr, std::memory_order_release);
		}

	private:
		std::atomic<std::shared_ptr<const Publication>> published_{};
	};

	template<typename Payload>
	[[nodiscard]] AnimationFallbackAvailability DescribeAnimationFallbackPublication(
		const std::shared_ptr<const AnimationFallbackPublication<Payload>>& publication) noexcept
	{
		if(!publication) {
			return {};
		}
		return {
			publication->generations,
			publication->payload != nullptr,
			publication->complete,
			publication->finite};
	}

	[[nodiscard]] TERRAENGINE_API std::string_view ToString(AnimationFallbackPolicy policy) noexcept;
	[[nodiscard]] TERRAENGINE_API std::string_view ToString(AnimationFallbackReason reason) noexcept;
	[[nodiscard]] TERRAENGINE_API std::string_view ToString(AnimationFallbackSource source) noexcept;
	[[nodiscard]] TERRAENGINE_API std::string_view ToString(
		AnimationFallbackDecisionStatus status) noexcept;
	[[nodiscard]] TERRAENGINE_API std::string_view ToString(
		AnimationFallbackPublishStatus status) noexcept;
}
