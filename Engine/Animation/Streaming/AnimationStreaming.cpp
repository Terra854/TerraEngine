// AnimationStreaming.cpp - bounded clip/database block request coordination.

#include "Animation/Streaming/AnimationStreaming.h"

#include "Resource/ResourceManager.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <limits>
#include <new>
#include <unordered_set>

namespace TerraEngine::Animation
{
	namespace
	{
		std::uint64_t AddSaturated(std::uint64_t left, std::uint64_t right) noexcept
		{
			return left > std::numeric_limits<std::uint64_t>::max() - right
				? std::numeric_limits<std::uint64_t>::max() : left + right;
		}

		bool IsTerminal(AnimationStreamState state) noexcept
		{
			return state == AnimationStreamState::Missing || state == AnimationStreamState::Failed
				|| state == AnimationStreamState::Cancelled;
		}

		AnimationStreamFallbackKind DefaultFallback(AnimationStreamPayloadKind kind) noexcept
		{
			return kind == AnimationStreamPayloadKind::ClipSegment
				? AnimationStreamFallbackKind::ReferencePose
				: AnimationStreamFallbackKind::DeterministicMotionSearch;
		}

		bool MatchesImmutableRequest(
			const AnimationStreamRequest& left,
			const AnimationStreamRequest& right) noexcept
		{
			return left.assetId == right.assetId && left.kind == right.kind
				&& left.blockIndex == right.blockIndex && left.byteSize == right.byteSize
				&& left.generation == right.generation
				&& left.prefetch == right.prefetch && left.priority == right.priority
				&& left.logicalPath.lexically_normal() == right.logicalPath.lexically_normal()
				&& left.byteOffset == right.byteOffset
				&& left.expectedContentFingerprint == right.expectedContentFingerprint;
		}

		AnimationStreamReadStatus ToStreamReadStatus(AnimationRangeReadStatus status) noexcept
		{
			switch (status)
			{
			case AnimationRangeReadStatus::Ok: return AnimationStreamReadStatus::Ok;
			case AnimationRangeReadStatus::Cancelled: return AnimationStreamReadStatus::Cancelled;
			case AnimationRangeReadStatus::NotFound: return AnimationStreamReadStatus::NotFound;
			case AnimationRangeReadStatus::OutOfRange: return AnimationStreamReadStatus::OutOfRange;
			case AnimationRangeReadStatus::ReadLimitExceeded: return AnimationStreamReadStatus::ReadLimitExceeded;
			case AnimationRangeReadStatus::NotAnimationAsset: return AnimationStreamReadStatus::NotAnimationAsset;
			case AnimationRangeReadStatus::InvalidRequest: return AnimationStreamReadStatus::Rejected;
			case AnimationRangeReadStatus::IoError: return AnimationStreamReadStatus::IoError;
			}
			return AnimationStreamReadStatus::IoError;
		}
	}

	std::uint64_t ComputeAnimationStreamContentFingerprint(
		std::span<const std::uint8_t> bytes) noexcept
	{
		std::uint64_t hash = 1469598103934665603ULL;
		for(const std::uint8_t value : bytes) {
			hash ^= value;
			hash *= 1099511628211ULL;
		}
		return hash;
	}

	const AnimationResidentPayload* AnimationStreamingResidencySnapshot::Find(
		const AnimationStreamRequest& request) const noexcept
	{
		const auto indexed = payloadIndex.find(MakeAnimationStreamLookupKey(request));
		if(indexed == payloadIndex.end() || indexed->second >= payloads.size()) return nullptr;
		const AnimationResidentPayload& payload = payloads[indexed->second];
		return MatchesImmutableRequest(payload.request, request) ? &payload : nullptr;
	}

	AnimationStreamingCoordinator::AnimationStreamingCoordinator(
		std::uint32_t maximumPrefetchRequests,
		std::uint32_t maximumPendingRequests,
		std::uint64_t maximumPendingBytes,
		std::uint32_t maximumTrackedPayloadGenerations)
		: m_maximumPrefetchRequests(std::max(1u, maximumPrefetchRequests)),
		m_maximumPendingRequests(std::max(1u, maximumPendingRequests)),
		m_maximumPendingBytes(std::max<std::uint64_t>(1u, maximumPendingBytes)),
		m_maximumTrackedPayloadGenerations(std::max(1u, maximumTrackedPayloadGenerations))
	{
		m_residencySnapshot = std::make_shared<AnimationStreamingResidencySnapshot>();
	}

