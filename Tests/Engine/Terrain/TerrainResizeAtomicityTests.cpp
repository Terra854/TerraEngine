// TerrainResizeAtomicityTests.cpp -- Resize failure containment regressions.

#include "TestHarness.h"
#include "Terrain/TerrainPublication.h"
#include "TerrainResizeTestFixtures.h"

namespace TerraEngine::Tests
{
	void RegisterTerrainResizeAtomicityTests(TestRegistry& registry)
	{
		registry.Add("Terrain.ResizeAtomicity.StaleCancelledAndInjectedFailurePreserveRoot", [](TestContext& context) {
			auto root = TerrainResizeFixtures::MakeRoot();
			Terrain::TerrainAssetStore store(root);
			Terrain::TerrainPublication publication(store);
			const auto descriptor = store.ResolveDescriptorOrImplicit(*root, {0, 0});
			auto makeTransaction = [&]() {
				Terrain::TerrainEditTransaction transaction{};
				transaction.transactionId = TerraEngine::GenerateUUID();
				transaction.targetAssetUuid = root->assetUuid;
				transaction.inputRootHash = root->rootContentHash;
				transaction.expectedLayerStackGeneration = root->layerStackGeneration;
				transaction.affectedBounds = {-256, 0, 256, 256};
				transaction.budget.maximumAreas = 2u;
				transaction.readSet = {{descriptor.coordinate, descriptor.editGeneration, descriptor.authoredContentHash}};
				Terrain::TerrainResizeDelta forward{};
				forward.previousBounds = root->authoredBounds;
				forward.proposedBounds = transaction.affectedBounds;
				Terrain::TerrainResizeDelta inverse = forward;
				inverse.kind = Terrain::TerrainResizeKind::Crop;
				inverse.previousBounds = forward.proposedBounds;
				inverse.proposedBounds = forward.previousBounds;
				transaction.resizeDelta = forward;
				transaction.inverseResizeDelta = inverse;
				return transaction;
			};

			bool allRejectedAtomically = true;
			for(std::size_t run = 0u; run < 100u; ++run)
			{
				auto stale = makeTransaction();
				stale.inputRootHash = {99u, 99u};
				const auto staleReceipt = publication.Publish(stale);
				auto cancelled = makeTransaction();
				cancelled.cancelled = true;
				const auto cancelledReceipt = publication.Publish(cancelled);
				auto conflict = makeTransaction();
				conflict.readSet.front().editGeneration++;
				const auto conflictReceipt = publication.Publish(conflict);
				auto overBudget = makeTransaction();
				overBudget.budget.maximumAreas = 0u;
				const auto budgetReceipt = publication.Publish(overBudget);
				publication.SetInjectFailureForTests(true);
				const auto failedReceipt = publication.Publish(makeTransaction());
				publication.SetInjectFailureForTests(false);
				std::shared_ptr<const Terrain::TerrainAssetRoot> invalidRoot;
				const auto rootBuild = store.BuildResizeCandidate(
					*root, {0, 0, 0, 256}, {}, {}, invalidRoot);
				allRejectedAtomically = allRejectedAtomically
					&& staleReceipt.outcome == Terrain::TerrainEditOutcome::Rejected
					&& cancelledReceipt.outcome == Terrain::TerrainEditOutcome::Cancelled
					&& conflictReceipt.outcome == Terrain::TerrainEditOutcome::Rejected
					&& budgetReceipt.outcome == Terrain::TerrainEditOutcome::Rejected
					&& failedReceipt.outcome == Terrain::TerrainEditOutcome::Rejected
					&& !rootBuild.Succeeded() && !invalidRoot
					&& store.CaptureRoot().root == root;
			}
			const auto after = store.CaptureRoot();
			context.Expect(allRejectedAtomically,
				"One hundred stale, cancelled, conflicting, over-budget, root-build, and injected publication failures must return terminal non-commit results.");
			context.Expect(after.root == root && after.publicationGeneration == 1u
				&& after.root->authoredBounds == Terrain::TerrainSampleBounds{0, 0, 256, 256},
				"Stale, cancelled, and candidate failures must preserve the exact previous root.");
		});
	}
}
