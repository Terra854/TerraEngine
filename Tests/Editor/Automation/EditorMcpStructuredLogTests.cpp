#include "../TestHarness.h"

#include "EditorAutomationLogStore.h"
#include "EditorAutomationService.h"
#include "Scripting/ScriptGlue.h"

#include <array>
#include <chrono>
#include <string>
#include <string_view>

namespace TerraEngine::EditorTests
{
	using namespace TerraEngine::Editor::Automation;

	namespace
	{
		StructuredLogRecord MakeRecord(
			std::string category,
			std::string source,
			std::string message,
			std::string correlationId,
			AutomationLogSeverity severity = AutomationLogSeverity::Info)
		{
			StructuredLogRecord record;
			record.category = std::move(category);
			record.source = std::move(source);
			record.message = std::move(message);
			record.correlationId = std::move(correlationId);
			record.severity = severity;
			return record;
		}
	}

	void RegisterEditorMcpStructuredLogTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.StructuredLogs.ManagedNativeExportReachesEditorSink", [](TestContext& context) {
			EditorAutomationLogStore logs;
			// Invoke the engine DLL export used by C#, rather than logging from this
			// executable: this catches accidentally module-local logger singletons.
			Native_Log(static_cast<int>(LogLevel::Info), "managed cross-module probe", "NavigationProbe.OnStart", 1);
			StructuredLogQuery query;
			query.text = "managed cross-module probe";
			context.ExpectEqual(std::size_t(1), logs.Query(query).records.size(),
				"Managed gameplay logs must reach the editor's subscribed engine sink.");
		});

		registry.Add("Editor.MCP.StructuredLogs.EngineSinkPreservesScopedCorrelation", [](TestContext& context) {
			EditorAutomationLogStore logs;
			{
				// The scoped context models an MCP request or worker job and proves the
				// logger-to-store path carries identities without parsing message text.
				Logger::ScopedContext logContext({"renderer", "VulkanContext", "render-run-9", "capture-request-2", "capture-job-4"});
				Logger::Instance().Log(LogLevel::Warning, "VulkanContext.cpp", 42, "VulkanContext::Present", "Swapchain was recreated");
			}

			StructuredLogQuery query;
			query.category = "renderer";
			query.correlationId = "render-run-9";
			query.requestId = "capture-request-2";
			query.jobId = "capture-job-4";
			const StructuredLogPage page = logs.Query(query);
			context.ExpectEqual(std::size_t(1), page.records.size(), "The engine sink should publish one correlated structured record synchronously.");
			if (!page.records.empty())
			{
				context.ExpectEqual(std::string("VulkanContext"), page.records.front().source, "An explicit structured source should override call-site inference.");
				context.Expect(page.records.front().severity == AutomationLogSeverity::Warning, "Engine severity should retain its structured automation equivalent.");
			}
		});

		registry.Add("Editor.MCP.StructuredLogs.IngestsEveryRequiredSubsystem", [](TestContext& context) {
			EditorAutomationLogStore logs;
			const std::array<std::pair<std::string_view, std::string_view>, 6> sources{
				std::pair{"engine", "Engine"},
				std::pair{"editor", "MainEditorSubsystem"},
				std::pair{"scripting", "ScriptCompilationSubsystem"},
				std::pair{"asset", "AssetDatabase"},
				std::pair{"export", "ExportPipeline"},
				std::pair{"renderer", "VulkanContext"}
			};

			for (const auto& [category, source] : sources)
			{
				(void)logs.Append(MakeRecord(std::string(category), std::string(source), std::string(category) + " event", "run-all"));
			}

			StructuredLogQuery query;
			query.correlationId = "run-all";
			const StructuredLogPage page = logs.Query(query);
			context.ExpectEqual(sources.size(), page.records.size(), "All required subsystem sinks should feed one ordered structured store.");
			for (std::size_t index = 1; index < page.records.size(); ++index)
			{
				context.Expect(page.records[index - 1].sequence < page.records[index].sequence, "Cross-subsystem ingestion must retain stable sequence order.");
			}
		});

		registry.Add("Editor.MCP.StructuredLogs.ComposesSeverityTextTimeAndCorrelationFilters", [](TestContext& context) {
			EditorAutomationLogStore logs;
			const auto now = std::chrono::system_clock::now();
			StructuredLogRecord old = MakeRecord("renderer", "VulkanContext", "device recovered", "other", AutomationLogSeverity::Warning);
			old.timestamp = now - std::chrono::minutes(2);
			(void)logs.Append(std::move(old));

			StructuredLogRecord match = MakeRecord("renderer", "VulkanContext", "Device lost while presenting", "play-42", AutomationLogSeverity::Critical);
			match.requestId = "capture-7";
			match.jobId = "job-9";
			match.timestamp = now;
			(void)logs.Append(std::move(match));

			StructuredLogQuery query;
			query.minimumSeverity = AutomationLogSeverity::Error;
			query.category = "renderer";
			query.source = "VulkanContext";
			query.text = "lost";
			query.correlationId = "play-42";
			query.requestId = "capture-7";
			query.jobId = "job-9";
			query.from = now - std::chrono::seconds(1);
			query.to = now + std::chrono::seconds(1);
			const StructuredLogPage page = logs.Query(query);
			context.ExpectEqual(std::size_t(1), page.records.size(), "All structured query filters should compose rather than overwrite each other.");
			if (!page.records.empty())
			{
				context.ExpectEqual(std::string("play-42"), page.records.front().correlationId, "The matching record should preserve correlation identity.");
			}
		});

		registry.Add("Editor.MCP.StructuredLogs.CursorPagingNeverDuplicatesRecords", [](TestContext& context) {
			EditorAutomationLogStore logs(16, 2, "play-log-session");
			for (int index = 0; index < 5; ++index)
			{
				(void)logs.Append(MakeRecord("scripting", "ScriptRuntime", "frame " + std::to_string(index), "play-cursor"));
			}

			StructuredLogQuery firstQuery;
			firstQuery.correlationId = "play-cursor";
			firstQuery.pageSize = 2;
			const StructuredLogPage first = logs.Query(firstQuery);
			StructuredLogQuery secondQuery = firstQuery;
			secondQuery.cursor = first.nextCursor;
			const StructuredLogPage second = logs.Query(secondQuery);

			context.ExpectEqual(std::size_t(2), first.records.size(), "The first page should respect the requested response bound.");
			context.ExpectEqual(std::size_t(2), second.records.size(), "The next page should continue at the cursor.");
			if (!first.records.empty() && !second.records.empty())
			{
				context.Expect(first.records.back().sequence < second.records.front().sequence, "Cursor paging must neither duplicate nor reorder records.");
			}
		});

		registry.Add("Editor.MCP.StructuredLogs.QueryToolReturnsBoundedCorrelatedPage", [](TestContext& context) {
			EditorAutomationService service;
			StructuredLogRecord record = MakeRecord("asset", "AssetDatabase", "Imported mesh", "asset-run");
			record.requestId = "asset-request";
			(void)service.GetLogStore().Append(std::move(record));

			const nlohmann::json result = service.HandleTool(
				"logs.query",
				{{"category", "asset"}, {"correlationId", "asset-run"}, {"pageSize", 1}},
				WriteMode::ReadOnly,
				"logs-query-1");
			context.Expect(result.value("ok", false), "logs.query should expose the structured store without depending on a visible console window.");
			context.ExpectEqual(std::size_t(1), result["data"]["records"].size(), "The tool should return a bounded matching page.");
			context.Expect(result["data"].contains("nextCursor"), "The tool response should always expose cursor continuation metadata.");
			context.Expect(result["data"].contains("droppedBeforeCursor"), "The tool response should disclose retention gaps.");
		});

		registry.Add("Editor.MCP.StructuredLogs.RequestCompletionIsAlwaysCorrelated", [](TestContext& context) {
			EditorAutomationService service;
			const nlohmann::json result = service.HandleTool(
				"project.templates.list",
				{{"correlationId", "live-authoring-run"}},
				WriteMode::ReadOnly,
				"templates-request");
			context.Expect(result.value("ok", false), "The semantic request should complete before its diagnostics are queried.");

			StructuredLogQuery query;
			query.correlationId = "live-authoring-run";
			query.requestId = "templates-request";
			const StructuredLogPage page = service.GetLogStore().Query(query);
			context.ExpectEqual(std::size_t(1), page.records.size(), "Every completed MCP request should publish one correlated semantic record even without a subsystem log.");
			if (!page.records.empty())
			{
				context.ExpectEqual(std::string("editor.mcp"), page.records.front().source, "Semantic completion records should use a stable MCP source.");
				context.ExpectEqual(std::string("project.templates.list"), page.records.front().details.value("capability", std::string()), "The record should identify the executed capability without parsing message text.");
			}
		});
	}
}
