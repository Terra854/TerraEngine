#include "../TestHarness.h"

#include "EditorAutomationIdempotencyStore.h"

#include <string>

namespace TerraEngine::EditorTests
{
	using namespace TerraEngine::Editor::Automation;

	namespace
	{
		IdempotencyRequest MakeRequest(std::string key, std::string fingerprint, int acceptedValue)
		{
			return IdempotencyRequest{
				"client-a",
				"object.create",
				std::move(key),
				std::move(fingerprint),
				{{"accepted", acceptedValue}}};
		}
	}

	void RegisterEditorAutomationIdempotencyTests(TestRegistry& registry)
	{
		registry.Add("Editor.Automation.Idempotency.SameFingerprintReplaysRetainedResult", [](TestContext& context) {
			EditorAutomationIdempotencyStore store;
			const IdempotencyRequest request = MakeRequest("create-1", "fingerprint-a", 1);
			context.Expect(store.Begin(request).disposition == IdempotencyDisposition::NewRequest, "The first key use should begin new work.");
			context.Expect(store.PublishResult("client-a", "object.create", "create-1", {{"objectId", "object-7"}}, true), "The completed result should be publishable.");

			const IdempotencyDecision replay = store.Begin(request);
			context.Expect(replay.disposition == IdempotencyDisposition::Replay, "An identical retry must replay instead of repeating work.");
			context.Expect(replay.terminal, "The replay should preserve terminal state.");
			context.ExpectEqual(std::string("object-7"), replay.result.value("objectId", std::string()), "The exact retained result should be returned.");
		});

		registry.Add("Editor.Automation.Idempotency.MismatchedFingerprintIsConflict", [](TestContext& context) {
			EditorAutomationIdempotencyStore store;
			context.Expect(store.Begin(MakeRequest("shared-key", "fingerprint-a", 1)).disposition == IdempotencyDisposition::NewRequest, "The original request should be accepted.");

			const IdempotencyDecision conflict = store.Begin(MakeRequest("shared-key", "fingerprint-b", 2));
			context.Expect(conflict.disposition == IdempotencyDisposition::Conflict, "Reusing a key for different arguments must be rejected.");
			context.ExpectEqual(std::size_t(1), store.Size(), "A conflicting retry must not replace the original record.");
		});

		registry.Add("Editor.Automation.Idempotency.ActiveRecordsCannotBeEvicted", [](TestContext& context) {
			EditorAutomationIdempotencyStore store(2);
			context.Expect(store.Begin(MakeRequest("active-one", "fingerprint-1", 1)).disposition == IdempotencyDisposition::NewRequest, "The first active record should be accepted.");
			context.Expect(store.Begin(MakeRequest("active-two", "fingerprint-2", 2)).disposition == IdempotencyDisposition::NewRequest, "The second active record should be accepted.");

			const IdempotencyDecision pressure = store.Begin(MakeRequest("active-three", "fingerprint-3", 3));
			context.Expect(pressure.disposition == IdempotencyDisposition::CapacityExceeded, "Capacity pressure must reject new work instead of evicting active records.");
			context.Expect(store.Begin(MakeRequest("active-one", "fingerprint-1", 1)).disposition == IdempotencyDisposition::Replay, "The first active record must remain replayable.");
			context.Expect(store.Begin(MakeRequest("active-two", "fingerprint-2", 2)).disposition == IdempotencyDisposition::Replay, "The second active record must remain replayable.");
		});

		registry.Add("Editor.Automation.Idempotency.TerminalRecordsEvictOldestFirst", [](TestContext& context) {
			EditorAutomationIdempotencyStore store(2);
			context.Expect(store.Begin(MakeRequest("terminal-one", "fingerprint-1", 1)).disposition == IdempotencyDisposition::NewRequest, "The first record should begin.");
			context.Expect(store.PublishResult("client-a", "object.create", "terminal-one", {{"done", 1}}, true), "The first record should become terminal.");
			context.Expect(store.Begin(MakeRequest("terminal-two", "fingerprint-2", 2)).disposition == IdempotencyDisposition::NewRequest, "The second record should begin.");
			context.Expect(store.PublishResult("client-a", "object.create", "terminal-two", {{"done", 2}}, true), "The second record should become terminal.");

			context.Expect(store.Begin(MakeRequest("terminal-three", "fingerprint-3", 3)).disposition == IdempotencyDisposition::NewRequest, "A new record should evict the oldest terminal entry.");
			context.ExpectEqual(std::size_t(2), store.Size(), "The idempotency window must stay bounded.");
			context.Expect(store.Begin(MakeRequest("terminal-one", "fingerprint-1", 1)).disposition == IdempotencyDisposition::NewRequest, "An evicted terminal key should be treated as new work.");
		});
	}
}