	std::string AnimationStreamingCoordinator::MakePayloadKey(const AnimationStreamRequest& request)
	{
		return TerraEngine::ToHexString(request.assetId) + ":"
			+ std::to_string(static_cast<unsigned>(request.kind)) + ":"
			+ std::to_string(request.blockIndex);
	}

	std::string AnimationStreamingCoordinator::MakeGenerationKey(const AnimationStreamRequest& request)
	{
		return MakePayloadKey(request) + ":" + std::to_string(request.generation);
	}

	AnimationStreamDecision* AnimationStreamingCoordinator::FindDecisionLocked(
		const AnimationStreamRequest& request) noexcept
	{
		const auto indexed = m_decisionIndex.find(MakeAnimationStreamLookupKey(request));
		if(indexed == m_decisionIndex.end() || indexed->second >= m_decisions.size()) return nullptr;
		return &m_decisions[indexed->second];
	}

	const AnimationStreamDecision* AnimationStreamingCoordinator::FindDecisionLocked(
		const AnimationStreamRequest& request) const noexcept
	{
		const auto indexed = m_decisionIndex.find(MakeAnimationStreamLookupKey(request));
		if(indexed == m_decisionIndex.end() || indexed->second >= m_decisions.size()) return nullptr;
		return &m_decisions[indexed->second];
	}

	void AnimationStreamingCoordinator::RebuildDecisionIndexLocked()
	{
		m_decisionIndex.clear();
		m_decisionIndex.reserve(m_decisions.size());
		for(std::size_t index = 0u; index < m_decisions.size(); ++index) {
			m_decisionIndex.insert_or_assign(
				MakeAnimationStreamLookupKey(m_decisions[index].request), index);
		}
	}

	void AnimationStreamingCoordinator::PruneTerminalDecisionsLocked()
	{
		if (m_decisions.size() < m_maximumTrackedPayloadGenerations)
		{
			return;
		}
		std::stable_sort(m_decisions.begin(), m_decisions.end(), [](const auto& left, const auto& right) {
			if (IsTerminal(left.state) != IsTerminal(right.state))
			{
				return IsTerminal(left.state);
			}
			if (left.publicationSequence != right.publicationSequence)
			{
				return left.publicationSequence < right.publicationSequence;
			}
			return MakeGenerationKey(left.request) < MakeGenerationKey(right.request);
		});
		while (m_decisions.size() >= m_maximumTrackedPayloadGenerations
			&& !m_decisions.empty() && IsTerminal(m_decisions.front().state))
		{
			const std::string payloadKey = MakePayloadKey(m_decisions.front().request);
			m_residentPayloads.erase(MakeGenerationKey(m_decisions.front().request));
			m_cancellationTokens.erase(MakeGenerationKey(m_decisions.front().request));
			m_decisions.erase(m_decisions.begin());
			const bool payloadStillTracked = std::any_of(m_decisions.begin(), m_decisions.end(),
				[&](const AnimationStreamDecision& decision) {
					return MakePayloadKey(decision.request) == payloadKey;
				});
			if (!payloadStillTracked)
			{
				m_latestRequestedGenerations.erase(payloadKey);
				m_activeGenerations.erase(payloadKey);
			}
		}
		// Sorting and front erasure both change vector indices even when the
		// remaining decision contents are untouched.
		RebuildDecisionIndexLocked();
	}

	void AnimationStreamingCoordinator::RebuildResidencySnapshotLocked()
	{
		auto snapshot = std::make_shared<AnimationStreamingResidencySnapshot>();
		snapshot->publicationSequence = m_publicationSequence;
		snapshot->payloads.reserve(m_residentPayloads.size());
		for(const AnimationStreamDecision& decision : m_decisions) {
			if(decision.state != AnimationStreamState::Resident) continue;
			const auto payload = m_residentPayloads.find(MakeGenerationKey(decision.request));
			if(payload == m_residentPayloads.end() || !payload->second) continue;
			snapshot->payloads.push_back({decision.request, payload->second});
		}
		std::sort(snapshot->payloads.begin(), snapshot->payloads.end(),
			[](const AnimationResidentPayload& left, const AnimationResidentPayload& right) {
				return MakeGenerationKey(left.request) < MakeGenerationKey(right.request);
			});
		snapshot->payloadIndex.reserve(snapshot->payloads.size());
		for(std::size_t index = 0u; index < snapshot->payloads.size(); ++index) {
			snapshot->payloadIndex.insert_or_assign(
				MakeAnimationStreamLookupKey(snapshot->payloads[index].request), index);
		}
		m_residencySnapshot = std::move(snapshot);
	}

