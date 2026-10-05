// These callbacks fail at the actual production receipt-retention boundary.
#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Scene/EditorTransactionReceiptBatch.h"
#include <stdexcept>

namespace TerraEngine::EditorTests {
	void RegisterEditorTransactionReceiptBatchTests(TestRegistry& registry) {
		registry.Add("Editor.TerrainIndependentCopy.ReceiptsSettleBeforeHistory", [](TestContext& context) {
			TerraEngine::Editor::EditorTransactionReceiptBatch batch;
			std::size_t settled = 0U, recorded = 0U;
			batch.Publish([] { return std::vector<TerraEngine::Scene::EditorTransactionCompletion>(3U); },
				[&](const auto&) { ++settled; }, [&](const auto&) {
					context.Expect(settled == 3U, "Every copy receipt must settle before fallible history handling begins.");
					++recorded;
				});
			context.Expect(recorded == 3U && !batch.HasPending(), "The complete receipt batch must publish once.");
		});
		registry.Add("Editor.TerrainIndependentCopy.ReceiptSettlementFailureRetainsBatch", [](TestContext& context) {
			TerraEngine::Editor::EditorTransactionReceiptBatch batch;
			std::size_t drains = 0U, settled = 0U, recorded = 0U;
			bool fail = true;
			auto drain = [&] { ++drains; return std::vector<TerraEngine::Scene::EditorTransactionCompletion>(3U); };
			auto settle = [&](const auto&) {
				if(settled == 1U && fail) { fail = false; throw std::runtime_error("Injected receipt consumer failure"); }
				++settled;
			};
			try { batch.Publish(drain, settle, [&](const auto&) { ++recorded; }); } catch(const std::runtime_error&) {}
			context.Expect(batch.HasPending() && recorded == 0U, "A failed settlement retains all later receipts and postpones history.");
			batch.Publish(drain, settle, [&](const auto&) { ++recorded; });
			context.Expect(drains == 1U && settled == 3U && recorded == 3U && !batch.HasPending(), "Recovery uses retained receipts without another drain or duplicate settlement.");
		});
		registry.Add("Editor.TerrainIndependentCopy.HistoryFailureCannotLoseCopyReceipts", [](TestContext& context) {
			TerraEngine::Editor::EditorTransactionReceiptBatch batch;
			std::size_t drains = 0U, settled = 0U, recorded = 0U;
			bool fail = true;
			auto drain = [&] { ++drains; return std::vector<TerraEngine::Scene::EditorTransactionCompletion>(3U); };
			auto history = [&](const auto&) {
				if(recorded == 1U && fail) { fail = false; throw std::runtime_error("Injected history failure before publication"); }
				++recorded;
			};
			try { batch.Publish(drain, [&](const auto&) { ++settled; }, history); } catch(const std::runtime_error&) {}
			context.Expect(settled == 3U && batch.HasPending(), "All copy owners already received their commit before a history failure.");
			batch.Publish(drain, [&](const auto&) { ++settled; }, history);
			context.Expect(drains == 1U && settled == 3U && recorded == 3U && !batch.HasPending(), "History retry cannot revive, drop or repeat settled copy ownership.");
		});
	}
}
