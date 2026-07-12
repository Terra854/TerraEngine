// EditorAutomationIdempotencyStore.cpp - Retry replay and conflict handling.

#include "EditorAutomationIdempotencyStore.h"

#include <algorithm>

namespace TerraEngine::Editor::Automation
{
	EditorAutomationIdempotencyStore::EditorAutomationIdempotencyStore(std::size_t capacity)
		: maxRecords(std::max<std::size_t>(capacity, 1))
	{
	}

	IdempotencyDecision EditorAutomationIdempotencyStore::Begin(const IdempotencyRequest& request)
	{
		if (request.key.empty())
			return {IdempotencyDisposition::NewRequest, request.initialResult, false};

		const std::string compositeKey = CompositeKey(request.clientScope, request.capability, request.key);
		std::scoped_lock lock(recordMutex);
		if (auto found = records.find(compositeKey); found != records.end())
		{
			found->second.lastAccessSequence = nextSequence++;
			if (found->second.fingerprint != request.fingerprint)
				return {IdempotencyDisposition::Conflict, {}, found->second.terminal};
			return {IdempotencyDisposition::Replay, found->second.result, found->second.terminal};
		}

		EvictTerminalRecordsLocked();
		if (records.size() >= maxRecords)
			return {IdempotencyDisposition::CapacityExceeded, {}, false};

		const std::uint64_t sequence = nextSequence++;
		records.emplace(compositeKey, Record{request.fingerprint, request.initialResult, false, sequence, sequence});
		return {IdempotencyDisposition::NewRequest, request.initialResult, false};
	}

	bool EditorAutomationIdempotencyStore::PublishResult(
		std::string_view clientScope,
		std::string_view capability,
		std::string_view key,
		nlohmann::json result,
		bool terminal)
	{
		if (key.empty())
			return false;
		std::scoped_lock lock(recordMutex);
		const auto found = records.find(CompositeKey(clientScope, capability, key));
		if (found == records.end() || found->second.terminal)
			return false;
		found->second.result = std::move(result);
		found->second.terminal = terminal;
		found->second.lastAccessSequence = nextSequence++;
		return true;
	}

	bool EditorAutomationIdempotencyStore::Erase(
		std::string_view clientScope,
		std::string_view capability,
		std::string_view key)
	{
		std::scoped_lock lock(recordMutex);
		return records.erase(CompositeKey(clientScope, capability, key)) != 0;
	}

	std::size_t EditorAutomationIdempotencyStore::Size() const
	{
		std::scoped_lock lock(recordMutex);
		return records.size();
	}

	std::string EditorAutomationIdempotencyStore::CompositeKey(
		std::string_view clientScope,
		std::string_view capability,
		std::string_view key)
	{
		// Length prefixes avoid ambiguous concatenations if a client-supplied key
		// itself contains the separator character.
		return std::to_string(clientScope.size()) + ":" + std::string(clientScope)
			+ std::to_string(capability.size()) + ":" + std::string(capability)
			+ std::to_string(key.size()) + ":" + std::string(key);
	}

	void EditorAutomationIdempotencyStore::EvictTerminalRecordsLocked()
	{
		while (records.size() >= maxRecords)
		{
			auto candidate = records.end();
			for (auto it = records.begin(); it != records.end(); ++it)
			{
				if (!it->second.terminal)
					continue;
				if (candidate == records.end() || it->second.lastAccessSequence < candidate->second.lastAccessSequence)
					candidate = it;
			}
			if (candidate == records.end())
				return;
			records.erase(candidate);
		}
	}
}
