#include "TestHarness.h"

#include "Profiling/ProfilerJournalV1.h"
#include "Profiling/Profiler.h"
#include "Profiling/ProfilerScope.h"
#include "ProfilerTestSupport.h"
#include "ThreadPool/ThreadPool.h"

#include <chrono>
#include <functional>
#include <thread>

namespace TerraEngine::Tests
{
namespace
{
class ProfilerThreadPoolSession
{
public:
	ProfilerThreadPoolSession()
	{
		ThreadPool::instance().shutdown();
		ThreadPool::instance().initialize(1u);
	}

	~ProfilerThreadPoolSession()
	{
		ThreadPool::instance().shutdown();
	}
};

bool WaitForCondition(const std::function<bool()>& predicate)
{
	// Durable journal I/O runs at background priority. Give it elapsed time and
	// release the polling thread between observations instead of spending an
	// arbitrary CPU iteration budget while repeatedly acquiring its state mutex.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (predicate())
		{
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return predicate();
}

std::string DescribeFinalization(const TerraEngine::Profiling::ProfilerJournalFinalizationStatus& status)
{
	// Preserve the actual terminal/error evidence when a bounded wait fails so
	// scheduling delay can be distinguished from stale ownership or sink failure.
	using TerraEngine::Profiling::ProfilerJournalFinalizationState;
	const char* state = "None";
	switch (status.state)
	{
	case ProfilerJournalFinalizationState::Draining: state = "Draining"; break;
	case ProfilerJournalFinalizationState::Completed: state = "Completed"; break;
	case ProfilerJournalFinalizationState::Failed: state = "Failed"; break;
	case ProfilerJournalFinalizationState::Stale: state = "Stale"; break;
	case ProfilerJournalFinalizationState::None: break;
	}
	return std::string(" Finalization state: ") + state + "; error: " + status.error;
}
}

void RegisterProfilerLifecycleTests(TestRegistry& registry)
{
	registry.Add("Engine.ProfilerLifecycle.AbandonedFrameDisarmsOpenSamplesAndPreservesHistory", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		Profiler& profiler = Profiler::Instance();
		ProfilerCaptureSettings settings;
		settings.detailLevel = ProfilerCaptureDetailLevel::Detailed;
		settings.recordMarkers = true;
		settings.taskExecutionEnabled = true;
		profiler.Clear();
		profiler.StartRecording({}, settings);
		profiler.BeginFrame();
		profiler.EndFrame();
		const auto priorFrames = profiler.GetFramesSnapshot();
		profiler.BeginFrame();
		const bool sample = profiler.BeginSample("abandoned open sample", ProfilerModuleId::CpuEngine);
		const bool task = profiler.RecordTaskStarted("abandoned task");
		context.Expect(sample && task && profiler.IsFrameRecordingActive(), "The failure fixture must begin with actual active samples and task instrumentation.");
		profiler.AbandonFrameAfterFailure();
		context.Expect(!profiler.IsFrameRecordingActive() && !profiler.BeginSample("late sample", ProfilerModuleId::CpuEngine)
			&& !profiler.RecordTaskStarted("late task"), "Failure abandonment must disarm both sample and task admissions.");
		// Scope unwind occurs after the protected failure boundary. Its old token
		// must not publish successful evidence or attach to a future clean frame.
		profiler.RecordTaskFinished(task);
		if (sample) profiler.EndSample();
		profiler.EndFrame();
		profiler.AbandonFrameAfterFailure();
		context.ExpectEqual(priorFrames.size(), profiler.GetFramesSnapshot().size(), "Abandonment must preserve completed history and discard the incomplete record.");
		profiler.BeginFrame();
		const bool clean = profiler.BeginSample("clean successor", ProfilerModuleId::CpuEngine);
		if (clean) profiler.EndSample();
		profiler.EndFrame();
		const auto frames = profiler.GetFramesSnapshot();
		context.Expect(frames.size() == priorFrames.size() + 1u && frames.back().complete && frames.back().samples.size() == 1u
			&& frames.back().samples.front().name == "clean successor" && frames.back().samples.front().parentSampleId == 0u,
			"A subsequent clean frame must not inherit abandoned samples or their ancestry.");
		(void)profiler.FinishRecording();
		profiler.StartRecording({}, settings);
		profiler.BeginFrame(); profiler.EndFrame();
		const auto replacement = profiler.GetFramesSnapshot();
		context.Expect(replacement.size() == 1u && replacement.front().complete && replacement.front().samples.empty(), "A clean replacement session must not inherit discarded producer records.");
		profiler.Clear();
	});

