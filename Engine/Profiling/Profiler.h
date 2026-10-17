// Profiler.h — Contains declarations for the Profiler file-specific types, helper functions, and module integration code used by runtime profiling sessions, timing markers, capture serialization, and scope helpers.

#pragma once

#include "Profiling/ProfilerCapture.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

struct RenderStatsSnapshot;

namespace TerraEngine::Animation
{
	struct AnimationDiagnosticsSnapshot;
}

namespace TerraEngine::Profiling
{
struct ProfilerHotPathState;
struct ProfilerDeferredSampleStorage;
class ProfilerJournalV1Writer;
class ProfilerJournalV1Sink;

// EditorSystem batches these fixed records and publishes them once at the end
// of its protected update. Literal names avoid temporary string allocation in
// the editor hot path; the profiler copies them only while a capture is active.
struct ProfilerEditorStageTiming
{
	const char* stageName = nullptr;
	std::uint64_t durationNanoseconds = 0u;
	std::uint64_t workItemCount = 1u;
};

// This lock-free scalar is available even while capture recording is idle. It
// lets automation measure Engine::Update itself without folding editor UI,
// compositor, or presentation time into the engine performance gate.
struct ProfilerLatestEngineFrameTiming
{
	std::uint64_t frameIndex = 0u;
	double durationMs = 0.0;
	bool valid = false;
};

enum class ProfilerJournalFinalizationState : std::uint8_t
{
	None,
	Draining,
	Completed,
	Failed,
	Stale
};

// A ticket is a generation-validated observation handle.  It lets editor
// callers poll completion without retaining or copying the recorded history.
struct ProfilerJournalFinalizationTicket
{
	std::uint64_t journalGeneration = 0u;
	bool accepted = false;
};

struct ProfilerJournalFinalizationStatus
{
	ProfilerJournalFinalizationTicket ticket;
	ProfilerJournalFinalizationState state = ProfilerJournalFinalizationState::None;
	ProfilerCaptureHealth health;
	std::string error;
};


class Profiler
{
public:
	static Profiler& Instance();

	// StartRecording is retained for small diagnostic and unit-test sessions that
	// do not have a project artifact destination. Its retained presentation cache
	// is deliberately capped; durable or duration-unbounded recording must use
	// StartJournalRecording below.
	void StartRecording(const ProfilerCaptureMetadata& metadata, const ProfilerCaptureSettings& settings);
	// StartJournalRecording opens the sole V1 artifact before producers become
	// active. StartRecording remains the in-memory diagnostic/test session API;
	// editor capture persistence always reserves and opens a V1 journal instead.
	bool StartJournalRecording(
		const ProfilerCaptureMetadata& metadata,
		const ProfilerCaptureSettings& settings,
		const std::filesystem::path& artifactPath,
		std::string& outError,
		std::shared_ptr<ProfilerJournalV1Sink> sink = {});
	void PauseRecording();
	void ResumeRecording();
	// Finishing always returns a generation-validated ticket. Journal sessions
	// drain asynchronously; bounded in-memory diagnostic sessions complete the
	// same ticket immediately. Callers that explicitly need an in-memory test
	// snapshot may request it after completion instead of making Finish copy the
	// entire retained history on the interactive thread.
	[[nodiscard]] ProfilerJournalFinalizationTicket FinishRecording();
	[[nodiscard]] ProfilerJournalFinalizationStatus GetJournalFinalizationStatus(
		ProfilerJournalFinalizationTicket ticket) const;
	void Clear();
	void LoadDocument(const ProfilerCaptureDocument& document);
	// Persistence workers transfer their retained capture into the profiler so a
	// completed recording does not duplicate every frame and sample hierarchy.
	void LoadDocument(ProfilerCaptureDocument&& document);

