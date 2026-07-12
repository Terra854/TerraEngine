// EditorAutomationLogStore.cpp - Bounded structured log paging.

#include "EditorAutomationLogStore.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace
{
	std::string Lowercase(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
		{
			return static_cast<char>(std::tolower(character));
		});
		return value;
	}

	std::string StableHash(std::string_view value)
	{
		// FNV-1a is used as an opaque cursor fingerprint, not as a security
		// primitive. Its stable output lets clients page across calls reliably.
		std::uint64_t hash = 14695981039346656037ull;
		for (const unsigned char character : value)
		{
			hash ^= character;
			hash *= 1099511628211ull;
		}
		std::ostringstream stream;
		stream << std::hex << std::setw(16) << std::setfill('0') << hash;
		return stream.str();
	}
}

namespace TerraEngine::Editor::Automation
{
	EditorAutomationLogStore::EditorAutomationLogStore(
		std::size_t capacity,
		std::size_t maximumPageSize,
		std::string sessionId)
		: maxRecords(std::max<std::size_t>(capacity, 1)),
		maxPageSize(std::max<std::size_t>(maximumPageSize, 1)),
		sessionToken(std::move(sessionId))
	{
		if (sessionToken.empty())
		{
			static std::atomic<std::uint64_t> nextSession{1};
			sessionToken = "automation-log-session-" + std::to_string(nextSession.fetch_add(1, std::memory_order_relaxed));
		}
		// The store subscribes at its ownership boundary so engine/editor logs are
		// available to MCP even when no diagnostics window is open.
		loggerSinkId = Logger::Instance().AddSink([this](const LogEntry& entry)
		{
			IngestEngineLog(entry);
		});
	}

	EditorAutomationLogStore::~EditorAutomationLogStore()
	{
		Logger::Instance().RemoveSink(loggerSinkId);
	}

	void EditorAutomationLogStore::IngestEngineLog(const LogEntry& entry)
	{
		StructuredLogRecord record;
		record.timestamp = entry.timestamp;
		record.severity = ConvertSeverity(entry.level);
		record.category = entry.category.empty() ? InferCategory(entry) : entry.category;
		record.source = entry.source.empty() ? entry.functionSignature : entry.source;
		record.message = entry.message;
		record.correlationId = entry.correlationId;
		record.requestId = entry.requestId;
		record.jobId = entry.jobId;
		(void)Append(std::move(record));
	}

	AutomationLogSeverity EditorAutomationLogStore::ConvertSeverity(LogLevel level)
	{
		switch (level)
		{
		case LogLevel::Debug: return AutomationLogSeverity::Debug;
		case LogLevel::Info: return AutomationLogSeverity::Info;
		case LogLevel::Warning: return AutomationLogSeverity::Warning;
		case LogLevel::Error: return AutomationLogSeverity::Error;
		case LogLevel::Critical: return AutomationLogSeverity::Critical;
		}
		return AutomationLogSeverity::Info;
	}

	std::string EditorAutomationLogStore::InferCategory(const LogEntry& entry)
	{
		const std::string identity = Lowercase(entry.file + " " + entry.functionSignature);
		if (identity.find("vulkan") != std::string::npos || identity.find("render") != std::string::npos)
			return "renderer";
		if (identity.find("script") != std::string::npos || identity.find("dotnet") != std::string::npos || entry.isManaged)
			return "scripting";
		if (identity.find("asset") != std::string::npos || identity.find("resource") != std::string::npos)
			return "asset";
		if (identity.find("export") != std::string::npos)
			return "export";
		if (identity.find("diagnostic") != std::string::npos || identity.find("crash") != std::string::npos)
			return "diagnostics";
		if (identity.find("editor") != std::string::npos)
			return "editor";
		return "engine";
	}

	std::uint64_t EditorAutomationLogStore::Append(StructuredLogRecord record)
	{
		std::scoped_lock lock(logMutex);
		record.sequence = nextSequence++;
		if (record.timestamp.time_since_epoch().count() == 0)
			record.timestamp = std::chrono::system_clock::now();
		const std::uint64_t publishedSequence = record.sequence;
		if (!record.correlationId.empty())
			correlationIndex[record.correlationId].push_back(publishedSequence);
		records.push_back(std::move(record));

		while (records.size() > maxRecords)
		{
			const StructuredLogRecord& evicted = records.front();
			if (!evicted.correlationId.empty())
			{
				auto indexIt = correlationIndex.find(evicted.correlationId);
				if (indexIt != correlationIndex.end())
				{
					while (!indexIt->second.empty() && indexIt->second.front() <= evicted.sequence)
						indexIt->second.pop_front();
					if (indexIt->second.empty())
						correlationIndex.erase(indexIt);
				}
			}
			records.pop_front();
		}
		return publishedSequence;
	}

