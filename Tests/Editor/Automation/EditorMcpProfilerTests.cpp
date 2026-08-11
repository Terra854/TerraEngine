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
					if (operation == "profiler.capture.stop")
					{
						context.Expect(SchemaHasProperty(capability->descriptor.resultSchema, "finishCaptureSubmissionMilliseconds"),
							"Profiler stop should separate protected ticket submission time from asynchronous journal finalization and MCP dispatch latency.");
					}
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

		registry.Add("Editor.MCP.Profiler.V1OptionsAndSummariesRemainAdditive", [](TestContext& context) {
			EditorAutomationService service;
			const auto start = FindProfilerCapability(service, "profiler.capture.start");
			const auto list = FindProfilerCapability(service, "profiler.capture.list");
			const auto summaryCapability = FindProfilerCapability(service, "profiler.capture.summary");
			context.Expect(start.has_value() && list.has_value() && summaryCapability.has_value(), "Profiler V1 operations should expose the capture lifecycle tools.");
			context.Expect(FindProfilerCapability(service, "profiler.capture.health").has_value()
				&& FindProfilerCapability(service, "profiler.capture.index").has_value()
				&& FindProfilerCapability(service, "profiler.capture.detail").has_value()
				&& FindProfilerCapability(service, "profiler.capture.operation").has_value(),
				"Profiler automation must expose bounded health, index, selected-detail, and operation reads.");
			if (const auto health = FindProfilerCapability(service, "profiler.capture.health"); health.has_value())
			{
				context.Expect(SchemaHasProperty(health->descriptor.resultSchema, "sessionArtifactId"),
					"Bounded health must identify the active V1 journal without exposing a filesystem path.");
				context.Expect(health->descriptor.resultSchema["properties"]["formatIdentifier"].value("const", std::string{}) == "TerraEngineProfilerJournalV1"
					&& health->descriptor.resultSchema["properties"]["formatVersion"].value("const", 0u) == 1u
					&& health->descriptor.resultSchema["properties"]["projectEpoch"].value("minimum", 1u) == 0u,
					"Bounded health must retain both V1 identity and its lifecycle epoch.");
			}
			if (start.has_value())
			{
				context.Expect(SchemaHasProperty(start->descriptor.inputSchema, "captureLevel"), "Capture start should accept an optional detail level.");
				const std::string legacyVersionSelector = "payload" "Version";
				context.Expect(!SchemaHasProperty(start->descriptor.inputSchema, legacyVersionSelector), "Capture start must not expose a legacy format selector.");
				const std::string legacyCaptureSelector = "Capture" "Version";
				context.Expect(!SchemaHasProperty(start->descriptor.inputSchema, legacyCaptureSelector), "Capture start must not expose an alternate capture-version selector.");
				context.Expect(SchemaHasProperty(start->descriptor.resultSchema, "formatIdentifier")
					&& SchemaHasProperty(start->descriptor.resultSchema, "formatVersion")
					&& start->descriptor.resultSchema["properties"]["formatIdentifier"].value("const", std::string{}) == "TerraEngineProfilerJournalV1"
					&& start->descriptor.resultSchema["properties"]["formatVersion"].value("const", 0u) == 1u,
					"Capture start results must identify exactly the fixed V1 journal format and version.");
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
				context.Expect(SchemaHasProperty(start->descriptor.resultSchema, "jobId"), "V1 capture start must return its durable job identifier.");
			}
			if (list.has_value())
			{
				context.Expect(SchemaHasProperty(list->descriptor.resultSchema, "artifacts"), "Capture listing should remain compatible while exposing artifact metadata additively.");
			}
			if (summaryCapability.has_value())
			{
				context.Expect(SchemaHasProperty(summaryCapability->descriptor.resultSchema, "summary"), "Capture summary should remain a structured read-only result.");
			}
			const auto index = FindProfilerCapability(service, "profiler.capture.index");
			const auto detail = FindProfilerCapability(service, "profiler.capture.detail");
			const auto operation = FindProfilerCapability(service, "profiler.capture.operation");
			context.Expect(index.has_value() && detail.has_value() && operation.has_value(),
				"Bounded index, detail, and operation contracts should remain discoverable.");
			if (index.has_value())
			{
				const auto& properties = index->descriptor.inputSchema["properties"];
				context.Expect(properties["offset"].value("type", std::string{}) == "integer"
					&& properties["offset"].value("minimum", 1u) == 0u
					&& properties["limit"].value("type", std::string{}) == "integer"
					&& properties["limit"].value("minimum", 0u) == 1u
					&& properties["limit"].value("maximum", 0u) == 512u
					&& index->descriptor.resultSchema["properties"]["formatVersion"].value("const", 0u) == 1u
					&& SchemaHasProperty(index->descriptor.resultSchema, "projectEpoch"),
					"Profiler index requests must advertise bounded non-negative page inputs.");
			}
			if (detail.has_value())
			{
				const auto& properties = detail->descriptor.inputSchema["properties"];
				context.Expect(properties["frameIndex"].value("type", std::string{}) == "integer"
					&& properties["frameIndex"].value("minimum", 1u) == 0u
					&& detail->descriptor.resultSchema["properties"]["formatIdentifier"].value("const", std::string{}) == "TerraEngineProfilerJournalV1"
					&& SchemaHasProperty(detail->descriptor.resultSchema, "projectEpoch"),
					"Profiler detail requests must expose one non-negative journal frame identity.");
			}
			if (operation.has_value())
			{
				const auto& inputProperties = operation->descriptor.inputSchema["properties"];
				const auto& resultProperties = operation->descriptor.resultSchema["properties"];
				context.Expect(inputProperties["operationId"].value("type", std::string{}) == "integer"
					&& inputProperties["operationId"].value("minimum", 0u) == 1u
					&& resultProperties["formatIdentifier"].value("const", std::string{}) == "TerraEngineProfilerJournalV1"
					&& resultProperties["formatVersion"].value("const", 0u) == 1u
					&& SchemaHasProperty(operation->descriptor.resultSchema, "operation")
					&& SchemaHasProperty(operation->descriptor.resultSchema, "finalization"),
					"Profiler operation polling must remain V1-only and expose one bounded terminal/finalization result.");
			}
		});

		registry.Add("Editor.MCP.Profiler.OperationStatesExposeTerminalLifecycle", [](TestContext& context) {
			// These names are part of the bounded operation-polling contract.  A
			// project close may publish Cancelled while a worker completion is stale,
			// so automation must never confuse it with a successful finalization.
			context.Expect(ToString(EditorProfilerOperationState::Queued) == "queued"
				&& ToString(EditorProfilerOperationState::Running) == "running"
				&& ToString(EditorProfilerOperationState::Completed) == "completed"
				&& ToString(EditorProfilerOperationState::Failed) == "failed"
				&& ToString(EditorProfilerOperationState::Cancelled) == "cancelled",
				"Profiler operation polling must preserve all bounded lifecycle states.");
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
