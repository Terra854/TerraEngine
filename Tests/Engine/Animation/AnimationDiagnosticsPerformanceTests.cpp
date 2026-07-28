// AnimationDiagnosticsPerformanceTests.cpp - Final-only diagnostics benchmark contracts.
//
// These tests freeze evidence shape and exercise bounded profiler capture. They
// deliberately use synthetic values and never perform wall-clock acceptance;
// measured diagnostics overhead remains owned by final Phase 18 task T283.

#include "TestHarness.h"

#include "Profiling/Profiler.h"
#include "Profiling/ProfilerCapture.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Profiling;

	namespace {
		inline constexpr double kMaximumCollapsedDiagnosticsMedianOverheadPercent = 2.0;
		inline constexpr std::string_view kStressManifestSha256 =
			"c5652867707f46b2db388a3fc27af14c71db6b13a431dc64a52d6584f555a891";

		std::filesystem::path SourcePath(std::string_view relativePath)
		{
			return std::filesystem::path(TERRAENGINE_SOURCE_DIR) / std::string(relativePath);
		}

		nlohmann::json ReadJson(std::string_view relativePath)
		{
			const std::filesystem::path path = SourcePath(relativePath);
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
			{
				throw TestFailure("Could not read diagnostics performance contract: " + path.string());
			}
			return nlohmann::json::parse(stream);
		}

		struct DiagnosticsBenchmarkResultV1
		{
			std::uint32_t schemaVersion = 1U;
			std::uint32_t phase = 18U;
			std::string fixtureSha256 = std::string(kStressManifestSha256);
			bool timedAcceptanceAttempted = false;
			bool referenceIdentityMatched = false;
			std::vector<double> disabledFrameMilliseconds;
			std::vector<double> collapsedFrameMilliseconds;
			std::uint64_t disabledCapturedFrames = 0U;
			std::uint64_t collapsedCapturedFrames = 0U;
			std::uint64_t attributedStageCount = 0U;
			std::uint64_t unavailableRequiredStageCount = 0U;
			std::uint64_t overLimitDetailRecordCount = 0U;
		};

		double Median(std::vector<double> values)
		{
			std::sort(values.begin(), values.end());
			return values[values.size() / 2U];
		}

		// Final evidence is accepted only when every paired sample and required
		// counter exists. The threshold is compiled from the reviewed contract and
		// cannot be supplied or weakened by a candidate result.
		bool ValidateDiagnosticsBenchmarkResult(
			const DiagnosticsBenchmarkResultV1& result,
			bool finalPhaseAcceptanceAllowed)
		{
			if (result.schemaVersion != 1U || result.phase != 18U
				|| result.fixtureSha256 != kStressManifestSha256
				|| !result.timedAcceptanceAttempted || !result.referenceIdentityMatched
				|| !finalPhaseAcceptanceAllowed
				|| result.disabledFrameMilliseconds.size() < 5U
				|| result.disabledFrameMilliseconds.size() != result.collapsedFrameMilliseconds.size()
				|| result.disabledCapturedFrames == 0U || result.collapsedCapturedFrames == 0U
				|| result.attributedStageCount == 0U || result.unavailableRequiredStageCount != 0U
				|| result.overLimitDetailRecordCount != 0U)
			{
				return false;
			}
			for (std::size_t index = 0U; index < result.disabledFrameMilliseconds.size(); ++index)
			{
				if (!std::isfinite(result.disabledFrameMilliseconds[index])
					|| !std::isfinite(result.collapsedFrameMilliseconds[index])
					|| result.disabledFrameMilliseconds[index] <= 0.0
					|| result.collapsedFrameMilliseconds[index] <= 0.0)
				{
					return false;
				}
			}
			const double disabledMedian = Median(result.disabledFrameMilliseconds);
			const double collapsedMedian = Median(result.collapsedFrameMilliseconds);
			const double overheadPercent = ((collapsedMedian - disabledMedian) / disabledMedian) * 100.0;
			return overheadPercent <= kMaximumCollapsedDiagnosticsMedianOverheadPercent;
		}

		ProfilerAnimationFrameTelemetry MakeCompleteTelemetry()
		{
			ProfilerAnimationFrameTelemetry telemetry;
			ProfilerAnimationFrameSummary& summary = telemetry.summary;
			summary.availableEvidenceMask = kProfilerAnimationAllEvidence;
			summary.schedulerMembershipChecks = 1U;
			summary.activeAnimators = 2U;
			summary.evaluatedAnimators = 2U;
			summary.cpuAnimators = 1U;
			summary.gpuVisualAnimators = 1U;
			summary.workerCount = 2U;
			summary.workerJobs = 2U;
			summary.workerUsefulItems = 2U;
			summary.workerBusyNanoseconds = 800U;
			summary.workerCapacityNanoseconds = 1000U;
			summary.sampledJoints = 100U;
			summary.blendedTracks = 4U;
			summary.evaluatedGraphNodes = 12U;
			summary.motionMatchingCandidates = 32U;
			summary.ikChains = 2U;
			summary.ragdollBodies = 3U;
			summary.gpuEvaluatedJoints = 100U;
			summary.gpuBatches = 1U;
			summary.frameArenaBytes = 4096U;
			summary.frameArenaHighWaterBytes = 8192U;
			summary.poseCacheBytes = 16384U;
			summary.poseCacheHighWaterBytes = 32768U;
			summary.gpuResidentBytes = 65536U;
			summary.gpuResidentHighWaterBytes = 131072U;
			summary.gpuTransferBytes = 512U;
			summary.gpuReadbackBytes = 16U;
			summary.boundedMetadataReadbackBytes = 16U;
			summary.poseCacheHits = 10U;
			summary.poseCacheMisses = 1U;
			summary.clipCacheHits = 20U;
			summary.clipCacheMisses = 2U;
			summary.residencyHits = 9U;
			summary.residencyMisses = 1U;
		for (std::size_t index = 0U; index < summary.stageTimings.size(); ++index)
		{
			summary.stageTimings[index] = ProfilerAnimationStageTiming{100U + index, true, false};
		}
		telemetry.details.push_back(ProfilerAnimationDetailRecord{
			101U, 1U, 0U, 100U, 6U, 256U, 16U, 500U, true});
		return telemetry;
	}
	}

	void RegisterAnimationDiagnosticsPerformanceTests(TestRegistry& registry)
	{
		registry.Add("Animation.DiagnosticsPerformance.FrozenDefinitionsRemainFinalOnly", [](TestContext& context) {
			const nlohmann::json performance = ReadJson("Tools/Animation/PerformanceWorkloads.json");
			const nlohmann::json stress = ReadJson("Tests/Fixtures/Animation/stress/workloads.json");
			const auto crowd = std::find_if(stress.at("workloads").begin(), stress.at("workloads").end(), [](const nlohmann::json& workload) {
				return workload.at("id") == "crowd-1000";
			});
			context.Expect(performance.at("sourceControlledThresholds").at("animationAcceptance")
				.at("collapsedDiagnosticsMaximumMedianOverheadPercent") == 2,
				"The source-controlled collapsed-diagnostics threshold must remain exactly two percent.");
			context.Expect(stress.at("performanceAcceptancePhase") == 18
				&& stress.at("performanceExecutionDeferred").get<bool>(),
				"Diagnostics measurements must remain explicitly deferred to final Phase 18.");
			context.Expect(crowd != stress.at("workloads").end()
				&& crowd->at("definition").at("diagnosticsModes") == nlohmann::json::array({"Disabled", "EnabledCollapsed"})
				&& crowd->at("definition").at("pairedMeasuredRuns") == 5,
				"The benchmark must retain paired disabled and enabled-collapsed profiles with five runs.");
		});

		registry.Add("Animation.DiagnosticsPerformance.TimedResultCannotPassBeforeFinalPhase", [](TestContext& context) {
			DiagnosticsBenchmarkResultV1 result;
			result.timedAcceptanceAttempted = true;
			result.referenceIdentityMatched = true;
			result.disabledFrameMilliseconds = {10.0, 10.0, 10.0, 10.0, 10.0};
			result.collapsedFrameMilliseconds = {10.2, 10.2, 10.2, 10.2, 10.2};
			result.disabledCapturedFrames = 3600U;
			result.collapsedCapturedFrames = 3600U;
			result.attributedStageCount = kProfilerAnimationStageCount;
			context.Expect(!ValidateDiagnosticsBenchmarkResult(result, false),
				"Synthetic timing values must not become pre-final acceptance evidence.");
			context.Expect(ValidateDiagnosticsBenchmarkResult(result, true),
				"An exact-boundary final result should satisfy the frozen schema.");
			// Change a majority of the sorted samples so the actual median, rather
			// than only one pre-sort element, crosses the frozen two-percent limit.
			result.collapsedFrameMilliseconds = {10.21, 10.21, 10.21, 10.2, 10.2};
			context.Expect(!ValidateDiagnosticsBenchmarkResult(result, true),
				"A median above two percent must fail without candidate threshold overrides.");
			result.collapsedFrameMilliseconds[2U] = std::numeric_limits<double>::quiet_NaN();
			context.Expect(!ValidateDiagnosticsBenchmarkResult(result, true),
				"A missing or non-finite frame metric must fail closed.");
		});

		registry.Add("Animation.DiagnosticsPerformance.TypedStagesAndDetailsAreBounded", [](TestContext& context) {
			ProfilerAnimationFrameTelemetry telemetry = MakeCompleteTelemetry();
			context.Expect(ValidateProfilerAnimationFrameTelemetry(telemetry),
				"A complete active aggregate with one copied detail should validate.");
			context.Expect(std::string(ToString(ProfilerAnimationStage::CpuGraphEvaluation)) == "Animation.CpuGraphEvaluation"
				&& std::string(ToString(ProfilerAnimationStage::GpuEvaluation)) == "Animation.GpuEvaluation"
				&& std::string(ToString(ProfilerAnimationStage::ProtectedPublication)) == "Animation.ProtectedPublication",
				"CPU, GPU, and protected stages need stable distinct profiler names.");
			telemetry.details.resize(kMaximumProfilerAnimationDetailRecordsPerFrame + 1U);
			context.Expect(!ValidateProfilerAnimationFrameTelemetry(telemetry),
				"Per-instance capture must reject rather than retain an over-limit detail vector.");
		});

		registry.Add("Animation.DiagnosticsPerformance.AggregateAndDormantCaptureStayMinimal", [](TestContext& context) {
			Profiler& profiler = Profiler::Instance();
			profiler.Clear();
			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Animation diagnostics bounded capture";
			ProfilerCaptureSettings settings;
			settings.detailLevel = ProfilerCaptureDetailLevel::Aggregate;
			settings.maxFrames = 2U;
			profiler.StartRecording(metadata, settings);
			profiler.BeginFrame();
			profiler.RecordAnimationTelemetry(MakeCompleteTelemetry());
			profiler.EndFrame();
			profiler.BeginFrame();
			profiler.RecordDormantAnimationFacts();
			profiler.EndFrame();
			const std::vector<ProfilerFrameRecord> frames = profiler.GetFramesSnapshot();
			context.Expect(frames.size() == 2U && frames[0U].animationTelemetry
				&& frames[0U].animationTelemetry->details.empty(),
				"Aggregate capture should retain fixed counters but no per-instance rows.");
			context.Expect(frames.size() == 2U && !frames[1U].animationTelemetry
				&& frames[1U].animationDiagnostics.has_value() && frames[1U].animationDiagnostics->dormant,
				"A zero-animator frame must retain only the established three scalar facts.");
			profiler.Clear();
			ProfilerFrameRecord defaultFrame;
			context.Expect(!defaultFrame.animationTelemetry,
				"Disabled profiler frames must not allocate an animation aggregate by default.");
		});

		registry.Add("Animation.DiagnosticsPerformance.V2RoundTripRetainsCriticalAggregate", [](TestContext& context) {
			ProfilerCaptureDocument document;
			document.metadata.projectName = "Animation telemetry round trip";
			document.metadata.settings.detailLevel = ProfilerCaptureDetailLevel::Detailed;
			ProfilerFrameRecord frame;
			frame.frameIndex = 7U;
			frame.moduleSummaries = MakeDefaultModuleSummaries(document.metadata.settings);
			frame.animationDiagnostics = ProfilerAnimationDiagnosticsFacts{1U, false, true};
			frame.animationTelemetry = std::make_shared<const ProfilerAnimationFrameTelemetry>(MakeCompleteTelemetry());
			document.frames.push_back(frame);
			const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
			const std::filesystem::path path = std::filesystem::temp_directory_path()
				/ ("terra-animation-profiler-" + std::to_string(suffix) + ".teprof");
			std::string error;
			const bool saved = SaveCaptureDocument(document, path, error);
			ProfilerCaptureDocument loaded;
			const bool opened = saved && LoadCaptureDocument(path, loaded, error);
			std::error_code removeError;
			std::filesystem::remove(path, removeError);
			context.Expect(opened && loaded.frames.size() == 1U && loaded.frames[0U].animationTelemetry,
				error.empty() ? "V2 must retain the typed animation aggregate." : error);
			if (opened && loaded.frames.size() == 1U && loaded.frames[0U].animationTelemetry)
			{
				const ProfilerAnimationFrameTelemetry& retained = *loaded.frames[0U].animationTelemetry;
				context.Expect(retained.summary.availableEvidenceMask == kProfilerAnimationAllEvidence
					&& retained.summary.blendedTracks == 4U
					&& retained.summary.gpuTransferBytes == 512U
					&& retained.details.size() == 1U
					&& retained.summary.stageTimings[static_cast<std::size_t>(ProfilerAnimationStage::GpuEvaluation)].available,
					"Saved telemetry must retain availability, volumes, transfers, detail, and stage attribution.");
			}
		});
	}
}
