// EditorTransactionReceiptBatch.h -- Retain protected receipts across recovery.
#pragma once
#include "Scene/SceneMutationQueue.h"
#include <utility>
#include <vector>

namespace TerraEngine::Editor {
	class EditorTransactionReceiptBatch {
	public:
		[[nodiscard]] bool HasPending() const noexcept { return !receipts.empty(); }
		// Move the sole drained batch into owned storage before invoking fallible
		// consumers. Settle every asset owner before any history serialization.
		// A thrown callback retains its receipt and cursor for the next frame;
		// successful callbacks are never repeated, and no second drain competes.
		template<class Drain, class Settle, class RecordHistory>
		void Publish(Drain&& drain, Settle&& settle, RecordHistory&& history) {
			if(receipts.empty()) receipts = std::forward<Drain>(drain)();
			while(settlementCursor < receipts.size()) {
				settle(receipts[settlementCursor]);
				++settlementCursor;
			}
			while(historyCursor < receipts.size()) {
				history(receipts[historyCursor]);
				++historyCursor;
			}
			receipts.clear();
			settlementCursor = historyCursor = 0U;
		}
	private:
		std::vector<TerraEngine::Scene::EditorTransactionCompletion> receipts;
		std::size_t settlementCursor = 0U, historyCursor = 0U;
	};
}
