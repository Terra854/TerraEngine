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
				context.Expect(SchemaHasProperty(start->descriptor.inputSchema, "maxFrames"), "Capture start should expose the bounded rolling-history limit instead of silently retaining ten thousand detailed frames.");
				if (SchemaHasProperty(start->descriptor.inputSchema, "maxFrames"))
				{
					context.ExpectEqual(
						TerraEngine::Profiling::kDefaultProfilerRollingFrameLimit,
						start->descriptor.inputSchema["properties"]["maxFrames"].value("default", 0u),
						"Profiler discovery should publish the same rolling-history default used at runtime.");
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
