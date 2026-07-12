#include "../TestHarness.h"

#include "EditorAutomationLogStore.h"

#include <string>

namespace TerraEngine::EditorTests
{
	using namespace TerraEngine::Editor::Automation;

	namespace
	{
		StructuredLogRecord MakeLog(
			AutomationLogSeverity severity,
			std::string category,
			std::string message,
			std::string correlation = {})
		{
			StructuredLogRecord record;
			record.severity = severity;
			record.category = std::move(category);
			record.source = "AutomationTest";
			record.message = std::move(message);
			record.correlationId = std::move(correlation);
			return record;
		}
	}

	void RegisterEditorAutomationLogTests(TestRegistry& registry)
	{
		registry.Add("Editor.Automation.Logs.FiltersStructuredRecordsWithStableOrder", [](TestContext& context) {
			EditorAutomationLogStore logs;
			(void)logs.Append(MakeLog(AutomationLogSeverity::Info, "asset", "Import started", "other"));
			(void)logs.Append(MakeLog(AutomationLogSeverity::Error, "scripting", "Compilation failed", "build-7"));
			(void)logs.Append(MakeLog(AutomationLogSeverity::Critical, "renderer", "Device lost", "build-7"));

			StructuredLogQuery query;
			query.minimumSeverity = AutomationLogSeverity::Error;
			query.correlationId = "build-7";
			const StructuredLogPage page = logs.Query(query);
			context.Expect(page.valid, "A valid structured filter should produce a page.");
			context.ExpectEqual(std::size_t(2), page.records.size(), "Severity and correlation filters should both apply.");
			context.Expect(page.records[0].sequence < page.records[1].sequence, "Matching records should retain monotonic sequence order.");
		});

		registry.Add("Editor.Automation.Logs.CursorRejectsDifferentFilterFingerprint", [](TestContext& context) {
			EditorAutomationLogStore logs(16, 1, "cursor-session");
			(void)logs.Append(MakeLog(AutomationLogSeverity::Info, "asset", "one"));
			(void)logs.Append(MakeLog(AutomationLogSeverity::Info, "asset", "two"));
			StructuredLogQuery firstQuery;
			firstQuery.category = "asset";
			firstQuery.pageSize = 1;
			const StructuredLogPage firstPage = logs.Query(firstQuery);

			StructuredLogQuery changedFilter;
			changedFilter.category = "renderer";
			changedFilter.cursor = firstPage.nextCursor;
			const StructuredLogPage rejected = logs.Query(changedFilter);
			context.Expect(!rejected.valid, "A cursor must be bound to the original filter fingerprint.");
			context.Expect(!rejected.error.empty(), "Invalid cursor reuse should return a structured reason.");
		});

		registry.Add("Editor.Automation.Logs.ReportsHistoryDroppedBeforeCursor", [](TestContext& context) {
			EditorAutomationLogStore logs(3, 1, "retention-session");
			(void)logs.Append(MakeLog(AutomationLogSeverity::Info, "test", "one"));
			(void)logs.Append(MakeLog(AutomationLogSeverity::Info, "test", "two"));
			const StructuredLogPage firstPage = logs.Query(StructuredLogQuery{.pageSize = 1});
			(void)logs.Append(MakeLog(AutomationLogSeverity::Info, "test", "three"));
			(void)logs.Append(MakeLog(AutomationLogSeverity::Info, "test", "four"));
			(void)logs.Append(MakeLog(AutomationLogSeverity::Info, "test", "five"));

			StructuredLogQuery resume;
			resume.cursor = firstPage.nextCursor;
			resume.pageSize = 1;
			const StructuredLogPage resumed = logs.Query(resume);
			context.Expect(resumed.valid, "A retained-session cursor should remain parseable after eviction.");
			context.Expect(resumed.droppedBeforeCursor, "The page must explicitly report records dropped before an old cursor.");
			context.ExpectEqual(std::uint64_t(3), resumed.oldestRetainedSequence, "The page should identify the oldest retained record.");
		});

		registry.Add("Editor.Automation.Logs.TenThousandRecordsRemainPaged", [](TestContext& context) {
			EditorAutomationLogStore logs(10000, 128, "scale-session");
			for (int index = 0; index < 10000; ++index)
			{
				(void)logs.Append(MakeLog(AutomationLogSeverity::Info, "scale", "record " + std::to_string(index)));
			}

			StructuredLogQuery query;
			query.pageSize = 10000;
			const StructuredLogPage page = logs.Query(query);
			context.ExpectEqual(std::size_t(10000), logs.Size(), "The performance contract retains ten thousand records.");
			context.ExpectEqual(std::size_t(128), page.records.size(), "A caller cannot exceed the configured response bound.");
			context.Expect(page.hasMore, "A bounded first page should report remaining records.");
			context.Expect(!page.nextCursor.empty(), "A bounded page with more data must provide a cursor.");
		});

		registry.Add("Editor.Automation.Logs.EvictionAndResponseSizesStayBounded", [](TestContext& context) {
			EditorAutomationLogStore logs(4, 2, "bounded-session");
			for (int index = 0; index < 8; ++index)
			{
				(void)logs.Append(MakeLog(AutomationLogSeverity::Debug, "bounded", std::to_string(index)));
			}

			const StructuredLogPage page = logs.Query(StructuredLogQuery{.pageSize = 50});
			context.ExpectEqual(std::size_t(4), logs.Size(), "Ingestion must evict records beyond retention capacity.");
			context.ExpectEqual(std::size_t(2), page.records.size(), "Queries must obey the maximum page size.");
			context.ExpectEqual(std::uint64_t(5), logs.OldestRetainedSequence(), "Eviction should preserve the newest bounded history.");
		});
	}
}
