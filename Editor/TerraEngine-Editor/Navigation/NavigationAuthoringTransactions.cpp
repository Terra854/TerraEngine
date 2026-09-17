// NavigationAuthoringTransactions.cpp - complete-state undo/redo after commit.

#include "NavigationAuthoringTransactions.h"

#include "NavigationInspectorSupport.h"
#include "UndoRedoSystem.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace TerraEngine::Editor::Navigation {
	namespace {
		[[nodiscard]] bool SameProfile(const TerraEngine::Navigation::NavigationProfile& left,
			const TerraEngine::Navigation::NavigationProfile& right) noexcept {
			return left == right;
		}

		class NavigationStateCommand final : public ICommand {
		public:
			NavigationStateCommand(NavigationAuthoringTransactions& owner, std::string name,
				NavigationAuthoredState before, NavigationAuthoredState after)
				: owner_(owner), name_(std::move(name)), before_(std::move(before)), after_(std::move(after)) {}
			void Undo() override { owner_.ApplyHistoryState(before_); }
			void Redo() override { owner_.ApplyHistoryState(after_); }
			std::string GetName() const override { return name_; }
			std::string_view GetHistoryDomain() const override { return "Navigation"; }
		private:
			NavigationAuthoringTransactions& owner_;
			std::string name_;
			NavigationAuthoredState before_;
			NavigationAuthoredState after_;
		};
	}

	bool NavigationAuthoredState::operator==(const NavigationAuthoredState& other) const noexcept {
		return profiles.size() == other.profiles.size()
			&& std::equal(profiles.begin(), profiles.end(), other.profiles.begin(), SameProfile)
			&& settings.maxActiveAgents == other.settings.maxActiveAgents
			&& settings.maxAdmittedRequests == other.settings.maxAdmittedRequests
			&& settings.maxPublishedAndRetiredBytes == other.settings.maxPublishedAndRetiredBytes;
	}

	NavigationAuthoringTransactions::NavigationAuthoringTransactions(UndoRedoSystem& history) : history_(history) {}

	NavigationAuthoringTransactionReceipt NavigationAuthoringTransactions::QueueReplace(
		std::string name, NavigationAuthoredState before, NavigationAuthoredState after) {
		const std::string id = "navigation-authoring-" + std::to_string(nextTransaction_++);
		current_ = before;
		pending_.push_back({id, std::move(name), std::move(before), std::move(after)});
		return {id, NavigationAuthoringTransactionState::Pending};
	}

	NavigationAuthoringTransactionReceipt NavigationAuthoringTransactions::CompleteAtProtectedBoundary(
		const std::string& transactionId, bool committed) {
		const auto pending = std::find_if(pending_.begin(), pending_.end(), [&transactionId](const PendingReplace& entry) {
			return entry.transactionId == transactionId;
		});
		if(pending == pending_.end()) return {transactionId, NavigationAuthoringTransactionState::Rejected};
		PendingReplace completed = std::move(*pending);
		pending_.erase(pending);
		if(!committed) {
			current_ = std::move(completed.before);
			return {transactionId, NavigationAuthoringTransactionState::Rejected};
		}
		current_ = completed.after;
		history_.PushCommand(std::make_unique<NavigationStateCommand>(*this, std::move(completed.name),
			std::move(completed.before), std::move(completed.after)));
		return {transactionId, NavigationAuthoringTransactionState::Committed};
	}

	bool NavigationAuthoringTransactions::Undo() {
		if(!history_.CanUndo()) return false;
		history_.Undo();
		return true;
	}

	bool NavigationAuthoringTransactions::Redo() {
		if(!history_.CanRedo()) return false;
		history_.Redo();
		return true;
	}

	const NavigationAuthoredState& NavigationAuthoringTransactions::CurrentState() const noexcept { return current_; }
	void NavigationAuthoringTransactions::ReplaceForTesting(NavigationAuthoredState state) { current_ = std::move(state); }
	NavigationAuthoredState NavigationAuthoringTransactions::Save() const { return current_; }

	bool NavigationAuthoringTransactions::Reload(const NavigationAuthoredState& saved) {
		for(const auto& profile : saved.profiles)
			if(!ValidateNavigationProfileDraft(profile).accepted) return false;
		current_ = saved;
		return true;
	}

	void NavigationAuthoringTransactions::EnterPlayMode() { prePlayState_ = current_; }
	void NavigationAuthoringTransactions::ExitPlayMode() {
		if(prePlayState_) current_ = std::move(*prePlayState_);
		prePlayState_.reset();
	}
	void NavigationAuthoringTransactions::ApplyHistoryState(const NavigationAuthoredState& state) { current_ = state; }
}
