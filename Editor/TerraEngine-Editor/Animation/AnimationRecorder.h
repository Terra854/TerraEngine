// AnimationRecorder.h - window-independent, bounded editor animation recording.
//
// Capture is deliberately separated from final publication. Callers may feed
// immutable runtime samples from scheduler-owned worker preparation, poll copied
// progress snapshots from UI/automation, and run Finalize on the shared task pool.
// The recorder never creates a private thread or mutates a live scene/component.

#pragma once

#include "Animation/Assets/AnimationClipAsset.h"
#include "Animation/Runtime/AnimationSnapshots.h"
#include "AnimationAuthoringTransaction.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Editor::Animation {
	// Recorder channel descriptors intentionally reuse the durable clip metadata
	// contract. This prevents editor, automation, and the binary codec from
	// assigning different numeric meanings to the same recorded channel kind.
	using AnimationRecordingChannelDescriptor = TerraEngine::Animation::AnimationClipRecordedChannel;

	struct AnimationRecordedValue {
		std::uint64_t stableChannelId = 0U;
		TerraEngine::Animation::AnimationFloat3 vectorValue{};
		TerraEngine::Animation::AnimationQuaternion rotationValue{};
		float scalarValue = 0.0F;

		auto operator<=>(const AnimationRecordedValue&) const = default;
	};

	struct AnimationRecordedEvent {
		std::uint64_t stableChannelId = 0U;
		std::uint64_t stableEventId = 0U;
		TerraEngine::Animation::AnimationEventPayloadType payloadType =
			TerraEngine::Animation::AnimationEventPayloadType::None;
		std::string payload;
		bool fireForward = true;
		bool fireReverse = true;

		auto operator<=>(const AnimationRecordedEvent&) const = default;
	};

	struct AnimationRecordedSample {
		TerraEngine::Animation::AnimationFixedTick tick = 0;
		// The scalar array preserves the original prototype API. New callers use
		// typed values carrying stable channel IDs; mixing both forms is rejected.
		std::vector<float> channels;
		std::vector<AnimationRecordedValue> values;
		std::vector<AnimationRecordedEvent> events;
	};

	struct AnimationRecordingReductionSettings {
		float translationToleranceMeters = 0.0005F;
		float rotationToleranceDegrees = 0.1F;
		float scaleToleranceFraction = 0.0005F;
		float morphTolerance = 0.0005F;
		float curveTolerance = 0.0005F;

		auto operator<=>(const AnimationRecordingReductionSettings&) const = default;
	};

	struct AnimationRecorderSourceProvenance {
		TerraEngine::Animation::AnimationGuid sourceAnimatorId{};
		TerraEngine::Animation::AnimationGuid sourceControllerAssetId{};
		std::string sourceSceneIdentity;
		std::string sourceSessionIdentity;

		auto operator<=>(const AnimationRecorderSourceProvenance&) const = default;
	};

	struct AnimationRecorderDescriptor {
		TerraEngine::Animation::AnimationGuid assetId{};
		TerraEngine::Animation::AnimationGuid skeletonAssetId{};
		TerraEngine::Animation::AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
		TerraEngine::Animation::AnimationFixedTick firstTick = 0;
		TerraEngine::Animation::AnimationFixedTick lastTick = 0;
		std::uint32_t sampleRate = TerraEngine::Animation::kAnimationFixedTicksPerSecond;
		TerraEngine::Animation::AnimationClipRecordingClock clock =
			TerraEngine::Animation::AnimationClipRecordingClock::FixedSimulation;
		std::vector<AnimationRecordingChannelDescriptor> channels;
		AnimationRecorderSourceProvenance provenance;
		AnimationRecordingReductionSettings reduction;
		TerraEngine::Animation::AnimationFixedTick segmentDurationTicks = 600;
		std::size_t samplesPerChunk = 256U;
		// Zero selects the safe derived maximum from the declared range and clip-key
		// limit. Explicit lower project budgets remain useful for automation jobs.
		std::uint64_t maximumCapturedSamples = 0U;
		std::uint64_t maximumStagedBytes = 256U * 1024U * 1024U;
		std::filesystem::path stagingRoot;
		std::filesystem::path destinationPath;
	};

	// The published pose has transforms by dense joint index while an authored
	// recording channel names a stable joint ID. This copied binding supplies the
	// checked index-to-ID relation captured with the session; it never exposes a
	// mutable skeleton or live animator record to the recorder worker.
	struct AnimationRecorderSnapshotBinding {
		TerraEngine::Animation::AnimatorInstanceHandle animator{};
		TerraEngine::Animation::AnimationGeneration expectedPublicationGeneration = 0U;
		std::vector<std::uint64_t> jointStableIds;
	};

	enum class AnimationRecorderStage : std::uint8_t {
		Idle = 0U,
		Capturing = 1U,
		LoadingChunks = 2U,
		ReducingAndCompressing = 3U,
		ValidatingCandidate = 4U,
		Publishing = 5U,
		CandidateReady = 6U,
		Completed = 7U,
		Canceled = 8U,
		Failed = 9U
	};

	enum class AnimationRecorderOutcome : std::uint8_t {
		None = 0U,
		CandidateReady = 1U,
		Published = 2U,
		Canceled = 3U,
		Failed = 4U
	};

	enum class AnimationRecordingDiagnosticSeverity : std::uint8_t {
		Info = 0U,
		Warning = 1U,
		Error = 2U
	};

	struct AnimationRecordingDiagnostic {
		std::string code;
		AnimationRecordingDiagnosticSeverity severity = AnimationRecordingDiagnosticSeverity::Info;
		AnimationRecorderStage stage = AnimationRecorderStage::Idle;
		TerraEngine::Animation::AnimationFixedTick tick = 0;
		std::uint64_t stableChannelId = 0U;
		std::string message;
	};

	struct AnimationRecorderProgress {
		AnimationRecorderStage stage = AnimationRecorderStage::Idle;
		bool active = false;
		bool cancellationRequested = false;
		bool canceled = false;
		std::uint64_t capturedSamples = 0U;
		std::uint64_t capturedEvents = 0U;
		std::uint64_t droppedSamples = 0U;
		std::uint64_t stagedChunkCount = 0U;
		std::uint64_t bufferedSampleCount = 0U;
		std::uint64_t peakBufferedSampleCount = 0U;
		std::uint64_t stagedBytes = 0U;
		std::uint64_t processedChunkCount = 0U;
		std::uint64_t sourceKeyCount = 0U;
		std::uint64_t retainedKeyCount = 0U;
		std::uint64_t removedKeyCount = 0U;
		float captureNormalized = 0.0F;
		float overallNormalized = 0.0F;
		TerraEngine::Animation::AnimationStatus codecStatus =
			TerraEngine::Animation::AnimationStatus::NotReady;
		std::string latestDiagnostic;
	};

	struct AnimationRecorderResult {
		AnimationRecorderOutcome outcome = AnimationRecorderOutcome::None;
		AnimationAuthoringStatus authoringStatus = AnimationAuthoringStatus::Rejected;
		TerraEngine::Animation::AnimationStatus codecStatus =
			TerraEngine::Animation::AnimationStatus::NotReady;
		bool candidateValidated = false;
		bool published = false;
		bool replacedPriorOutput = false;
		std::filesystem::path destinationPath;
		std::filesystem::path stagingDirectory;
		TerraEngine::Animation::AnimationGuid assetId{};
		TerraEngine::Animation::AnimationGuid skeletonAssetId{};
		TerraEngine::Animation::AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
		TerraEngine::Animation::AnimationFixedTick durationTicks = 0;
		std::uint32_t sampleRate = 0U;
		TerraEngine::Animation::AnimationClipRecordingClock clock =
			TerraEngine::Animation::AnimationClipRecordingClock::FixedSimulation;
		std::uint64_t capturedSamples = 0U;
		std::uint64_t capturedEvents = 0U;
		std::uint64_t stagedChunkCount = 0U;
		std::uint64_t stagedBytes = 0U;
		std::uint64_t outputBytes = 0U;
		std::uint64_t sourceKeyCount = 0U;
		std::uint64_t retainedKeyCount = 0U;
		std::uint64_t removedKeyCount = 0U;
		std::uint64_t peakBufferedSampleCount = 0U;
		float measuredTranslationErrorMeters = 0.0F;
		float measuredRotationErrorDegrees = 0.0F;
		float measuredScaleErrorFraction = 0.0F;
		float measuredMorphError = 0.0F;
		float measuredCurveError = 0.0F;
		std::string diagnostic;
	};

	class AnimationRecorder final {
	public:
		AnimationRecorder();
		~AnimationRecorder();

		AnimationRecorder(const AnimationRecorder&) = delete;
		AnimationRecorder& operator=(const AnimationRecorder&) = delete;
		AnimationRecorder(AnimationRecorder&&) = delete;
		AnimationRecorder& operator=(AnimationRecorder&&) = delete;

		AnimationAuthoringStatus Begin(const AnimationRecorderDescriptor& descriptor) noexcept;
		// Compatibility wrapper: creates deterministic scalar curve descriptors and
		// supports candidate-only finalization through Finalize(std::string&).
		AnimationAuthoringStatus Begin(
			std::uint64_t firstTick,
			std::uint64_t lastTick,
			std::uint32_t channelCount) noexcept;
		AnimationAuthoringStatus Capture(AnimationRecordedSample sample) noexcept;
		// Convert one immutable AnimationSystem publication into the selected typed
		// channels. Callers obtain the snapshot through LoadAnimatorSnapshot at a
		// protected boundary and may retain only that immutable shared snapshot.
		AnimationAuthoringStatus CapturePublishedSnapshot(
			const TerraEngine::Animation::AnimationSnapshot& snapshot,
			const AnimationRecorderSnapshotBinding& binding) noexcept;
		AnimationAuthoringStatus Cancel() noexcept;
		// Preferred finalization validates and atomically replaces destinationPath.
		AnimationAuthoringStatus Finalize() noexcept;
		// Compatibility finalization returns validated binary bytes but explicitly
		// records CandidateReady instead of pretending the bytes were published.
		AnimationAuthoringStatus Finalize(std::string& outTeanimBytes) noexcept;

		[[nodiscard]] AnimationRecorderProgress Progress() const noexcept;
		[[nodiscard]] AnimationRecorderResult Result() const noexcept;
		[[nodiscard]] std::vector<AnimationRecordingDiagnostic> Diagnostics() const noexcept;

	private:
		class Impl;
		std::unique_ptr<Impl> impl_;
	};
}
