// AudioCommands.h - bounded typed commands for protected audio control.
//
// Producers may submit batches from gameplay, managed code, or editor tooling,
// but they never mutate live playback state. AudioSystem drains complete batches
// at its declared snapshot boundary and applies a deterministic total order.

#pragma once

#include "Core/Export.h"
#include "Utilities/UUID.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TerraEngine::Audio {
	using AudioFrameIndex = std::uint64_t;
	using AudioBatchSequence = std::uint64_t;

	enum class AudioControlKind : std::uint8_t {
		Play,
		Pause,
		Resume,
		Stop,
		SetSourceVolume,
		SetSourcePitch,
		SetGroupVolume,
		SetGroupMuted,
		SetMasterVolume,
		SetMasterMuted
	};

	enum class AudioControlStatus : std::uint8_t {
		Queued,
		Accepted,
		Rejected,
		Applied
	};

	enum class AudioControlRejection : std::uint8_t {
		None,
		QueueStopped,
		EmptyBatch,
		BatchCapacity,
		GlobalCapacity,
		ProducerCapacity,
		MetadataTooLarge,
		IdempotencyConflict,
		ExpiredFrame,
		InvalidTarget,
		StaleTarget,
		InvalidValue,
		InternalFailure
	};

	// Durable UUIDs plus generations prevent a delayed command from controlling a
	// newly-created component that happens to reuse the same ECS storage slot.
	struct AudioSourceTarget {
		TerraEngine::UUID objectId{};
		std::uint32_t objectGeneration = 0U;
		std::uint32_t componentGeneration = 0U;
		std::uint32_t playbackGeneration = 0U;
	};

	struct AudioControlCommand {
		AudioControlKind kind = AudioControlKind::Play;
		AudioSourceTarget source;
		TerraEngine::UUID mixGroupId{};
		float scalarValue = 0.0F;
		bool booleanValue = false;
		std::uint32_t commandIndex = 0U;
	};

	struct AudioControlBatch {
		AudioBatchSequence batchSequence = 0U;
		AudioFrameIndex requestedFrame = 0U;
		std::uint64_t producerId = 0U;
		std::string correlationId;
		std::string idempotencyKey;
		std::string requestFingerprint;
		std::vector<AudioControlCommand> commands;
	};

	struct AudioControlReceipt {
		AudioBatchSequence batchSequence = 0U;
		AudioFrameIndex requestedFrame = 0U;
		AudioControlStatus status = AudioControlStatus::Rejected;
		AudioControlRejection rejection = AudioControlRejection::None;
		std::string correlationId;
		std::string diagnostic;
	};

	struct AudioControlQueueLimits {
		std::size_t maximumQueuedCommands = 4096U;
		std::size_t maximumProducerCommands = 1024U;
		std::size_t maximumCommandsPerBatch = 256U;
		std::size_t maximumMetadataBytes = 256U;
		std::size_t maximumRetainedIdempotencyKeys = 4096U;
	};

	// AudioControlQueue reserves capacity for a whole batch under one lock. That
	// all-or-nothing admission rule is the first half of batch atomicity; the
	// AudioSystem performs generation/value preflight before applying the batch.
	class TERRAENGINE_API AudioControlQueue final {
	public:
		explicit AudioControlQueue(AudioControlQueueLimits limits = {});

		AudioControlReceipt Submit(AudioControlBatch batch);
		std::vector<AudioControlBatch> DrainEligible(AudioFrameIndex frame);
		void Stop();
		void Reset();

		[[nodiscard]] std::size_t QueuedCommandCount() const;
		[[nodiscard]] std::size_t Capacity() const noexcept;
		[[nodiscard]] bool IsStopped() const;

	private:
		struct IdempotencyRecord {
			std::string fingerprint;
			AudioBatchSequence sequence = 0U;
		};

		AudioControlReceipt Reject(
			const AudioControlBatch& batch,
			AudioControlRejection reason,
			std::string diagnostic) const;

		AudioControlQueueLimits m_Limits;
		mutable std::mutex m_Mutex;
		std::deque<AudioControlBatch> m_Queued;
		std::unordered_map<std::uint64_t, std::size_t> m_ProducerCommandCounts;
		std::unordered_map<std::string, IdempotencyRecord> m_Idempotency;
		std::size_t m_QueuedCommandCount = 0U;
		AudioBatchSequence m_NextSequence = 1U;
		bool m_Stopped = false;
	};

	TERRAENGINE_API const char* ToString(AudioControlRejection rejection) noexcept;
}
