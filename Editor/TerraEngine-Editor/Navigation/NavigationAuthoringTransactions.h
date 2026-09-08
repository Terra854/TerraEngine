// NavigationAuthoringTransactions.h - post-commit navigation editor history.

#pragma once

#include "Components/Navigation/NavigationConnectionComponent.h"
#include "Components/Navigation/NavigationSourceComponent.h"
#include "Navigation/NavigationSettings.h"

#include <optional>
#include <cstdint>
#include <string>
#include <vector>

class UndoRedoSystem;

namespace TerraEngine::Editor::Navigation {
	struct NavigationAuthoredState {
		std::vector<NavigationSourceComponent> sources;
		std::vector<TerraEngine::Navigation::NavigationProfile> profiles;
		std::vector<NavigationConnectionComponent> connections;

		[[nodiscard]] bool operator==(const NavigationAuthoredState& other) const noexcept;
	};

	enum class NavigationAuthoringTransactionState : std::uint8_t { Pending, Committed, Rejected };

	struct NavigationAuthoringTransactionReceipt {
		std::string transactionId;
		NavigationAuthoringTransactionState state = NavigationAuthoringTransactionState::Pending;

		[[nodiscard]] bool IsPending() const noexcept { return state == NavigationAuthoringTransactionState::Pending; }
		[[nodiscard]] bool IsCommitted() const noexcept { return state == NavigationAuthoringTransactionState::Committed; }
		[[nodiscard]] bool IsRejected() const noexcept { return state == NavigationAuthoringTransactionState::Rejected; }
	};

	class NavigationAuthoringTransactions final {
	public:
		explicit NavigationAuthoringTransactions(UndoRedoSystem& history);

		[[nodiscard]] NavigationAuthoringTransactionReceipt QueueReplace(
			std::string name, NavigationAuthoredState before, NavigationAuthoredState after);
		[[nodiscard]] NavigationAuthoringTransactionReceipt CompleteAtProtectedBoundary(
			const std::string& transactionId, bool committed);
		[[nodiscard]] bool Undo();
		[[nodiscard]] bool Redo();
		[[nodiscard]] const NavigationAuthoredState& CurrentState() const noexcept;
		void ReplaceForTesting(NavigationAuthoredState state);
		[[nodiscard]] NavigationAuthoredState Save() const;
		[[nodiscard]] bool Reload(const NavigationAuthoredState& saved);
		void EnterPlayMode();
		void ExitPlayMode();

		// ICommand uses this narrow callback; ordinary authoring still changes state
		// only through a protected completion receipt.
		void ApplyHistoryState(const NavigationAuthoredState& state);

	private:
		struct PendingReplace {
			std::string transactionId;
			std::string name;
			NavigationAuthoredState before;
			NavigationAuthoredState after;
		};

		UndoRedoSystem& history_;
		NavigationAuthoredState current_{};
		std::vector<PendingReplace> pending_;
		std::optional<NavigationAuthoredState> prePlayState_;
		std::uint64_t nextTransaction_ = 1U;
	};
}
