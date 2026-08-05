// AnimationStreaming.h - bounded streaming decisions for cooked animation data.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "Core/Export.h"
#include "Utilities/UUID.h"

class ResourceManager;

namespace TerraEngine::Animation
{
	enum class AnimationStreamPayloadKind : std::uint8_t
	{
		ClipSegment,
		MotionDatabaseBlock
	};

	enum class AnimationStreamState : std::uint8_t
	{
		Missing,
		Requested,
		Resident,
		Failed,
		Cancelled
	};

	enum class AnimationStreamFallbackKind : std::uint8_t
	{
		ReferencePose,
		LastResidentGeneration,
		DeterministicMotionSearch
	};

	struct AnimationStreamRequest
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		AnimationStreamPayloadKind kind = AnimationStreamPayloadKind::ClipSegment;
		std::uint32_t blockIndex = 0u;
		std::uint64_t byteSize = 0u;
		std::uint64_t generation = 0u;
		bool gameplayCritical = false;
		bool prefetch = false;
		std::int32_t priority = 0;
		// The cooked logical path and range are immutable request data. They are
		// appended so existing aggregate call sites keep their serialized fields.
		std::filesystem::path logicalPath;
		std::uint64_t byteOffset = 0u;
		// The producer hashes the decoded segment bytes that this physical range
		// must contain. A replaced/corrupt loose file therefore cannot publish a
		// same-sized but unrelated payload into a live clip generation.
		std::uint64_t expectedContentFingerprint = 0u;
	};

	// The four numeric fields below identify one immutable payload generation.
	// Physical range metadata is validated after the indexed lookup, so hash
	// collisions or an incorrectly reused generation still fail closed.
	struct AnimationStreamLookupKey
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		AnimationStreamPayloadKind kind = AnimationStreamPayloadKind::ClipSegment;
		std::uint32_t blockIndex = 0u;
		std::uint64_t generation = 0u;

		[[nodiscard]] bool operator==(const AnimationStreamLookupKey&) const noexcept = default;
	};

	struct AnimationStreamLookupKeyHash
	{
		[[nodiscard]] std::size_t operator()(const AnimationStreamLookupKey& key) const noexcept
		{
			std::size_t hash = std::hash<TerraEngine::UUID>{}(key.assetId);
			const auto combine = [&hash](std::size_t value) {
				hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6u) + (hash >> 2u);
			};
			combine(std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(key.kind)));
			combine(std::hash<std::uint32_t>{}(key.blockIndex));
			combine(std::hash<std::uint64_t>{}(key.generation));
			return hash;
		}
	};

	[[nodiscard]] inline AnimationStreamLookupKey MakeAnimationStreamLookupKey(
		const AnimationStreamRequest& request) noexcept
	{
		return {request.assetId, request.kind, request.blockIndex, request.generation};
	}

	// A protected crowd reconciliation aggregates per-instance ownership into one
	// counted update per immutable payload. Criticality is optional because ordinary
	// retain changes must not republish an otherwise unchanged residency snapshot.
	struct AnimationStreamRetainAdjustment
	{
		AnimationStreamRequest request;
		std::int64_t retainDelta = 0;
		bool updateGameplayCritical = false;
		bool gameplayCritical = false;
	};

	struct AnimationStreamDecision
	{
		AnimationStreamRequest request;
		AnimationStreamState state = AnimationStreamState::Missing;
		AnimationStreamFallbackKind fallbackKind = AnimationStreamFallbackKind::ReferencePose;
		bool accepted = false;
		bool useSafeFallback = true;
		bool activeGeneration = false;
		std::uint32_t retainCount = 0u;
		std::uint64_t fallbackGeneration = 0u;
		std::uint64_t publicationSequence = 0u;
		std::string reason;
	};

	enum class AnimationStreamReadStatus : std::uint8_t
	{
		Ok,
		Rejected,
		Cancelled,
		NotFound,
		OutOfRange,
		ReadLimitExceeded,
		NotAnimationAsset,
		IoError
	};

	// A worker owns these bytes until the protected caller accepts the matching
	// generation through PublishReadResult. Failed and cancelled reads carry no
	// payload and leave the coordinator's deterministic fallback authoritative.
	struct AnimationStreamReadResult
	{
		AnimationStreamRequest request;
		AnimationStreamReadStatus status = AnimationStreamReadStatus::Rejected;
		std::vector<std::uint8_t> bytes;
		std::string reason;

		[[nodiscard]] bool Succeeded() const noexcept
		{
			return status == AnimationStreamReadStatus::Ok;
		}
	};

	struct AnimationStreamReadJob
	{
		AnimationStreamDecision decision;
		std::shared_future<AnimationStreamReadResult> completion;
	};

	// Resident bytes are immutable after protected publication. Frame capture
	// holds this shared snapshot while workers evaluate, so later IO completion,
	// cancellation, reload, or eviction cannot change an in-flight frame view.
	struct AnimationResidentPayload
	{
		AnimationStreamRequest request;
		std::shared_ptr<const std::vector<std::uint8_t>> bytes;
	};

	struct AnimationStreamingResidencySnapshot
	{
		std::uint64_t publicationSequence = 0u;
		std::vector<AnimationResidentPayload> payloads;
		// Readers execute during protected animator-binding reconciliation. This
		// immutable index turns each lookup from a full resident-payload scan into
		// one numeric hash probe followed by exact immutable-range validation.
		std::unordered_map<AnimationStreamLookupKey, std::size_t,
			AnimationStreamLookupKeyHash> payloadIndex;

		[[nodiscard]] const AnimationResidentPayload* Find(
			const AnimationStreamRequest& request) const noexcept;
	};

	// Stable FNV-1a is sufficient here because this is a generation-local guard
	// against wrong physical ranges, not the asset format's cryptographic hash.
	[[nodiscard]] TERRAENGINE_API std::uint64_t ComputeAnimationStreamContentFingerprint(
		std::span<const std::uint8_t> bytes) noexcept;

	struct AnimationStreamingDiagnostics
	{
		std::uint32_t trackedPayloadGenerations = 0u;
		std::uint32_t requestedPayloads = 0u;
		std::uint32_t residentPayloads = 0u;
		std::uint32_t pendingPrefetchPayloads = 0u;
		std::uint32_t retainedGameplayPayloads = 0u;
		std::uint32_t cancelledPayloads = 0u;
		std::uint32_t failedPayloads = 0u;
		std::uint64_t pendingBytes = 0u;
		std::uint64_t residentBytes = 0u;
		std::uint64_t protectedBytes = 0u;
		std::uint64_t rejectedRequests = 0u;
		std::uint64_t staleCompletions = 0u;
		std::uint64_t fallbackUses = 0u;
	};

	// The coordinator is a worker-safe preparation object. Backend IO consumes
	// the returned decisions at a protected boundary and stale completions are
	// rejected by generation, keeping visible pose data last-known-good.
	class TERRAENGINE_API AnimationStreamingCoordinator final
	{
	public:
		explicit AnimationStreamingCoordinator(
			std::uint32_t maximumPrefetchRequests = 8u,
			std::uint32_t maximumPendingRequests = 64u,
			std::uint64_t maximumPendingBytes = 64u * 1024u * 1024u,
			std::uint32_t maximumTrackedPayloadGenerations = 4096u);
		AnimationStreamDecision Request(const AnimationStreamRequest& request);
		// QueueRead dispatches bounded ContentMount/loose-editor IO through the
		// shared task pool. The future never captures this coordinator, so it stays
		// safe if an owning scene is destroyed while the read is pending.
		AnimationStreamReadJob QueueRead(
			::ResourceManager& resourceManager,
			const AnimationStreamRequest& request);
		// Call only at the declared animation streaming publication boundary.
		// A stale/cancelled generation is rejected without exposing its bytes.
		bool PublishReadResult(const AnimationStreamReadResult& result);
		// A prefetch window either queues every new block or queues none of them.
		// This avoids a partially advanced look-ahead horizon when the bounded
		// request/byte budget is exhausted halfway through a worker plan.
		std::vector<AnimationStreamDecision> RequestBatch(std::span<const AnimationStreamRequest> requests);
		bool Complete(const AnimationStreamRequest& request, bool success);
		bool Cancel(const AnimationStreamRequest& request);
		bool Retain(const AnimationStreamRequest& request);
		bool Release(const AnimationStreamRequest& request);
		// Apply a counted ownership delta under one coordinator lock. Crowd binding
		// reconciliation aggregates identical per-instance changes before calling
		// this method, avoiding one mutex acquisition and decision scan per animator.
		bool AdjustRetainCount(
			const AnimationStreamRequest& request,
			std::int64_t adjustment);
		// Validate the complete unique-payload batch before mutating any decision, then
		// publish at most one rebuilt residency snapshot for all criticality changes.
		bool ApplyRetainAdjustments(
			std::span<const AnimationStreamRetainAdjustment> adjustments);
		// Active bindings may promote or demote a shared payload as gameplay
		// consumers appear and disappear. Physical range identity is unchanged.
		bool SetGameplayCritical(
			const AnimationStreamRequest& request,
			bool gameplayCritical);
		std::vector<AnimationStreamDecision> ApplyResidentBudget(std::uint64_t residentBudgetBytes);
		[[nodiscard]] std::optional<AnimationStreamDecision> Query(const AnimationStreamRequest& request) const;
		[[nodiscard]] std::vector<AnimationStreamDecision> Snapshot() const;
		[[nodiscard]] std::shared_ptr<const AnimationStreamingResidencySnapshot>
			LoadResidencySnapshot() const;
		[[nodiscard]] AnimationStreamingDiagnostics BuildDiagnostics() const;

	private:
		[[nodiscard]] static std::string MakePayloadKey(const AnimationStreamRequest& request);
		[[nodiscard]] static std::string MakeGenerationKey(const AnimationStreamRequest& request);
		[[nodiscard]] AnimationStreamDecision* FindDecisionLocked(
			const AnimationStreamRequest& request) noexcept;
		[[nodiscard]] const AnimationStreamDecision* FindDecisionLocked(
			const AnimationStreamRequest& request) const noexcept;
		void RebuildDecisionIndexLocked();
		[[nodiscard]] AnimationStreamDecision RequestLocked(const AnimationStreamRequest& request);
		void SignalSupersededReadsLocked(const AnimationStreamRequest& request);
		void PruneTerminalDecisionsLocked();
		bool CompleteLocked(
			const AnimationStreamRequest& request,
			bool success,
			std::shared_ptr<const std::vector<std::uint8_t>> payload);
		void RebuildResidencySnapshotLocked();
		std::uint32_t m_maximumPrefetchRequests = 8u;
		std::uint32_t m_maximumPendingRequests = 64u;
		std::uint64_t m_maximumPendingBytes = 64u * 1024u * 1024u;
		std::uint32_t m_maximumTrackedPayloadGenerations = 4096u;
		std::uint64_t m_publicationSequence = 0u;
		std::uint64_t m_rejectedRequests = 0u;
		std::uint64_t m_staleCompletions = 0u;
		std::uint64_t m_fallbackUses = 0u;
		mutable std::mutex m_mutex;
		std::vector<AnimationStreamDecision> m_decisions;
		// The decision vector remains the deterministic diagnostics/publication
		// order. This companion index supplies O(1) generation lookup without
		// changing that stable external ordering contract.
		std::unordered_map<AnimationStreamLookupKey, std::size_t,
			AnimationStreamLookupKeyHash> m_decisionIndex;
		std::unordered_map<std::string, std::uint64_t> m_latestRequestedGenerations;
		std::unordered_map<std::string, std::uint64_t> m_activeGenerations;
		std::unordered_map<std::string, std::shared_ptr<std::atomic_bool>> m_cancellationTokens;
		std::unordered_map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>> m_residentPayloads;
		std::shared_ptr<const AnimationStreamingResidencySnapshot> m_residencySnapshot;
	};
}
