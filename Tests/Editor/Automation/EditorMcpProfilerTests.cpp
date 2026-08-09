#include "../TestHarness.h"

#include "EditorAutomationService.h"
#include "Editor/TerraEngine-Editor/Profiling/EditorProfilerService.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace TerraEngine::EditorTests
{
	using namespace TerraEngine::Editor::Automation;

	namespace
	{
		std::optional<AutomationCapabilitySnapshot> FindProfilerCapability(
			const EditorAutomationService& service,
			std::string_view name)
		{
			return service.GetRegistry().Find(name, 1, service.BuildAutomationStateSnapshot());
		}

		bool SchemaHasProperty(const nlohmann::json& schema, std::string_view property)
		{
			return schema.contains("properties")
				&& schema["properties"].is_object()
				&& schema["properties"].contains(std::string(property));
		}
	}

	void RegisterEditorMcpProfilerTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.Profiler.ServiceBuildsBoundedCaptureSummary", [](TestContext& context) {
			TerraEngine::Profiling::ProfilerCaptureDocument document;
			document.frames = {
				{.frameIndex = 1u, .durationMs = 10.0, .complete = true},
				{.frameIndex = 2u, .durationMs = 30.0, .complete = false}
			};
			const EditorProfilerCaptureSummary summary = EditorProfilerService::SummarizeDocument("profiler-test", document);
			context.ExpectEqual(std::uint64_t(2), summary.frameCount, "The owning service should summarize every captured frame.");
			context.ExpectEqual(std::uint64_t(1), summary.completeFrameCount, "Complete and incomplete frames should remain distinguishable.");
			context.ExpectEqual(std::uint64_t(1), summary.incompleteFrameCount, "Incomplete capture evidence should not be silently discarded.");
			context.ExpectEqual(20.0, summary.averageFrameMs, "The summary should publish a bounded average instead of raw samples.");
			context.ExpectEqual(10.0, summary.minimumFrameMs, "The summary should retain the minimum frame duration.");
			context.ExpectEqual(30.0, summary.maximumFrameMs, "The summary should retain the maximum frame duration.");
			context.ExpectEqual(10.0, summary.frameMedianMs, "Frame percentiles should include complete frames only.");
			context.ExpectEqual(10.0, summary.frameP95Ms, "An incomplete tail must not inflate the complete-frame p95.");
			context.ExpectEqual(10.0, summary.frameP99Ms, "An incomplete tail must not inflate the complete-frame p99.");
			context.ExpectEqual(10.0, summary.frameWorstMs, "The bounded performance maximum should exclude incomplete frames.");
		});

		registry.Add("Editor.MCP.Profiler.WorkerSummaryUsesCaptureLocalDeltas", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			auto available = [](std::uint64_t nanoseconds) {
				return TelemetryDuration{TelemetryAvailability::Available, nanoseconds, {}};
			};
			auto worker = [&](std::uint64_t queueDepth, std::uint64_t dispatches, std::uint64_t usefulItems, std::uint64_t run, std::uint64_t wait, std::uint64_t idle) {
				return WorkerTelemetryRecord{7u, queueDepth, dispatches, usefulItems, available(run), available(wait), available(idle)};
			};

			ProfilerCaptureDocument document;
			ProfilerFrameRecord first{.frameIndex = 1u, .durationMs = 1.0, .complete = true};
			first.workerTelemetry.push_back(worker(1u, 100u, 1'000u, 10'000u, 2'000u, 5'000u));
			ProfilerFrameRecord second{.frameIndex = 2u, .durationMs = 1.0, .complete = true};
			second.workerTelemetry.push_back(worker(4u, 103u, 1'030u, 10'300u, 2'100u, 5'400u));
			ProfilerFrameRecord third{.frameIndex = 3u, .durationMs = 1.0, .complete = true};
			third.workerTelemetry.push_back(worker(2u, 108u, 1'080u, 10'800u, 2'250u, 6'000u));
			document.frames = {first, second, third};

			const EditorProfilerCaptureSummary summary = EditorProfilerService::SummarizeDocument("worker-deltas", document);
			context.ExpectEqual(std::size_t(1u), summary.workerHistories.size(), "One captured worker should produce one bounded worker summary.");
			if (!summary.workerHistories.empty())
			{
				const EditorProfilerWorkerSummary& result = summary.workerHistories.front();
				context.ExpectEqual(std::uint64_t(8u), result.totalDispatchCount, "Capture totals must subtract the pre-capture cumulative dispatch baseline.");
				context.ExpectEqual(std::uint64_t(80u), result.totalUsefulWorkItems, "Useful work totals must contain only work observed during the capture.");
				context.ExpectEqual(std::uint64_t(4u), result.maximumQueueDepth, "Queue depth remains an instantaneous maximum rather than a delta.");
				context.ExpectEqual(std::uint64_t(2u), result.runDuration.sampleCount, "Three cumulative observations should produce two duration intervals.");
				context.Expect(result.runDuration.maximum > 0.00049 && result.runDuration.maximum < 0.00051,
					"Worker run percentiles should use the largest per-interval delta, not the process-lifetime total.");
			}
		});

		registry.Add("Editor.MCP.Profiler.DiscoveryCoversCaptureLifecycle", [](TestContext& context) {
			EditorAutomationService service;
			const std::array<std::string_view, 5> operations{
				"profiler.capture.start",
				"profiler.capture.stop",
				"profiler.capture.list",
				"profiler.capture.load",
				"profiler.capture.summary"
			};

			for (const std::string_view operation : operations)
			{
				const auto capability = FindProfilerCapability(service, operation);
				context.Expect(capability.has_value(), std::string(operation) + " should be registered by the profiler provider.");
				if (capability.has_value())
				{
					context.ExpectEqual(std::string("profiler"), capability->descriptor.category, "Profiler tools should share a stable category.");
					context.Expect(capability->descriptor.availability.requiresProject, "Profiler captures should be scoped to the active project.");
				}
			}
		});

		registry.Add("Editor.MCP.Profiler.VisibilityReportsWindowCpuTiming", [](TestContext& context) {
			EditorAutomationService service;
			const auto capability = FindProfilerCapability(service, "profiler.window.visibility.set");
			context.Expect(capability.has_value(), "Profiler visibility control should remain discoverable for installed idle-overhead measurements.");
			if (capability.has_value())
			{
				context.Expect(SchemaHasProperty(capability->descriptor.resultSchema, "editorFrameTimeMs"),
					"Visibility control should retain the host-frame delta used by the existing A/B gate.");
				context.Expect(SchemaHasProperty(capability->descriptor.resultSchema, "profilerWindowDrawMs")
					&& SchemaHasProperty(capability->descriptor.resultSchema, "profilerWindowDrawValid"),
					"Visibility control should report ProfilerWindow CPU timing separately from frame-level presentation cost.");
				context.Expect(SchemaHasProperty(capability->descriptor.resultSchema, "imguiRenderMs")
					&& SchemaHasProperty(capability->descriptor.resultSchema, "presentUiMs"),
					"Visibility control should identify draw-data construction separately from the Vulkan UI presentation boundary.");
			}
		});

		registry.Add("Editor.MCP.Profiler.StartStopReturnDurableJobs", [](TestContext& context) {
			EditorAutomationService service;
			for (const std::string_view operation : {"profiler.capture.start", "profiler.capture.stop"})
			{
				const auto capability = FindProfilerCapability(service, operation);
				context.Expect(capability.has_value(), std::string(operation) + " should be discoverable.");
				if (capability.has_value())
				{
					context.Expect(capability->descriptor.permission == AutomationPermission::ProjectWrite, "Profiler recording transitions should require project-write permission.");
					context.Expect(capability->descriptor.execution.protectedCommit, "Profiler recording transitions should cross a protected editor boundary.");
					context.Expect(SchemaHasProperty(capability->descriptor.resultSchema, "jobId"), "Profiler transitions should return a durable job identifier.");
					context.Expect(SchemaHasProperty(capability->descriptor.resultSchema, "state"), "Profiler jobs should return their initial state.");
				}
			}
		});

		registry.Add("Editor.MCP.Profiler.ArtifactsUseIdentifiersNotArbitraryPaths", [](TestContext& context) {
			EditorAutomationService service;
			const auto load = FindProfilerCapability(service, "profiler.capture.load");
			const auto summary = FindProfilerCapability(service, "profiler.capture.summary");
			context.Expect(load.has_value() && summary.has_value(), "Profiler artifact read operations should be registered.");
			if (load.has_value())
			{
				context.Expect(SchemaHasProperty(load->descriptor.inputSchema, "artifactId"), "Capture loading should resolve an editor-issued artifact identifier.");
				context.Expect(!SchemaHasProperty(load->descriptor.inputSchema, "path"), "Capture loading must not accept arbitrary filesystem paths.");
				context.Expect(SchemaHasProperty(load->descriptor.resultSchema, "jobId"), "Asynchronous capture loading should return a durable job identifier.");
			}
			if (summary.has_value())
			{
				context.Expect(SchemaHasProperty(summary->descriptor.inputSchema, "artifactId"), "Capture summaries should identify a retained artifact.");
				context.Expect(SchemaHasProperty(summary->descriptor.resultSchema, "summary"), "Capture summaries should return structured profiler data.");
			}
		});

		registry.Add("Editor.MCP.Profiler.V2OptionsAndSummariesRemainAdditive", [](TestContext& context) {
			EditorAutomationService service;
			const auto start = FindProfilerCapability(service, "profiler.capture.start");
			const auto list = FindProfilerCapability(service, "profiler.capture.list");
			const auto summaryCapability = FindProfilerCapability(service, "profiler.capture.summary");
			context.Expect(start.has_value() && list.has_value() && summaryCapability.has_value(), "Profiler v2 operations should preserve the legacy capture lifecycle tools.");
			if (start.has_value())
			{
				context.Expect(SchemaHasProperty(start->descriptor.inputSchema, "captureLevel"), "Capture start should accept an optional detail level.");
				context.Expect(SchemaHasProperty(start->descriptor.inputSchema, "payloadVersion"), "Capture start should declare its emitted capture version.");
				context.Expect(SchemaHasProperty(start->descriptor.inputSchema, "maximumChunkFrames"), "Capture start should accept a bounded chunk-frame limit.");
				context.Expect(SchemaHasProperty(start->descriptor.inputSchema, "maxFrames"), "Capture start should expose an optional explicit rolling-history limit.");
				if (SchemaHasProperty(start->descriptor.inputSchema, "maxFrames"))
				{
					context.ExpectEqual(
						0u,
						start->descriptor.inputSchema["properties"]["maxFrames"].value("default", 0u),
						"Profiler discovery should publish unlimited history as the runtime default.");
					context.ExpectEqual(
						0,
						start->descriptor.inputSchema["properties"]["maxFrames"].value("minimum", -1),
						"Profiler discovery should document zero as the unlimited-history sentinel.");
				}
				context.Expect(SchemaHasProperty(start->descriptor.resultSchema, "bounds"), "Capture start should return effective bounds without changing durable job fields.");
				context.Expect(SchemaHasProperty(start->descriptor.resultSchema, "jobId"), "V2 capture start must retain the existing durable job identifier.");
			}
			if (list.has_value())
			{
				context.Expect(SchemaHasProperty(list->descriptor.resultSchema, "artifacts"), "Capture listing should remain compatible while exposing artifact metadata additively.");
			}
			if (summaryCapability.has_value())
			{
				context.Expect(SchemaHasProperty(summaryCapability->descriptor.resultSchema, "summary"), "Capture summary should remain a structured read-only result.");
			}
		});

		registry.Add("Editor.MCP.Profiler.TypedSummaryKeepsUnavailableTelemetryDistinct", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			ProfilerCaptureDocument document;
			document.payloadVersion = 2u;
			document.committedFrameCount = 2u;
			document.complete = false;
			document.recoveredIncompleteTail = true;
			ProfilerFrameRecord first;
			first.frameIndex = 1u;
			first.durationMs = 10.0;
			first.performanceStages.push_back({PerformanceStageDomain::Physics, "provider_reads", {TelemetryAvailability::Available, 10'000'000u, {}}, {}, 10u, false});
			first.performanceStages.push_back({PerformanceStageDomain::SceneView, "projection", {TelemetryAvailability::Unavailable, 0u, "Scene View hidden"}, {}, 0u, false});
			first.physicsWorkVolume = PhysicsWorkVolumeRecord{1u, 100u, 80u, 70u, 60u, 50u, 40u, 30u};
			ProfilerFrameRecord second = first;
			second.frameIndex = 2u;
			second.durationMs = 30.0;
			second.performanceStages.front().duration.nanoseconds = 30'000'000u;
			document.frames = {first, second};

			const EditorProfilerCaptureSummary summary = EditorProfilerService::SummarizeDocument("v2-summary", document);
			context.ExpectEqual(std::uint32_t(2u), summary.payloadVersion, "Typed summaries should retain the capture payload version.");
			context.Expect(summary.recoveredIncompleteTail, "Recovery state should be visible instead of silently discarding tail evidence.");
			context.ExpectEqual(std::size_t(2u), summary.stageTimings.size(), "Physics and Scene View typed stages should have separate bounded summary rows.");
			context.Expect(summary.stageTimings.front().available, "Valid physics stage timings should remain available.");
			context.Expect(summary.stageTimings.back().unavailableCount > 0u, "Unavailable Scene View timing should remain unavailable rather than becoming zero milliseconds.");
			context.ExpectEqual(std::uint64_t(30u), summary.workVolumes.publishedBodies, "Work-volume summary should preserve the published-body count.");
		});

		registry.Add("Editor.MCP.Profiler.FramePublicationAggregateKeepsLanesAvailabilityAndConservation", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			auto evidenceFor = [](std::uint64_t availabilityMask,
				const std::array<std::uint64_t, 8U>& timings) {
				ProfilerFramePublicationEvidence evidence;
				evidence.availabilityMask = availabilityMask;
				evidence.disjointTimingNanoseconds = timings;
				return evidence;
			};

			ProfilerFrameRecord complete;
			complete.frameIndex = 1U;
			complete.durationMs = 10.0;
			complete.framePublication = evidenceFor(0x7fU,
				{1'000'000U, 1'000'000U, 1'000'000U, 1'000'000U,
					1'000'000U, 1'000'000U, 0U, 4'000'000U});
			ProfilerFrameRecord missing;
			missing.frameIndex = 2U;
			missing.durationMs = 20.0;
			ProfilerFrameRecord dynamicUnavailable;
			dynamicUnavailable.frameIndex = 3U;
			dynamicUnavailable.durationMs = 8.0;
			dynamicUnavailable.framePublication = evidenceFor(0x7dU,
				{1'000'000U, 999'000U, 1'000'000U, 1'000'000U,
					1'000'000U, 1'000'000U, 0U, 2'000'000U});

			const nlohmann::json aggregate =
				EditorAutomationService::SummarizeFramePublicationEvidence(
					{complete, missing, dynamicUnavailable});
			context.Expect(aggregate.value("available", false),
				"A capture with publication records should report aggregate availability.");
			context.ExpectEqual(std::uint64_t(2U), aggregate.value("evidenceFrameCount", 0U),
				"Only frames that carried optional publication evidence should count as measured.");
			context.ExpectEqual(std::uint64_t(1U), aggregate.value("missingEvidenceFrameCount", 0U),
				"Frames without publication evidence must remain distinguishable from zero-time frames.");
			context.ExpectEqual(std::uint64_t(0x7fU), aggregate.value("availabilityMaskUnion", 0U),
				"The union should retain every lane observed during the capture.");
			context.ExpectEqual(std::uint64_t(0x7dU), aggregate.value("availabilityMaskIntersection", 0U),
				"The intersection should expose a lane that was unavailable in one measured frame.");

			const nlohmann::json& lanes = aggregate["lanes"];
			context.ExpectEqual(std::size_t(7U), lanes.size(),
				"Automation should expose all seven public disjoint lanes, excluding the reserved slot.");
			context.ExpectEqual(std::string("persistent_state"), lanes[0].value("id", std::string{}),
				"The first aggregate lane should retain the persistent-state identity.");
			context.ExpectEqual(std::string("uninstrumented_remainder"), lanes[6].value("id", std::string{}),
				"The final aggregate lane should expose the explicit profiler-owned remainder.");
			context.ExpectEqual(std::uint64_t(1U), lanes[1].value("availableFrameCount", 0U),
				"A masked-out dynamic lane must not be folded into an observed zero duration.");
			context.ExpectEqual(std::uint64_t(2U), lanes[1].value("unavailableFrameCount", 0U),
				"Missing evidence and an unavailable lane should both remain visible to automation.");

			const nlohmann::json& conservation = aggregate["conservation"];
			context.ExpectEqual(std::uint64_t(1U), conservation.value("eligibleFrameCount", 0U),
				"Conservation should evaluate only frames with all seven public lanes available.");
			context.ExpectEqual(std::uint64_t(1U), conservation.value("conservedFrameCount", 0U),
				"The known six phases plus explicit remainder should reconcile to the full frame clock.");
			context.ExpectEqual(std::uint64_t(4'000'000U), conservation.value("totalRemainderNanoseconds", 0U),
				"The aggregate should preserve the uninstrumented remainder instead of hiding it in a total.");
			context.ExpectEqual(std::uint64_t(0U), conservation.value("maximumAbsoluteErrorNanoseconds", 1U),
				"A reconciled frame should have no aggregate conservation error.");
		});

		registry.Add("Editor.MCP.Profiler.FramePublicationAggregateExcludesIncompleteFramesFromConservation", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			ProfilerFramePublicationEvidence evidence;
			evidence.availabilityMask = 0x7fU;
			evidence.disjointTimingNanoseconds = {
				1'000'000U, 1'000'000U, 1'000'000U, 1'000'000U,
				1'000'000U, 1'000'000U, 0U, 4'000'000U};

			ProfilerFrameRecord completed;
			completed.frameIndex = 1U;
			completed.durationMs = 10.0;
			completed.complete = true;
			completed.framePublication = evidence;
			ProfilerFrameRecord interrupted = completed;
			interrupted.frameIndex = 2U;
			interrupted.complete = false;

			const nlohmann::json aggregate =
				EditorAutomationService::SummarizeFramePublicationEvidence({completed, interrupted});
			context.ExpectEqual(std::uint64_t(1U), aggregate.value("completeFrameCount", 0U),
				"The aggregate should report the complete selected frame separately.");
			context.ExpectEqual(std::uint64_t(1U), aggregate.value("incompleteFrameCount", 0U),
				"An interrupted capture tail must remain visible to automation clients.");
			context.ExpectEqual(std::uint64_t(2U), aggregate["lanes"][0].value("availableFrameCount", 0U),
				"Incomplete frames should retain their per-lane evidence instead of being discarded.");

			const nlohmann::json& conservation = aggregate["conservation"];
			context.ExpectEqual(std::uint64_t(1U), conservation.value("eligibleFrameCount", 0U),
				"Only the naturally completed frame should participate in conservation.");
			context.ExpectEqual(std::uint64_t(1U), conservation.value("conservedFrameCount", 0U),
				"The completed frame should still reconcile against the profiler-owned clock.");
			context.ExpectEqual(std::uint64_t(10'000'000U), conservation.value("totalFrameNanoseconds", 0U),
				"Interrupted frames must not inflate conservation frame-time totals.");
		});

		registry.Add("Editor.MCP.Profiler.NoProjectProducesUnavailableState", [](TestContext& context) {
			EditorAutomationService service;
			for (const std::string_view operation : {"profiler.capture.start", "profiler.capture.list"})
			{
				const auto capability = FindProfilerCapability(service, operation);
				context.Expect(capability.has_value(), std::string(operation) + " should remain discoverable without a project.");
				if (capability.has_value())
				{
					context.Expect(!capability->availability.available, "Profiler operations should be unavailable before a project opens.");
					context.Expect(!capability->availability.reason.empty(), "Profiler unavailability should include a machine-readable discovery reason.");
				}
			}
		});
	}
}
