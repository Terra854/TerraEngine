#include "TestHarness.h"

#include "Profiling/ProfilerCapture.h"
#include "Profiling/Profiler.h"
#include "ProfilerTestSupport.h"
#include "ThreadPool/ThreadPool.h"

#include <chrono>
#include <filesystem>
#include <thread>

namespace TerraEngine::Tests
{
	namespace
	{
		// A failed expectation still unwinds this guard. That is essential for
		// singleton profiler tests: no queued drain may outlive its controlled
		// ThreadPool or sink and poison the following test process.
		class ProfilerThreadPoolCleanup
		{
		public:
			~ProfilerThreadPoolCleanup()
			{
				Profiling::Profiler::Instance().Clear();
				ThreadPool::instance().shutdown();
			}
		};
	}

	void RegisterProfilerCapturePerformanceTests(TestRegistry& registry)
	{
		registry.Add("Engine.ProfilerCapturePerformance.FiveSecondAggregateStaysUnderTenMiB", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			ProfilerCaptureDocument document;
			document.metadata.settings.detailLevel = ProfilerCaptureDetailLevel::Aggregate;
			document.metadata.settings.maximumChunkFrames = 120u;
			for (std::uint64_t index = 0u; index < 300u; ++index)
			{
				ProfilerFrameRecord frame;
				frame.frameIndex = index;
				frame.durationMs = 16.0;
				frame.performanceStages.push_back({PerformanceStageDomain::Physics, "Physics.ProviderRead", {TelemetryAvailability::Available, 1000u, {}}, "PhysicsSystem", 10000u, false});
				frame.physicsWorkVolume = PhysicsWorkVolumeRecord{index, 10000u, 1000u, 10000u, 1000u, 1000u, 10u, 10u};
				document.frames.push_back(std::move(frame));
			}
			const std::filesystem::path path = std::filesystem::temp_directory_path() / "TerraEngine-five-second-aggregate.teprof";
			std::string error;
			context.Expect(SaveCaptureDocument(document, path, error), "The deterministic five-second gate capture should save: " + error);
			std::error_code sizeError;
			const std::uintmax_t bytes = std::filesystem::file_size(path, sizeError);
			context.Expect(!sizeError && bytes <= 10u * 1024u * 1024u, "The five-second aggregate capture must stay at or below the 10 MiB acceptance gate.");
			std::error_code ignored;
			std::filesystem::remove(path, ignored);
		});