	void AnimationStreamingCoordinator::SignalSupersededReadsLocked(
		const AnimationStreamRequest& request)
	{
		const std::string payloadKey = MakePayloadKey(request);
		for (const AnimationStreamDecision& decision : m_decisions)
		{
			if (MakePayloadKey(decision.request) != payloadKey
				|| decision.request.generation >= request.generation
				|| decision.state != AnimationStreamState::Cancelled)
			{
				continue;
			}
			if (const auto token = m_cancellationTokens.find(MakeGenerationKey(decision.request));
				token != m_cancellationTokens.end())
			{
				token->second->store(true, std::memory_order_release);
			}
		}
	}

	AnimationStreamDecision AnimationStreamingCoordinator::RequestLocked(const AnimationStreamRequest& request)
	{
		AnimationStreamDecision result;
		result.request = request;
		result.fallbackKind = DefaultFallback(request.kind);
		if (request.assetId == TerraEngine::EMPTY_UUID || request.generation == 0u || request.byteSize == 0u)
		{
			++m_rejectedRequests;
			result.reason = "A streaming request requires a non-empty asset, nonzero generation, and nonzero payload size.";
			return result;
		}

		const std::string payloadKey = MakePayloadKey(request);
		const std::string generationKey = MakeGenerationKey(request);
		if (const auto latest = m_latestRequestedGenerations.find(payloadKey);
			latest != m_latestRequestedGenerations.end() && request.generation < latest->second)
		{
			++m_rejectedRequests;
			result.reason = "A newer payload generation already owns this streaming request.";
			return result;
		}

		AnimationStreamDecision* existing = FindDecisionLocked(request);
		AnimationStreamDecision* evictedRetry = nullptr;
		if (existing != nullptr)
		{
			const bool identical = MatchesImmutableRequest(existing->request, request);
			if (!identical)
			{
				++m_rejectedRequests;
				result.reason = "The same payload generation was reused with different immutable request data.";
				return result;
			}
			if(existing->state == AnimationStreamState::Missing) {
				// Missing is the recoverable terminal state produced only by budget
				// eviction. Requeueing the exact immutable generation lets an active
				// consumer reactivate it without inventing an asset reload generation.
				evictedRetry = existing;
			} else {
				result = *existing;
				result.accepted = !IsTerminal(existing->state);
				if(result.accepted) {
					result.reason = "Idempotent streaming request joined the existing work.";
				} else if(existing->state == AnimationStreamState::Failed) {
					result.reason = "A failed payload requires a newer asset generation before retry.";
				} else {
					result.reason = "A cancelled payload requires a newer asset generation before retry.";
				}
				return result;
			}
		}

		// Newer work makes older pending IO incapable of publication. Transition it
		// to a terminal state now so pending-count/byte bounds describe work that
		// can still complete instead of leaking superseded requests indefinitely.
		for (AnimationStreamDecision& decision : m_decisions)
		{
			if (MakePayloadKey(decision.request) == payloadKey
				&& decision.request.generation < request.generation
				&& decision.state == AnimationStreamState::Requested)
			{
				decision.state = AnimationStreamState::Cancelled;
				decision.accepted = false;
				decision.useSafeFallback = true;
				decision.publicationSequence = ++m_publicationSequence;
				decision.reason = "Superseded by a newer payload generation before publication.";
				++m_fallbackUses;
			}
		}

		std::uint32_t pendingCount = 0u;
		std::uint32_t pendingPrefetchCount = 0u;
		std::uint64_t pendingBytes = 0u;
		for (const AnimationStreamDecision& value : m_decisions)
		{
			if (value.state != AnimationStreamState::Requested)
			{
				continue;
			}
			++pendingCount;
			pendingBytes = AddSaturated(pendingBytes, value.request.byteSize);
			pendingPrefetchCount += value.request.prefetch ? 1u : 0u;
		}
		if (pendingCount >= m_maximumPendingRequests
			|| request.byteSize > m_maximumPendingBytes - std::min(pendingBytes, m_maximumPendingBytes))
		{
			++m_rejectedRequests;
			result.reason = "The bounded streaming request or byte budget is full; safe fallback remains active.";
			return result;
		}
		if (request.prefetch && pendingPrefetchCount >= m_maximumPrefetchRequests)
		{
			++m_rejectedRequests;
			result.reason = "The bounded prefetch horizon is full; demand requests retain priority.";
			return result;
		}

		if(evictedRetry != nullptr) {
			result = *evictedRetry;
			result.request = request;
			result.state = AnimationStreamState::Requested;
			result.accepted = true;
			result.useSafeFallback = true;
			result.activeGeneration = false;
			result.fallbackGeneration = 0U;
			result.publicationSequence = ++m_publicationSequence;
			result.reason = "An exact same-generation payload was requeued after budget eviction.";
			*evictedRetry = result;
			m_latestRequestedGenerations[payloadKey] = request.generation;
			// QueueRead installs a new token after Request returns. Removing any
			// historical entry here guarantees cancellation controls only fresh IO.
			m_cancellationTokens.erase(generationKey);
			++m_fallbackUses;
			return result;
		}

		PruneTerminalDecisionsLocked();
		if (m_decisions.size() >= m_maximumTrackedPayloadGenerations)
		{
			++m_rejectedRequests;
			result.reason = "The bounded streaming diagnostics/history table is full.";
			return result;
		}

		if (const auto active = m_activeGenerations.find(payloadKey); active != m_activeGenerations.end())
		{
			result.fallbackKind = AnimationStreamFallbackKind::LastResidentGeneration;
			result.fallbackGeneration = active->second;
		}
		result.state = AnimationStreamState::Requested;
		result.accepted = true;
		result.useSafeFallback = true;
		result.publicationSequence = ++m_publicationSequence;
		result.reason = "Queued on the asynchronous IO path; fallback remains authoritative until protected publication.";
		m_latestRequestedGenerations[payloadKey] = request.generation;
		m_decisions.push_back(result);
		m_decisionIndex.insert_or_assign(
			MakeAnimationStreamLookupKey(request), m_decisions.size() - 1u);
		++m_fallbackUses;
		return result;
	}

