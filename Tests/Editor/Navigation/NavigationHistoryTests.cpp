// NavigationHistoryTests.cpp - post-commit authoring history and lifecycle contracts.
//
// Navigation edits are scene transactions. These tests ensure their dedicated
// coordinator only hands complete changes to UndoRedoSystem after the protected
// visibility boundary, and preserves exact authored state across editor modes.

#include "TestHarness.h"

#include "Components/Navigation/NavigationConnectionComponent.h"
#include "Components/Navigation/NavigationSourceComponent.h"
#include "Navigation/NavigationSettings.h"
#include "Editor/TerraEngine-Editor/Navigation/NavigationAuthoringTransactions.h"
#include "UndoRedoSystem.h"

namespace TerraEngine::EditorTests
{
	namespace
	{
		TerraEngine::Editor::Navigation::NavigationAuthoredState MakeAuthoredState()
		{
			TerraEngine::Editor::Navigation::NavigationAuthoredState state;
			NavigationSourceComponent source;
			source.sourceId[0] = 0x51U;
			source.sourceKind = TerraEngine::Navigation::NavigationSourceKind::Terrain;
			source.geometryRevision = 7U;
			state.sources.push_back(source);
			state.profiles.push_back(TerraEngine::Navigation::NavigationProfile::Defaults());
			NavigationConnectionComponent connection;
			connection.connectionId[0] = 0x61U;
			connection.traversalCost = 2.0F;
			state.connections.push_back(connection);
			return state;
		}
	}

	void RegisterNavigationHistoryTests(TestRegistry& registry)
	{
		registry.Add("Editor.NavigationHistory.RecordsOnlyCompleteProtectedCommits", [](TestContext& context) {
			UndoRedoSystem& history = UndoRedoSystem::Instance();
			history.Clear();
			TerraEngine::Editor::Navigation::NavigationAuthoringTransactions authoring(history);
			const auto before = MakeAuthoredState();
			auto after = before;
			after.profiles.front().radiusMetres = 0.6;

			const auto pending = authoring.QueueReplace("Edit Navigation Profile", before, after);
			context.Expect(pending.IsPending() && history.GetUndoDepth() == 0U,
				"A queued navigation edit must not become undoable before its protected transaction commits.");
			const auto committed = authoring.CompleteAtProtectedBoundary(pending.transactionId, true);
			context.Expect(committed.IsCommitted() && history.GetUndoDepth() == 1U,
				"Only the complete protected commit must create one navigation undo item.");

			context.Expect(authoring.Undo() && authoring.CurrentState() == before,
				"Undo must restore the complete prior source, profile, and connection revision.");
			context.Expect(authoring.Redo() && authoring.CurrentState() == after,
				"Redo must restore the same complete authored revision without creating a new history item.");
			history.Clear();
		});

		registry.Add("Editor.NavigationHistory.RejectedTransactionsNeverEnterUndoRedo", [](TestContext& context) {
			UndoRedoSystem& history = UndoRedoSystem::Instance();
			history.Clear();
			TerraEngine::Editor::Navigation::NavigationAuthoringTransactions authoring(history);
			const auto before = MakeAuthoredState();
			auto invalid = before;
			invalid.profiles.front().heightMetres = 0.0;

			const auto rejected = authoring.QueueReplace("Reject Invalid Navigation Profile", before, invalid);
			const auto completion = authoring.CompleteAtProtectedBoundary(rejected.transactionId, false);
			context.Expect(completion.IsRejected() && history.GetUndoDepth() == 0U
				&& authoring.CurrentState() == before,
				"A rejected navigation batch must retain the prior complete state and consume no undo slot.");
			history.Clear();
		});

		registry.Add("Editor.NavigationHistory.SaveReloadAndPlayModeRestoreAuthoredState", [](TestContext& context) {
			UndoRedoSystem& history = UndoRedoSystem::Instance();
			history.Clear();
			TerraEngine::Editor::Navigation::NavigationAuthoringTransactions authoring(history);
			const auto authored = MakeAuthoredState();
			authoring.ReplaceForTesting(authored);

			const auto saved = authoring.Save();
			authoring.ReplaceForTesting({});
			context.Expect(authoring.Reload(saved) && authoring.CurrentState() == authored,
				"Save/reload must preserve every navigation source, profile, and explicit connection field.");

			authoring.EnterPlayMode();
			auto playModeState = authored;
			playModeState.sources.front().enabled = false;
			authoring.ReplaceForTesting(playModeState);
			authoring.ExitPlayMode();
			context.Expect(authoring.CurrentState() == authored,
				"Leaving Play Mode must restore the complete pre-play authored navigation state.");
			history.Clear();
		});
	}
}