	StructuredLogPage EditorAutomationLogStore::Query(const StructuredLogQuery& query) const
	{
		std::scoped_lock lock(logMutex);
		StructuredLogPage page;
		page.oldestRetainedSequence = records.empty() ? nextSequence : records.front().sequence;
		const std::string fingerprint = Fingerprint(query);
		std::uint64_t requestedSequence = page.oldestRetainedSequence;
		if (!query.cursor.empty())
		{
			std::string cursorFingerprint;
			if (!ParseCursor(query.cursor, requestedSequence, cursorFingerprint))
			{
				page.valid = false;
				page.error = "The log cursor is malformed or belongs to another editor session.";
				return page;
			}
			if (cursorFingerprint != fingerprint)
			{
				page.valid = false;
				page.error = "The log cursor does not match the current filters.";
				return page;
			}
		}

		if (requestedSequence < page.oldestRetainedSequence)
		{
			page.droppedBeforeCursor = true;
			requestedSequence = page.oldestRetainedSequence;
		}

		const std::size_t requestedPageSize = query.pageSize == 0 ? 1 : query.pageSize;
		const std::size_t pageSize = std::min(requestedPageSize, maxPageSize);
		std::uint64_t resumeSequence = requestedSequence;
		for (const StructuredLogRecord& record : records)
		{
			if (record.sequence < requestedSequence)
				continue;
			resumeSequence = record.sequence + 1;
			if (!Matches(record, query))
				continue;
			if (page.records.size() < pageSize)
				page.records.push_back(record);
			else
			{
				page.hasMore = true;
				// Resume from this unreturned matching record, not after it.
				resumeSequence = record.sequence;
				break;
			}
		}
		if (!page.hasMore)
			resumeSequence = nextSequence;
		page.nextCursor = MakeCursor(resumeSequence, fingerprint);
		return page;
	}

	std::size_t EditorAutomationLogStore::Size() const
	{
		std::scoped_lock lock(logMutex);
		return records.size();
	}

	std::uint64_t EditorAutomationLogStore::OldestRetainedSequence() const
	{
		std::scoped_lock lock(logMutex);
		return records.empty() ? nextSequence : records.front().sequence;
	}

	std::string EditorAutomationLogStore::Fingerprint(const StructuredLogQuery& query) const
	{
		auto ticks = [](const auto& value)
		{
			return value ? std::to_string(value->time_since_epoch().count()) : std::string("-");
		};
		std::ostringstream stream;
		stream << (query.minimumSeverity ? static_cast<int>(*query.minimumSeverity) : -1) << '|'
			<< query.category << '|' << query.source << '|' << Lowercase(query.text) << '|'
			<< query.correlationId << '|' << query.requestId << '|' << query.jobId << '|'
			<< ticks(query.from) << '|' << ticks(query.to);
		return StableHash(stream.str());
	}

	std::string EditorAutomationLogStore::MakeCursor(std::uint64_t sequence, std::string_view fingerprint) const
	{
		return sessionToken + ":" + std::to_string(sequence) + ":" + std::string(fingerprint);
	}

	bool EditorAutomationLogStore::ParseCursor(std::string_view cursor, std::uint64_t& sequence, std::string& fingerprint) const
	{
		const std::string prefix = sessionToken + ":";
		if (!cursor.starts_with(prefix))
			return false;
		const std::size_t separator = cursor.find(':', prefix.size());
		if (separator == std::string_view::npos)
			return false;
		try
		{
			const std::string number(cursor.substr(prefix.size(), separator - prefix.size()));
			std::size_t parsed = 0;
			sequence = std::stoull(number, &parsed);
			if (parsed != number.size())
				return false;
		}
		catch (...)
		{
			return false;
		}
		fingerprint = std::string(cursor.substr(separator + 1));
		return !fingerprint.empty();
	}

	bool EditorAutomationLogStore::Matches(const StructuredLogRecord& record, const StructuredLogQuery& query)
	{
		if (query.minimumSeverity && record.severity < *query.minimumSeverity)
			return false;
		if (!query.category.empty() && record.category != query.category)
			return false;
		if (!query.source.empty() && record.source != query.source)
			return false;
		if (!query.correlationId.empty() && record.correlationId != query.correlationId)
			return false;
		if (!query.requestId.empty() && record.requestId != query.requestId)
			return false;
		if (!query.jobId.empty() && record.jobId != query.jobId)
			return false;
		if (query.from && record.timestamp < *query.from)
			return false;
		if (query.to && record.timestamp > *query.to)
			return false;
		if (!query.text.empty() && Lowercase(record.message).find(Lowercase(query.text)) == std::string::npos)
			return false;
		return true;
	}
}
