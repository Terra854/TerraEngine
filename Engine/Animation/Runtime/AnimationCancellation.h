// AnimationCancellation.h - Typed cancellation and lifecycle generation latches.
//
// Worker jobs capture a token from immutable lifecycle generations. The owner
// checks that token again at its protected publication boundary, preventing an
// old object, scene, asset, script, renderer, or physics job from becoming live.

#pragma once

#include "Animation/AnimationTypes.h"
#include "Core/Export.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

namespace TerraEngine::Animation {
	enum class AnimationLifecycleBoundary : std::uint8_t {
		Object = 0U,
		Component = 1U,
		Scene = 2U,
		Asset = 3U,
		ScriptDomain = 4U,
		PlayMode = 5U,
		PhysicsWorld = 6U,
		Renderer = 7U,
		Device = 8U,
		Shutdown = 9U,
		Count = 10U
	};

	inline constexpr std::size_t kAnimationLifecycleBoundaryCount =
		static_cast<std::size_t>(AnimationLifecycleBoundary::Count);
	using AnimationLifecycleMask = std::uint32_t;

	[[nodiscard]] constexpr AnimationLifecycleMask AnimationLifecycleBoundaryMask(
		AnimationLifecycleBoundary boundary) noexcept
	{
		const std::size_t index = static_cast<std::size_t>(boundary);
		return index < kAnimationLifecycleBoundaryCount
			? AnimationLifecycleMask{1U} << index
			: AnimationLifecycleMask{0U};
	}

	inline constexpr AnimationLifecycleMask kAllAnimationLifecycleBoundaries =
		(AnimationLifecycleMask{1U} << kAnimationLifecycleBoundaryCount) - 1U;

	struct AnimationLifecycleGenerationSnapshot {
		std::array<std::uint64_t, kAnimationLifecycleBoundaryCount> generations{};

		[[nodiscard]] std::uint64_t Generation(AnimationLifecycleBoundary boundary) const noexcept;
		[[nodiscard]] bool IsValid(AnimationLifecycleMask mask) const noexcept;
		auto operator<=>(const AnimationLifecycleGenerationSnapshot&) const = default;
	};

	struct AnimationLifecycleValidation {
		bool current = false;
		AnimationLifecycleBoundary boundary = AnimationLifecycleBoundary::Count;
		std::uint64_t capturedGeneration = 0U;
		std::uint64_t currentGeneration = 0U;
	};

	// The mutex makes capturing several related generations one atomic snapshot.
	// Invalidation is a short protected-boundary operation, not worker evaluation.
	class TERRAENGINE_API AnimationGenerationLatch {
	public:
		AnimationGenerationLatch() noexcept;
		AnimationGenerationLatch(const AnimationGenerationLatch&) = delete;
		AnimationGenerationLatch& operator=(const AnimationGenerationLatch&) = delete;

		[[nodiscard]] AnimationLifecycleGenerationSnapshot Capture() const noexcept;
		[[nodiscard]] AnimationLifecycleValidation Validate(
			const AnimationLifecycleGenerationSnapshot& snapshot,
			AnimationLifecycleMask mask) const noexcept;
		[[nodiscard]] AnimationStatus Invalidate(AnimationLifecycleBoundary boundary) noexcept;

		// Generation exhaustion is practical only in a regression fixture. The
		// setter cannot revive a terminal zero generation or move a value backward.
		[[nodiscard]] AnimationStatus SetGenerationForTesting(
			AnimationLifecycleBoundary boundary,
			std::uint64_t generation) noexcept;

	private:
		mutable std::mutex mutex_;
		AnimationLifecycleGenerationSnapshot current_{};
	};

	// The operation kind lets diagnostics identify which pipeline discarded work
	// without interpreting an untyped callback or provider-specific job handle.
	enum class AnimationCancellationOperation : std::uint8_t {
		Import = 0U,
		CpuEvaluation = 1U,
		GpuBatch = 2U,
		Recording = 3U,
		Reload = 4U,
		ScenePublication = 5U,
		PhysicsHandoff = 6U,
		ManagedDelivery = 7U,
		Shutdown = 8U,
		Count = 9U
	};

