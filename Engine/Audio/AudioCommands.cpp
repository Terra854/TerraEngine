// AudioCommands.cpp - atomic admission and deterministic drain for audio batches.

#include "AudioCommands.h"

#include <algorithm>
#include <utility>

namespace TerraEngine::Audio {
	AudioControlQueue::AudioControlQueue(AudioControlQueueLimits limits)
		: m_Limits(limits) {
		// Zero capacities would make every request fail in a surprising way. Keep
		// construction total by normalizing each bound to one usable entry.
		m_Limits.maximumQueuedCommands = std::max<std::size_t>(1U, m_Limits.maximumQueuedCommands);
		m_Limits.maximumProducerCommands = std::max<std::size_t>(1U, m_Limits.maximumProducerCommands);
		m_Limits.maximumCommandsPerBatch = std::max<std::size_t>(1U, m_Limits.maximumCommandsPerBatch);
		m_Limits.maximumMetadataBytes = std::max<std::size_t>(1U, m_Limits.maximumMetadataBytes);
		m_Limits.maximumRetainedIdempotencyKeys = std::max<std::size_t>(1U, m_Limits.maximumRetainedIdempotencyKeys);
	}

	AudioControlReceipt AudioControlQueue::Reject(
		const AudioControlBatch& batch,
		AudioControlRejection reason,
		std::string diagnostic) const {
		AudioControlReceipt receipt;
		receipt.batchSequence = batch.batchSequence;
		receipt.requestedFrame = batch.requestedFrame;
		receipt.status = AudioControlStatus::Rejected;
		receipt.rejection = reason;
		receipt.correlationId = batch.correlationId;
		receipt.diagnostic = std::move(diagnostic);
		return receipt;
	}

	AudioControlReceipt AudioControlQueue::Submit(AudioControlBatch batch) {
		std::scoped_lock lock(m_Mutex);
		if(m_Stopped)
			return Reject(batch, AudioControlRejection::QueueStopped, "Audio control queue is stopped.");
		if(batch.commands.empty())
			return Reject(batch, AudioControlRejection::EmptyBatch, "Audio control batches must contain at least one command.");
		if(batch.commands.size() > m_Limits.maximumCommandsPerBatch)
			return Reject(batch, AudioControlRejection::BatchCapacity, "Audio control batch exceeds the configured command limit.");

		const std::size_t metadataBytes = batch.correlationId.size()
			+ batch.idempotencyKey.size() + batch.requestFingerprint.size();
		if(metadataBytes > m_Limits.maximumMetadataBytes)
			return Reject(batch, AudioControlRejection::MetadataTooLarge, "Audio control metadata exceeds the configured byte limit.");
		if(m_QueuedCommandCount + batch.commands.size() > m_Limits.maximumQueuedCommands)
			return Reject(batch, AudioControlRejection::GlobalCapacity, "Audio control queue is at capacity.");

		const std::size_t producerCount = m_ProducerCommandCounts[batch.producerId];
		if(producerCount + batch.commands.size() > m_Limits.maximumProducerCommands)
			return Reject(batch, AudioControlRejection::ProducerCapacity, "Audio control producer is at capacity.");

		if(!batch.idempotencyKey.empty()) {
			const auto found = m_Idempotency.find(batch.idempotencyKey);
			if(found != m_Idempotency.end()) {
				if(found->second.fingerprint != batch.requestFingerprint)
					return Reject(batch, AudioControlRejection::IdempotencyConflict, "Audio idempotency key was reused for a different request.");

				AudioControlReceipt receipt;
				receipt.batchSequence = found->second.sequence;
				receipt.requestedFrame = batch.requestedFrame;
				receipt.status = AudioControlStatus::Accepted;
				receipt.correlationId = batch.correlationId;
				receipt.diagnostic = "Matching audio control batch was already accepted.";
				return receipt;
			}
			if(m_Idempotency.size() >= m_Limits.maximumRetainedIdempotencyKeys)
				return Reject(batch, AudioControlRejection::GlobalCapacity, "Audio idempotency history is at capacity.");
		}

		batch.batchSequence = m_NextSequence++;
		for(std::size_t index = 0U; index < batch.commands.size(); ++index)
			batch.commands[index].commandIndex = static_cast<std::uint32_t>(index);
		if(!batch.idempotencyKey.empty())
			m_Idempotency.emplace(batch.idempotencyKey, IdempotencyRecord{batch.requestFingerprint, batch.batchSequence});

		m_QueuedCommandCount += batch.commands.size();
		m_ProducerCommandCounts[batch.producerId] += batch.commands.size();

		AudioControlReceipt receipt;
		receipt.batchSequence = batch.batchSequence;
		receipt.requestedFrame = batch.requestedFrame;
		receipt.status = AudioControlStatus::Queued;
		receipt.correlationId = batch.correlationId;
		m_Queued.push_back(std::move(batch));
		return receipt;
	}

	std::vector<AudioControlBatch> AudioControlQueue::DrainEligible(AudioFrameIndex frame) {
		std::scoped_lock lock(m_Mutex);
		std::vector<AudioControlBatch> eligible;
		std::deque<AudioControlBatch> future;
		while(!m_Queued.empty()) {
			AudioControlBatch batch = std::move(m_Queued.front());
			m_Queued.pop_front();
			if(batch.requestedFrame <= frame) {
				m_QueuedCommandCount -= batch.commands.size();
				auto producer = m_ProducerCommandCounts.find(batch.producerId);
				if(producer != m_ProducerCommandCounts.end()) {
					producer->second -= batch.commands.size();
					if(producer->second == 0U)
						m_ProducerCommandCounts.erase(producer);
				}
				eligible.push_back(std::move(batch));
			} else {
				future.push_back(std::move(batch));
			}
		}
		m_Queued.swap(future);
		std::stable_sort(eligible.begin(), eligible.end(), [](const AudioControlBatch& left, const AudioControlBatch& right) {
			if(left.requestedFrame != right.requestedFrame)
				return left.requestedFrame < right.requestedFrame;
			return left.batchSequence < right.batchSequence;
		});
		return eligible;
	}

	void AudioControlQueue::Stop() {
		std::scoped_lock lock(m_Mutex);
		m_Stopped = true;
	}

	void AudioControlQueue::Reset() {
		std::scoped_lock lock(m_Mutex);
		m_Queued.clear();
		m_ProducerCommandCounts.clear();
		m_Idempotency.clear();
		m_QueuedCommandCount = 0U;
		m_NextSequence = 1U;
		m_Stopped = false;
	}

	std::size_t AudioControlQueue::QueuedCommandCount() const {
		std::scoped_lock lock(m_Mutex);
		return m_QueuedCommandCount;
	}

	std::size_t AudioControlQueue::Capacity() const noexcept {
		return m_Limits.maximumQueuedCommands;
	}

	bool AudioControlQueue::IsStopped() const {
		std::scoped_lock lock(m_Mutex);
		return m_Stopped;
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
