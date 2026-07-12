// EditorAutomationIdempotencyStore.h - Session retry deduplication records.

#pragma once

#include "EditorAutomationTypes.h"

#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace TerraEngine::Editor::Automation
{
	class EditorAutomationIdempotencyStore
	{
	public:
		explicit EditorAutomationIdempotencyStore(std::size_t capacity = 1024);

		[[nodiscard]] IdempotencyDecision Begin(const IdempotencyRequest& request);
		[[nodiscard]] bool PublishResult(
			std::string_view clientScope,
			std::string_view capability,
			std::string_view key,
			nlohmann::json result,
			bool terminal);
		[[nodiscard]] bool Erase(std::string_view clientScope, std::string_view capability, std::string_view key);
		[[nodiscard]] std::size_t Size() const;

	private:
		struct Record
		{
			std::string fingerprint;
			nlohmann::json result;
			bool terminal = false;
			std::uint64_t createdSequence = 0;
			std::uint64_t lastAccessSequence = 0;
		};

		[[nodiscard]] static std::string CompositeKey(
			std::string_view clientScope,
			std::string_view capability,
			std::string_view key);
		void EvictTerminalRecordsLocked();

		mutable std::mutex recordMutex;
		std::unordered_map<std::string, Record> records;
		std::size_t maxRecords = 1024;
		std::uint64_t nextSequence = 1;
	};
}
