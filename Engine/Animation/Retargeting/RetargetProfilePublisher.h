// RetargetProfilePublisher.h - Atomic compile/hot-reload publication boundary.

#pragma once

#include "Animation/Retargeting/RetargetValidator.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

class ThreadPool;

namespace TerraEngine::Animation {
	inline constexpr std::size_t kRetargetPublisherMaximumPendingJobs = 64U;
	inline constexpr std::size_t kRetargetPublisherMaximumRetiredGenerations = 64U;

	// Request owns all three authored inputs. The worker never reads a live asset
	// slot that can change while validation or compilation is running.
	struct RetargetProfileCompileRequest {
		RetargetProfileAsset profile{};
		SkeletonAsset sourceSkeleton{};
		SkeletonAsset targetSkeleton{};
		AnimationGeneration sourceGeneration = 0U;
	};

	// The protected boundary receives only copied identity/generation facts from
	// the resource owner; no mutable ResourceManager object crosses this API.
	struct RetargetProfileGenerationSnapshot {
		AnimationGuid profileAssetId{};
		AnimationGeneration sourceGeneration = 0U;
		AnimationAssetHandle sourceSkeleton{};
		AnimationAssetHandle targetSkeleton{};
		AnimationCompatibilityFingerprint sourceSkeletonFingerprint = 0U;
		AnimationCompatibilityFingerprint targetSkeletonFingerprint = 0U;
		std::uint64_t sourceReferencePoseHash = 0U;
		std::uint64_t targetReferencePoseHash = 0U;

		auto operator<=>(const RetargetProfileGenerationSnapshot&) const = default;
	};

	struct RetargetProfilePublishRecord {
		AnimationGuid profileAssetId{};
		AnimationGeneration sourceGeneration = 0U;
		AnimationGeneration profileGeneration = 0U;
		AnimationGeneration dependentInvalidationGeneration = 0U;
		AnimationAssetHandle sourceSkeleton{};
		AnimationAssetHandle targetSkeleton{};
		std::uint64_t stableHash = 0U;
		std::shared_ptr<const RetargetCompiledProfile> compiledProfile;

		auto operator<=>(const RetargetProfilePublishRecord&) const = default;
	};

	struct RetargetRetiredGeneration {
		AnimationGeneration profileGeneration = 0U;
		std::uint64_t stableHash = 0U;

		auto operator<=>(const RetargetRetiredGeneration&) const = default;
	};

	struct RetargetProfilePublishPollResult {
		AnimationStatus status = AnimationStatus::NotReady;
		bool published = false;
		AnimationGeneration dependentInvalidationGeneration = 0U;
		std::size_t retainedOldGenerationCount = 0U;
	};

	class RetargetProfileCompileExecutor {
	public:
		virtual ~RetargetProfileCompileExecutor() = default;
		virtual void Enqueue(std::function<void()> work) = 0;
	};

	TERRAENGINE_API std::shared_ptr<RetargetProfileCompileExecutor>
		CreateThreadPoolRetargetProfileCompileExecutor(ThreadPool& pool);

	class RetargetProfilePublisher {
	public:
		TERRAENGINE_API explicit RetargetProfilePublisher(
			std::shared_ptr<RetargetProfileCompileExecutor> executor,
			std::size_t maxPendingJobs = 16U,
			std::size_t maxRetiredGenerations = 8U);
		TERRAENGINE_API ~RetargetProfilePublisher();

		RetargetProfilePublisher(const RetargetProfilePublisher&) = delete;
		RetargetProfilePublisher& operator=(const RetargetProfilePublisher&) = delete;

		TERRAENGINE_API AnimationStatus RequestPublication(
			const RetargetProfileCompileRequest& request);
		// Serialized hot reload uses the same checked reader as normal resource
		// loading. Corrupt bytes fail before enqueue and cannot affect last-good.
		TERRAENGINE_API AnimationStatus RequestSerializedPublication(
			std::span<const std::uint8_t> bytes,
			AnimationCompatibilityFingerprint expectedProfileFingerprint,
			const SkeletonAsset& sourceSkeleton,
			const SkeletonAsset& targetSkeleton,
			AnimationGeneration sourceGeneration);
		TERRAENGINE_API void CancelPending() noexcept;
		TERRAENGINE_API RetargetProfilePublishPollResult PollAndPublish(
			const RetargetProfileGenerationSnapshot& current);

		[[nodiscard]] TERRAENGINE_API std::optional<RetargetProfilePublishRecord>
			GetPublishedRecord() const;
		[[nodiscard]] TERRAENGINE_API std::vector<RetargetRetiredGeneration>
			GetRetiredGenerations() const;
		// The resource/backend owner calls this after every consumer fence that can
		// reference generations up to completedGeneration has retired.
		TERRAENGINE_API void RetireThrough(
			AnimationGeneration completedGeneration) noexcept;

	private:
		struct SharedState;
		std::shared_ptr<SharedState> state_;
		std::shared_ptr<RetargetProfileCompileExecutor> executor_;
	};
}
