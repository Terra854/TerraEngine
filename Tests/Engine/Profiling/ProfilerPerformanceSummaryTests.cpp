#include "TestHarness.h"

#include "Profiling/Profiler.h"
#include "Profiling/ProfilerCapture.h"
#include "Profiling/ProfilerJournalV1.h"
#include "ProfilerTestSupport.h"
#include "Renderer/Graph/RenderDiagnostics.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		Profiling::ProfilerSealedFrame MakeCommittedSummaryFrame(
			std::uint64_t frameIndex,
			double frameStartMs,
			double durationMs,
			double engineCpuMs,
			bool complete,
			double cpuModuleMs,
			bool taskModuleAvailable,
			double taskModuleMs)
		{
			Profiling::ProfilerSealedFrame sealed;
			sealed.frame.frameIndex = frameIndex;
			sealed.frame.frameStartMs = frameStartMs;
			sealed.frame.durationMs = durationMs;
			sealed.frame.complete = complete;
			sealed.frame.timingStreams.push_back({
				"engine_cpu", "Engine CPU", Profiling::ProfilerTimingStreamScope::EngineCpu,
				Profiling::ProfilerTimingStreamSource::ProfilerClock,
				Profiling::ProfilerTimingStreamStatus::Valid, engineCpuMs, {}});
			sealed.frame.moduleSummaries = {
				{Profiling::ProfilerModuleId::CpuEngine, true, cpuModuleMs, {}},
				{Profiling::ProfilerModuleId::TaskExecution,
					taskModuleAvailable, taskModuleMs, {}}};
			// Writer admission uses this exact measured value to keep the test on the
			// same bounded persistence path as a live sealed-frame handoff.
			sealed.ownedByteCount = Profiling::EstimateProfilerFrameOwnedBytes(sealed.frame);
			return sealed;
		}

		Profiling::ProfilerCaptureMetadata MakeSummaryJournalMetadata()
		{
			Profiling::ProfilerCaptureMetadata metadata;
			metadata.terraEngineVersion = "unit-test";
			metadata.projectName = "ProfilerSummary";
			metadata.scenePath = "Scenes/ProfilerSummary.tescene";
			metadata.startedAt = "2026-08-11";
			// Two frames per commit forces the fixture through more than one durable
			// segment while staying far below every configured byte bound.
			metadata.settings.maximumChunkFrames = 2u;
			metadata.settings.maximumChunkBytes = 1024u * 1024u;
			return metadata;
		}

		const Profiling::ProfilerMetric* FindMetric(
			const Profiling::ProfilerModuleSummary& module,
			std::string_view name)
		{
			const auto metric = std::find_if(module.metrics.begin(), module.metrics.end(),
				[name](const Profiling::ProfilerMetric& candidate) {
					return candidate.name == name;
				});
			return metric == module.metrics.end() ? nullptr : &*metric;
		}

		class ScopedRenderDiagnosticsRequest
		{
		public:
			ScopedRenderDiagnosticsRequest()
			{
				Renderer::SetRenderDiagnosticsRequested(true);
			}

			~ScopedRenderDiagnosticsRequest()
			{
				Renderer::SetRenderDiagnosticsRequested(false);
			}
		};
	}

	void RegisterProfilerPerformanceSummaryTests(TestRegistry& registry)
	{
		registry.Add("ProfilerPerformanceSummary.TimingPercentilesKeepUnitsAndAvailability", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			std::vector<ProfilerFrameRecord> frames;
			for (std::uint64_t index = 0u; index < 5u; ++index)
			{
				ProfilerFrameRecord frame;
				frame.frameIndex = index;
				AddOrReplaceTimingStreamSample(frame, {"physics.provider_reads", "Physics provider reads", ProfilerTimingStreamScope::EngineCpu, ProfilerTimingStreamSource::ProfilerClock, ProfilerTimingStreamStatus::Valid, static_cast<double>((index + 1u) * 10u), {}});
				frames.push_back(std::move(frame));
			}

			const std::vector<ProfilerTimingStreamAggregate> summaries = BuildTimingStreamAggregates(frames);
			context.ExpectEqual(std::size_t(1u), summaries.size(), "A typed timing identity should produce one bounded aggregate row.");
			if (!summaries.empty())
			{
				const ProfilerTimingStreamAggregate& summary = summaries.front();
				context.ExpectEqual(std::string("physics.provider_reads"), summary.id, "Summary identity should retain the stage name for regression comparison.");
				context.ExpectEqual(std::size_t(5u), summary.sampleCount, "Every valid stage sample should participate in percentiles.");
				context.ExpectEqual(30.0, summary.medianMs, "Median should retain the timing unit in milliseconds.");
				context.ExpectEqual(48.0, summary.p95Ms, "P95 should interpolate deterministically for a short capture.");
				context.ExpectEqual(49.6, summary.p99Ms, "P99 should identify near-maximum stage cost.");
				context.ExpectEqual(50.0, summary.maxMs, "Maximum should preserve the observed worst stage duration.");
			}
		});

		registry.Add("ProfilerPerformanceSummary.UnavailableStreamDoesNotBecomeZero", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			ProfilerFrameRecord frame;
			frame.frameIndex = 1u;
			AddOrReplaceTimingStreamSample(frame, {"scene_view.projection", "Scene View projection", ProfilerTimingStreamScope::ViewportCpu, ProfilerTimingStreamSource::Unavailable, ProfilerTimingStreamStatus::Unavailable, 0.0, "Scene View hidden"});

			const std::vector<ProfilerTimingStreamAggregate> summaries = BuildTimingStreamAggregates({frame});
			context.ExpectEqual(std::size_t(1u), summaries.size(), "Unavailable streams should remain represented for availability diagnostics.");
			if (!summaries.empty())
			{
				context.ExpectEqual(std::size_t(0u), summaries.front().sampleCount, "Unavailable streams must not participate as a zero-duration percentile sample.");
				context.ExpectEqual(std::size_t(1u), summaries.front().missingCount, "Unavailable streams should report their missing observation count.");
				context.Expect(summaries.front().selectedStatus == ProfilerTimingStreamStatus::Unavailable, "The selected stream status should preserve unavailable semantics.");
			}
		});

		registry.Add("Engine.ProfilerPerformanceSummary.TerminalConservesCrossSegmentTimingAndCounts",
			[](TestContext& context) {
				using namespace TerraEngine::Profiling;
				ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerPerformanceSummaryTerminal");
				ProfilerTestSupport::DeterministicClock clock;
				const std::filesystem::path path = workspace.Path("summary.teprof");
				std::string error;
				ProfilerJournalV1Writer writer;
				context.Expect(writer.Open(path, MakeSummaryJournalMetadata(), error)
					== ProfilerJournalV1ResultCode::Ok,
					"The cross-segment summary fixture should open its journal: " + error);

				// Advance deterministic frame starts instead of sleeping. These three
				// records deliberately span a two-frame and a one-frame durable segment.
				clock.Advance(std::chrono::milliseconds(10));
				const ProfilerSealedFrame first = MakeCommittedSummaryFrame(41u,
					static_cast<double>(clock.Now().count()) / 1000000.0,
					10.0, 1.25, true, 2.0, true, 4.0);
				clock.Advance(std::chrono::milliseconds(10));
				const ProfilerSealedFrame second = MakeCommittedSummaryFrame(42u,
					static_cast<double>(clock.Now().count()) / 1000000.0,
					20.0, 2.5, false, 3.0, false, 0.0);
				clock.Advance(std::chrono::milliseconds(10));
				const ProfilerSealedFrame third = MakeCommittedSummaryFrame(43u,
					static_cast<double>(clock.Now().count()) / 1000000.0,
					30.0, 4.25, true, 4.0, true, 6.0);
				const std::array<ProfilerSealedFrame, 2u> firstSegment{first, second};
				context.Expect(writer.AppendCommittedSegment(
					std::span<const ProfilerSealedFrame>(firstSegment), error)
					== ProfilerJournalV1ResultCode::Ok,
					"The first durable summary segment should commit: " + error);
				context.Expect(writer.AppendCommittedSegment(std::span(&third, 1u), error)
					== ProfilerJournalV1ResultCode::Ok,
					"The second durable summary segment should commit: " + error);
				context.Expect(writer.Finalize(true, error) == ProfilerJournalV1ResultCode::Ok,
					"The cross-segment summary fixture should write Terminal: " + error);

				ProfilerCaptureDocument document;
				ProfilerJournalV1OpenResult result;
				context.Expect(OpenProfilerJournalV1(path, document, result, error)
					== ProfilerJournalV1ResultCode::Ok,
					"Reopening Terminal must validate its incremental summary against every committed segment: " + error);
				const ProfilerJournalV1SubsystemSummary& cpu = result.summary.subsystemSummaries[
					static_cast<std::size_t>(ProfilerModuleId::CpuEngine)];
				const ProfilerJournalV1SubsystemSummary& tasks = result.summary.subsystemSummaries[
					static_cast<std::size_t>(ProfilerModuleId::TaskExecution)];
				context.Expect(result.complete && !result.recoveredIncompleteTail
					&& document.committedFrameCount == 3u && document.frames.size() == 3u
					&& result.summary.frameCount == 3u && result.summary.completeFrameCount == 2u
					&& result.summary.committedSegmentCount == 2u
					&& result.summary.totalDurationNanoseconds == 60000000u,
					"Terminal counts must conserve every committed frame exactly across both segments.");
				context.Expect(result.summary.engineCpuSampleCount == 3u
					&& result.summary.engineCpuTotalNanoseconds == 8000000u
					&& cpu.availableFrameCount == 3u && cpu.totalNanoseconds == 9000000u
					&& tasks.availableFrameCount == 2u && tasks.totalNanoseconds == 10000000u,
					"Terminal timing and subsystem lanes must add only their valid, available committed observations.");
			});

		registry.Add("Engine.ProfilerPerformanceSummary.DetailedCoverageKeepsParallelLaneConservation",
			[](TestContext& context) {
				using namespace TerraEngine::Profiling;
				Profiler& profiler = Profiler::Instance();
				profiler.Clear();
				ProfilerCaptureSettings settings;
				settings.detailLevel = ProfilerCaptureDetailLevel::Detailed;
				settings.maxFrames = 1u;
				profiler.StartRecording({}, settings);
				profiler.BeginFrame();
				const bool mainScope = profiler.BeginSample(
					"Summary main lane", ProfilerModuleId::CpuEngine);
				std::thread worker([&profiler]() {
					const bool workerScope = profiler.BeginSample(
						"Summary worker lane", ProfilerModuleId::CpuEngine);
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
					if (workerScope)
					{
						profiler.EndSample();
					}
				});
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
				worker.join();
				if (mainScope)
				{
					profiler.EndSample();
				}
				profiler.EndFrame();

				const std::vector<ProfilerFrameRecord> frames = profiler.GetFramesSnapshot();
				context.Expect(frames.size() == 1u,
					"The detailed parallel-lane fixture should publish exactly one frame.");
				if (!frames.empty())
				{
					const auto module = std::find_if(frames.front().moduleSummaries.begin(),
						frames.front().moduleSummaries.end(), [](const ProfilerModuleSummary& candidate) {
							return candidate.moduleId == ProfilerModuleId::CpuEngine;
						});
					const ProfilerMetric* count = module == frames.front().moduleSummaries.end()
						? nullptr : FindMetric(*module, "Engine sample count");
					const ProfilerMetric* wall = module == frames.front().moduleSummaries.end()
						? nullptr : FindMetric(*module, "Captured wall time");
					context.Expect(module != frames.front().moduleSummaries.end()
						&& count != nullptr && wall != nullptr && count->value == 2.0
						&& wall->value > 0.0 && module->totalMs >= wall->value,
						"Detailed summaries must retain both scopes and merge parallel lanes without reducing CPU work below wall coverage.");
				}
				profiler.Clear();
			});

		registry.Add("Engine.ProfilerPerformanceSummary.TypedAnimationTelemetryDefersMetricAliases",
			[](TestContext& context) {
				using namespace TerraEngine::Profiling;
				ProfilerAnimationFrameTelemetry telemetry;
				telemetry.summary.availableEvidenceMask = kProfilerAnimationAllEvidence;
				telemetry.summary.schedulerMembershipChecks = 1u;
				telemetry.summary.activeAnimators = 4000u;
				telemetry.summary.gpuVisualAnimators = 3998u;
				telemetry.summary.gpuTransferBytes = 8192u;
				telemetry.summary.stageTimings[static_cast<std::size_t>(
					ProfilerAnimationStage::GpuEvaluation)] = {125000u, true, false};

				// The live frame retains only the typed telemetry at its protected
				// boundary. Public snapshots must still expose the long-standing metric
				// aliases after they have left that hot path.
				Profiler& profiler = Profiler::Instance();
				profiler.Clear();
				ProfilerCaptureSettings settings;
				settings.detailLevel = ProfilerCaptureDetailLevel::Detailed;
				settings.maxFrames = 1u;
				profiler.StartRecording({}, settings);
				profiler.BeginFrame();
				profiler.RecordAnimationTelemetry(telemetry);
				profiler.EndFrame();
				const std::vector<ProfilerFrameRecord> frames = profiler.GetFramesSnapshot();
				const ProfilerModuleSummary* module = nullptr;
				if (!frames.empty())
				{
					const auto found = std::find_if(frames.front().moduleSummaries.begin(),
						frames.front().moduleSummaries.end(), [](const ProfilerModuleSummary& candidate) {
							return candidate.moduleId == ProfilerModuleId::CpuEngine;
						});
					if (found != frames.front().moduleSummaries.end())
					{
						module = &*found;
					}
				}
				const ProfilerMetric* active = module == nullptr ? nullptr
					: FindMetric(*module, "Animation.ActiveAnimators");
				const ProfilerMetric* transfer = module == nullptr ? nullptr
					: FindMetric(*module, "Animation.GpuTransferBytes");
				context.Expect(frames.size() == 1u && frames.front().animationTelemetry
					&& active != nullptr && active->value == 4000.0
					&& transfer != nullptr && transfer->value == 8192.0,
					"Typed animation telemetry must retain exact presentation aliases after snapshot materialization.");
				profiler.Clear();

				ProfilerFrameRecord journalFrame;
				journalFrame.frameIndex = 11u;
				journalFrame.moduleSummaries = MakeDefaultModuleSummaries(settings);
				journalFrame.animationDiagnostics = ProfilerAnimationDiagnosticsFacts{1u, false, true};
				journalFrame.animationTelemetry =
					std::make_shared<const ProfilerAnimationFrameTelemetry>(telemetry);
				std::string payload;
				std::string error;
				ProfilerFrameRecord decoded;
				context.Expect(EncodeProfilerJournalFrame(journalFrame, settings, payload, error)
					&& DecodeProfilerJournalFrame(payload, decoded, error),
					"The V1 typed animation frame should round-trip without presentation metric storage: " + error);
				const auto decodedModule = std::find_if(decoded.moduleSummaries.begin(),
					decoded.moduleSummaries.end(), [](const ProfilerModuleSummary& candidate) {
						return candidate.moduleId == ProfilerModuleId::CpuEngine;
					});
				const ProfilerMetric* decodedActive = decodedModule == decoded.moduleSummaries.end()
					? nullptr : FindMetric(*decodedModule, "Animation.ActiveAnimators");
				context.Expect(decoded.animationTelemetry && decodedActive != nullptr
					&& decodedActive->value == 4000.0,
					"A reopened V1 frame must materialize the same typed animation metric aliases.");
			});

		registry.Add("Engine.ProfilerPerformanceSummary.RenderSnapshotKeepsOnlyFrameEvidence",
			[](TestContext& context) {
				using namespace TerraEngine;
				using namespace TerraEngine::Profiling;
				ScopedRenderDiagnosticsRequest requested;
				Renderer::RenderDiagnosticsSummary diagnostics;
				diagnostics.frameId = 87u;
				// These deliberately unrelated detailed strings model a dense renderer
				// diagnostics tree. The profiler snapshot must not need this payload.
				diagnostics.resourceWarnings.assign(128u, "unrelated renderer detail");
				diagnostics.presentationWaits.push_back({
					PresentationWaitKind::RenderQueueBackpressure,
					{TelemetryAvailability::Available, 321u, {}},
					"bounded presentation wait"});
				Renderer::PublishRenderDiagnostics(std::move(diagnostics));

				const Renderer::RenderProfilerFrameSnapshot snapshot =
					Renderer::LatestRenderProfilerFrameSnapshot();
				context.Expect(snapshot.frameId == 87u && snapshot.presentationWaits.size() == 1u
					&& snapshot.presentationWaits.front().duration.nanoseconds == 321u,
					"The narrow renderer snapshot must retain every profiler-owned frame fact.");

				Profiler& profiler = Profiler::Instance();
				profiler.Clear();
				ProfilerCaptureSettings settings;
				settings.detailLevel = ProfilerCaptureDetailLevel::Detailed;
				settings.maxFrames = 1u;
				profiler.StartRecording({}, settings);
				profiler.BeginFrame();
				profiler.EndFrame();
				const std::vector<ProfilerFrameRecord> frames = profiler.GetFramesSnapshot();
				context.Expect(frames.size() == 1u && frames.front().responsiveness.has_value()
					&& frames.front().responsiveness->presentationSequence == 87u
					&& frames.front().presentationWaits.size() == 1u
					&& frames.front().presentationWaits.front().detail == "bounded presentation wait",
					"EndFrame must preserve exact renderer identity and presentation waits through the compact snapshot.");
				profiler.Clear();
			});
	}
}
