// MotionMatchGpu.h - Production backend contract for bounded asynchronous top-K search.

#pragma once

#include "Animation/MotionMatching/MotionMatchCpu.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	inline constexpr std::size_t kMotionMatchGpuMaximumResidentBlocks =
		kMotionDatabaseMaximumResidentBlocks;
	inline constexpr std::size_t kMotionMatchGpuMaximumInFlightRequests = 64U;
	// A tile is four times the maximum top-K so every pass strictly reduces its
	// input, including the supported top-K-256 edge case.
	inline constexpr std::uint32_t kMotionMatchGpuReductionTileSize = 1024U;
	inline constexpr std::size_t kMotionMatchGpuMaximumReductionPasses = 16U;

	struct MotionMatchGpuGeneration {
		AnimationAssetHandle database{};
		std::uint64_t schemaFingerprint = 0U;
		std::uint64_t querySequence = 0U;

		constexpr bool IsValid() const noexcept {
			return database.IsValid() && schemaFingerprint != 0U && querySequence != 0U;
		}
		auto operator<=>(const MotionMatchGpuGeneration&) const = default;
	};

	struct MotionMatchGpuDispatchRange {
		std::uint32_t blockIndex = 0U;
		std::uint32_t firstSample = 0U;
		std::uint32_t sampleCount = 0U;
		std::uint32_t threadGroupCount = 0U;
		std::uint64_t gpuByteOffset = 0U;
		std::uint64_t gpuByteSize = 0U;
	};

	struct MotionMatchGpuReductionPass {
		std::uint32_t inputCandidateCount = 0U;
		std::uint32_t groupCount = 0U;
		std::uint32_t outputCandidateCount = 0U;
		std::uint32_t topK = 0U;
	};

	// The prefix retains the original fields so existing renderer planning code
	// can adopt the richer block/pass metadata without changing all initialization.
	struct MotionMatchGpuDispatchPlan {
		MotionMatchGpuGeneration generation{};
		std::uint32_t firstSample = 0U;
		std::uint32_t sampleCount = 0U;
		std::uint32_t featureCount = kMotionDatabaseFeatureCount;
		std::uint32_t topK = kMotionDatabaseTopKDefault;
		std::uint32_t scoreDispatchCount = 0U;
		std::uint32_t reductionDispatchCount = 0U;
		std::uint32_t readbackStrideBytes = 0U;
		std::uint32_t readbackBytes = 0U;
		std::vector<MotionMatchGpuDispatchRange> scoreRanges;
		std::vector<MotionMatchGpuReductionPass> reductionPasses;
	};

	// Top-K readback carries component totals, while the CPU authoritative scorer
	// reconstructs all 48 per-feature costs for the chosen bounded candidate set.
	struct MotionMatchGpuCandidateRecord {
		std::uint64_t stableSampleId = 0U;
		std::uint64_t sourceClipId = 0U;
		AnimationFixedTick sourceTick = 0;
		std::array<std::uint64_t, kMotionDatabaseFeatureBlockCount> featureBlockCosts{};
		std::uint64_t featureCost = 0U;
		std::uint64_t phaseCost = 0U;
		std::uint64_t tagCost = 0U;
		std::uint64_t contactCost = 0U;
		std::uint64_t continuityCost = 0U;
		std::uint64_t transitionCost = 0U;
		std::uint64_t totalCost = 0U;
		std::uint32_t exclusionReasons = 0U;
		std::uint32_t reserved = 0U;

		auto operator<=>(const MotionMatchGpuCandidateRecord&) const = default;
	};

	static_assert(sizeof(MotionMatchGpuCandidateRecord) == 160U,
		"Motion-match top-K readback has a frozen 160-byte stride.");
	inline constexpr std::size_t kMotionMatchGpuMaximumReadbackBytes =
		kMotionDatabaseMaximumTopK * sizeof(MotionMatchGpuCandidateRecord);

	// This packet owns every query value that a renderer backend may need after
	// Submit returns. Spans and caller-owned arrays never cross the async boundary.
	struct MotionMatchGpuQueryPacket {
		std::array<std::int32_t, kMotionDatabaseFeatureCount> features{};
		std::array<std::uint32_t, kMotionDatabaseFeatureCount> featureWeights{};
		std::array<std::uint64_t, kMotionDatabaseMaximumTopK> excludedSampleIds{};
		std::uint32_t excludedSampleCount = 0U;
		std::uint32_t maximumPhaseDistance = UINT16_MAX;
		std::uint64_t requiredTags = 0U;
		std::uint64_t excludedTags = 0U;
		std::uint64_t preferredTags = 0U;
		std::uint64_t lastSampleId = 0U;
		std::uint64_t lastSourceClipId = 0U;
		std::uint32_t requiredContacts = 0U;
		std::uint32_t preferredContacts = 0U;
		std::uint32_t preferredContinuityGroup = 0U;
		std::uint32_t excludedRegionFlags = 0U;
		std::uint32_t preferredPhase = 0U;
		std::uint32_t requiredTransitionFlags = 0U;
		std::uint32_t excludedTransitionFlags = 0U;
		std::uint32_t preferredTransitionFlags = 0U;
		std::uint32_t phaseWeight = 0U;
		std::uint32_t continuityPenalty = 0U;
		std::uint32_t continuityBreakPenalty = 0U;
		std::uint32_t sameClipPenalty = 0U;
		std::uint32_t missingPreferredTagPenalty = 0U;
		std::uint32_t contactMismatchPenalty = 0U;
		std::uint32_t transitionMismatchPenalty = 0U;
	};

	struct MotionMatchGpuSubmission {
		MotionMatchGpuDispatchPlan plan{};
		MotionMatchGpuQueryPacket query{};
	};

	struct MotionMatchGpuBackendTicket {
		std::uint64_t submissionId = 0U;
		std::uint64_t signalFenceValue = 0U;
		std::uint64_t readbackOffsetBytes = 0U;
		std::uint32_t readbackCapacity = 0U;
		std::uint32_t readbackStrideBytes = 0U;

		[[nodiscard]] bool IsValid() const noexcept {
			return submissionId != 0U && signalFenceValue != 0U
				&& readbackCapacity != 0U && readbackCapacity <= kMotionDatabaseMaximumTopK
				&& readbackStrideBytes == sizeof(MotionMatchGpuCandidateRecord);
		}
		auto operator<=>(const MotionMatchGpuBackendTicket&) const = default;
	};

	// Vulkan command recording, buffers, and fences remain renderer-owned. This
	// interface freezes what the animation side may ask for: a copied submission,
	// a bounded readback, and a poll that must return without waiting on the fence.
	class TERRAENGINE_API MotionMatchGpuBackend {
	public:
		virtual ~MotionMatchGpuBackend() = default;
		virtual AnimationStatus Submit(const MotionMatchGpuSubmission& submission,
			MotionMatchGpuBackendTicket& ticket) noexcept = 0;
		virtual AnimationStatus TryReadback(const MotionMatchGpuBackendTicket& ticket,
			std::span<MotionMatchGpuCandidateRecord> destination,
			std::uint32_t& candidateCount,
			std::uint64_t& completedFenceValue) noexcept = 0;
		virtual void Cancel(const MotionMatchGpuBackendTicket& ticket) noexcept = 0;
	};

	enum class MotionMatchGpuRequestState : std::uint8_t {
		Submitted = 0U,
		AwaitingFence = 1U,
		ReadbackReady = 2U,
		Cancelled = 3U,
		Stale = 4U,
		Failed = 5U
	};

	struct MotionMatchGpuReadback {
		MotionMatchGpuGeneration generation{};
		AnimationStatus status = AnimationStatus::NotReady;
		bool cancelled = false;
		// Readback is a value object, so an externally supplied successful record
		// is ready unless a queue explicitly stamps its in-flight state.
		MotionMatchGpuRequestState state = MotionMatchGpuRequestState::ReadbackReady;
		std::uint64_t completedFenceValue = 0U;
		std::vector<MotionMatchCandidate> candidates;
	};

	TERRAENGINE_API MotionMatchGpuCandidateRecord MakeMotionMatchGpuCandidateRecord(
		const MotionMatchCandidate& candidate) noexcept;
	TERRAENGINE_API AnimationStatus PlanMotionMatchGpuDispatch(
		const MotionDatabaseAsset& database, const MotionMatchQuery& query,
		std::span<const std::uint32_t> residentBlockIndices,
		const MotionMatchGpuGeneration& generation,
		MotionMatchGpuDispatchPlan& output) noexcept;

	// Queue methods are intended for renderer protected boundaries. Worker jobs may
	// prepare query/database snapshots, but they never call a Vulkan backend or
	// mutate live animation state through this class.
	class TERRAENGINE_API MotionMatchGpuQueue {
	public:
		explicit MotionMatchGpuQueue(std::shared_ptr<MotionMatchGpuBackend> backend = {}) noexcept;
		[[nodiscard]] AnimationStatus SetBackend(
			std::shared_ptr<MotionMatchGpuBackend> backend) noexcept;
		[[nodiscard]] AnimationStatus Submit(const MotionDatabaseAsset& database,
			const MotionMatchQuery& query, std::size_t residentBlockCount,
			MotionMatchGpuGeneration& generation, MotionMatchGpuDispatchPlan& plan) noexcept;
		[[nodiscard]] AnimationStatus Submit(const MotionDatabaseAsset& database,
			const MotionMatchQuery& query, std::span<const std::uint32_t> residentBlockIndices,
			MotionMatchGpuGeneration& generation, MotionMatchGpuDispatchPlan& plan) noexcept;
		// PollBackends never waits. Matching fences publish copied readbacks; stale
		// database/schema generations are cancelled before candidates become visible.
		[[nodiscard]] AnimationStatus PollBackends(
			AnimationAssetHandle activeDatabase,
			std::uint64_t activeSchemaFingerprint) noexcept;
		void Cancel(const MotionMatchGpuGeneration& generation) noexcept;
		[[nodiscard]] AnimationStatus TryConsume(const MotionMatchGpuGeneration& expected,
			MotionMatchGpuReadback& output) noexcept;
		[[nodiscard]] std::optional<MotionMatchGpuRequestState> State(
			const MotionMatchGpuGeneration& generation) const noexcept;
		[[nodiscard]] std::size_t PendingCount() const noexcept;

	private:
		struct Entry {
			MotionMatchGpuGeneration generation{};
			MotionMatchGpuBackendTicket backendTicket{};
			MotionMatchGpuRequestState state = MotionMatchGpuRequestState::Submitted;
			MotionMatchGpuReadback readback{};
		};
		mutable std::mutex mutex_;
		std::shared_ptr<MotionMatchGpuBackend> backend_;
		std::vector<Entry> entries_;
		std::uint64_t nextSequence_ = 1U;
	};
}