		registry.Add("Engine.ProfilerCapturePerformance.TenThousandFramePersistenceStaysLinear", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			ProfilerCaptureDocument document;
			document.metadata.settings.detailLevel = ProfilerCaptureDetailLevel::Aggregate;
			document.metadata.settings.maximumChunkFrames = 1024u;
			document.metadata.settings.maximumChunkBytes = 8u * 1024u * 1024u;
			document.frames.reserve(10000u);
			for (std::uint64_t index = 0u; index < 10000u; ++index)
			{
				// Repeated typed rows reproduce the shape of a high-frame-rate live
				// aggregate capture without depending on a renderer or wall clock.
				ProfilerFrameRecord frame;
				frame.frameIndex = index;
				frame.durationMs = 1.0;
				frame.performanceStages.push_back({
					PerformanceStageDomain::Physics,
					"Animation.Evaluate",
					{TelemetryAvailability::Available, 250000u, {}},
					"AnimationSystem",
					4u,
					false});
				frame.physicsWorkVolume = PhysicsWorkVolumeRecord{
					index, 10u, 10u, 10u, 10u, 10u, 10u, 10u};
				document.frames.push_back(std::move(frame));
			}

			const std::filesystem::path path =
				std::filesystem::temp_directory_path() / "TerraEngine-ten-thousand-frame-linear.teprof";
			std::string error;
			const auto started = std::chrono::steady_clock::now();
			const bool saved = SaveCaptureDocument(document, path, error);
			const auto elapsed = std::chrono::steady_clock::now() - started;
			context.Expect(saved, "The 10,000-frame aggregate capture should save: " + error);
			context.Expect(elapsed < std::chrono::seconds(5),
				"Chunk persistence must remain linear rather than repeatedly serializing the growing chunk.");

			std::error_code ignored;
			std::filesystem::remove(path, ignored);
		});

		registry.Add("Engine.ProfilerCapturePerformance.LiveThirtyThousandFramesStayResidentBounded", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			// One worker and one gated write make the producer/consumer hand-off
			// deterministic while still exercising the real live journal path.
			ProfilerThreadPoolCleanup cleanup;
			ThreadPool::instance().shutdown();
			ThreadPool::instance().initialize(1u);
			ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerThirtyThousandLive");
			auto sink = std::make_shared<ProfilerTestSupport::ControlledJournalSink>();
			sink->BlockWrite(2u);
			ProfilerCaptureSettings settings;
			// Detailed profiler frames can exceed an 8 MiB raw segment when grouped
			// into thousand-frame batches. Keep this realistic byte-safe batch size;
			// the controlled sink itself is scalar-bounded, so 250 such drains no
			// longer make the fixture retain duration-scaled test data.
			settings.maximumChunkFrames = 120u;
			settings.maximumChunkBytes = 8u * 1024u * 1024u;
			settings.maximumPendingBytes = 16u * 1024u * 1024u;
			settings.residentByteBudget = 16u * 1024u * 1024u;
			settings.maximumSealedFrameBytes = 64u * 1024u;
			Profiler& profiler = Profiler::Instance();
			profiler.Clear();
			std::string error;
			context.Expect(profiler.StartJournalRecording({}, settings,
				workspace.Path("long-live.teprof"), error, sink),
				"The live bounded fixture should start: " + error);
			for (std::uint32_t frame = 0u; frame < settings.maximumChunkFrames; ++frame)
			{
				profiler.BeginFrame();
				profiler.EndFrame();
			}
			context.Expect(sink->WaitUntilBlocked(std::chrono::seconds(2)),
				"The injected sink must reach its controlled slow write.");
			const ProfilerCaptureHealth blocked = profiler.GetCaptureHealth();
			context.Expect(blocked.residentBytes <= settings.residentByteBudget
				&& blocked.inFlightBytes > 0u,
				"A slow sink must retain only the accounted bounded batch while producers return.");
			sink->Release();
			// One capture-wide allowance stays deterministic under a busy complete
			// EngineTests process. Per-batch short spinning can starve the single
			// worker, then turn one delayed write into hundreds of repeated timeouts.
			const auto captureDrainDeadline = std::chrono::steady_clock::now()
				+ std::chrono::seconds(60);
			auto waitForCommitted = [&profiler, captureDrainDeadline](std::uint64_t expectedCommitted) {
				while (profiler.GetCaptureHealth().committedFrameCount < expectedCommitted
					&& std::chrono::steady_clock::now() < captureDrainDeadline)
				{
					// Yield the CPU long enough for a contended shared worker to reach
					// its journal write without turning this producer-side test loop into
					// a busy-spin scheduler competitor.
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				return profiler.GetCaptureHealth().committedFrameCount >= expectedCommitted;
			};
			bool drainedEveryBatch = waitForCommitted(settings.maximumChunkFrames);
			for (std::uint32_t frame = settings.maximumChunkFrames;
				frame < 30000u && drainedEveryBatch; ++frame)
			{
				profiler.BeginFrame();
				profiler.EndFrame();
				// The test sink is deliberately slow but not unavailable. Allow each
				// bounded segment to drain before producing the next one so this long
				// fixture measures bounded residency, not intentional core overflow.
				if ((frame + 1u) % settings.maximumChunkFrames == 0u)
				{
					drainedEveryBatch = waitForCommitted(frame + 1u);
				}
			}
			const ProfilerJournalFinalizationTicket ticket = profiler.FinishRecording();
			bool completed = false;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
			while (std::chrono::steady_clock::now() < deadline)
			{
				completed = profiler.GetJournalFinalizationStatus(ticket).state
					== ProfilerJournalFinalizationState::Completed;
				if (completed)
				{
					break;
				}
				std::this_thread::yield();
			}
			const ProfilerCaptureHealth health = profiler.GetCaptureHealth();
			const bool conserved = drainedEveryBatch && completed && !health.terminalFailure
				&& health.producedFrameCount == 30000u && health.committedFrameCount == 30000u
				&& health.residentHighWaterBytes <= settings.residentByteBudget;
			// Tests must retire the singleton writer before the controlled pool is
			// destroyed, even when an assertion below fails. Otherwise a queued drain
			// could outlive its sink and turn one failed assertion into process exit.
			context.Expect(conserved, "A long live journal must conserve compact cores while resident ownership stays duration-independent "
				"(produced=" + std::to_string(health.producedFrameCount)
				+ ", committed=" + std::to_string(health.committedFrameCount)
				+ ", rejected=" + std::to_string(health.rejectedCoreFrameCount)
				+ ", terminal=" + std::to_string(health.terminalFailure)
				+ ", drainBatches=" + std::to_string(drainedEveryBatch)
				+ ", completed=" + std::to_string(completed) + ").");
		});
	}
}