	registry.Add("Engine.ProfilerLifecycle.AbandonedJournalFramePreservesBufferedEvidence", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerThreadPoolSession pool;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerLifecycleAbandonedJournal");
		Profiler& profiler = Profiler::Instance();
		ProfilerCaptureSettings settings;
		settings.detailLevel = ProfilerCaptureDetailLevel::Detailed;
		settings.maximumChunkFrames = 8u;
		settings.maximumChunkBytes = 1024u * 1024u;
		settings.maximumPendingBytes = 1024u * 1024u;
		settings.maximumSealedFrameBytes = 1024u * 1024u;
		profiler.Clear();
		std::string error;
		const auto path = workspace.Path("abandoned.teprof");
		context.Expect(profiler.StartJournalRecording({}, settings, path, error), "The journal abandonment fixture must open: " + error);
		profiler.BeginFrame(); profiler.EndFrame();
		profiler.BeginFrame();
		{
			ProfilerScope sample("discarded journal sample", ProfilerModuleId::CpuEngine);
		}
		profiler.AbandonFrameAfterFailure();
		profiler.BeginFrame(); profiler.EndFrame();
		const auto ticket = profiler.FinishRecording();
		const bool finalized = WaitForCondition([&] { return profiler.GetJournalFinalizationStatus(ticket).state == ProfilerJournalFinalizationState::Completed; });
		context.Expect(finalized, "Buffered valid journal frames must still finalize after abandonment."
			+ DescribeFinalization(profiler.GetJournalFinalizationStatus(ticket)));
		ProfilerJournalV1OpenResult index;
		context.Expect(ReadProfilerJournalV1Index(path, index, error) == ProfilerJournalV1ResultCode::Ok && index.index.size() == 2u,
			"The journal must retain its two valid frames and omit the discarded frame: " + error);
		for (const auto& entry : index.index)
		{
			ProfilerFrameRecord frame;
			context.Expect(ReadProfilerJournalV1SelectedFrame(path, entry.frameIndex, frame, error) == ProfilerJournalV1ResultCode::Ok
				&& frame.complete && frame.samples.empty(), "A durable valid frame must not inherit discarded deferred samples: " + error);
		}
		profiler.Clear();
	});