	AnimationStreamDecision AnimationStreamingCoordinator::Request(const AnimationStreamRequest& request)
	{
		std::scoped_lock lock(m_mutex);
		AnimationStreamDecision result = RequestLocked(request);
		if (result.accepted)
		{
			SignalSupersededReadsLocked(request);
		}
		return result;
	}

	AnimationStreamReadJob AnimationStreamingCoordinator::QueueRead(
		::ResourceManager& resourceManager,
		const AnimationStreamRequest& request)
	{
		AnimationStreamReadJob job;
		job.decision = Request(request);
		std::shared_ptr<std::atomic_bool> cancellation = std::make_shared<std::atomic_bool>(false);
		if (job.decision.accepted)
		{
			std::scoped_lock lock(m_mutex);
			const AnimationStreamDecision* found = FindDecisionLocked(request);
			if (found != nullptr && found->state == AnimationStreamState::Requested)
			{
				auto [token, inserted] = m_cancellationTokens.try_emplace(
					MakeGenerationKey(request), cancellation);
				cancellation = token->second;
				(void)inserted;
			}
			else
			{
				cancellation->store(true, std::memory_order_release);
			}
		}
		else
		{
			cancellation->store(true, std::memory_order_release);
		}

		const bool requestAccepted = job.decision.accepted;
		auto read = [&resourceManager, request, cancellation, requestAccepted]() {
			AnimationStreamReadResult result;
			result.request = request;
			if (cancellation->load(std::memory_order_acquire))
			{
				result.status = requestAccepted
					? AnimationStreamReadStatus::Cancelled : AnimationStreamReadStatus::Rejected;
				result.reason = "Streaming IO was rejected or cancelled before the worker read began.";
				return result;
			}
			if (request.logicalPath.empty())
			{
				result.status = AnimationStreamReadStatus::NotAnimationAsset;
				result.reason = "A streaming IO request requires a cooked animation logical path.";
				return result;
			}

			AnimationRangeReadResult range = resourceManager.ReadAnimationAssetRange(
				request.logicalPath, request.byteOffset, request.byteSize, request.byteSize);
			if (cancellation->load(std::memory_order_acquire))
			{
				range.bytes.clear();
				result.status = AnimationStreamReadStatus::Cancelled;
				result.reason = "Cancellation won after IO and before protected animation publication.";
				return result;
			}
			result.status = ToStreamReadStatus(range.status);
			result.bytes = std::move(range.bytes);
			result.reason = std::move(range.message);
			return result;
		};

		// Engine startup normally owns worker creation. A zero-worker test/tool
		// session executes the same bounded callable inline instead of returning a
		// future that can never become ready.
		if (ThreadPool::instance().getThreadCount() == 0u)
		{
			std::promise<AnimationStreamReadResult> ready;
			ready.set_value(read());
			job.completion = ready.get_future().share();
			return job;
		}
		auto task = ThreadPool::instance().createTask(std::move(read));
		job.completion = task->getFuture().share();
		ThreadPool::instance().enqueue(task, 1u);
		return job;
	}

