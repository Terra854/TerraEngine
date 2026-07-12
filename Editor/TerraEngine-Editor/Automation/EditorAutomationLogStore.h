// EditorAutomationLogStore.h - Bounded structured diagnostics with cursors.

#pragma once

#include "EditorAutomationTypes.h"
#include "Engine/Core/Logger/Logger.h"

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace TerraEngine::Editor::Automation
{
	class EditorAutomationLogStore
	{
	public:
		explicit EditorAutomationLogStore(
			std::size_t capacity = 10000,
			std::size_t maximumPageSize = 256,
			std::string sessionId = {});
		~EditorAutomationLogStore();
		EditorAutomationLogStore(const EditorAutomationLogStore&) = delete;
		EditorAutomationLogStore& operator=(const EditorAutomationLogStore&) = delete;

		[[nodiscard]] std::uint64_t Append(StructuredLogRecord record);
		[[nodiscard]] StructuredLogPage Query(const StructuredLogQuery& query) const;
		[[nodiscard]] std::size_t Size() const;
		[[nodiscard]] std::uint64_t OldestRetainedSequence() const;

	private:
		[[nodiscard]] std::string Fingerprint(const StructuredLogQuery& query) const;
		[[nodiscard]] std::string MakeCursor(std::uint64_t sequence, std::string_view fingerprint) const;
		[[nodiscard]] bool ParseCursor(std::string_view cursor, std::uint64_t& sequence, std::string& fingerprint) const;
		[[nodiscard]] static bool Matches(const StructuredLogRecord& record, const StructuredLogQuery& query);
		void IngestEngineLog(const LogEntry& entry);
		[[nodiscard]] static AutomationLogSeverity ConvertSeverity(LogLevel level);
		[[nodiscard]] static std::string InferCategory(const LogEntry& entry);

		mutable std::mutex logMutex;
		std::deque<StructuredLogRecord> records;
		std::size_t maxRecords = 10000;
		std::size_t maxPageSize = 256;
		std::string sessionToken;
		std::uint64_t nextSequence = 1;
		std::unordered_map<std::string, std::deque<std::uint64_t>> correlationIndex;
		Logger::SinkId loggerSinkId = 0u;
	};
}
