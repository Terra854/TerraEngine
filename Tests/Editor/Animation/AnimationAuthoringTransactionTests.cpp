// AnimationAuthoringTransactionTests.cpp - publisher-backed generation contracts.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Animation/AnimationAuthoringTransaction.h"
#include "Engine/Utilities/UUID.h"

namespace TerraEngine::EditorTests {
	void RegisterAnimationAuthoringTransactionTests(TestRegistry& registry) {
		registry.Add("Editor.Animation.AuthoringTransaction.AtomicPublisherStaleCancel", [](TestContext& context) {
			using namespace TerraEngine::Editor::Animation;
			const TerraEngine::Animation::AnimationGuid assetId{TerraEngine::GenerateUUID()};
			AnimationAuthoringRevision durable{assetId, 4U, "before"};
			AnimationAuthoringTransaction transaction("Grouped controller edit");
			context.Expect(transaction.Begin(durable, assetId, 4U) == AnimationAuthoringStatus::Ok
				&& transaction.Replace("after") == AnimationAuthoringStatus::Ok,
				"A matching GUID and generation must begin one detached edit.");

			AnimationAuthoringCompileRequest first;
			context.Expect(transaction.PrepareCompile(first) == AnimationAuthoringStatus::Ok
				&& transaction.Replace("newer after") == AnimationAuthoringStatus::Ok,
				"Replacing a candidate must cancel the older copied worker request.");
			AnimationAuthoringCompileResult stale{
				first.assetId, first.expectedGeneration, first.requestId, first.candidateBytes, true, false, {}};
			AnimationAuthoringRevision published;
			std::string diagnostic;
			bool publisherCalled = false;
			auto publisher = [&](const AnimationAuthoringRevision& expected,
				const AnimationAuthoringRevision& replacement, std::string&) {
				publisherCalled = true;
				if(durable != expected) return false;
				durable = replacement;
				return true;
			};
			context.Expect(transaction.CompleteCompile(stale, durable, publisher, published, diagnostic)
				== AnimationAuthoringStatus::Stale && !publisherCalled && durable.bytes == "before",
				"An invalidated worker packet must not reach the publication callback.");

			AnimationAuthoringCompileRequest currentRequest;
			context.Expect(transaction.PrepareCompile(currentRequest) == AnimationAuthoringStatus::Ok,
				"The replacement candidate must produce a new worker request.");
			AnimationAuthoringCompileResult currentResult{currentRequest.assetId,
				currentRequest.expectedGeneration, currentRequest.requestId,
				currentRequest.candidateBytes, true, false, "compiled"};
			AnimationAuthoringRevision wrongGeneration = durable;
			wrongGeneration.generation = 5U;
			context.Expect(transaction.CompleteCompile(currentResult, wrongGeneration,
				publisher, published, diagnostic) == AnimationAuthoringStatus::Stale
				&& durable.bytes == "before",
				"A changed current generation must fail before publication.");
			context.Expect(transaction.CompleteCompile(currentResult, durable,
				publisher, published, diagnostic) == AnimationAuthoringStatus::Ok
				&& durable.bytes == "newer after" && durable.generation == 5U,
				"The protected boundary must publish the exact validated bytes once.");

			AnimationAuthoringTransaction canceled("Canceled edit");
			AnimationAuthoringCompileRequest canceledRequest;
			context.Expect(canceled.Begin(durable, assetId, 5U) == AnimationAuthoringStatus::Ok
				&& canceled.Replace("discard") == AnimationAuthoringStatus::Ok
				&& canceled.PrepareCompile(canceledRequest) == AnimationAuthoringStatus::Ok
				&& canceled.Cancel() == AnimationAuthoringStatus::Canceled
				&& canceledRequest.cancellationRequested->load()
				&& durable.bytes == "newer after",
				"Cancel must signal the worker token and preserve the durable revision.");
		});

		registry.Add("Editor.Animation.AuthoringTransaction.UndoRedoPublishesMonotonicGenerations", [](TestContext& context) {
			using namespace TerraEngine::Editor::Animation;
			const TerraEngine::Animation::AnimationGuid assetId{TerraEngine::GenerateUUID()};
			const AnimationAuthoringRevision before{assetId, 8U, "before"};
			const AnimationAuthoringRevision after{assetId, 9U, "after"};
			AnimationAuthoringRevision durable = after;
			AnimationAuthoringHistory history;
			std::string diagnostic;
			auto publisher = [&](const AnimationAuthoringRevision& expected,
				const AnimationAuthoringRevision& replacement, std::string&) {
				if(durable != expected) return false;
				durable = replacement;
				return true;
			};
			AnimationAuthoringRevision published;
			context.Expect(history.Commit(before, after) == AnimationAuthoringStatus::Ok
				&& history.Undo(durable, publisher, published, diagnostic) == AnimationAuthoringStatus::Ok
				&& durable.bytes == "before" && durable.generation == 10U,
				"Undo must publish the complete before bytes as a new generation.");
			context.Expect(history.Redo(durable, publisher, published, diagnostic) == AnimationAuthoringStatus::Ok
				&& durable.bytes == "after" && durable.generation == 11U,
				"Redo must publish the complete after bytes without rewinding generation.");
		});
	}
}
