#include "TestHarness.h"

#include "Profiling/Profiler.h"
#include "Profiling/ProfilerCapture.h"
#include "Renderer/RenderTypes.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
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

		registry.Add("Engine.ProfilerCapture.SaveLoadAndArtifactRoundTripPreservesFrameData", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			TempProfilerWorkspace workspace;
			const std::filesystem::path capturePath = workspace.Path("round-trip.teprof");
			ProfilerCaptureDocument document = MakeCaptureDocument();
			std::string error;

			context.Expect(SaveCaptureDocument(document, capturePath, error), "Saving a profiler capture document should succeed.");

			ProfilerCaptureDocument loaded;
			context.Expect(LoadCaptureDocument(capturePath, loaded, error), "Loading a saved profiler capture document should succeed.");
			context.ExpectEqual(std::string("Profiler Tests"), loaded.metadata.projectName, "Profiler capture metadata should round-trip through JSON.");
			context.Expect(loaded.metadata.settings.captureMode == ProfilerCaptureMode::LoadedReadOnly, "Loaded capture settings should be marked read-only.");
			context.ExpectEqual(1u, static_cast<unsigned int>(loaded.frames.size()), "Profiler capture frames should round-trip through JSON.");
			context.ExpectEqual(3ULL, loaded.frames.front().frameIndex, "Profiler frame indices should round-trip through JSON.");
			context.Expect(HasSampleNamed(loaded.frames.front(), "Frame"), "Profiler samples should round-trip through JSON.");
			context.Expect(HasSampleNamed(loaded.frames.front(), "ThreadPool Task"), "Task profiler samples should round-trip through JSON.");
			context.ExpectEqual(2u, static_cast<unsigned int>(loaded.frames.front().timingStreams.size()), "Profiler timing streams should round-trip through JSON.");

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
			context.ExpectEqual(std::string("Ready (metadata)"), metadataArtifact.status, "Newly saved captures should place metadata before frames so sidebar refresh can stop before sample payloads.");
		});

		registry.Add("Engine.ProfilerCapture.LoadRejectsInvalidFormatAndUnknownModules", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			TempProfilerWorkspace workspace;
			std::string error;
			ProfilerCaptureDocument loaded;
			workspace.WriteTextFile("wrong-format.teprof", R"({
				"format": "SomethingElse",
				"payloadVersion": 1,
				"frames": []
			})");
			context.Expect(!LoadCaptureDocument(workspace.Path("wrong-format.teprof"), loaded, error), "Profiler loading should reject files with the wrong format marker.");
			context.Expect(error.find("not a TerraEngine profiler capture") != std::string::npos, "Wrong-format failures should report a clear diagnostic.");

			workspace.WriteTextFile("unknown-module.teprof", R"({
				"format": "TerraEngineProfilerCapture",
				"payloadVersion": 1,
				"metadata": {
					"settings": {}
				},
				"frames": [{
					"frameIndex": 0,
					"frameStartMs": 0.0,
					"durationMs": 1.0,
					"complete": true,
					"modules": [{
						"moduleId": "future_module",
						"available": true,
						"totalMs": 1.0,
						"metrics": []
					}],
					"samples": []
				}]
			})");
			context.Expect(!LoadCaptureDocument(workspace.Path("unknown-module.teprof"), loaded, error), "Profiler loading should reject unknown module identifiers.");
			context.Expect(error.find("unknown module id") != std::string::npos, "Unknown module failures should report the unsupported module id.");
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

			RenderStatsSnapshot sceneStats;
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

		registry.Add("Engine.Profiler.RecordingAggregatesSamplesAndTaskCounters", [](TestContext& context) {
			using namespace TerraEngine::Profiling;

			Profiler& profiler = Profiler::Instance();
			profiler.Clear();

			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Runtime Profiling";
			ProfilerCaptureSettings settings;
			settings.maxFrames = 4U;

			profiler.StartRecording(metadata, settings);
			profiler.RecordTaskQueued();
			profiler.BeginFrame();
			const bool outerSampleActive = profiler.BeginSample("Outer", ProfilerModuleId::CpuEngine);
			context.Expect(outerSampleActive, "Profiler scopes should report that a sample was opened while recording.");
			const bool taskSampleActive = profiler.RecordTaskStarted("UnitTask");
			context.Expect(taskSampleActive, "Task profiler scopes should report an active sample while recording.");
			profiler.RecordTaskFinished(taskSampleActive);
			profiler.EndSample();
			profiler.EndFrame();

			std::vector<ProfilerFrameRecord> frames = profiler.GetFramesSnapshot();
			context.ExpectEqual(1u, static_cast<unsigned int>(frames.size()), "Ending a recorded frame should publish one frame snapshot.");
			context.Expect(frames.front().complete, "Completed frames should be marked complete.");
			context.Expect(HasSampleNamed(frames.front(), "Outer"), "Explicit profiler samples should be stored in the completed frame.");
			context.Expect(HasSampleNamed(frames.front(), "ThreadPool Task"), "Task profiler samples should be stored in the completed frame.");

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

			ProfilerCaptureDocument finishedDocument = profiler.FinishRecording();
			context.ExpectEqual(1u, static_cast<unsigned int>(finishedDocument.frames.size()), "Finishing after a complete frame should preserve recorded frames.");
			context.Expect(profiler.GetState() == ProfilerSessionState::Finalizing, "Finishing a capture should transition the profiler to finalizing.");
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
