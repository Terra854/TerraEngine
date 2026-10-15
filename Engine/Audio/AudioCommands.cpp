// AudioCommands.cpp - staged admission and terminal ownership for audio batches.

#include "AudioCommands.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <utility>

namespace TerraEngine::Audio {
	AudioControlQueue::AudioControlQueue(AudioControlQueueLimits limits)
		: m_Limits(limits) {
		// Normalize unusable zero bounds; completed history bounds only terminal
		// keyed requests, while every pending request keeps its own live charge.
		m_Limits.maximumQueuedCommands = std::max<std::size_t>(1U, m_Limits.maximumQueuedCommands);
		m_Limits.maximumProducerCommands = std::max<std::size_t>(1U, m_Limits.maximumProducerCommands);
		m_Limits.maximumCommandsPerBatch = std::max<std::size_t>(1U, m_Limits.maximumCommandsPerBatch);
		m_Limits.maximumMetadataBytes = std::max<std::size_t>(1U, m_Limits.maximumMetadataBytes);
		m_Limits.maximumRetainedIdempotencyKeys = std::max<std::size_t>(1U, m_Limits.maximumRetainedIdempotencyKeys);
	}

	AudioControlReceipt AudioControlQueue::Reject(const AudioControlBatch& batch,
		AudioControlRejection reason, std::string diagnostic) const {
		AudioControlReceipt receipt;
		receipt.batchSequence = batch.batchSequence;
		receipt.requestedFrame = batch.requestedFrame;
		receipt.status = AudioControlStatus::Rejected;
		receipt.rejection = reason;
		receipt.correlationId = batch.correlationId;
		receipt.diagnostic = std::move(diagnostic);
		return receipt;
	}

	bool AudioControlQueue::Equivalent(const AudioControlBatch& left, const AudioControlBatch& right) noexcept {
		if(left.requestedFrame != right.requestedFrame || left.requestFingerprint != right.requestFingerprint
			|| left.commands.size() != right.commands.size()) return false;
		for(std::size_t index = 0U; index < left.commands.size(); ++index) {
			const auto& a = left.commands[index];
			const auto& b = right.commands[index];
			// Compare original typed values rather than trusting a caller-provided
			// fingerprint. Bit comparison also gives NaNs a stable retry signature.
			if(a.kind != b.kind || a.targetMode != b.targetMode || a.source.objectId != b.source.objectId
				|| a.source.sceneGeneration != b.source.sceneGeneration
				|| a.source.objectGeneration != b.source.objectGeneration
				|| a.source.componentGeneration != b.source.componentGeneration
				|| a.source.playbackGeneration != b.source.playbackGeneration
				|| a.mixGroupId != b.mixGroupId || a.booleanValue != b.booleanValue
				|| std::bit_cast<std::uint32_t>(a.scalarValue) != std::bit_cast<std::uint32_t>(b.scalarValue)) return false;
		}
		return true;
	}