	bool AnimationStreamingCoordinator::PublishReadResult(
		const AnimationStreamReadResult& result)
	{
		const AnimationStreamReadResult* resultPointer = &result;
		return PublishReadResults(
			std::span<const AnimationStreamReadResult* const>(&resultPointer, 1U)) == 1U;
	}

	std::size_t AnimationStreamingCoordinator::PublishReadResults(
		std::span<const AnimationStreamReadResult* const> results)
	{
		struct PreparedPublication final
		{
			const AnimationStreamRequest* request = nullptr;
			bool complete = false;
			std::shared_ptr<const std::vector<std::uint8_t>> payload;
		};

		// Validate and allocate immutable payload storage before taking the
		// coordinator lock. Only the short deterministic state transition and one
		// final snapshot rebuild remain inside the protected boundary.
		std::vector<PreparedPublication> prepared;
		prepared.reserve(results.size());
		for(const AnimationStreamReadResult* result : results) {
			if(result == nullptr
				|| result->status == AnimationStreamReadStatus::Cancelled
				|| result->status == AnimationStreamReadStatus::Rejected) {
				continue;
			}
			const bool sizeMatches = result->Succeeded()
				&& result->bytes.size() == result->request.byteSize;
			const bool fingerprintMatches = sizeMatches
				&& (result->request.expectedContentFingerprint == 0U
					|| ComputeAnimationStreamContentFingerprint(result->bytes)
						== result->request.expectedContentFingerprint);
			PreparedPublication publication;
			publication.request = &result->request;
			if(fingerprintMatches) {
				try {
					publication.payload =
						std::make_shared<const std::vector<std::uint8_t>>(result->bytes);
				} catch(const std::bad_alloc&) {
					publication.payload.reset();
				}
			}
			publication.complete = fingerprintMatches && publication.payload != nullptr;
			prepared.push_back(std::move(publication));
		}

		std::size_t successfulPublications = 0U;
		bool residencyChanged = false;
		std::scoped_lock lock(m_mutex);
		for(PreparedPublication& publication : prepared) {
			const bool published = CompleteLocked(
				*publication.request,
				publication.complete,
				std::move(publication.payload),
				false);
			if(!published) continue;
			m_cancellationTokens.erase(MakeGenerationKey(*publication.request));
			if(publication.complete) {
				++successfulPublications;
				residencyChanged = true;
			}
		}
		if(residencyChanged) RebuildResidencySnapshotLocked();
		return successfulPublications;
	}

	std::vector<AnimationStreamDecision> AnimationStreamingCoordinator::RequestBatch(
		std::span<const AnimationStreamRequest> requests)
	{
		std::scoped_lock lock(m_mutex);
		if (requests.empty())
		{
			return {};
		}
		if (requests.size() > m_maximumPendingRequests)
		{
			m_rejectedRequests += requests.size();
			std::vector<AnimationStreamDecision> rejected;
			rejected.reserve(requests.size());
			for (const AnimationStreamRequest& request : requests)
			{
				AnimationStreamDecision decision;
				decision.request = request;
				decision.fallbackKind = DefaultFallback(request.kind);
				decision.reason = "The atomic streaming batch exceeds the hard request bound.";
				rejected.push_back(std::move(decision));
			}
			return rejected;
		}

		const auto previousDecisions = m_decisions;
		const auto previousLatest = m_latestRequestedGenerations;
		const auto previousActive = m_activeGenerations;
		const std::uint64_t previousSequence = m_publicationSequence;
		const std::uint64_t previousRejected = m_rejectedRequests;
		const std::uint64_t previousFallbackUses = m_fallbackUses;
		std::vector<AnimationStreamDecision> results;
		results.reserve(requests.size());
		bool accepted = true;
		std::unordered_set<std::string> identities;
		for (const AnimationStreamRequest& request : requests)
		{
			if (!identities.insert(MakeGenerationKey(request)).second)
			{
				AnimationStreamDecision duplicate;
				duplicate.request = request;
				duplicate.reason = "An atomic streaming batch cannot contain duplicate payload generations.";
				results.push_back(std::move(duplicate));
				accepted = false;
				break;
			}
			results.push_back(RequestLocked(request));
			if (!results.back().accepted)
			{
				accepted = false;
				break;
			}
		}
		if (accepted)
		{
			for (const AnimationStreamRequest& request : requests)
			{
				SignalSupersededReadsLocked(request);
			}
			return results;
		}

		m_decisions = previousDecisions;
		RebuildDecisionIndexLocked();
		m_latestRequestedGenerations = previousLatest;
		m_activeGenerations = previousActive;
		m_publicationSequence = previousSequence;
		m_fallbackUses = previousFallbackUses;
		m_rejectedRequests = previousRejected + requests.size();
		results.clear();
		for (const AnimationStreamRequest& request : requests)
		{
			AnimationStreamDecision rejected;
			rejected.request = request;
			rejected.fallbackKind = DefaultFallback(request.kind);
			rejected.reason = "The atomic streaming batch was rejected without queuing partial work.";
			results.push_back(std::move(rejected));
		}
		return results;
	}

