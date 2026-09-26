// AnimationConsumerRegistry.h - Immutable authoritative-consumer declarations.

#pragma once

#include "Animation/Runtime/AnimationExecutionMode.h"
#include "Animation/Runtime/AnimatorInstanceStore.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	enum class AnimationConsumerKind : std::uint8_t {
		Gameplay = 0U,
		Physics = 1U,
		Attachment = 2U,
		Networking = 3U,
		Rollback = 4U,
		Replay = 5U,
		Recording = 6U,
		Automation = 7U,
		Diagnostics = 8U,
		Procedural = 9U
	};

	enum class AnimationConsumerReason : std::uint32_t {
		None = 0U,
		Joints = 1U << 0U,
		Curves = 1U << 1U,
		Morphs = 1U << 2U,
		Events = 1U << 3U,
		RootMotion = 1U << 4U,
		Contacts = 1U << 5U,
		ProceduralResults = 1U << 6U,
		CompletePose = 1U << 7U,
		Deterministic = 1U << 8U
	};

	constexpr AnimationConsumerReason operator|(
		AnimationConsumerReason left,
		AnimationConsumerReason right) noexcept
	{
		return static_cast<AnimationConsumerReason>(
			static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
	}

	struct AnimationConsumerDeclaration {
		std::uint64_t consumerId = 0U;
		AnimationConsumerKind kind = AnimationConsumerKind::Gameplay;
		AnimatorInstanceHandle animator;
		AnimationGeneration declarationGeneration = 0U;
		AnimationFixedTick declarationTick = 0;
		AnimationFixedTick maximumSnapshotAgeTicks = 0;
		std::vector<std::uint32_t> joints;
		std::vector<std::uint64_t> curves;
		std::vector<std::uint64_t> events;
		bool morphs = false;
		bool rootMotion = false;
		bool contacts = false;
		bool proceduralResults = false;
		bool completePose = false;
		bool deterministic = false;

		auto operator<=>(const AnimationConsumerDeclaration&) const = default;
	};

	struct AnimationConsumerAttribution {
		std::uint64_t consumerId = 0U;
		AnimationConsumerKind kind = AnimationConsumerKind::Gameplay;
		AnimationConsumerReason reasons = AnimationConsumerReason::None;
		AnimationFixedTick maximumSnapshotAgeTicks = 0;
		std::uint32_t requestedJointCount = 0U;
		std::uint32_t closedJointCount = 0U;

		auto operator<=>(const AnimationConsumerAttribution&) const = default;
	};

	struct AnimationResolvedConsumerRequirements {
		AnimationStatus status = AnimationStatus::Ok;
		AnimationGeneration declarationGeneration = 0U;
		AnimationFixedTick declarationTick = 0;
		AnimationFixedTick maximumSnapshotAgeTicks = 0;
		AnimationConsumerClosure closure;
		std::vector<std::uint64_t> curves;
		std::vector<std::uint64_t> events;
		std::vector<AnimationConsumerAttribution> attribution;
		bool deterministic = false;
		std::uint64_t fingerprint = 0U;
	};

	// Producers replace a complete declaration set before frame capture. Worker
	// resolution atomically loads the immutable set and never retains producer
	// vectors or locks the registry while walking skeleton ancestors.
	class TERRAENGINE_API AnimationConsumerRegistry {
		struct PublishedDeclarations;

	public:
		// A root-only overlay is prepared before Locomotion publishes ownership.
		// Holding the reservation lock makes a successful commit allocation-free and
		// prevents a concurrent ordinary declaration batch changing total capacity.
		class PreparedRootMotionConsumers {
		public:
			PreparedRootMotionConsumers() = default;
			PreparedRootMotionConsumers(PreparedRootMotionConsumers&&) noexcept = default;
			PreparedRootMotionConsumers& operator=(PreparedRootMotionConsumers&&) noexcept = default;
			[[nodiscard]] AnimationStatus Status() const noexcept { return status_; }

		private:
			friend class AnimationConsumerRegistry;
			AnimationStatus status_ = AnimationStatus::NotReady;
			std::unique_lock<std::mutex> reservation_;
			std::shared_ptr<const PublishedDeclarations> candidate_;
		};

		// One captured view pins an immutable declaration publication for every
		// worker partition in a frame. Resolve performs no atomic shared_ptr traffic,
		// so a handful of gameplay consumers cannot serialize a large visual crowd.
		class ReadView {
		public:
			[[nodiscard]] AnimationResolvedConsumerRequirements Resolve(
				AnimatorInstanceHandle animator,
				std::span<const std::int32_t> jointParents,
				AnimationFixedTick fixedTick) const;
			[[nodiscard]] bool HasDeclarations() const noexcept;

		private:
			friend class AnimationConsumerRegistry;
			std::shared_ptr<const PublishedDeclarations> snapshot_;
			std::shared_ptr<const PublishedDeclarations> rootMotionSnapshot_;
		};

		static AnimationConsumerRegistry& Instance();

		AnimationStatus PublishDeclarations(
			std::span<const AnimationConsumerDeclaration> declarations,
			AnimationGeneration generation,
			AnimationFixedTick declarationTick);
		[[nodiscard]] PreparedRootMotionConsumers PrepareRootMotionConsumers(
			std::span<const AnimatorInstanceHandle> activeAnimators,
			AnimationGeneration generation,
			AnimationFixedTick declarationTick) const;
		void CommitRootMotionConsumers(PreparedRootMotionConsumers&& prepared) noexcept;
		// Lifecycle retirement must not need a fresh generation or allocation.
		void ClearRootMotionConsumers() noexcept;
		AnimationResolvedConsumerRequirements Resolve(
			AnimatorInstanceHandle animator,
			std::span<const std::int32_t> jointParents,
			AnimationFixedTick fixedTick) const;
		[[nodiscard]] ReadView CaptureReadView() const noexcept;
		void Clear() noexcept;
		[[nodiscard]] AnimationGeneration PublishedGeneration() const noexcept;
		[[nodiscard]] std::size_t DeclarationCount() const noexcept;
		[[nodiscard]] std::size_t RootMotionDeclarationCount() const noexcept;

	private:
		mutable std::mutex publishMutex_;
		std::shared_ptr<const PublishedDeclarations> published_;
		std::shared_ptr<const PublishedDeclarations> rootMotionPublished_;
	};
}