	AudioControlReceipt AudioControlQueue::Submit(AudioControlBatch batch,
		AdmissionResolver resolver, void* resolverContext) {
		std::scoped_lock lock(m_Mutex);
		if(m_Stopped) return Reject(batch, AudioControlRejection::QueueStopped, "Audio control queue is stopped.");
		if(batch.commands.empty()) return Reject(batch, AudioControlRejection::EmptyBatch, "Audio control batches must contain a command.");
		if(batch.commands.size() > m_Limits.maximumCommandsPerBatch)
			return Reject(batch, AudioControlRejection::BatchCapacity, "Audio control batch exceeds the command limit.");
		// Subtraction checks avoid overflow when a caller supplies oversized strings.
		std::size_t remainingMetadata = m_Limits.maximumMetadataBytes;
		for(const auto* value : {&batch.correlationId, &batch.idempotencyKey, &batch.requestFingerprint}) {
			if(value->size() > remainingMetadata)
				return Reject(batch, AudioControlRejection::MetadataTooLarge, "Audio control metadata exceeds the byte limit.");
			remainingMetadata -= value->size();
		}
		if(!batch.idempotencyKey.empty()) {
			const auto found = m_Idempotency.find(batch.idempotencyKey);
			if(found != m_Idempotency.end()) {
				if(!Equivalent(found->second->original, batch))
					return Reject(batch, AudioControlRejection::IdempotencyConflict, "Audio retry key describes a different typed request.");
				// Receipt copying may allocate, but changes no ownership or retention.
				auto receipt = found->second->receipt;
				receipt.correlationId = batch.correlationId;
				if(found->second->state != RequestState::Terminal) receipt.status = AudioControlStatus::Accepted;
				return receipt;
			}
		}
		const auto producer = m_ProducerCommandCounts.find(batch.producerId);
		const auto producerCount = producer == m_ProducerCommandCounts.end() ? 0U : producer->second;
		if(batch.commands.size() > m_Limits.maximumQueuedCommands - m_PendingCommandCount)
			return Reject(batch, AudioControlRejection::GlobalCapacity, "Audio pending control capacity is exhausted.");
		if(batch.commands.size() > m_Limits.maximumProducerCommands - producerCount)
			return Reject(batch, AudioControlRejection::ProducerCapacity, "Audio producer pending capacity is exhausted.");
		if(m_NextSequence == 0U)
			return Reject(batch, AudioControlRejection::GlobalCapacity, "Audio control sequence space is exhausted.");

		RequestList staged;
		bool insertedProducer = false;
		bool insertedKey = false;
		bool insertedSequence = false;
		const auto producerId = batch.producerId;
		// Roll back index insertions before destroying their staged list node.
		// Existing producer charges and sequence numbers are never changed here.
		auto rollback = [&]() noexcept {
			if(insertedSequence) m_BySequence.erase(m_NextSequence);
			if(insertedKey) m_Idempotency.erase(staged.front().original.idempotencyKey);
			if(insertedProducer) m_ProducerCommandCounts.erase(producerId);
		};
		try {
			staged.emplace_back();
			auto node = staged.begin();
			node->original = batch;
			node->resolved = std::move(batch);
			node->resolved.batchSequence = m_NextSequence;
			node->resolved.completionToken = {m_Epoch, m_NextSequence};
			for(std::size_t index = 0U; index < node->resolved.commands.size(); ++index)
				node->resolved.commands[index].commandIndex = static_cast<std::uint32_t>(index);
			node->receipt.batchSequence = m_NextSequence;
			node->receipt.requestedFrame = node->original.requestedFrame;
			node->receipt.correlationId = node->original.correlationId;
			node->receipt.status = AudioControlStatus::Queued;
			node->receipt.completionToken = node->resolved.completionToken;
			// Both receipt string copies are prepared before external resolution.
			AudioControlReceipt outgoing = node->receipt;
			insertedProducer = m_ProducerCommandCounts.try_emplace(producerId, 0U).second;
			if(!node->original.idempotencyKey.empty())
				insertedKey = m_Idempotency.emplace(node->original.idempotencyKey, node).second;
			insertedSequence = m_BySequence.emplace(m_NextSequence, node).second;
			if(resolver != nullptr && !resolver(resolverContext, node->resolved, outgoing)) {
				rollback();
				outgoing.status = AudioControlStatus::Rejected;
				return outgoing;
			}
			// Resolution may fill numeric receipt fields. Retained diagnostics stay
			// empty on admission so the final ownership publication cannot allocate.
			node->receipt.resolvedPlaybackGeneration = outgoing.resolvedPlaybackGeneration;
			node->receipt.requestedFrame = outgoing.requestedFrame;
			m_ProducerCommandCounts.find(producerId)->second += node->original.commands.size();
			m_QueuedCommandCount += node->original.commands.size();
			m_PendingCommandCount += node->original.commands.size();
			m_Pending.splice(m_Pending.end(), staged, node);
			m_NextSequence = m_NextSequence == std::numeric_limits<AudioBatchSequence>::max() ? 0U : m_NextSequence + 1U;
			return outgoing;
		} catch(...) {
			rollback();
			return Reject(staged.empty() ? batch : staged.front().original,
				AudioControlRejection::InternalFailure, "Audio admission preparation failed; no request was published.");
		}
	}

	std::vector<AudioControlBatch> AudioControlQueue::DrainEligible(AudioFrameIndex frame) {
		std::scoped_lock lock(m_Mutex);
		std::size_t count = 0U;
		for(const auto& node : m_Pending)
			if(node.state == RequestState::Queued && node.resolved.requestedFrame <= frame) ++count;
		std::vector<AudioControlBatch> eligible;
		// Reserve before moving any request. A failed allocation leaves every
		// queued payload and charge intact for a later protected boundary.
		eligible.reserve(count);
		for(auto& node : m_Pending) {
			if(node.state != RequestState::Queued || node.resolved.requestedFrame > frame) continue;
			eligible.push_back(std::move(node.resolved));
			node.state = RequestState::InFlight;
			m_QueuedCommandCount -= node.original.commands.size();
		}
		std::sort(eligible.begin(), eligible.end(), [](const auto& left, const auto& right) {
			return left.requestedFrame != right.requestedFrame ? left.requestedFrame < right.requestedFrame
				: left.batchSequence < right.batchSequence;
		});
		return eligible;
	}