	registry.Add("Engine.ProfilerLifecycle.JournalFinalizationIsExplicitAndSingleShot", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerLifecycleJournal");
		ProfilerCaptureMetadata metadata;
		metadata.settings.maximumChunkFrames = 1u;
		std::string error;
		ProfilerJournalV1Writer writer;
		context.Expect(writer.Open(workspace.Path("session.teprof"), metadata, error)
			== ProfilerJournalV1ResultCode::Ok,
			"A journal session must open before finalization: " + error);
		context.Expect(writer.Finalize(false, error) == ProfilerJournalV1ResultCode::Ok,
			"A controlled incomplete finalization must produce one terminal state: " + error);
		context.Expect(writer.Finalize(false, error) == ProfilerJournalV1ResultCode::AlreadyFinalized,
			"A second finalization must not append a conflicting terminal.");
	});

	registry.Add("Engine.ProfilerLifecycle.JournalTicketRejectsStaleSession", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerLifecycleTicket");
		ProfilerCaptureMetadata metadata;
		ProfilerCaptureSettings settings;
		settings.maximumChunkFrames = 1u;
		settings.maximumChunkBytes = 1024u * 1024u;
		settings.maximumPendingBytes = 1024u * 1024u;
		settings.maximumSealedFrameBytes = 1024u * 1024u;
		Profiler& profiler = Profiler::Instance();
		profiler.Clear();
		std::string error;
		context.Expect(profiler.StartJournalRecording(metadata, settings, workspace.Path("ticket.teprof"), error),
			"A ticket fixture must start its journal session: " + error);
		profiler.BeginFrame();
		profiler.EndFrame();
		const ProfilerJournalFinalizationTicket ticket = profiler.FinishRecording();
		context.Expect(ticket.accepted, "Finish must return an accepted generation ticket.");
		// Starting a new session is cancellation from the prior caller's point of
		// view.  Its ticket must never describe the replacement session.
		profiler.StartRecording(metadata, settings);
		context.Expect(profiler.GetJournalFinalizationStatus(ticket).state
			== ProfilerJournalFinalizationState::Stale,
			"A replacement recording session must invalidate the earlier ticket.");
		profiler.Clear();
	});

	registry.Add("Engine.ProfilerLifecycle.ScopeCopiesDynamicLabelAndPreservesLiteralAncestry", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerCaptureSettings settings;
		settings.detailLevel = ProfilerCaptureDetailLevel::Detailed;
		settings.recordMarkers = true;
		Profiler& profiler = Profiler::Instance();
		profiler.Clear();
		profiler.StartRecording({}, settings);
		profiler.BeginFrame();
		std::string dynamicTaskLabel = "dynamic label before mutation";
		{
			// Literal names are stable scope identities, while the task description
			// models a scheduler-owned string that can change after work begins.
			ProfilerScope parent("Literal parent scope", ProfilerModuleId::CpuEngine);
			ProfilerScope child("Literal child scope", ProfilerModuleId::TaskExecution,
				dynamicTaskLabel.c_str());
			dynamicTaskLabel.assign(512u, 'x');
		}
		profiler.EndFrame();

		const std::vector<ProfilerFrameRecord> frames = profiler.GetFramesSnapshot();
		const ProfilerSample* parentSample = nullptr;
		const ProfilerSample* childSample = nullptr;
		if (!frames.empty())
		{
			for (const ProfilerSample& sample : frames.front().samples)
			{
				if (sample.name == "Literal parent scope")
				{
					parentSample = &sample;
				}
				else if (sample.name == "Literal child scope")
				{
					childSample = &sample;
				}
			}
		}
		context.Expect(parentSample != nullptr && childSample != nullptr,
			"Detailed scopes must retain both literal names after the producer frame closes.");
		if (parentSample != nullptr && childSample != nullptr)
		{
			context.ExpectEqual(parentSample->sampleId,
				childSample->parentSampleId,
				"A nested literal scope must retain its exact parent identity.");
			context.ExpectEqual(std::string("dynamic label before mutation"),
				childSample->taskLabel,
				"A dynamic task label must be copied at scope entry before caller mutation.");
		}
		const ProfilerJournalFinalizationTicket ticket = profiler.FinishRecording();
		context.Expect(profiler.GetJournalFinalizationStatus(ticket).state
			== ProfilerJournalFinalizationState::Completed,
			"The in-memory scope fixture must still finish through the common ticket contract.");
		profiler.Clear();
	});

	registry.Add("Engine.ProfilerLifecycle.DurableWorkerPreservesDeferredScopesViewportAndAccounting", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		// This test owns the worker pool it needs so filtered or reordered registry
		// runs cannot depend on a previously executed profiler fixture.
		ProfilerThreadPoolSession pool;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerLifecycleDeferredWorker");
		ProfilerCaptureSettings settings;
		settings.detailLevel = ProfilerCaptureDetailLevel::Detailed;
		settings.recordMarkers = true;
		settings.maximumChunkFrames = 1u;
		settings.maximumChunkBytes = 1024u * 1024u;
		settings.maximumPendingBytes = 1024u * 1024u;
		settings.maximumSealedFrameBytes = 1024u * 1024u;
		Profiler& profiler = Profiler::Instance();
		profiler.Clear();
		std::string error;
		const std::filesystem::path journalPath = workspace.Path("deferred-worker.teprof");
		context.Expect(profiler.StartJournalRecording({}, settings, journalPath, error),
			"The deferred-worker journal must open before scope hand-off: " + error);
		profiler.BeginFrame();
		{
			// This is the production viewport-submit identity.  The journal path must
			// derive its timing stream after worker-side scope aggregation, not lose it
			// merely because EndFrame transfers the raw hierarchy immediately.
			ProfilerScope viewport("RenderSystem: Submit Game View Cameras",
				ProfilerModuleId::CpuEngine);
			ProfilerScope child("Deferred journal child", ProfilerModuleId::TaskExecution,
				"durable hand-off");
		}
		profiler.EndFrame();
		const ProfilerJournalFinalizationTicket ticket = profiler.FinishRecording();
		context.Expect(ticket.accepted,
			"A durable detailed frame must return a finalization ticket immediately.");
		const bool finalized = WaitForCondition([&]() {
			return profiler.GetJournalFinalizationStatus(ticket).state
				== ProfilerJournalFinalizationState::Completed;
		});
		context.Expect(finalized, "The dedicated journal worker must aggregate, persist, and finalize the frame."
			+ DescribeFinalization(profiler.GetJournalFinalizationStatus(ticket)));

		const ProfilerCaptureHealth health = profiler.GetCaptureHealth();
		context.Expect(health.committedFrameCount == 1u && health.producedFrameCount == 1u
			&& health.pendingBytes == 0u && health.inFlightBytes == 0u
			&& health.residentBytes == 0u && !health.terminalFailure,
			"Worker aggregation must release exactly its admitted resident bytes after durable commit.");

		ProfilerJournalV1OpenResult index;
		error.clear();
		context.Expect(ReadProfilerJournalV1Index(journalPath, index, error)
			== ProfilerJournalV1ResultCode::Ok && index.index.size() == 1u,
			"The finalized worker artifact must expose one durable frame-list row: " + error);
		ProfilerFrameRecord frame;
		if (index.index.size() == 1u)
		{
			error.clear();
			context.Expect(ReadProfilerJournalV1SelectedFrame(journalPath,
				index.index.front().frameIndex, frame, error) == ProfilerJournalV1ResultCode::Ok,
				"Selected durable detail must decode after deferred worker aggregation: " + error);
		}

		const ProfilerSample* viewportSample = nullptr;
		const ProfilerSample* childSample = nullptr;
		for (const ProfilerSample& sample : frame.samples)
		{
			if (sample.name == "RenderSystem: Submit Game View Cameras")
			{
				viewportSample = &sample;
			}
			else if (sample.name == "Deferred journal child")
			{
				childSample = &sample;
			}
		}
		bool hasViewportCpu = false;
		for (const ProfilerTimingStreamSample& timing : frame.timingStreams)
		{
			if (timing.id == "viewport_cpu"
				&& timing.source == ProfilerTimingStreamSource::ProfilerClock
				&& timing.status == ProfilerTimingStreamStatus::Valid)
			{
				hasViewportCpu = true;
			}
		}
		context.Expect(viewportSample != nullptr && childSample != nullptr
			&& childSample->parentSampleId == viewportSample->sampleId,
			"Deferred aggregation must preserve the nested raw-sample hierarchy exactly.");
		context.Expect(hasViewportCpu,
			"Deferred aggregation must derive the viewport CPU stream from the durable submit scope.");
		profiler.Clear();
	});

	registry.Add("Engine.ProfilerLifecycle.ClearInvalidatesBlockedDrainBeforeReplacementSession", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerThreadPoolSession pool;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerLifecycleReplacementDrain");
		auto blockedSink = std::make_shared<ProfilerTestSupport::ControlledJournalSink>();
		// The header is write one. Holding the first segment leaves an old drain
		// in flight while the protected thread clears and starts a replacement.
		blockedSink->BlockWrite(2u);
		ProfilerCaptureMetadata metadata;
		ProfilerCaptureSettings settings;
		settings.maximumChunkFrames = 1u;
		settings.maximumChunkBytes = 1024u * 1024u;
		settings.maximumPendingBytes = 1024u * 1024u;
		settings.maximumSealedFrameBytes = 1024u * 1024u;
		Profiler& profiler = Profiler::Instance();
		profiler.Clear();
		std::string error;
		context.Expect(profiler.StartJournalRecording(metadata, settings,
			workspace.Path("old-session.teprof"), error, blockedSink),
			"The blocked old journal must start before testing replacement ownership: " + error);
		profiler.BeginFrame();
		profiler.EndFrame();
		context.Expect(blockedSink->WaitUntilBlocked(std::chrono::seconds(2)),
			"The old journal drain must be in flight before Clear changes its generation.");
		const ProfilerJournalFinalizationTicket oldTicket = profiler.FinishRecording();
		context.Expect(oldTicket.accepted,
			"The blocked old journal must publish a finish ticket before replacement.");

		// Clear is the protected cancellation boundary. The old executor may still
		// finish its sink call, but its generation must no longer own live profiler
		// state or affect the replacement writer.
		profiler.Clear();
		context.Expect(profiler.GetJournalFinalizationStatus(oldTicket).state
			== ProfilerJournalFinalizationState::Stale,
			"Clear must make an in-flight old drain ticket stale before a new session starts.");

		auto replacementSink = std::make_shared<ProfilerTestSupport::ControlledJournalSink>();
		error.clear();
		context.Expect(profiler.StartJournalRecording(metadata, settings,
			workspace.Path("replacement-session.teprof"), error, replacementSink),
			"The replacement journal must start independently of the stale drain: " + error);
		profiler.BeginFrame();
		profiler.EndFrame();
		const ProfilerJournalFinalizationTicket replacementTicket = profiler.FinishRecording();
		context.Expect(replacementTicket.accepted,
			"The replacement journal must publish its own finish ticket.");

		// Release only after the new generation exists. This forces the stale worker
		// completion to race a live session and catches accidental stale publication.
		blockedSink->Release();
		context.Expect(WaitForCondition([&]() {
			return profiler.GetJournalFinalizationStatus(replacementTicket).state
				== ProfilerJournalFinalizationState::Completed;
		}), "A stale drain completion must not prevent replacement finalization.");
		const ProfilerCaptureHealth replacementHealth = profiler.GetCaptureHealth();
		context.Expect(!replacementHealth.terminalFailure
			&& replacementHealth.committedFrameCount == 1u,
			"A stale drain must not overwrite replacement health or lose its compact frame core.");
		profiler.Clear();
	});

	registry.Add("Engine.ProfilerLifecycle.SlowSinkNeverBlocksProducerOrFinish", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerThreadPoolSession pool;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerLifecycleSlowSink");
		auto sink = std::make_shared<ProfilerTestSupport::ControlledJournalSink>();
		// Write one is the header. Hold the first segment write until the test
		// proves both EndFrame and Finish returned on the interactive thread.
		sink->BlockWrite(2u);
		ProfilerCaptureMetadata metadata;
		ProfilerCaptureSettings settings;
		settings.maximumChunkFrames = 1u;
		settings.maximumChunkBytes = 1024u * 1024u;
		settings.maximumPendingBytes = 1024u * 1024u;
		settings.maximumSealedFrameBytes = 1024u * 1024u;
		Profiler& profiler = Profiler::Instance();
		profiler.Clear();
		std::string error;
		context.Expect(profiler.StartJournalRecording(
			metadata, settings, workspace.Path("slow.teprof"), error, sink),
			"The injected slow-sink journal should start: " + error);
		profiler.BeginFrame();
		// Leave a dense frame open deliberately. Finish must publish its ticket
		// before merging these thread-buffer samples; the drain worker owns that
		// expensive partial-frame sealing work after the visible boundary.
		for (std::uint32_t sample = 0u; sample < 1024u; ++sample)
		{
			if (profiler.BeginSample("Deferred finish sample", ProfilerModuleId::CpuEngine))
			{
				profiler.EndSample();
			}
		}

		const auto finishStart = std::chrono::steady_clock::now();
		const ProfilerJournalFinalizationTicket ticket = profiler.FinishRecording();
		const double finishMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - finishStart).count();
		context.Expect(ticket.accepted, "Finish should publish a journal ticket while storage is blocked.");
		context.Expect(finishMilliseconds <= 50.0,
			"Finish must return within 50 ms without waiting for a blocked sink or merging a dense partial frame.");
		context.Expect(sink->WaitUntilBlocked(std::chrono::seconds(2)),
			"The persistence worker should seal the deferred partial frame before its blocked write.");
		const ProfilerCaptureHealth blockedHealth = profiler.GetCaptureHealth();
		context.Expect(blockedHealth.inFlightBytes > 0u
			&& blockedHealth.residentBytes <= settings.residentByteBudget,
			"Blocked persistence must remain byte-accounted and inside the resident budget.");

		sink->Release();
		context.Expect(sink->WaitForWriteCount(4u, std::chrono::seconds(2)),
			"Released persistence should commit the segment and terminal records.");
		context.Expect(WaitForCondition([&]() {
			return profiler.GetJournalFinalizationStatus(ticket).state
				== ProfilerJournalFinalizationState::Completed;
		}), "Finalization should complete after the sink is released.");
		const ProfilerCaptureHealth completedHealth = profiler.GetCaptureHealth();
		context.ExpectEqual(1ULL,
			static_cast<unsigned long long>(completedHealth.committedFrameCount),
			"The slow writable sink must conserve the compact frame core.");
		context.ExpectEqual(0ULL,
			static_cast<unsigned long long>(completedHealth.rejectedCoreFrameCount),
			"A temporary writable backlog must not reject compact frame core.");
		context.Expect(completedHealth.collectionNanoseconds > 0u
			&& completedHealth.sealingNanoseconds > 0u
			&& completedHealth.queueingNanoseconds > 0u
			&& completedHealth.persistenceNanoseconds > 0u
			&& completedHealth.finalizationNanoseconds > 0u,
			"A completed journal must attribute every profiler-owned runtime stage.");
		context.Expect(completedHealth.persistenceLagHighWaterFrames >= 1u
			&& completedHealth.persistedBytes > 0u
			&& completedHealth.persistenceOperationCount == 1u
			&& completedHealth.persistenceThroughputBytesPerSecond > 0u,
			"Persistence health must report backlog, committed bytes, operations, and throughput.");
		profiler.Clear();
	});

	registry.Add("Engine.ProfilerLifecycle.JournalBatchesFramesBeforeDurableWrite", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerThreadPoolSession pool;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerLifecycleBatching");
		auto sink = std::make_shared<ProfilerTestSupport::ControlledJournalSink>();
		ProfilerCaptureMetadata metadata;
		ProfilerCaptureSettings settings;
		settings.maximumChunkFrames = 60u;
		// Keep the synthetic 60-frame fixture in one byte-bounded segment even
		// when earlier full-suite tests have published richer subsystem summaries.
		settings.maximumChunkBytes = 8u * 1024u * 1024u;
		settings.maximumPendingBytes = 16u * 1024u * 1024u;
		settings.maximumSealedFrameBytes = 1024u * 1024u;
		Profiler& profiler = Profiler::Instance();
		profiler.Clear();
		std::string error;
		context.Expect(profiler.StartJournalRecording(metadata, settings,
			workspace.Path("batched.teprof"), error, sink),
			"The batching journal should open: " + error);
		for (std::uint32_t frame = 0u; frame < 59u; ++frame)
		{
			profiler.BeginFrame();
			profiler.EndFrame();
		}
		context.ExpectEqual(1ULL, static_cast<unsigned long long>(sink->GetWriteCount()),
			"Frames below the configured batch bound must not cause a durable write per frame.");
		profiler.BeginFrame();
		profiler.EndFrame();
		context.Expect(sink->WaitForWriteCount(2u, std::chrono::seconds(2)),
			"The sixtieth frame should produce one committed multi-frame segment.");
		const ProfilerJournalFinalizationTicket ticket = profiler.FinishRecording();
		context.Expect(WaitForCondition([&]() {
			return profiler.GetJournalFinalizationStatus(ticket).state
				== ProfilerJournalFinalizationState::Completed;
		}), "Finish must force any partial batch and terminal record.");
		const ProfilerCaptureHealth health = profiler.GetCaptureHealth();
		context.ExpectEqual(1ULL, static_cast<unsigned long long>(health.persistenceOperationCount),
			"One complete 60-frame batch must remain one persistence operation.");
		context.ExpectEqual(60ULL, static_cast<unsigned long long>(health.committedFrameCount),
			"Batching must preserve every sealed frame.");
		profiler.Clear();
	});

	registry.Add("Engine.ProfilerLifecycle.HeavyFramesScheduleBeforeCoreReserveIsSpent", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerThreadPoolSession pool;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerLifecycleEarlyDrain");
		auto sink = std::make_shared<ProfilerTestSupport::ControlledJournalSink>();
		// Hold the first segment write so the observed resident bytes describe the
		// exact point at which the shared-pool drain was scheduled.
		sink->BlockWrite(2u);
		ProfilerCaptureMetadata metadata;
		ProfilerCaptureSettings settings;
		settings.maximumChunkFrames = 4096u;
		settings.maximumChunkBytes = 4u * 1024u * 1024u;
		settings.maximumPendingBytes = 4u * 1024u * 1024u;
		settings.maximumSealedFrameBytes = 1024u * 1024u;
		Profiler& profiler = Profiler::Instance();
		profiler.Clear();
		std::string error;
		context.Expect(profiler.StartJournalRecording(metadata, settings,
			workspace.Path("early-drain.teprof"), error, sink),
			"The early-drain journal should open: " + error);

		bool workerReachedSink = false;
		for (std::uint32_t frame = 0u; frame < 2048u && !workerReachedSink; ++frame)
		{
			profiler.BeginFrame();
			// A dense real capture owns many retained aggregate scopes. Repeating a
			// bounded scope here makes queue pressure deterministic without sleeps or
			// an oversized synthetic frame.
			for (std::uint32_t sample = 0u; sample < 128u; ++sample)
			{
				if (profiler.BeginSample("Early Drain Aggregate", ProfilerModuleId::CpuEngine))
				{
					profiler.EndSample();
				}
			}
			profiler.EndFrame();
			workerReachedSink = sink->WaitUntilBlocked(std::chrono::milliseconds(1));
		}
		context.Expect(workerReachedSink,
			"Heavy frames must schedule persistence before reaching the frame-count batch limit.");
		const ProfilerCaptureHealth blocked = profiler.GetCaptureHealth();
		const std::uint64_t compactReserveBytes = settings.maximumPendingBytes / 4u;
		context.Expect(!blocked.terminalFailure
			&& blocked.residentHighWaterBytes <= compactReserveBytes + settings.maximumSealedFrameBytes,
			"Drain scheduling must begin while the compact-core reserve still has bounded worker-start headroom.");

		const ProfilerJournalFinalizationTicket ticket = profiler.FinishRecording();
		context.Expect(ticket.accepted, "Finish should fence producers while the early drain is blocked.");
		sink->Release();
		context.Expect(WaitForCondition([&]() {
			return profiler.GetJournalFinalizationStatus(ticket).state
				== ProfilerJournalFinalizationState::Completed;
		}), "The released early-drain session must commit and finalize.");
		const ProfilerCaptureHealth completed = profiler.GetCaptureHealth();
		context.ExpectEqual(
			static_cast<unsigned long long>(completed.producedFrameCount),
			static_cast<unsigned long long>(completed.committedFrameCount),
			"A writable heavy-frame session must conserve every compact frame core.");
		context.ExpectEqual(0ULL,
			static_cast<unsigned long long>(completed.rejectedCoreFrameCount),
			"Early drain scheduling must prevent healthy-worker latency from rejecting core evidence.");
		profiler.Clear();
	});

	registry.Add("Engine.ProfilerLifecycle.UnavailableSinkFailsClosedWithoutPartialSuccess", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerThreadPoolSession pool;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerLifecycleUnavailableSink");
		auto sink = std::make_shared<ProfilerTestSupport::ControlledJournalSink>();
		sink->FailWrite(2u);
		ProfilerCaptureMetadata metadata;
		ProfilerCaptureSettings settings;
		settings.maximumChunkFrames = 1u;
		settings.maximumChunkBytes = 1024u * 1024u;
		settings.maximumPendingBytes = 1024u * 1024u;
		settings.maximumSealedFrameBytes = 1024u * 1024u;
		Profiler& profiler = Profiler::Instance();
		profiler.Clear();
		std::string error;
		context.Expect(profiler.StartJournalRecording(
			metadata, settings, workspace.Path("unavailable.teprof"), error, sink),
			"The unavailable-sink fixture should start from a valid header: " + error);
		profiler.BeginFrame();
		profiler.EndFrame();
		context.Expect(sink->WaitForWriteCount(2u, std::chrono::seconds(2)),
			"The persistence worker should reach the injected unavailable write.");
		context.Expect(WaitForCondition([&]() {
			return profiler.GetCaptureHealth().terminalFailure;
		}), "An unavailable destination must publish one terminal failure.");
		const ProfilerCaptureHealth health = profiler.GetCaptureHealth();
		context.Expect(!health.destinationWritable && health.committedFrameCount == 0u,
			"A failed segment write must not publish partial committed success.");
		profiler.Clear();
	});

	registry.Add("Engine.ProfilerLifecycle.CompactCoreOverflowFailsClosed", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerThreadPoolSession pool;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerLifecycleCoreOverflow");
		ProfilerCaptureMetadata metadata;
		ProfilerCaptureSettings settings;
		settings.maximumChunkFrames = 1u;
		settings.maximumChunkBytes = 1024u;
		settings.maximumPendingBytes = 1024u;
		// A valid minimal journal frame is intentionally larger than this cap even
		// after optional detail is removed. The producer must fail closed rather
		// than silently dropping timing/module core evidence.
		settings.maximumSealedFrameBytes = 32u;
		Profiler& profiler = Profiler::Instance();
		profiler.Clear();
		std::string error;
		context.Expect(profiler.StartJournalRecording(metadata, settings,
			workspace.Path("core-overflow.teprof"), error),
			"The core-overflow fixture should create its bounded journal: " + error);
		profiler.BeginFrame();
		profiler.EndFrame();
		const ProfilerCaptureHealth health = profiler.GetCaptureHealth();
		context.Expect(health.terminalFailure && health.rejectedCoreFrameCount == 1u
			&& health.committedFrameCount == 0u && health.pendingBytes == 0u,
			"A compact core that cannot fit its configured bound must publish one terminal failure without partial commit.");
		profiler.Clear();
	});
}
}