	bool AnimationStreamingCoordinator::CompleteLocked(
		const AnimationStreamRequest& request,
		bool success,
		std::shared_ptr<const std::vector<std::uint8_t>> payload,
		bool rebuildResidencySnapshot)
	{
		const std::string payloadKey = MakePayloadKey(request);
		const std::string generationKey = MakeGenerationKey(request);
		AnimationStreamDecision* found = FindDecisionLocked(request);
		const auto latest = m_latestRequestedGenerations.find(payloadKey);
		if (found == nullptr || found->state != AnimationStreamState::Requested
			|| !MatchesImmutableRequest(found->request, request)
			|| latest == m_latestRequestedGenerations.end() || latest->second != request.generation)
		{
			++m_staleCompletions;
			return false;
		}

		found->publicationSequence = ++m_publicationSequence;
		if (!success)
		{
			m_residentPayloads.erase(generationKey);
			found->state = AnimationStreamState::Failed;
			found->accepted = false;
			found->useSafeFallback = true;
			found->reason = "IO or validation failed; the prior resident block or deterministic fallback remains active.";
			++m_fallbackUses;
			return true;
		}

		if (const auto active = m_activeGenerations.find(payloadKey); active != m_activeGenerations.end())
		{
			const std::uint64_t previousGeneration = active->second;
			AnimationStreamRequest previousRequest = request;
			previousRequest.generation = previousGeneration;
			if(AnimationStreamDecision* previous = FindDecisionLocked(previousRequest)) {
				previous->activeGeneration = false;
			}
		}
		m_activeGenerations[payloadKey] = request.generation;
		found->state = AnimationStreamState::Resident;
		found->accepted = true;
		found->useSafeFallback = false;
		found->activeGeneration = true;
		found->fallbackGeneration = 0u;
		found->reason = "Complete block published at the protected animation streaming boundary.";
		if(payload) {
			m_residentPayloads[generationKey] = std::move(payload);
			if(rebuildResidencySnapshot) RebuildResidencySnapshotLocked();
		}
		return true;
	}

	bool AnimationStreamingCoordinator::Complete(const AnimationStreamRequest& request, bool success)
	{
		std::scoped_lock lock(m_mutex);
		return CompleteLocked(request, success, {});
	}

	bool AnimationStreamingCoordinator::Cancel(const AnimationStreamRequest& request)
	{
		std::scoped_lock lock(m_mutex);
		AnimationStreamDecision* found = FindDecisionLocked(request);
		if (found == nullptr || found->state != AnimationStreamState::Requested
			|| !MatchesImmutableRequest(found->request, request))
		{
			return false;
		}
		found->state = AnimationStreamState::Cancelled;
		found->accepted = false;
		found->useSafeFallback = true;
		found->publicationSequence = ++m_publicationSequence;
		found->reason = "Cancellation won before protected publication; stale completion will be rejected.";
		if (const auto token = m_cancellationTokens.find(MakeGenerationKey(request));
			token != m_cancellationTokens.end())
		{
			token->second->store(true, std::memory_order_release);
			m_cancellationTokens.erase(token);
		}
		++m_fallbackUses;
		return true;
	}

	bool AnimationStreamingCoordinator::Retain(const AnimationStreamRequest& request)
	{
		return AdjustRetainCount(request, 1);
	}

	bool AnimationStreamingCoordinator::Release(const AnimationStreamRequest& request)
	{
		return AdjustRetainCount(request, -1);
	}

	bool AnimationStreamingCoordinator::AdjustRetainCount(
		const AnimationStreamRequest& request,
		std::int64_t adjustment)
	{
		if(adjustment == 0) return true;
		std::scoped_lock lock(m_mutex);
		AnimationStreamDecision* found = FindDecisionLocked(request);
		if (found == nullptr || !MatchesImmutableRequest(found->request, request)
			|| (adjustment > 0 && IsTerminal(found->state)))
		{
			return false;
		}
		const std::int64_t current = found->retainCount;
		if(adjustment < -current
			|| adjustment > static_cast<std::int64_t>(
				std::numeric_limits<std::uint32_t>::max()) - current) {
			return false;
		}
		found->retainCount = static_cast<std::uint32_t>(current + adjustment);
		return true;
	}