	bool AudioControlQueue::Complete(AudioControlCompletionToken token, AudioControlStatus status,
		AudioControlRejection rejection) noexcept {
		std::scoped_lock lock(m_Mutex);
		if(token.epoch != m_Epoch || (status != AudioControlStatus::Applied && status != AudioControlStatus::Rejected)) return false;
		const auto found = m_BySequence.find(token.sequence);
		if(found == m_BySequence.end() || found->second->state != RequestState::InFlight) return false;
		const auto node = found->second;
		node->state = RequestState::Terminal;
		node->receipt.status = status;
		node->receipt.rejection = rejection;
		m_PendingCommandCount -= node->original.commands.size();
		const auto producer = m_ProducerCommandCounts.find(node->original.producerId);
		producer->second -= node->original.commands.size();
		if(producer->second == 0U) m_ProducerCommandCounts.erase(producer);
		m_BySequence.erase(found);
		if(node->original.idempotencyKey.empty()) m_Pending.erase(node);
		else {
			// List splicing and erasure reuse existing nodes: completion is safe
			// even while the allocator is failing elsewhere in frame processing.
			m_Completed.splice(m_Completed.end(), m_Pending, node);
			while(m_Completed.size() > m_Limits.maximumRetainedIdempotencyKeys) {
				m_Idempotency.erase(m_Completed.front().original.idempotencyKey);
				m_Completed.pop_front();
				++m_EvictedRecords;
			}
		}
		return true;
	}

	void AudioControlQueue::RetireAll() noexcept {
		m_Idempotency.clear();
		m_BySequence.clear();
		m_Pending.clear();
		m_Completed.clear();
		m_ProducerCommandCounts.clear();
		m_QueuedCommandCount = 0U;
		m_PendingCommandCount = 0U;
		m_EvictedRecords = 0U;
		// An exhausted epoch permanently closes this queue rather than allowing
		// an ancient completion token to acknowledge a replacement request.
		if(m_Epoch == std::numeric_limits<std::uint64_t>::max()) m_Epoch = 0U;
		else if(m_Epoch != 0U) ++m_Epoch;
	}

	void AudioControlQueue::Stop() {
		std::scoped_lock lock(m_Mutex);
		RetireAll();
		m_Stopped = true;
	}

	void AudioControlQueue::Reset() {
		std::scoped_lock lock(m_Mutex);
		RetireAll();
		m_NextSequence = 1U;
		m_Stopped = m_Epoch == 0U;
	}

	std::size_t AudioControlQueue::QueuedCommandCount() const {
		std::scoped_lock lock(m_Mutex);
		return m_QueuedCommandCount;
	}
	std::size_t AudioControlQueue::Capacity() const noexcept { return m_Limits.maximumQueuedCommands; }
	bool AudioControlQueue::IsStopped() const { std::scoped_lock lock(m_Mutex); return m_Stopped; }
	AudioControlQueueStatistics AudioControlQueue::Statistics() const {
		std::scoped_lock lock(m_Mutex);
		return {m_QueuedCommandCount, m_PendingCommandCount - m_QueuedCommandCount,
			m_PendingCommandCount, m_Completed.size(), m_Idempotency.size(), m_EvictedRecords, m_Epoch};
	}

	const char* ToString(AudioControlRejection rejection) noexcept {
		switch(rejection) {
		case AudioControlRejection::None: return "none";
		case AudioControlRejection::QueueStopped: return "queue-stopped";
		case AudioControlRejection::EmptyBatch: return "empty-batch";
		case AudioControlRejection::BatchCapacity: return "batch-capacity";
		case AudioControlRejection::GlobalCapacity: return "global-capacity";
		case AudioControlRejection::ProducerCapacity: return "producer-capacity";
		case AudioControlRejection::MetadataTooLarge: return "metadata-too-large";
		case AudioControlRejection::IdempotencyConflict: return "idempotency-conflict";
		case AudioControlRejection::ExpiredFrame: return "expired-frame";
		case AudioControlRejection::InvalidTarget: return "invalid-target";
		case AudioControlRejection::StaleTarget: return "stale-target";
		case AudioControlRejection::InvalidValue: return "invalid-value";
		case AudioControlRejection::InternalFailure: return "internal-failure";
		}
		return "unknown";
	}
}