	// A recording can remain armed while Play Mode is stopped. Passing false
	// keeps the session active but prevents editor-preview frames from entering it.
	void BeginFrame(bool playModeActive = true);
	void EndFrame();
	// The protected coordinator calls this only after graph work has settled.
	// Discard the failing frame without formatting status or publishing success;
	// already buffered session/journal evidence remains owned by the profiler.
	void AbandonFrameAfterFailure() noexcept;
	// EditorSystem runs inside the protected scheduler frame, but its ImGui and
	// presentation work is editor overhead rather than exported engine CPU work.
	// This paired boundary removes the interval from enclosing CPU samples and
	// from the compressed game-only frame clock.
	bool BeginEditorWork();
	void EndEditorWork(bool active);
	bool BeginSample(const char* name, ProfilerModuleId moduleId, const char* taskLabel = nullptr);
	// Literal scope names have program lifetime. Keeping that identity in the
	// producer ring avoids copying the same label bytes thousands of times while
	// preserving the dynamic API for caller-owned strings.
	bool BeginStaticSample(
		const char* name,
		std::size_t nameLength,
		ProfilerModuleId moduleId,
		const char* taskLabel = nullptr);
	void EndSample();
	void RecordRenderStats(const RenderStatsSnapshot& gameStats, const RenderStatsSnapshot& sceneStats);
	// Animation publishes one fixed-size copied summary at its protected frame
	// boundary. This method is a no-op when capture is inactive, keeping dormant
	// projects out of the profiler lock and allocation path.
	void RecordAnimationDiagnostics(const TerraEngine::Animation::AnimationDiagnosticsSnapshot& snapshot);
	// The zero-animator scheduler path has already established dormancy and the
	// frozen baseline identity. Record its three public facts directly so a
	// capture does not construct or validate the full active diagnostics payload.
	void RecordDormantAnimationFacts(std::uint64_t schedulerMembershipChecks = 1U);
	// Advanced animation producers may publish the complete typed aggregate after
	// worker preparation reaches its protected boundary. Invalid or over-limit
	// detail is rejected atomically and never replaces an earlier complete record.
	void RecordAnimationTelemetry(const ProfilerAnimationFrameTelemetry& telemetry);
	// Renderer/animation owners publish one immutable, typed evidence snapshot at
	// the protected boundary; inactive captures take the cheap atomic fast path.
	void RecordFramePublicationEvidence(const ProfilerFramePublicationEvidence& evidence);
	// Copy value-only editor stage telemetry into the current capture frame.
	// The profiler never retains the Scene View window or its mutable caches.
	void RecordSceneViewStages(std::span<const PerformanceStageRecord> stages);
	// Editor-only timings use a dedicated typed domain and never become ordinary
	// CPU samples. Publishing one bounded batch avoids one profiler lock per stage.
	void RecordEditorStages(std::span<const ProfilerEditorStageTiming> stages);
	// The live Engine CPU clock follows the same editor-exclusion contract as a
	// recorded capture, but remains available while recording is disabled. The
	// engine owns the begin/publish pair and EditorSystem contributes only its
	// outer protected interval through BeginEditorWork/EndEditorWork.
	void BeginLatestEngineFrameTiming() noexcept;
	void PublishLatestEngineFrameTiming() noexcept;
	[[nodiscard]] ProfilerLatestEngineFrameTiming GetLatestEngineFrameTiming() const noexcept;
	void RecordTaskQueued();
	bool RecordTaskStarted(const char* taskLabel);
	void RecordTaskFinished(bool sampleActive);
	void SelectFrame(std::uint64_t frameIndex);

	ProfilerSessionState GetState() const;
	ProfilerCaptureSettings GetSettings() const;
	ProfilerCaptureDocument GetDocumentSnapshot() const;
	std::vector<ProfilerFrameListItem> GetFrameListSnapshot() const;
	std::vector<ProfilerFrameRecord> GetFramesSnapshot() const;
	std::optional<ProfilerFrameRecord> GetSelectedFrameSnapshot() const;
	std::optional<std::filesystem::path> GetSourcePath() const;
	std::string GetLastError() const;
	std::string GetStatusText() const;
	[[nodiscard]] ProfilerCaptureHealth GetCaptureHealth() const;
	bool IsCapturing() const;
	// Scheduler hot paths use this atomic observation after BeginFrame. Unlike
	// IsCapturing(), it takes no document mutex and is true only while the current
	// Play Mode frame is actively accepting capture evidence.
	bool IsFrameRecordingActive() const noexcept;
	// UI readers poll this atomic revision before requesting deep capture copies.
	// Recording threads never wait for a profiler window that is only redrawing.
	std::uint64_t GetSnapshotRevision() const noexcept;

private:
	// A non-journal caller has no durable sink to drain into. Keep its inspection
	// cache small and fixed so this compatibility entry point cannot silently
	// recreate the old duration-scaled live capture ownership.
	static constexpr std::size_t kMaximumInMemoryDiagnosticFrames = 512u;
	Profiler();
	~Profiler();

	double NowMs() const;
	// Pause and Finish are protected scheduler-boundary operations. When either
	// interrupts a live frame, retain the elapsed measurements as one explicitly
	// partial record instead of publishing zero-duration samples.  A journal
	// Finish supplies its action-time timestamp later from the drain worker so
	// merging a dense frame never delays interactive ticket publication.
	void FinalizePartialFrameLocked(std::optional<std::int64_t> endingNanoseconds = std::nullopt);
	// Only the interactive caller can close its thread-local editor interval.
	// Keep this tiny action-time accounting separate from the potentially large
	// worker-side sample merge performed by FinalizePartialFrameLocked.
	void CloseInterruptedEditorWorkLocked(std::int64_t endingNanoseconds);
	bool BeginSampleImpl(
		const char* name,
		std::size_t nameLength,
		bool stableName,
		ProfilerModuleId moduleId,
		const char* taskLabel);
	// Completed frames move into a deque-backed rolling history. Removing the
	// oldest detailed frame is therefore constant-time and never shifts every
	// retained string/vector payload on the editor thread.
	void CommitCurrentFrameLocked();
	void MergeThreadSamplesForFrameLocked(
		std::uint64_t sessionGeneration,
		std::uint64_t frameToken,
		bool deferSampleMaterialization = false);
	void RebuildCurrentFrameModules();
	void AggregateSamplesForFrame(ProfilerFrameRecord& frame, const ProfilerSealedFrame* sealed = nullptr) const;
	void UpdateFastRecordingFlagsLocked();
	void CommitCurrentFrameToJournalLocked();
	void ScheduleJournalDrainLocked();
	void DrainJournalQueue(std::uint64_t expectedJournalGeneration);
	void JournalWorkerMain();