	bool AnimationStreamingCoordinator::ApplyRetainAdjustments(
		std::span<const AnimationStreamRetainAdjustment> adjustments)
	{
		if(adjustments.empty()) return true;
		std::scoped_lock lock(m_mutex);
		struct ValidatedAdjustment
		{
			AnimationStreamDecision* decision = nullptr;
			const AnimationStreamRetainAdjustment* adjustment = nullptr;
			std::string generationKey;
		};
		std::vector<ValidatedAdjustment> validated;
		std::unordered_set<std::string> uniqueKeys;
		try {
			validated.reserve(adjustments.size());
			uniqueKeys.reserve(adjustments.size());
			for(const AnimationStreamRetainAdjustment& adjustment : adjustments) {
				std::string generationKey = MakeGenerationKey(adjustment.request);
				if(!uniqueKeys.insert(generationKey).second) return false;
				AnimationStreamDecision* found = FindDecisionLocked(adjustment.request);
				if(found == nullptr
					|| !MatchesImmutableRequest(found->request, adjustment.request)
					|| (adjustment.retainDelta > 0 && IsTerminal(found->state))) {
					return false;
				}
				const std::int64_t current = found->retainCount;
				if(adjustment.retainDelta < -current
					|| adjustment.retainDelta > static_cast<std::int64_t>(
						std::numeric_limits<std::uint32_t>::max()) - current) {
					return false;
				}
				validated.push_back({found, &adjustment, std::move(generationKey)});
			}
		} catch(const std::bad_alloc&) {
			return false;
		}

		bool rebuildSnapshot = false;
		for(const ValidatedAdjustment& item : validated) {
			AnimationStreamDecision& decision = *item.decision;
			const AnimationStreamRetainAdjustment& adjustment = *item.adjustment;
			decision.retainCount = static_cast<std::uint32_t>(
				static_cast<std::int64_t>(decision.retainCount)
					+ adjustment.retainDelta);
			if(!adjustment.updateGameplayCritical
				|| decision.request.gameplayCritical == adjustment.gameplayCritical) {
				continue;
			}
			decision.request.gameplayCritical = adjustment.gameplayCritical;
			decision.publicationSequence = ++m_publicationSequence;
			rebuildSnapshot = rebuildSnapshot
				|| (decision.state == AnimationStreamState::Resident
					&& m_residentPayloads.contains(item.generationKey));
		}
		if(rebuildSnapshot) RebuildResidencySnapshotLocked();
		return true;
	}

	bool AnimationStreamingCoordinator::SetGameplayCritical(
		const AnimationStreamRequest& request,
		bool gameplayCritical)
	{
		std::scoped_lock lock(m_mutex);
		AnimationStreamDecision* found = FindDecisionLocked(request);
		if(found == nullptr || !MatchesImmutableRequest(found->request, request)) {
			return false;
		}
		if(found->request.gameplayCritical == gameplayCritical) return true;
		found->request.gameplayCritical = gameplayCritical;
		found->publicationSequence = ++m_publicationSequence;
		if(found->state == AnimationStreamState::Resident
			&& m_residentPayloads.contains(MakeGenerationKey(found->request))) {
			RebuildResidencySnapshotLocked();
		}
		return true;
	}