	// Reasons are append-only diagnostic identities. The first explicit reason is
	// retained so racing teardown paths cannot produce nondeterministic reports.
	enum class AnimationCancellationReason : std::uint8_t {
		None = 0U,
		Explicit = 1U,
		OwnerReleased = 2U,
		ObjectReused = 3U,
		ComponentRemoved = 4U,
		SceneUnloaded = 5U,
		AssetReloaded = 6U,
		ScriptReloaded = 7U,
		PlayModeStopped = 8U,
		PhysicsWorldReplaced = 9U,
		RendererRecovered = 10U,
		DeviceLost = 11U,
		Shutdown = 12U,
		GenerationExhausted = 13U
	};

	struct AnimationCancellationPollResult {
		AnimationStatus status = AnimationStatus::Cancelled;
		AnimationCancellationOperation operation = AnimationCancellationOperation::Count;
		AnimationCancellationReason reason = AnimationCancellationReason::OwnerReleased;
		AnimationLifecycleBoundary boundary = AnimationLifecycleBoundary::Count;
		std::uint64_t capturedGeneration = 0U;
		std::uint64_t currentGeneration = 0U;

		[[nodiscard]] bool CanPublish() const noexcept { return status == AnimationStatus::Ok; }
	};

	struct AnimationCancellationState;

	class TERRAENGINE_API AnimationCancellationToken {
	public:
		AnimationCancellationToken() noexcept = default;

		[[nodiscard]] bool IsValid() const noexcept { return state_ != nullptr; }
		[[nodiscard]] AnimationCancellationPollResult Poll(
			const AnimationGenerationLatch& latch) const noexcept;
		[[nodiscard]] bool IsCancellationRequested(
			const AnimationGenerationLatch& latch) const noexcept {
			return !Poll(latch).CanPublish();
		}

	private:
		friend class AnimationCancellationSource;
		explicit AnimationCancellationToken(
			std::shared_ptr<AnimationCancellationState> state) noexcept
			: state_(std::move(state)) {}

		std::shared_ptr<AnimationCancellationState> state_;
	};

	// A source owns the explicit cancellation side of a job. Destroying it latches
	// OwnerReleased, so tokens never silently become uncancellable when an owner
	// tears down before its workers finish.
	class TERRAENGINE_API AnimationCancellationSource {
	public:
		AnimationCancellationSource() noexcept = default;
		~AnimationCancellationSource();
		AnimationCancellationSource(const AnimationCancellationSource&) = delete;
		AnimationCancellationSource& operator=(const AnimationCancellationSource&) = delete;
		AnimationCancellationSource(AnimationCancellationSource&& other) noexcept;
		AnimationCancellationSource& operator=(AnimationCancellationSource&& other) noexcept;

		[[nodiscard]] AnimationStatus Initialize(
			AnimationCancellationOperation operation,
			const AnimationGenerationLatch& latch,
			AnimationLifecycleMask mask = 0U) noexcept;
		[[nodiscard]] AnimationCancellationToken GetToken() const noexcept;
		[[nodiscard]] bool Cancel(
			AnimationCancellationReason reason = AnimationCancellationReason::Explicit) noexcept;

	private:
		std::shared_ptr<AnimationCancellationState> state_;
	};

	[[nodiscard]] TERRAENGINE_API AnimationLifecycleMask DefaultAnimationCancellationMask(
		AnimationCancellationOperation operation) noexcept;
	[[nodiscard]] TERRAENGINE_API AnimationCancellationReason CancellationReasonForBoundary(
		AnimationLifecycleBoundary boundary,
		std::uint64_t currentGeneration) noexcept;
	[[nodiscard]] TERRAENGINE_API std::string_view ToString(
		AnimationLifecycleBoundary boundary) noexcept;
	[[nodiscard]] TERRAENGINE_API std::string_view ToString(
		AnimationCancellationOperation operation) noexcept;
	[[nodiscard]] TERRAENGINE_API std::string_view ToString(
		AnimationCancellationReason reason) noexcept;
}
