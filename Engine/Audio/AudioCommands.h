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
#include <list>
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
		std::uint64_t objectGeneration = 0U;
		std::uint64_t componentGeneration = 0U;
		std::uint64_t playbackGeneration = 0U;
		std::uint64_t sceneGeneration = 0U;
	};

	// Exact callers keep an observed playback; bind-current callers ask admission
	// to freeze the latest eligible instance before the request becomes visible.
	enum class AudioControlTargetMode : std::uint8_t { ExactInstance, BindCurrentAtAdmission };
	struct AudioControlCompletionToken {
		std::uint64_t epoch = 0U;
		AudioBatchSequence sequence = 0U;
	};

	struct AudioControlCommand {
		AudioControlKind kind = AudioControlKind::Play;
		AudioSourceTarget source;
		TerraEngine::UUID mixGroupId{};
		float scalarValue = 0.0F;
		bool booleanValue = false;
		std::uint32_t commandIndex = 0U;
		AudioControlTargetMode targetMode = AudioControlTargetMode::ExactInstance;
	};

	struct AudioControlBatch {
		AudioBatchSequence batchSequence = 0U;
		AudioFrameIndex requestedFrame = 0U;
		std::uint64_t producerId = 0U;
		std::string correlationId;
		std::string idempotencyKey;
		std::string requestFingerprint;
		std::vector<AudioControlCommand> commands;
		AudioControlCompletionToken completionToken;
	};

	struct AudioControlReceipt {
		AudioBatchSequence batchSequence = 0U;
		AudioFrameIndex requestedFrame = 0U;
		AudioControlStatus status = AudioControlStatus::Rejected;
		AudioControlRejection rejection = AudioControlRejection::None;
		std::string correlationId;
		std::string diagnostic;
		AudioControlCompletionToken completionToken;
		std::uint64_t resolvedPlaybackGeneration = 0U;
	};

	// Counters are copied under the queue lock. Queued observations deliberately
	// differ from pending capacity, which remains owned until acknowledgement.
	struct AudioControlQueueStatistics {
		std::size_t queuedCommands = 0U;
		std::size_t inFlightCommands = 0U;
		std::size_t pendingCommands = 0U;
		std::size_t completedRecords = 0U;
		std::size_t retainedKeyCount = 0U;
		std::uint64_t evictedRecords = 0U;
		std::uint64_t epoch = 0U;
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

		// The optional resolver runs only for a new, fully staged admission. It
		// must stage external reservations before returning true, and undo them on
		// failure. No queue allocation follows it, so successful publication cannot
		// strand an allocated playback identity or accepted predecessor record.
		using AdmissionResolver = bool(*)(void*, AudioControlBatch&, AudioControlReceipt&);
		AudioControlReceipt Submit(AudioControlBatch batch, AdmissionResolver resolver = nullptr, void* resolverContext = nullptr);
		std::vector<AudioControlBatch> DrainEligible(AudioFrameIndex frame);
		bool Complete(AudioControlCompletionToken token, AudioControlStatus status,
			AudioControlRejection rejection = AudioControlRejection::None) noexcept;
		void Stop();
		void Reset();

		[[nodiscard]] std::size_t QueuedCommandCount() const;
		[[nodiscard]] std::size_t Capacity() const noexcept;
		[[nodiscard]] bool IsStopped() const;
		[[nodiscard]] AudioControlQueueStatistics Statistics() const;

	private:
		enum class RequestState : std::uint8_t { Queued, InFlight, Terminal };
		struct RequestRecord {
			AudioControlBatch original;
			AudioControlBatch resolved;
			AudioControlReceipt receipt;
			RequestState state = RequestState::Queued;
		};
		using RequestList = std::list<RequestRecord>;
		using RequestIterator = RequestList::iterator;
		static bool Equivalent(const AudioControlBatch& left, const AudioControlBatch& right) noexcept;
		void RetireAll() noexcept;

		AudioControlReceipt Reject(
			const AudioControlBatch& batch,
			AudioControlRejection reason,
			std::string diagnostic) const;

		AudioControlQueueLimits m_Limits;
		mutable std::mutex m_Mutex;
		RequestList m_Pending;
		RequestList m_Completed;
		std::unordered_map<std::uint64_t, std::size_t> m_ProducerCommandCounts;
		std::unordered_map<std::string, RequestIterator> m_Idempotency;
		std::unordered_map<AudioBatchSequence, RequestIterator> m_BySequence;
		std::size_t m_QueuedCommandCount = 0U;
		std::size_t m_PendingCommandCount = 0U;
		std::uint64_t m_Epoch = 1U;
		std::uint64_t m_EvictedRecords = 0U;
		AudioBatchSequence m_NextSequence = 1U;
		bool m_Stopped = false;
	};

	TERRAENGINE_API const char* ToString(AudioControlRejection rejection) noexcept;
}