	mutable std::mutex mutex;
	// Journal compression and durable I/O are deliberately isolated from the
	// shared frame-job pool. This serialized owner cannot displace animation,
	// rendering, or physics tasks while a slow destination is flushing.
	std::condition_variable journalWorkerCondition;
	std::thread journalWorker;
	bool journalWorkerStopping = false;
	// Scope producers write only to thread-owned fixed buffers. The opaque state
	// keeps those implementation details out of every translation unit that
	// includes the public profiler API.
	std::unique_ptr<ProfilerHotPathState> hotPath;
	ProfilerSessionState state = ProfilerSessionState::Idle;
	// The public capture document keeps the stable serialization schema, while
	// live retention uses a deque so every-frame eviction stays O(1).
	ProfilerCaptureDocument document;
	std::deque<ProfilerFrameRecord> retainedFrames;
	// The journal owns two sealed FIFO tiers. Full-detail frames may use only the
	// optional admission budget; compact frames can also use the reserved core
	// budget. The worker merges their fronts by frame index so persistence remains
	// strictly chronological without letting optional data starve critical data.
	std::shared_ptr<ProfilerJournalV1Writer> journalWriter;
	std::deque<ProfilerSealedFrame> journalCoreQueue;
	std::deque<ProfilerSealedFrame> journalDetailQueue;
	ProfilerCaptureHealth journalHealth;
	bool journalDrainScheduled = false;
	// Pause/finish force the partial batch out of worker ownership. Ordinary
	// frames wait for the configured batch bound to avoid one I/O operation per
	// frame while keeping producer work nonblocking.
	bool journalDrainForced = false;
	bool journalTerminalFailure = false;
	bool journalFinalizeRequested = false;
	// Finish records the exact protected-boundary time, then lets the drain worker
	// seal any incomplete journal frame.  This preserves partial-frame duration
	// semantics without sorting/allocating detailed samples on the UI thread.
	std::optional<std::int64_t> journalPartialFrameEndingNanoseconds;
	// The drain worker publishes terminal durability before it reacquires the
	// main profiler mutex. This prevents a frame-rate status poll from starving
	// the worker after the terminal record is already safely on disk.
	std::atomic_bool journalFinalizationCompleted{false};
	std::uint64_t journalGeneration = 0u;
	ProfilerCaptureSettings settings;
	std::optional<ProfilerFrameRecord> currentFrame;
	// Complete journal frames copy fixed producer records into one flat owned
	// hand-off. The dedicated writer later creates strings and derived hierarchy;
	// partial and in-memory frames keep their immediate presentation semantics.
	std::shared_ptr<ProfilerDeferredSampleStorage> currentDeferredSamples;
	std::optional<std::uint64_t> selectedFrameIndex;
	std::chrono::steady_clock::time_point sessionStart{};
	std::uint64_t nextFrameIndex = 0u;
	std::uint64_t queuedTaskCount = 0u;
	std::uint64_t startedTaskCount = 0u;
	std::uint64_t finishedTaskCount = 0u;
	std::uint64_t droppedSampleCount = 0u;
	std::uint64_t truncatedSampleLabelCount = 0u;
	std::uint64_t lateSampleCount = 0u;
	std::string statusText = "Idle";
	std::string lastError;
	// These atomics are read by hot instrumentation sites before they touch the
	// profiler mutex. The locked state remains authoritative; the flags only
	// keep idle frames from paying synchronization costs for skipped samples.
	std::atomic_bool frameRecordingActive = false;
	std::atomic_bool markerRecordingActive = false;
	std::atomic_bool taskRecordingActive = false;
	std::atomic_uint64_t snapshotRevision = 1u;
	// Even values are stable snapshots; odd values mean the single producer is
	// replacing the duration. Dividing a stable value by two yields frame index.
	std::atomic_uint64_t latestEngineFrameSequence = 0u;
	std::atomic<double> latestEngineFrameDurationMs = 0.0;
	// These values have one producer: the main thread that runs Engine::Update
	// and the protected EditorSystem boundary. Readers consume only the published
	// atomic snapshot above, so the in-progress clock needs no synchronization.
	std::int64_t latestEngineFrameStartNanoseconds = 0;
	std::int64_t latestExcludedEditorNanoseconds = 0;
	std::int64_t latestEditorWorkStartNanoseconds = 0;
	std::uint32_t latestEditorWorkDepth = 0u;
};
}