	std::vector<AnimationStreamDecision> AnimationStreamingCoordinator::ApplyResidentBudget(
		std::uint64_t residentBudgetBytes)
	{
		std::scoped_lock lock(m_mutex);
		std::uint64_t residentBytes = 0u;
		for (AnimationStreamDecision& decision : m_decisions)
		{
			if (decision.state != AnimationStreamState::Resident)
			{
				continue;
			}
			residentBytes = AddSaturated(residentBytes, decision.request.byteSize);
		}
		// A settled crowd normally remains below its fixed residency budget. Do not
		// allocate and sort the complete eviction-candidate list unless pressure is
		// actually present; this path runs on the protected animation boundary.
		if(residentBytes <= residentBudgetBytes) return {};
		std::vector<AnimationStreamDecision*> candidates;
		candidates.reserve(m_decisions.size());
		for(AnimationStreamDecision& decision : m_decisions) {
			if(decision.state == AnimationStreamState::Resident
				&& !decision.request.gameplayCritical && decision.retainCount == 0U) {
				candidates.push_back(&decision);
			}
		}
		std::sort(candidates.begin(), candidates.end(), [](const auto* left, const auto* right) {
			if (left->activeGeneration != right->activeGeneration) return !left->activeGeneration;
			if (left->request.priority != right->request.priority) return left->request.priority < right->request.priority;
			if (left->publicationSequence != right->publicationSequence) return left->publicationSequence < right->publicationSequence;
			return MakeGenerationKey(left->request) < MakeGenerationKey(right->request);
		});
		std::vector<AnimationStreamDecision> released;
		bool snapshotChanged = false;
		for (AnimationStreamDecision* decision : candidates)
		{
			if (residentBytes <= residentBudgetBytes) break;
			residentBytes = residentBytes > decision->request.byteSize
				? residentBytes - decision->request.byteSize : 0u;
			if (decision->activeGeneration)
			{
				m_activeGenerations.erase(MakePayloadKey(decision->request));
				decision->activeGeneration = false;
			}
			decision->state = AnimationStreamState::Missing;
			decision->accepted = false;
			decision->useSafeFallback = true;
			decision->fallbackKind = DefaultFallback(decision->request.kind);
			decision->publicationSequence = ++m_publicationSequence;
			decision->reason = "Resident block released under pressure; bounded safe fallback is active.";
			m_residentPayloads.erase(MakeGenerationKey(decision->request));
			snapshotChanged = true;
			released.push_back(*decision);
			++m_fallbackUses;
		}
		if(snapshotChanged) RebuildResidencySnapshotLocked();
		return released;
	}

	std::optional<AnimationStreamDecision> AnimationStreamingCoordinator::Query(
		const AnimationStreamRequest& request) const
	{
		std::scoped_lock lock(m_mutex);
		const AnimationStreamDecision* found = FindDecisionLocked(request);
		return found == nullptr || !MatchesImmutableRequest(found->request, request)
			? std::optional<AnimationStreamDecision>{}
			: std::optional<AnimationStreamDecision>{*found};
	}

	std::vector<AnimationStreamDecision> AnimationStreamingCoordinator::Snapshot() const
	{
		std::scoped_lock lock(m_mutex);
		std::vector<AnimationStreamDecision> copy = m_decisions;
		std::sort(copy.begin(), copy.end(), [](const auto& left, const auto& right) {
			return MakeGenerationKey(left.request) < MakeGenerationKey(right.request);
		});
		return copy;
	}

	std::shared_ptr<const AnimationStreamingResidencySnapshot>
	AnimationStreamingCoordinator::LoadResidencySnapshot() const
	{
		std::scoped_lock lock(m_mutex);
		return m_residencySnapshot;
	}

	AnimationStreamingDiagnostics AnimationStreamingCoordinator::BuildDiagnostics() const
	{
		std::scoped_lock lock(m_mutex);
		AnimationStreamingDiagnostics diagnostics;
		diagnostics.trackedPayloadGenerations = static_cast<std::uint32_t>(m_decisions.size());
		diagnostics.rejectedRequests = m_rejectedRequests;
		diagnostics.staleCompletions = m_staleCompletions;
		diagnostics.fallbackUses = m_fallbackUses;
		for (const AnimationStreamDecision& decision : m_decisions)
		{
			diagnostics.retainedPayloads += decision.retainCount != 0U ? 1U : 0U;
			switch (decision.state)
			{
			case AnimationStreamState::Requested:
				++diagnostics.requestedPayloads;
				diagnostics.pendingBytes = AddSaturated(diagnostics.pendingBytes, decision.request.byteSize);
				diagnostics.pendingPrefetchPayloads += decision.request.prefetch ? 1u : 0u;
				break;
			case AnimationStreamState::Resident:
				++diagnostics.residentPayloads;
				diagnostics.residentBytes = AddSaturated(diagnostics.residentBytes, decision.request.byteSize);
				break;
			case AnimationStreamState::Failed:
				++diagnostics.failedPayloads;
				break;
			case AnimationStreamState::Cancelled:
				++diagnostics.cancelledPayloads;
				break;
			case AnimationStreamState::Missing:
				break;
			}
			if ((decision.request.gameplayCritical || decision.retainCount != 0u)
				&& (decision.state == AnimationStreamState::Resident || decision.state == AnimationStreamState::Requested))
			{
				++diagnostics.retainedGameplayPayloads;
				diagnostics.protectedBytes = AddSaturated(diagnostics.protectedBytes, decision.request.byteSize);
			}
		}
		return diagnostics;
	}
}
