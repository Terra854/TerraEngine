#include "TestHarness.h"

#include "Profiling/Profiler.h"
#include "Profiling/ProfilerCapture.h"
#include "Renderer/RenderTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		class TempProfilerWorkspace {
		public:
			TempProfilerWorkspace()
				: root_(std::filesystem::temp_directory_path() / "TerraEngineEngineTests" / "ProfilerCapture") {
				std::filesystem::remove_all(root_);
				std::filesystem::create_directories(root_);
			}

			~TempProfilerWorkspace() {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
			}

			[[nodiscard]] std::filesystem::path Path(const std::string& fileName) const {
				return root_ / fileName;
			}

			void WriteTextFile(const std::string& fileName, const std::string& contents) const {
				// Tests write deliberately tiny capture fixtures so failure cases
				// stay readable when CTest prints diagnostics.
				std::ofstream stream(Path(fileName), std::ios::binary | std::ios::trunc);
				stream << contents;
			}

		private:
			std::filesystem::path root_;
		};

		TerraEngine::Profiling::ProfilerModuleSummary* FindModule(std::vector<TerraEngine::Profiling::ProfilerModuleSummary>& modules, TerraEngine::Profiling::ProfilerModuleId moduleId) {
			const auto iter = std::find_if(modules.begin(), modules.end(), [moduleId](const TerraEngine::Profiling::ProfilerModuleSummary& module) {
				return module.moduleId == moduleId;
			});
			return iter != modules.end() ? &(*iter) : nullptr;
		}

		const TerraEngine::Profiling::ProfilerModuleSummary* FindModule(const std::vector<TerraEngine::Profiling::ProfilerModuleSummary>& modules, TerraEngine::Profiling::ProfilerModuleId moduleId) {
			const auto iter = std::find_if(modules.begin(), modules.end(), [moduleId](const TerraEngine::Profiling::ProfilerModuleSummary& module) {
				return module.moduleId == moduleId;
			});
			return iter != modules.end() ? &(*iter) : nullptr;
		}

		const TerraEngine::Profiling::ProfilerMetric* FindMetric(const TerraEngine::Profiling::ProfilerModuleSummary& module, const std::string& name) {
			const auto iter = std::find_if(module.metrics.begin(), module.metrics.end(), [&name](const TerraEngine::Profiling::ProfilerMetric& metric) {
				return metric.name == name;
			});
			return iter != module.metrics.end() ? &(*iter) : nullptr;
		}

		bool HasSampleNamed(const TerraEngine::Profiling::ProfilerFrameRecord& frame, const std::string& name) {
			return std::any_of(frame.samples.begin(), frame.samples.end(), [&name](const TerraEngine::Profiling::ProfilerSample& sample) {
				return sample.name == name;
			});
		}

		TerraEngine::Profiling::ProfilerCaptureDocument MakeCaptureDocument() {
			using namespace TerraEngine::Profiling;

			ProfilerCaptureDocument document;
			document.metadata.terraEngineVersion = "unit-test";
			document.metadata.projectName = "Profiler Tests";
			document.metadata.projectPathHint = "C:/Projects/TerraEngineUnit";
			document.metadata.scenePath = "Scenes/Test.tescene";
			document.metadata.startedAt = "2026-05-29 10:00:00";
			document.metadata.savedAt = "2026-05-29 10:00:01";
			document.metadata.settings.maxFrames = 16U;
			document.metadata.settings.maxDurationSeconds = 5.0;

			ProfilerModuleSummary cpuModule;
			cpuModule.moduleId = ProfilerModuleId::CpuEngine;
			cpuModule.available = true;
			cpuModule.totalMs = 4.5;
			cpuModule.metrics.push_back({"Frame duration", 4.5, "ms"});

			ProfilerModuleSummary taskModule;
			taskModule.moduleId = ProfilerModuleId::TaskExecution;
			taskModule.available = true;
			taskModule.totalMs = 1.25;
			taskModule.metrics.push_back({"Queued tasks", 2.0, "count"});

			ProfilerFrameRecord frame;
			frame.frameIndex = 3U;
			frame.frameStartMs = 12.5;
			frame.durationMs = 4.5;
			frame.complete = true;
			frame.timingStreams.push_back({
				"engine_cpu",
				"Engine CPU",
				ProfilerTimingStreamScope::EngineCpu,
				ProfilerTimingStreamSource::ProfilerClock,
				ProfilerTimingStreamStatus::Valid,
				4.5,
				{}
			});
			frame.timingStreams.push_back({
				"viewport_gpu",
				"Viewport GPU",
				ProfilerTimingStreamScope::ViewportGpu,
				ProfilerTimingStreamSource::Unavailable,
				ProfilerTimingStreamStatus::Unavailable,
				0.0,
				"GPU timestamp queries are not captured by this fixture."
			});
			frame.moduleSummaries = {cpuModule, taskModule};
			frame.samples.push_back({1U, 0U, ProfilerModuleId::CpuEngine, "Frame", "main", "", 0.0, 4.5, 0U, 1U, true});
			frame.samples.push_back({2U, 1U, ProfilerModuleId::TaskExecution, "ThreadPool Task", "worker", "Physics", 1.0, 1.25, 1U, 1U, true});
			document.frames.push_back(std::move(frame));

			return document;
		}
	}

	void RegisterProfilerCaptureTests(TestRegistry& registry) {
		registry.Add("Engine.ProfilerCapture.SealedFramesAccountBytesAndCoalesceLossDeterministically", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			ProfilerFrameRecord frame;
			frame.frameIndex = 11u;
			frame.durationMs = 2.0;
			frame.samples.push_back({1u, 0u, ProfilerModuleId::CpuEngine,
				"Optional leaf", "main", "task", 0.0, 2.0, 0u, 1u, true, false});
			ProfilerSealedFrame sealed;
			sealed.frame = frame;
			sealed.ownedByteCount = EstimateProfilerFrameOwnedBytes(sealed.frame);
			std::string error;
			context.Expect(ValidateProfilerSealedFrame(sealed, error),
				"A sealed frame with exact logical byte ownership should validate: " + error);
			sealed.ownedByteCount -= 1u;
			context.Expect(!ValidateProfilerSealedFrame(sealed, error),
				"Under-reporting owned bytes must reject before queue admission.");

			std::vector<ProfilerCaptureLossRange> ranges;
			CoalesceProfilerCaptureLossRange(ranges, {3u, 3u,
				ProfilerCaptureLossClass::SampleLeaves, ProfilerCaptureLossReason::QueuePressure, 2u, 10u});
			CoalesceProfilerCaptureLossRange(ranges, {4u, 4u,
				ProfilerCaptureLossClass::SampleLeaves, ProfilerCaptureLossReason::QueuePressure, 3u, 11u});
			context.ExpectEqual(1u, static_cast<unsigned int>(ranges.size()),
				"Adjacent equal loss policy records must coalesce into one deterministic range.");
			context.ExpectEqual(5ULL, static_cast<unsigned long long>(ranges.front().discardedRecordCount),
				"Coalesced loss counts must remain exact.");
		});

		registry.Add("Engine.ProfilerCapture.SnapshotRevisionChangesOnlyForPublishedPresentationState", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();
			const std::uint64_t clearedRevision = profiler.GetSnapshotRevision();
			profiler.LoadDocument(MakeCaptureDocument());
			const std::uint64_t loadedRevision = profiler.GetSnapshotRevision();
			context.Expect(loadedRevision > clearedRevision,
				"Loading immutable capture data should publish a newer presentation revision.");

			(void)profiler.GetFrameListSnapshot();
			(void)profiler.GetSelectedFrameSnapshot();
			context.ExpectEqual(loadedRevision, profiler.GetSnapshotRevision(),
				"Read-only UI snapshots must not invalidate the presentation revision.");

			profiler.SelectFrame(999u);
			context.Expect(profiler.GetSnapshotRevision() > loadedRevision,
				"Changing the selected frame should publish a newer presentation revision.");
			profiler.Clear();
		});

		registry.Add("Engine.ProfilerCapture.HelpersReportModulesAndTrimFrameBuffers", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			ProfilerCaptureSettings settings;
			settings.cpuEngineEnabled = false;
			settings.taskExecutionEnabled = true;
			context.Expect(AnyModuleEnabled(settings), "At least one enabled profiler module should make capture settings usable.");

			std::vector<ProfilerModuleSummary> modules = MakeDefaultModuleSummaries(settings);
			const ProfilerModuleSummary* cpuModule = FindModule(modules, ProfilerModuleId::CpuEngine);
			const ProfilerModuleSummary* taskModule = FindModule(modules, ProfilerModuleId::TaskExecution);
			context.Expect(cpuModule != nullptr && !cpuModule->available, "Default module summaries should keep disabled CPU modules unavailable.");
			context.Expect(taskModule != nullptr && taskModule->available, "Default module summaries should keep enabled task modules available.");

			settings.maxFrames = 2U;
			settings.maxDurationSeconds = 0.0;
			std::vector<ProfilerFrameRecord> frameLimited{{1U}, {2U}, {3U}};
			TrimFrameBuffer(frameLimited, settings);
			context.ExpectEqual(2u, static_cast<unsigned int>(frameLimited.size()), "Frame trimming should cap the rolling buffer to maxFrames.");
			context.ExpectEqual(2ULL, frameLimited.front().frameIndex, "Frame trimming should discard the oldest records first.");

			settings.maxFrames = 0U;
			std::vector<ProfilerFrameRecord> unlimited{{1U}, {2U}, {3U}};
			TrimFrameBuffer(unlimited, settings);
			context.ExpectEqual(3u, static_cast<unsigned int>(unlimited.size()), "A zero maxFrames value should retain the complete capture.");
			context.ExpectEqual(1ULL, unlimited.front().frameIndex, "Unlimited capture history should preserve the oldest recorded frame.");

			settings.maxFrames = 10U;
			settings.maxDurationSeconds = 1.0;
			std::vector<ProfilerFrameRecord> durationLimited;
			durationLimited.push_back({1U, 0.0});
			durationLimited.push_back({2U, 1000.0});
			durationLimited.push_back({3U, 2500.0});
			TrimFrameBuffer(durationLimited, settings);
			context.ExpectEqual(1u, static_cast<unsigned int>(durationLimited.size()), "Duration trimming should discard frames outside the rolling time window.");
			context.ExpectEqual(3ULL, durationLimited.front().frameIndex, "Duration trimming should keep the newest frame in the time window.");
		});

		registry.Add("Engine.ProfilerCapture.InMemoryRecordingCapsHistoryWithoutJournal", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();
			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Bounded In-Memory History";
			ProfilerCaptureSettings settings;
			settings.detailLevel = ProfilerCaptureDetailLevel::Aggregate;
			settings.maxFrames = 0u;
			profiler.StartRecording(metadata, settings);
			// Cross both the former legacy boundary and the diagnostic cap. The public
			// non-journal API has no durable destination, so it must never retain an
			// unbounded detailed history merely because maxFrames was zero.
			for (std::uint32_t frame = 0u; frame < 2050u; ++frame)
			{
				profiler.BeginFrame();
				profiler.EndFrame();
			}
			const ProfilerJournalFinalizationTicket finishTicket = profiler.FinishRecording();
			context.Expect(finishTicket.accepted
				&& profiler.GetJournalFinalizationStatus(finishTicket).state == ProfilerJournalFinalizationState::Completed,
				"A bounded in-memory capture should complete the asynchronous finish contract immediately.");
			const ProfilerCaptureDocument capture = profiler.GetDocumentSnapshot();
			context.ExpectEqual(
				static_cast<std::size_t>(512u),
				capture.frames.size(),
				"In-memory recording must retain only its fixed diagnostic presentation cache.");
			context.Expect(!capture.frames.empty()
				&& capture.frames.front().frameIndex == 1538u
				&& capture.frames.back().frameIndex == 2049u,
				"The bounded cache must evict only the oldest chronological frames.");
			profiler.Clear();
		});

		registry.Add("Engine.ProfilerCapture.SaveLoadAndArtifactRoundTripPreservesFrameData", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			TempProfilerWorkspace workspace;
			const std::filesystem::path capturePath = workspace.Path("round-trip.teprof");
			ProfilerCaptureDocument document = MakeCaptureDocument();
			std::string error;

			context.Expect(SaveCaptureDocument(document, capturePath, error), "Saving a profiler capture document should succeed: " + error);

			ProfilerCaptureDocument loaded;
			const bool loadedCapture = LoadCaptureDocument(capturePath, loaded, error);
			context.Expect(loadedCapture, "Loading a saved profiler capture document should succeed: " + error);
			context.ExpectEqual(std::string("Profiler Tests"), loaded.metadata.projectName, "Profiler capture metadata should round-trip through Journal V1.");
			context.Expect(loaded.metadata.settings.captureMode == ProfilerCaptureMode::LoadedReadOnly, "Loaded capture settings should be marked read-only.");
			context.ExpectEqual(1u, static_cast<unsigned int>(loaded.frames.size()), "Profiler capture frames should round-trip through Journal V1.");
			context.ExpectEqual(3ULL, loaded.frames.front().frameIndex, "Profiler frame indices should round-trip through Journal V1.");
			context.Expect(HasSampleNamed(loaded.frames.front(), "Frame"), "Profiler samples should round-trip through Journal V1.");
			context.Expect(HasSampleNamed(loaded.frames.front(), "ThreadPool Task"), "Task profiler samples should round-trip through Journal V1.");
			context.ExpectEqual(2u, static_cast<unsigned int>(loaded.frames.front().timingStreams.size()), "Profiler timing streams should round-trip through Journal V1.");

			const ProfilerModuleSummary* taskModule = FindModule(loaded.frames.front().moduleSummaries, ProfilerModuleId::TaskExecution);
			context.Expect(taskModule != nullptr, "Loaded captures should preserve task module summaries.");
			context.Expect(taskModule != nullptr && FindMetric(*taskModule, "Queued tasks") != nullptr, "Loaded captures should preserve module metrics.");

			ProfilerCaptureArtifact artifact;
			context.Expect(TryReadCaptureArtifact(capturePath, artifact), "Capture artifact inspection should accept a saved capture.");
			context.Expect(artifact.supported, "Saved captures should be reported as supported artifacts.");
			context.ExpectEqual(std::string("round-trip.teprof"), artifact.fileName, "Capture artifacts should expose the source file name.");
			context.ExpectEqual(1ULL, artifact.frameCount, "Capture artifacts should expose the saved frame count.");

			ProfilerCaptureArtifact metadataArtifact;
			context.Expect(TryReadCaptureArtifactMetadata(capturePath, metadataArtifact), "Capture artifact metadata inspection should accept a saved capture without loading frames.");
			context.Expect(metadataArtifact.supported, "Metadata-only capture inspection should report saved captures as supported artifacts.");
			context.ExpectEqual(std::string("Profiler Tests"), metadataArtifact.metadata.projectName, "Metadata-only inspection should preserve the saved project name.");
			context.ExpectEqual(1ULL, metadataArtifact.frameCount, "Metadata-only inspection should expose the saved frame count.");
			context.ExpectEqual(std::string("Ready (journal index)"), metadataArtifact.status, "Newly saved captures should expose a bounded V1 index without loading sample payloads.");
		});

		registry.Add("Engine.ProfilerCapture.LoadRejectsOpaqueHistoricalFormat", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			TempProfilerWorkspace workspace;
			std::string error;
			ProfilerCaptureDocument loaded;
			const std::string oldFormat = "TerraEngineProfiler" "Capture";
			const std::string oldVersionField = "payload" "Version";
			workspace.WriteTextFile("historical.teprof",
				"{\"format\":\"" + oldFormat + "\",\"" + oldVersionField + "\":1}");
			context.Expect(!LoadCaptureDocument(workspace.Path("historical.teprof"), loaded, error),
				"The V1-only loader must reject historical JSON before parsing its fields.");
			context.Expect(error.find("Unsupported profiler capture") != std::string::npos,
				"Historical bytes should fail at the unambiguous V1 magic check.");
		});

		registry.Add("Engine.ProfilerCapture.TimingStreamsAggregateValidAndMissingSamples", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			ProfilerFrameRecord first;
			first.frameIndex = 1u;
			AddOrReplaceTimingStreamSample(first, {"engine_cpu", "Engine CPU", ProfilerTimingStreamScope::EngineCpu, ProfilerTimingStreamSource::ProfilerClock, ProfilerTimingStreamStatus::Valid, 1.0, {}});
			AddOrReplaceTimingStreamSample(first, {"viewport_gpu", "Viewport GPU", ProfilerTimingStreamScope::ViewportGpu, ProfilerTimingStreamSource::Unavailable, ProfilerTimingStreamStatus::Unavailable, 0.0, "not captured"});

			ProfilerFrameRecord second;
			second.frameIndex = 2u;
			AddOrReplaceTimingStreamSample(second, {"engine_cpu", "Engine CPU", ProfilerTimingStreamScope::EngineCpu, ProfilerTimingStreamSource::ProfilerClock, ProfilerTimingStreamStatus::Valid, 3.0, {}});
			AddOrReplaceTimingStreamSample(second, {"viewport_gpu", "Viewport GPU", ProfilerTimingStreamScope::ViewportGpu, ProfilerTimingStreamSource::Unavailable, ProfilerTimingStreamStatus::Unavailable, 0.0, "not captured"});

			const std::vector<ProfilerTimingStreamAggregate> aggregates = BuildTimingStreamAggregates({first, second}, second);
			const auto engine = std::find_if(aggregates.begin(), aggregates.end(), [](const ProfilerTimingStreamAggregate& aggregate) {
				return aggregate.id == "engine_cpu";
			});
			const auto gpu = std::find_if(aggregates.begin(), aggregates.end(), [](const ProfilerTimingStreamAggregate& aggregate) {
				return aggregate.id == "viewport_gpu";
			});

			context.Expect(engine != aggregates.end(), "Engine CPU timing aggregate should be present.");
			context.Expect(engine != aggregates.end() && engine->sampleCount == 2u, "Valid timing streams should count measured samples.");
			context.Expect(engine != aggregates.end() && engine->meanMs == 2.0, "Timing aggregates should compute means from valid samples.");
			context.Expect(engine != aggregates.end() && engine->medianMs == 2.0, "Timing aggregates should compute medians from valid samples.");
			context.Expect(gpu != aggregates.end(), "GPU timing aggregate should be present even when unavailable.");
			context.Expect(gpu != aggregates.end() && gpu->sampleCount == 0u, "Unavailable GPU samples should not count as valid measurements.");
			context.Expect(gpu != aggregates.end() && gpu->missingCount == 2u, "Unavailable GPU samples should be counted as missing.");
			context.Expect(gpu != aggregates.end() && gpu->selectedStatus == ProfilerTimingStreamStatus::Unavailable, "Selected unavailable GPU timing should preserve status instead of showing 0 ms.");
		});

		registry.Add("Engine.Profiler.RecordRenderStatsPublishesGpuTimestampStreams", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();

			ProfilerCaptureMetadata metadata;
			metadata.projectName = "GPU Timing";
			ProfilerCaptureSettings settings;
			settings.maxFrames = 2U;

			RenderStatsSnapshot gameStats;
			gameStats.valid = true;
			gameStats.frameTimeMs = 4.0f;
			gameStats.gpuTimestampValid = true;
			gameStats.gpuTimestampMs = 2.5f;
			gameStats.optimization.frameGeneration = 8U;
			gameStats.optimization.topologyGeneration = 3U;
			gameStats.optimization.viewGeneration = 4U;
			gameStats.optimization.rendererGeneration = 2U;
			gameStats.optimization.visibleInstanceCount = 100U;
			gameStats.optimization.groupedInstanceCount = 80U;
			gameStats.optimization.fallbackInstanceCount = 20U;
			gameStats.optimization.groupedSubmissionCount = 1U;
			gameStats.optimization.fallbackSubmissionCount = 20U;
			gameStats.optimization.avoidedSubmissionCount = 79U;
			gameStats.optimization.instanceStreamBytes = 6400U;
			gameStats.optimization.instanceStreamCapacityBytes = 8192U;
			gameStats.optimization.valid = 1U;

			RenderStatsSnapshot sceneStats;
			sceneStats.viewType = RenderViewType::Scene;
			sceneStats.valid = true;
			sceneStats.frameTimeMs = 6.0f;
			sceneStats.gpuTimestampValid = false;
			sceneStats.gpuTimestampReason = "No Scene View timestamp was written.";

			profiler.StartRecording(metadata, settings);
			profiler.BeginFrame();
			profiler.RecordRenderStats(gameStats, sceneStats);
			profiler.EndFrame();

			const std::vector<ProfilerFrameRecord> frames = profiler.GetFramesSnapshot();
			context.Expect(!frames.empty(), "Render stats should publish timing streams into the active profiler frame.");
			if (!frames.empty()) {
				const auto gameGpu = std::find_if(frames.front().timingStreams.begin(), frames.front().timingStreams.end(), [](const ProfilerTimingStreamSample& sample) {
					return sample.id == "viewport_gpu";
				});
				context.Expect(gameGpu != frames.front().timingStreams.end(), "Consolidated Viewport GPU timing stream should be present.");
				context.Expect(gameGpu != frames.front().timingStreams.end() && gameGpu->source == ProfilerTimingStreamSource::BackendGpuTimestamp, "Valid renderer GPU timestamps should be marked as backend GPU timestamp samples.");
				context.Expect(gameGpu != frames.front().timingStreams.end() && gameGpu->status == ProfilerTimingStreamStatus::Valid && gameGpu->milliseconds == 2.5, "Valid renderer GPU timestamps should preserve the measured duration.");
				context.Expect(frames.front().renderOptimization.has_value(),
					"Render stats should publish fixed renderer optimization evidence with the same completed frame.");
				if (frames.front().renderOptimization.has_value())
				{
					const ProfilerRenderOptimizationViewSummary& evidence =
						frames.front().renderOptimization->views[0];
					context.Expect(evidence.valid && evidence.frameGeneration == 8U
						&& evidence.groupedInstanceCount == 80U
						&& evidence.fallbackInstanceCount == 20U
						&& evidence.avoidedSubmissionCount == 79U,
						"Captured renderer evidence must preserve valid generations and conserved group counters.");
				}

				const ProfilerModuleSummary* cpuModule = FindModule(frames.front().moduleSummaries, ProfilerModuleId::CpuEngine);
				context.Expect(cpuModule != nullptr, "Render statistics should retain the CPU/Pipeline diagnostic module.");
				if (cpuModule != nullptr) {
					context.Expect(FindMetric(*cpuModule, "Frame duration") != nullptr, "CPU/Pipeline should expose the selected profiler frame duration.");
					context.Expect(FindMetric(*cpuModule, "Game frame time") == nullptr, "A delayed Game View input delta must not be presented as selected-frame profiler timing.");
					context.Expect(FindMetric(*cpuModule, "Scene frame time") == nullptr, "A delayed Scene View input delta must not be presented as selected-frame profiler timing.");
					context.Expect(FindMetric(*cpuModule, "Game draw calls") != nullptr, "Non-timing renderer diagnostics should remain available.");
				}
			}

			profiler.Clear();
		});

		registry.Add("Engine.Profiler.ViewportCpuUsesMeasuredProfilerScopes", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();

			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Viewport CPU";
			ProfilerCaptureSettings settings;
			settings.maxFrames = 2U;

			RenderStatsSnapshot gameStats;
			gameStats.valid = true;
			gameStats.frameTimeMs = 40.0f;
			RenderStatsSnapshot sceneStats;
			sceneStats.valid = true;
			sceneStats.frameTimeMs = 50.0f;

			profiler.StartRecording(metadata, settings);
			profiler.BeginFrame();
			context.Expect(profiler.BeginSample("RenderSystem: Submit Game View Cameras", ProfilerModuleId::CpuEngine), "RenderSystem view submit scopes should be captured while recording.");
			profiler.EndSample();
			profiler.RecordRenderStats(gameStats, sceneStats);
			profiler.EndFrame();

			const std::vector<ProfilerFrameRecord> frames = profiler.GetFramesSnapshot();
			context.Expect(!frames.empty(), "Viewport CPU timing should be written into the completed profiler frame.");
			if (!frames.empty()) {
				const auto viewportCpu = std::find_if(frames.front().timingStreams.begin(), frames.front().timingStreams.end(), [](const ProfilerTimingStreamSample& sample) {
					return sample.id == "viewport_cpu";
				});
				context.Expect(viewportCpu != frames.front().timingStreams.end(), "Viewport CPU timing should be present when a viewport submit scope was measured.");
				context.Expect(viewportCpu != frames.front().timingStreams.end() && viewportCpu->source == ProfilerTimingStreamSource::ProfilerClock, "Viewport CPU timing should come from profiler scopes, not renderer frame stats.");
				context.Expect(viewportCpu != frames.front().timingStreams.end() && viewportCpu->milliseconds < 90.0, "Viewport CPU timing should not copy RenderStatsSnapshot frameTimeMs.");
			}

			profiler.Clear();
		});

		registry.Add("Engine.Profiler.EditorWorkIsExcludedFromGameProfiler", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();
			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Editor exclusion";
			ProfilerCaptureSettings settings;
			settings.maxFrames = 2U;

			profiler.StartRecording(metadata, settings);
			const auto rawFrameStarted = std::chrono::steady_clock::now();
			profiler.BeginFrame();
			const bool engineSample = profiler.BeginSample("Engine::Update", ProfilerModuleId::CpuEngine);
			const bool editorWrapper = profiler.BeginSample("JobGraph::ProtectedJob", ProfilerModuleId::CpuEngine, "EditorSystem");
			const bool editorBoundary = profiler.BeginEditorWork();
			context.Expect(!profiler.BeginSample("EditorWindow::Draw", ProfilerModuleId::CpuEngine, "Profiler"),
				"Editor markers must be suppressed while the editor exclusion interval is active.");
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			profiler.EndEditorWork(editorBoundary);
			if (editorWrapper) profiler.EndSample();
			if (engineSample) profiler.EndSample();
			profiler.EndFrame();
			const double rawFrameMs = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - rawFrameStarted).count();

			const std::vector<ProfilerFrameRecord> frames = profiler.GetFramesSnapshot();
			context.ExpectEqual(1U, static_cast<unsigned int>(frames.size()),
				"The separated editor interval must still publish one complete profiler frame.");
			if (!frames.empty())
			{
				const ProfilerFrameRecord& frame = frames.front();
				context.Expect(!HasSampleNamed(frame, "EditorWindow::Draw"),
					"Editor windows must never appear in CPU Engine samples or Top Offenders.");
				context.Expect(!HasSampleNamed(frame, "JobGraph::ProtectedJob"),
					"The scheduler wrapper dedicated to EditorSystem must be removed with the editor interval.");
				const auto engineTiming = std::find_if(frame.timingStreams.begin(), frame.timingStreams.end(), [](const ProfilerTimingStreamSample& sample) {
					return sample.id == "engine_cpu";
				});
				const auto editorTiming = std::find_if(frame.timingStreams.begin(), frame.timingStreams.end(), [](const ProfilerTimingStreamSample& sample) {
					return sample.id == "editor_wall";
				});
				context.Expect(engineTiming != frame.timingStreams.end(),
					"The game-facing Engine CPU timing stream must remain available.");
				context.Expect(editorTiming == frame.timingStreams.end(),
					"Editor Wall must not be published by the game-facing profiler.");
				context.Expect(engineTiming != frame.timingStreams.end()
					&& std::abs(engineTiming->milliseconds - frame.durationMs) <= 0.000001,
					"Profiler frame duration must equal editor-excluded Engine CPU time.");
				context.Expect(frame.durationMs + 10.0 < rawFrameMs,
					"Profiler frame duration must remove the measured editor interval from raw wall time.");
				context.Expect(std::all_of(frame.samples.begin(), frame.samples.end(), [&frame](const ProfilerSample& sample) {
					return sample.startMs + sample.durationMs <= frame.durationMs + 0.000001;
				}), "Editor-excluded sample timelines must remain inside the game-facing frame duration.");
			}
			profiler.Clear();
		});

		registry.Add("Engine.Profiler.LatestEngineTimingExcludesIdleEditorWork", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();
			const auto rawFrameStarted = std::chrono::steady_clock::now();
			profiler.BeginLatestEngineFrameTiming();
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			const bool editorBoundary = profiler.BeginEditorWork();
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			profiler.EndEditorWork(editorBoundary);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			profiler.PublishLatestEngineFrameTiming();
			const double rawFrameMs = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - rawFrameStarted).count();
			const ProfilerLatestEngineFrameTiming latest = profiler.GetLatestEngineFrameTiming();

			context.Expect(latest.valid,
				"The capture-independent Engine CPU clock must publish while the profiler is idle.");
			context.Expect(latest.durationMs + 10.0 < rawFrameMs,
				"The idle Engine CPU clock must exclude the protected EditorSystem interval.");
			context.Expect(latest.frameIndex != 0u,
				"Each capture-independent Engine CPU publication must advance its stable frame index.");
		});

		registry.Add("Engine.Profiler.SceneViewAndPresentationStreamsRemainExplicit", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();
			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Typed editor evidence";
			ProfilerCaptureSettings settings;
			settings.maxFrames = 2U;
			const std::vector<PerformanceStageRecord> stages = {{
				PerformanceStageDomain::SceneView,
				"SceneView.Projection",
				{TelemetryAvailability::Available, 42'000U, {}},
				"Scene View marker preparation",
				128U,
				false}};
			const std::array<ProfilerEditorStageTiming, 1u> editorStages{{
				{"Editor.MainDraw", 63'000U, 1U}}};

			profiler.StartRecording(metadata, settings);
			profiler.BeginFrame();
			profiler.RecordSceneViewStages(stages);
			profiler.RecordEditorStages(editorStages);
			profiler.EndFrame();

			const std::vector<ProfilerFrameRecord> frames = profiler.GetFramesSnapshot();
			context.ExpectEqual(std::size_t(1U), frames.size(), "Typed editor evidence should publish with its owning profiler frame.");
			if (!frames.empty())
			{
				context.Expect(std::any_of(frames.front().performanceStages.begin(), frames.front().performanceStages.end(), [](const PerformanceStageRecord& stage) {
					return stage.domain == PerformanceStageDomain::SceneView && stage.stageName == "SceneView.Projection";
				}), "Validated Scene View stages must survive the protected editor boundary as value records.");
				context.Expect(std::any_of(frames.front().performanceStages.begin(), frames.front().performanceStages.end(), [](const PerformanceStageRecord& stage) {
					return stage.domain == PerformanceStageDomain::Editor && stage.stageName == "Editor.MainDraw"
						&& stage.duration.nanoseconds == 63'000U;
				}), "Batched editor stage timings must survive without becoming ordinary CPU samples.");
				context.Expect(!frames.front().presentationWaits.empty(), "Presentation-wait evidence must remain present even when its clock is explicitly unavailable.");
				context.Expect(std::all_of(frames.front().presentationWaits.begin(), frames.front().presentationWaits.end(), [](const PresentationWaitRecord& wait) {
					return ValidatePresentationWaitRecord(wait).valid;
				}), "Available and unavailable presentation-wait records must both retain valid typed semantics.");
			}
			profiler.Clear();
		});

		registry.Add("Engine.Profiler.RecordingAggregatesSamplesAndTaskCounters", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();

			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Runtime Profiling";
			ProfilerCaptureSettings settings;
			settings.maxFrames = 4U;

			profiler.StartRecording(metadata, settings);
			profiler.BeginFrame();
			profiler.RecordTaskQueued();
			const bool outerSampleActive = profiler.BeginSample("Outer", ProfilerModuleId::CpuEngine);
			context.Expect(outerSampleActive, "Profiler scopes should report that a sample was opened while recording.");
			const bool innerSampleActive = profiler.BeginSample("Inner", ProfilerModuleId::CpuEngine);
			context.Expect(innerSampleActive, "Nested CPU scopes should be retained for self-time analysis.");
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			profiler.EndSample();
			const bool taskSampleActive = profiler.RecordTaskStarted("UnitTask");
			context.Expect(taskSampleActive, "Task profiler scopes should report an active sample while recording.");
			profiler.RecordTaskFinished(taskSampleActive);
			profiler.EndSample();
			profiler.EndFrame();

			std::vector<ProfilerFrameRecord> frames = profiler.GetFramesSnapshot();
			context.ExpectEqual(1u, static_cast<unsigned int>(frames.size()), "Ending a recorded frame should publish one frame snapshot.");
			context.Expect(frames.front().complete, "Completed frames should be marked complete.");
			context.Expect(HasSampleNamed(frames.front(), "Outer"), "Explicit profiler samples should be stored in the completed frame.");
			context.Expect(HasSampleNamed(frames.front(), "Inner"), "Nested profiler samples should be stored in the completed frame.");
			context.Expect(HasSampleNamed(frames.front(), "ThreadPool Task"), "Task profiler samples should be stored in the completed frame.");
			const ProfilerModuleSummary* cpuModule = FindModule(frames.front().moduleSummaries, ProfilerModuleId::CpuEngine);
			double inclusiveCpuSumMs = 0.0;
			for (const ProfilerSample& sample : frames.front().samples)
			{
				if (sample.moduleId == ProfilerModuleId::CpuEngine)
				{
					inclusiveCpuSumMs += sample.durationMs;
				}
			}
			context.Expect(cpuModule != nullptr && cpuModule->totalMs < inclusiveCpuSumMs,
				"Module CPU work should merge nested intervals instead of adding inclusive scope durations.");
			const ProfilerMetric* coverageMetric = cpuModule != nullptr ? FindMetric(*cpuModule, "Scope coverage") : nullptr;
			context.Expect(coverageMetric != nullptr && coverageMetric->value >= 0.0 && coverageMetric->value <= 100.0,
				"CPU summaries should expose bounded wall-time scope coverage.");

			const ProfilerModuleSummary* taskModule = FindModule(frames.front().moduleSummaries, ProfilerModuleId::TaskExecution);
			const ProfilerMetric* queuedMetric = taskModule != nullptr ? FindMetric(*taskModule, "Queued tasks") : nullptr;
			const ProfilerMetric* startedMetric = taskModule != nullptr ? FindMetric(*taskModule, "Started tasks") : nullptr;
			const ProfilerMetric* finishedMetric = taskModule != nullptr ? FindMetric(*taskModule, "Finished tasks") : nullptr;
			context.Expect(taskModule != nullptr && taskModule->available, "Task module summaries should become available when task samples are recorded.");
			context.Expect(queuedMetric != nullptr && queuedMetric->value == 1.0, "Task queue counters should be aggregated into frame metrics.");
			context.Expect(startedMetric != nullptr && startedMetric->value == 1.0, "Task start counters should be aggregated into frame metrics.");
			context.Expect(finishedMetric != nullptr && finishedMetric->value == 1.0, "Task finish counters should be aggregated into frame metrics.");

			const std::vector<ProfilerFrameListItem> frameList = profiler.GetFrameListSnapshot();
			context.ExpectEqual(1u, static_cast<unsigned int>(frameList.size()), "The profiler window should be able to request a lightweight frame list.");
			context.ExpectEqual(frames.front().frameIndex, frameList.front().frameIndex, "Frame list items should expose the recorded frame index.");
			context.ExpectEqual(frames.front().complete, frameList.front().complete, "Frame list items should expose completion state without copying samples.");

			const ProfilerJournalFinalizationTicket finishTicket = profiler.FinishRecording();
			const ProfilerCaptureDocument finishedDocument = profiler.GetDocumentSnapshot();
			context.ExpectEqual(1u, static_cast<unsigned int>(finishedDocument.frames.size()), "Finishing after a complete frame should preserve recorded frames.");
			context.Expect(profiler.GetJournalFinalizationStatus(finishTicket).state == ProfilerJournalFinalizationState::Completed,
				"The in-memory finish ticket should report completion without copying history.");
			context.Expect(profiler.GetState() == ProfilerSessionState::Finalizing, "Finishing a capture should transition the profiler to finalizing.");
			profiler.Clear();
		});

		registry.Add("Engine.Profiler.RollingHistoryRetainsNewestFramesWithoutShiftingPayloads", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();
			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Rolling deque history";
			ProfilerCaptureSettings settings;
			settings.maxFrames = 3U;

			profiler.StartRecording(metadata, settings);
			for (std::uint64_t frameIndex = 0U; frameIndex < 9U; ++frameIndex)
			{
				profiler.BeginFrame(true);
				const bool sampleActive = profiler.BeginSample("Detailed Rolling Sample", ProfilerModuleId::CpuEngine);
				if (sampleActive)
				{
					profiler.EndSample();
				}
				profiler.EndFrame();
			}

			const std::vector<ProfilerFrameRecord> frames = profiler.GetFramesSnapshot();
			const std::vector<ProfilerFrameListItem> compactFrames = profiler.GetFrameListSnapshot();
			context.ExpectEqual(std::size_t(3U), frames.size(),
				"Rolling detailed history should remain at the exact configured frame limit.");
			context.ExpectEqual(std::size_t(3U), compactFrames.size(),
				"Compact frame snapshots should expose the same bounded retained history.");
			if (frames.size() == 3U && compactFrames.size() == 3U)
			{
				context.ExpectEqual(std::uint64_t(6U), frames.front().frameIndex,
					"Constant-time eviction should discard the oldest detailed frames first.");
				context.ExpectEqual(std::uint64_t(8U), frames.back().frameIndex,
					"Constant-time eviction should preserve the newest completed frame.");
				context.ExpectEqual(frames.front().frameIndex, compactFrames.front().frameIndex,
					"Detailed and compact snapshots should retain the same chronological order.");
			}

			const ProfilerJournalFinalizationTicket finishTicket = profiler.FinishRecording();
			const ProfilerCaptureDocument finished = profiler.GetDocumentSnapshot();
			context.Expect(profiler.GetJournalFinalizationStatus(finishTicket).state == ProfilerJournalFinalizationState::Completed,
				"Rolling capture finish should use the common ticket contract.");
			context.ExpectEqual(std::size_t(3U), finished.frames.size(),
				"Finishing should materialize the deque-backed rolling history into the stable vector artifact schema.");
			profiler.Clear();
		});

		registry.Add("Engine.Profiler.LoadedHistoryNormalizesFrameIndexOrder", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			ProfilerCaptureDocument loaded;
			loaded.metadata.projectName = "Recovered out-of-order capture";
			loaded.frames = {
				{.frameIndex = 9U, .durationMs = 9.0, .complete = true},
				{.frameIndex = 3U, .durationMs = 3.0, .complete = true},
				{.frameIndex = 7U, .durationMs = 7.0, .complete = true}};

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();
			profiler.LoadDocument(loaded);
			const std::vector<ProfilerFrameListItem> frames = profiler.GetFrameListSnapshot();
			context.ExpectEqual(std::size_t(3U), frames.size(),
				"Loading should retain every readable recovered frame.");
			if (frames.size() == 3U)
			{
				context.ExpectEqual(std::uint64_t(3U), frames[0].frameIndex,
					"Loaded compact history should begin with the smallest frame index.");
				context.ExpectEqual(std::uint64_t(7U), frames[1].frameIndex,
					"Loaded compact history should remain monotonically ordered.");
				context.ExpectEqual(std::uint64_t(9U), frames[2].frameIndex,
					"Loaded compact history should end with the largest frame index.");
			}
			profiler.Clear();
		});

		registry.Add("Engine.Profiler.CaptureDetailControlsLiveInstrumentationCost", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();
			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Capture detail cost";

			ProfilerCaptureSettings aggregateSettings;
			aggregateSettings.detailLevel = ProfilerCaptureDetailLevel::Aggregate;
			profiler.StartRecording(metadata, aggregateSettings);
			profiler.BeginFrame(true);
			context.Expect(!profiler.BeginSample("Aggregate Scope", ProfilerModuleId::CpuEngine),
				"Aggregate captures should skip optional marker locking in the live instrumentation path.");
			context.Expect(!profiler.RecordTaskStarted("Aggregate Task"),
				"Aggregate captures should skip optional task locking in the live instrumentation path.");
			profiler.EndFrame();
			const std::vector<ProfilerFrameRecord> aggregateFrames = profiler.GetFramesSnapshot();
			context.ExpectEqual(std::size_t(1U), aggregateFrames.size(),
				"Aggregate captures must still retain their critical frame evidence.");
			context.ExpectEqual(std::size_t(1U), aggregateFrames.front().samples.size(),
				"Aggregate captures should retain one fixed frame-root sample for timeline consumers.");
			context.Expect(HasSampleNamed(aggregateFrames.front(), "Engine Frame")
				&& aggregateFrames.front().samples.front().retainInAggregate,
				"The aggregate frame root must be explicitly marked so compact persistence can retain it.");
			TempProfilerWorkspace aggregateWorkspace;
			const std::filesystem::path aggregatePath = aggregateWorkspace.Path("aggregate-root.teprof");
			const ProfilerJournalFinalizationTicket aggregateFinishTicket = profiler.FinishRecording();
			context.Expect(profiler.GetJournalFinalizationStatus(aggregateFinishTicket).state
				== ProfilerJournalFinalizationState::Completed,
				"Aggregate in-memory capture should finish through the ticket contract.");
			ProfilerCaptureDocument aggregateDocument = profiler.GetDocumentSnapshot();
			std::string aggregateError;
			context.Expect(SaveCaptureDocument(aggregateDocument, aggregatePath, aggregateError),
				"Aggregate frame roots should save through the compact capture format: " + aggregateError);
			ProfilerCaptureDocument loadedAggregateDocument;
			// Evaluate the reader before constructing its diagnostic. Function-call
			// argument evaluation order must not hide the error text on a failure.
			const bool loadedAggregate = LoadCaptureDocument(
				aggregatePath, loadedAggregateDocument, aggregateError);
			context.Expect(loadedAggregate,
				"Aggregate frame roots should load through the compact capture format: " + aggregateError);
			context.Expect(!loadedAggregateDocument.frames.empty()
				&& HasSampleNamed(loadedAggregateDocument.frames.front(), "Engine Frame")
				&& loadedAggregateDocument.frames.front().samples.front().retainInAggregate,
				"Saved aggregate captures must preserve the fixed frame-root sample and its retention contract.");
			context.Expect(!loadedAggregateDocument.frames.empty()
				&& !loadedAggregateDocument.frames.front().timingStreams.empty(),
				"Saved aggregate captures must preserve compact timing lanes alongside the frame root.");
			const std::vector<ProfilerTimingStreamAggregate> aggregateTimingStreams =
				BuildTimingStreamAggregates(loadedAggregateDocument.frames);
			const auto engineTiming = std::find_if(
				aggregateTimingStreams.begin(),
				aggregateTimingStreams.end(),
				[](const ProfilerTimingStreamAggregate& stream) { return stream.id == "engine_cpu"; });
			context.Expect(engineTiming != aggregateTimingStreams.end() && engineTiming->sampleCount == 1U,
				"Aggregate timing summaries must count the persisted engine frame timing.");
			profiler.Clear();

			ProfilerCaptureSettings sampledSettings;
			sampledSettings.detailLevel = ProfilerCaptureDetailLevel::Sampled;
			sampledSettings.sampleIntervalFrames = 4U;
			profiler.StartRecording(metadata, sampledSettings);
			for (std::uint32_t frameIndex = 0U; frameIndex < 5U; ++frameIndex)
			{
				profiler.BeginFrame(true);
				profiler.RecordTaskQueued();
				const bool taskSampleActive = profiler.RecordTaskStarted("Sampled Task");
				const bool sampleActive = profiler.BeginSample("Sampled Scope", ProfilerModuleId::CpuEngine);
				context.ExpectEqual(frameIndex == 0U || frameIndex == 4U, sampleActive,
					"Sampled captures should enable optional scopes only on their deterministic cadence.");
				if (sampleActive)
				{
					profiler.EndSample();
				}
				profiler.RecordTaskFinished(taskSampleActive);
				profiler.EndFrame();
			}
			const std::vector<ProfilerFrameRecord> sampledFrames = profiler.GetFramesSnapshot();
			context.ExpectEqual(std::size_t(5U), sampledFrames.size(),
				"Sampled capture cadence must not drop critical frame records.");
			context.Expect(HasSampleNamed(sampledFrames[0], "Sampled Scope")
				&& sampledFrames[1].samples.empty()
				&& sampledFrames[2].samples.empty()
				&& sampledFrames[3].samples.empty()
				&& HasSampleNamed(sampledFrames[4], "Sampled Scope"),
				"Only cadence frames should retain sampled optional markers.");
			context.Expect(std::all_of(sampledFrames.begin(), sampledFrames.end(), [](const ProfilerFrameRecord& frame) {
				const ProfilerModuleSummary* taskModule =
					FindModule(frame.moduleSummaries, ProfilerModuleId::TaskExecution);
				const ProfilerMetric* queuedMetric =
					taskModule != nullptr ? FindMetric(*taskModule, "Queued tasks") : nullptr;
				const ProfilerMetric* startedMetric =
					taskModule != nullptr ? FindMetric(*taskModule, "Started tasks") : nullptr;
				const ProfilerMetric* finishedMetric =
					taskModule != nullptr ? FindMetric(*taskModule, "Finished tasks") : nullptr;
				return queuedMetric != nullptr && queuedMetric->value == 1.0
					&& startedMetric != nullptr && startedMetric->value == 1.0
					&& finishedMetric != nullptr && finishedMetric->value == 1.0;
			}), "Sampled captures should retain compact task counters on every frame, including off-cadence frames.");
			profiler.Clear();
		});

		registry.Add("Engine.Profiler.RecordingArmsOutsidePlayModeAndCapturesOnlyPlayFrames", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();
			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Play-only profiling";
			ProfilerCaptureSettings settings;
			settings.maxFrames = 4U;

			profiler.StartRecording(metadata, settings);
			profiler.BeginFrame(false);
			context.Expect(profiler.GetState() == ProfilerSessionState::Recording,
				"Recording should remain armed while Play Mode is stopped.");
			context.Expect(profiler.GetStatusText().find("waiting for Play Mode") != std::string::npos,
				"An armed recording should explain that it is waiting for Play Mode.");
			context.Expect(!profiler.BeginSample("Editor Preview", ProfilerModuleId::CpuEngine),
				"Armed recording must keep editor-preview profiler markers disabled.");
			context.Expect(!profiler.RecordTaskStarted("Editor Preview Task"),
				"Armed recording must keep editor-preview task instrumentation disabled.");
			profiler.EndFrame();
			context.Expect(profiler.GetFramesSnapshot().empty(),
				"No frame should be captured before Play Mode starts.");

			profiler.BeginFrame(true);
			const bool playSample = profiler.BeginSample("Play Frame", ProfilerModuleId::CpuEngine);
			context.Expect(playSample, "Profiler markers should activate when a Play Mode frame begins.");
			if (playSample) profiler.EndSample();
			profiler.EndFrame();
			context.ExpectEqual(1U, static_cast<unsigned int>(profiler.GetFramesSnapshot().size()),
				"Exactly one frame should be captured during the Play Mode interval.");

			profiler.BeginFrame(false);
			context.ExpectEqual(1U, static_cast<unsigned int>(profiler.GetFramesSnapshot().size()),
				"Leaving Play Mode should keep the recording armed without appending editor frames.");
			context.Expect(profiler.GetState() == ProfilerSessionState::Recording,
				"Leaving Play Mode should not finish the armed recording session.");
			profiler.Clear();
		});

		registry.Add("Engine.Profiler.DetailedCaptureRetainsScopesOnEveryFrame", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();
			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Every-frame detailed scopes";
			ProfilerCaptureSettings settings;
			settings.detailLevel = ProfilerCaptureDetailLevel::Detailed;
			settings.maxFrames = 16U;
			profiler.StartRecording(metadata, settings);

			for (std::uint32_t frameIndex = 0U; frameIndex < 8U; ++frameIndex)
			{
				profiler.BeginFrame(true);
				const bool sampleActive =
					profiler.BeginSample("Every Frame Scope", ProfilerModuleId::CpuEngine);
				context.Expect(sampleActive,
					"Detailed capture should arm the bounded scope buffer on every frame.");
				if (sampleActive)
				{
					profiler.EndSample();
				}
				profiler.EndFrame();
			}

			const std::vector<ProfilerFrameRecord> frames = profiler.GetFramesSnapshot();
			context.ExpectEqual(std::size_t(8U), frames.size(),
				"Every detailed frame should retain its lightweight timing record and scope evidence.");
			context.Expect(std::all_of(frames.begin(), frames.end(), [](const ProfilerFrameRecord& frame) {
				return HasSampleNamed(frame, "Every Frame Scope")
					&& std::any_of(frame.timingStreams.begin(), frame.timingStreams.end(), [](const ProfilerTimingStreamSample& timing) {
						return timing.id == "engine_cpu" && timing.status == ProfilerTimingStreamStatus::Valid;
					});
			}), "Detailed capture should publish both timing and scope data for every frame.");
			profiler.Clear();
		});

		registry.Add("Engine.Profiler.ThreadOwnedBuffersMergeWorkerScopesAndTaskCounters", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();
			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Thread-owned profiler buffers";
			ProfilerCaptureSettings settings;
			settings.detailLevel = ProfilerCaptureDetailLevel::Detailed;
			settings.maxFrames = 4U;
			profiler.StartRecording(metadata, settings);
			profiler.BeginFrame(true);

			constexpr std::uint32_t workerCount = 4U;
			constexpr std::uint32_t tasksPerWorker = 128U;
			std::vector<std::thread> workers;
			workers.reserve(workerCount);
			for (std::uint32_t workerIndex = 0U; workerIndex < workerCount; ++workerIndex)
			{
				workers.emplace_back([&profiler, workerIndex]() {
					const std::string taskLabel = "Profiler Worker " + std::to_string(workerIndex);
					for (std::uint32_t taskIndex = 0U; taskIndex < tasksPerWorker; ++taskIndex)
					{
						profiler.RecordTaskQueued();
						const bool taskSampleActive = profiler.RecordTaskStarted(taskLabel.c_str());
						profiler.RecordTaskFinished(taskSampleActive);
					}
				});
			}
			for (std::thread& worker : workers)
			{
				worker.join();
			}
			profiler.EndFrame();

			const std::vector<ProfilerFrameRecord> frames = profiler.GetFramesSnapshot();
			context.ExpectEqual(std::size_t(1U), frames.size(),
				"The protected frame boundary should merge one immutable worker-scope frame.");
			if (!frames.empty())
			{
				const ProfilerFrameRecord& frame = frames.front();
				const std::size_t expectedTaskCount =
					static_cast<std::size_t>(workerCount * tasksPerWorker);
				const std::size_t retainedTaskCount = static_cast<std::size_t>(std::count_if(
					frame.samples.begin(),
					frame.samples.end(),
					[](const ProfilerSample& sample) {
						return sample.moduleId == ProfilerModuleId::TaskExecution
							&& sample.name == "ThreadPool Task";
					}));
				context.ExpectEqual(expectedTaskCount, retainedTaskCount,
					"Each worker should publish into its own bounded buffer without losing task scopes.");

				std::vector<std::uint64_t> sampleIds;
				sampleIds.reserve(frame.samples.size());
				for (const ProfilerSample& sample : frame.samples)
				{
					sampleIds.push_back(sample.sampleId);
				}
				std::sort(sampleIds.begin(), sampleIds.end());
				context.Expect(std::adjacent_find(sampleIds.begin(), sampleIds.end()) == sampleIds.end(),
					"Merged worker samples must retain globally unique identifiers.");

				const ProfilerModuleSummary* taskModule =
					FindModule(frame.moduleSummaries, ProfilerModuleId::TaskExecution);
				const ProfilerMetric* queuedMetric =
					taskModule != nullptr ? FindMetric(*taskModule, "Queued tasks") : nullptr;
				const ProfilerMetric* startedMetric =
					taskModule != nullptr ? FindMetric(*taskModule, "Started tasks") : nullptr;
				const ProfilerMetric* finishedMetric =
					taskModule != nullptr ? FindMetric(*taskModule, "Finished tasks") : nullptr;
				context.Expect(queuedMetric != nullptr && queuedMetric->value == static_cast<double>(expectedTaskCount),
					"Per-thread queued-task counters should merge exactly once at EndFrame.");
				context.Expect(startedMetric != nullptr && startedMetric->value == static_cast<double>(expectedTaskCount),
					"Per-thread started-task counters should merge exactly once at EndFrame.");
				context.Expect(finishedMetric != nullptr && finishedMetric->value == static_cast<double>(expectedTaskCount),
					"Per-thread finished-task counters should merge exactly once at EndFrame.");

				const ProfilerModuleSummary* cpuModule =
					FindModule(frame.moduleSummaries, ProfilerModuleId::CpuEngine);
				const ProfilerMetric* droppedMetric =
					cpuModule != nullptr ? FindMetric(*cpuModule, "Profiler dropped scopes") : nullptr;
				const ProfilerMetric* lateMetric =
					cpuModule != nullptr ? FindMetric(*cpuModule, "Profiler late scopes") : nullptr;
				context.Expect(droppedMetric != nullptr && droppedMetric->value == 0.0,
					"A bounded worker burst below capacity should not drop scopes.");
				context.Expect(lateMetric != nullptr && lateMetric->value == 0.0,
					"Joined worker scopes should publish before the protected merge boundary.");
			}
			profiler.Clear();
		});

		registry.Add("Engine.Profiler.TransientThreadBuffersAreReclaimedBetweenFrames", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();
			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Transient profiler workers";
			ProfilerCaptureSettings settings;
			settings.detailLevel = ProfilerCaptureDetailLevel::Detailed;
			settings.maxFrames = 4U;
			profiler.StartRecording(metadata, settings);

			constexpr std::size_t workersPerFrame = 80u;
			// Four frames prove the reusable protected-boundary scratch releases
			// transient worker references after every merge, not only once.
			for (std::size_t frameIndex = 0u; frameIndex < 4u; ++frameIndex)
			{
				profiler.BeginFrame(true);
				std::vector<std::thread> workers;
				workers.reserve(workersPerFrame);
				for (std::size_t workerIndex = 0u; workerIndex < workersPerFrame; ++workerIndex)
				{
					workers.emplace_back([&profiler]() {
						const bool active =
							profiler.BeginSample("Transient Worker", ProfilerModuleId::CpuEngine);
						if (active)
						{
							profiler.EndSample();
						}
					});
				}
				for (std::thread& worker : workers)
				{
					worker.join();
				}
				profiler.EndFrame();
			}

			const std::vector<ProfilerFrameRecord> frames = profiler.GetFramesSnapshot();
			context.ExpectEqual(std::size_t(4u), frames.size(),
				"Both transient-worker frames should be committed.");
			for (const ProfilerFrameRecord& frame : frames)
			{
				const std::size_t retained = static_cast<std::size_t>(std::count_if(
					frame.samples.begin(),
					frame.samples.end(),
					[](const ProfilerSample& sample) {
						return sample.name == "Transient Worker";
					}));
				context.ExpectEqual(workersPerFrame, retained,
					"Drained exited-thread buffers should release registry slots before the next frame.");
				const ProfilerModuleSummary* cpuModule =
					FindModule(frame.moduleSummaries, ProfilerModuleId::CpuEngine);
				const ProfilerMetric* droppedMetric =
					cpuModule != nullptr ? FindMetric(*cpuModule, "Profiler dropped scopes") : nullptr;
				context.Expect(droppedMetric != nullptr && droppedMetric->value == 0.0,
					"Reclaimed transient slots should prevent later frames from rejecting worker registration.");
			}
			profiler.Clear();
		});

		registry.Add("Engine.Profiler.BoundedThreadBufferReportsOverflow", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();
			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Bounded profiler overflow";
			ProfilerCaptureSettings settings;
			settings.detailLevel = ProfilerCaptureDetailLevel::Detailed;
			settings.maxFrames = 2U;
			profiler.StartRecording(metadata, settings);
			profiler.BeginFrame(true);
			for (std::uint32_t sampleIndex = 0U; sampleIndex < 1200U; ++sampleIndex)
			{
				const bool active = profiler.BeginSample("Bounded Scope", ProfilerModuleId::CpuEngine);
				if (active)
				{
					profiler.EndSample();
				}
			}
			profiler.EndFrame();

			const std::vector<ProfilerFrameRecord> frames = profiler.GetFramesSnapshot();
			context.ExpectEqual(std::size_t(1U), frames.size(),
				"Overflow should preserve the frame instead of rejecting the capture.");
			if (!frames.empty())
			{
				const ProfilerModuleSummary* cpuModule =
					FindModule(frames.front().moduleSummaries, ProfilerModuleId::CpuEngine);
				const ProfilerMetric* droppedMetric =
					cpuModule != nullptr ? FindMetric(*cpuModule, "Profiler dropped scopes") : nullptr;
				context.Expect(droppedMetric != nullptr && droppedMetric->value > 0.0,
					"A full thread-owned ring should expose an explicit dropped-scope count.");
				context.Expect(frames.front().samples.size() < 1200U,
					"The fixed ring must remain bounded when a producer exceeds its per-frame capacity.");
			}
			profiler.Clear();
		});

		registry.Add("Engine.Profiler.OverflowNeverRetainsOrphanedChildScopes", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();
			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Profiler hierarchy overflow";
			ProfilerCaptureSettings settings;
			settings.detailLevel = ProfilerCaptureDetailLevel::Detailed;
			settings.maxFrames = 2U;
			profiler.StartRecording(metadata, settings);
			profiler.BeginFrame(true);
			const bool parentActive =
				profiler.BeginSample("Overflow Parent", ProfilerModuleId::CpuEngine);
			for (std::uint32_t childIndex = 0U; childIndex < 1200U; ++childIndex)
			{
				const bool childActive =
					profiler.BeginSample("Overflow Child", ProfilerModuleId::CpuEngine);
				if (childActive)
				{
					profiler.EndSample();
				}
			}
			if (parentActive)
			{
				profiler.EndSample();
			}
			profiler.EndFrame();

			const std::vector<ProfilerFrameRecord> frames = profiler.GetFramesSnapshot();
			context.ExpectEqual(std::size_t(1U), frames.size(),
				"Hierarchy overflow should preserve its containing frame.");
			if (!frames.empty())
			{
				std::vector<std::uint64_t> retainedIds;
				retainedIds.reserve(frames.front().samples.size());
				for (const ProfilerSample& sample : frames.front().samples)
				{
					retainedIds.push_back(sample.sampleId);
				}
				context.Expect(std::all_of(
					frames.front().samples.begin(),
					frames.front().samples.end(),
					[&retainedIds](const ProfilerSample& sample) {
						return sample.parentSampleId == 0u
							|| std::find(
								retainedIds.begin(),
								retainedIds.end(),
								sample.parentSampleId) != retainedIds.end();
					}),
					"Every retained child scope must resolve to a retained parent after ring overflow.");
				context.Expect(HasSampleNamed(frames.front(), "Overflow Parent"),
					"The ring should reserve the final slot needed to publish an accepted parent scope.");
			}
			profiler.Clear();
		});

		registry.Add("Engine.Profiler.NewSessionDiscardsPublishedOldSessionData", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();
			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Profiler session generation";
			ProfilerCaptureSettings settings;
			settings.detailLevel = ProfilerCaptureDetailLevel::Detailed;
			settings.maxFrames = 2U;

			profiler.StartRecording(metadata, settings);
			profiler.BeginFrame(true);
			profiler.RecordTaskQueued();
			const bool staleSampleActive =
				profiler.BeginSample("Old Session Scope", ProfilerModuleId::CpuEngine);
			if (staleSampleActive)
			{
				profiler.EndSample();
			}

			// Replace the active session before EndFrame. Already-published ring
			// entries and compact counters must not leak into the new document.
			profiler.Clear();
			profiler.StartRecording(metadata, settings);
			profiler.BeginFrame(true);
			profiler.EndFrame();

			const std::vector<ProfilerFrameRecord> frames = profiler.GetFramesSnapshot();
			context.ExpectEqual(std::size_t(1U), frames.size(),
				"The replacement session should publish only its own first frame.");
			if (!frames.empty())
			{
				context.Expect(!HasSampleNamed(frames.front(), "Old Session Scope"),
					"A completed old-session scope must be discarded during session replacement.");
				const ProfilerModuleSummary* taskModule =
					FindModule(frames.front().moduleSummaries, ProfilerModuleId::TaskExecution);
				const ProfilerMetric* queuedMetric =
					taskModule != nullptr ? FindMetric(*taskModule, "Queued tasks") : nullptr;
				context.Expect(queuedMetric != nullptr && queuedMetric->value == 0.0,
					"Old-session task counters must not appear in the replacement frame.");
			}
			profiler.Clear();
		});

		registry.Add("Engine.Profiler.PauseFinalizesAnExplicitPartialFrame", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();
			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Profiler pause boundary";
			ProfilerCaptureSettings settings;
			settings.detailLevel = ProfilerCaptureDetailLevel::Detailed;
			settings.maxFrames = 4U;
			profiler.StartRecording(metadata, settings);
			profiler.BeginFrame(true);
			const bool active =
				profiler.BeginSample("Before Pause", ProfilerModuleId::CpuEngine);
			if (active)
			{
				profiler.EndSample();
			}
			profiler.PauseRecording();

			const std::vector<ProfilerFrameRecord> pausedFrames = profiler.GetFramesSnapshot();
			context.ExpectEqual(std::size_t(1U), pausedFrames.size(),
				"Pausing during a frame should publish its elapsed evidence immediately.");
			if (!pausedFrames.empty())
			{
				context.Expect(!pausedFrames.front().complete,
					"A frame interrupted by Pause should be labelled partial.");
				context.Expect(pausedFrames.front().durationMs >= 0.0,
					"The partial frame should receive a measured non-negative duration.");
				context.Expect(HasSampleNamed(pausedFrames.front(), "Before Pause"),
					"Scopes completed before Pause should remain inspectable.");
			}

			profiler.ResumeRecording();
			profiler.BeginFrame(true);
			const bool resumedActive =
				profiler.BeginSample("After Resume", ProfilerModuleId::CpuEngine);
			if (resumedActive)
			{
				profiler.EndSample();
			}
			profiler.EndFrame();
			const std::vector<ProfilerFrameRecord> resumedFrames = profiler.GetFramesSnapshot();
			context.ExpectEqual(std::size_t(2U), resumedFrames.size(),
				"Resume should begin capturing again at the next declared frame boundary.");
			context.Expect(HasSampleNamed(resumedFrames.back(), "After Resume"),
				"The first complete frame after Resume should retain detailed scopes.");
			profiler.Clear();
		});

		registry.Add("Engine.Profiler.IdleInstrumentationSkipsSamplesAndTaskCounters", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();

			context.Expect(!profiler.BeginSample("Idle", ProfilerModuleId::CpuEngine), "Idle profiler scopes should report that no sample was opened.");
			const bool taskSampleActive = profiler.RecordTaskStarted("IdleTask");
			context.Expect(!taskSampleActive, "Idle task instrumentation should avoid opening profiler samples.");
			profiler.RecordTaskQueued();
			profiler.RecordTaskFinished(taskSampleActive);
			context.Expect(profiler.GetFramesSnapshot().empty(), "Idle instrumentation should not create profiler frames.");
			context.Expect(profiler.GetFrameListSnapshot().empty(), "Idle instrumentation should leave the lightweight frame list empty.");
		});

	}
}
