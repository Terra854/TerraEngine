// TerrainEditLifecycleTests.cpp -- Cancellation and root-identity dirty checks.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Terrain/TerrainAuthoringSystem.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainDirtyState.h"

#include "Terrain/TerrainPublication.h"
#include "Terrain/TerrainGarbageCollector.h"

namespace TerraEngine::EditorTests
{
	namespace
	{
		struct LifecycleFixture
		{
			LifecycleFixture()
			{
				const std::vector<std::uint8_t> bytes{0x12U, 0x34U};
				const auto hash = TerraEngine::Terrain::HashTerrainBytes(bytes);
				auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>();
				root->assetUuid = TerraEngine::GenerateUUID(); root->publicationGeneration = 1U; root->rootContentHash = {21U, 22U}; root->authoredBounds = {0, 0, 256, 256}; root->layerStackGeneration = 1U;
				TerraEngine::Terrain::TerrainAreaDescriptor descriptor; descriptor.coordinate = {0, 0}; descriptor.editGeneration = 1U; descriptor.authoredContentHash = hash; descriptor.channels.heightHash = hash;
				TerraEngine::Terrain::TerrainDirectoryShard shard; shard.areaBounds = root->authoredBounds; shard.identity = {25U, 26U}; shard.descriptors.emplace(descriptor.coordinate, descriptor); root->directoryShards.push_back(std::move(shard));
				store = std::make_unique<TerraEngine::Terrain::TerrainAssetStore>(root);
				store->Blobs().PutVerified(hash, bytes);
				publication = std::make_unique<TerraEngine::Terrain::TerrainPublication>(*store);
				authoring.Configure(queue, *publication); authoring.Initialize();
			}

			bool QueueDetached(std::string& outError)
			{
				const auto capture = store->CaptureRoot();
				TerraEngine::Terrain::TerrainEditTransaction transaction;
				transaction.transactionId = TerraEngine::GenerateUUID(); transaction.targetAssetUuid = capture.root->assetUuid;
				transaction.inputRootHash = capture.root->rootContentHash; transaction.expectedLayerStackGeneration = capture.root->layerStackGeneration;
				transaction.affectedBounds = capture.root->authoredBounds; transaction.budget.maximumAreas = 1U;
				return authoring.EnqueueDetachedCommit(std::move(transaction), outError);
			}

			void CancelCannotPublish(TestContext& context, std::string_view boundary)
			{
				std::string error;
				const auto before = store->CaptureRoot().publicationGeneration;
				const bool queued = QueueDetached(error);
				const std::size_t cancelled = authoring.CancelPreparedWork(boundary);
				authoring.PublishAtProtectedBoundary();
				const auto receipts = authoring.DrainReceipts();
				context.Expect(queued && cancelled == 1U && receipts.size() == 1U
					&& receipts.front().outcome == TerraEngine::Terrain::TerrainEditOutcome::Cancelled
					&& store->CaptureRoot().publicationGeneration == before,
					"A lifecycle cancellation must discard detached terrain work before the protected boundary.");
			}

			std::unique_ptr<TerraEngine::Terrain::TerrainAssetStore> store;
			TerraEngine::Terrain::TerrainMutationQueue queue;
			std::unique_ptr<TerraEngine::Terrain::TerrainPublication> publication;
			TerraEngine::Editor::Terrain::TerrainAuthoringSystem authoring;
		};

		void RegisterCancellationCase(TestRegistry& registry, std::string name, std::string boundary)
		{
			registry.Add(std::move(name), [boundary = std::move(boundary)](TestContext& context) {
				LifecycleFixture fixture;
				fixture.CancelCannotPublish(context, boundary);
			});
		}
	}

	void RegisterTerrainEditLifecycleTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainLifecycle.RootContentIdentityControlsDirtyState", [](TestContext& context) {
			auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>(); root->publicationGeneration = 3U; root->rootContentHash = {11U, 12U};
			TerraEngine::Editor::Terrain::TerrainDirtyState dirty;
			dirty.MarkSaved({root, root->publicationGeneration});
			root->publicationGeneration = 99U; dirty.Observe({root, root->publicationGeneration});
			context.Expect(!dirty.IsDirty(), "A publication generation alone must not dirty terrain; root content identity is authoritative.");
			root->rootContentHash = {13U, 14U}; dirty.Observe({root, root->publicationGeneration});
			context.Expect(dirty.IsDirty(), "A changed root content hash must dirty terrain even when the scene is unchanged.");
			dirty.MarkSaved({root, root->publicationGeneration});
			context.Expect(!dirty.IsDirty(), "Saving the current root must restore the clean terrain identity.");
		});

		RegisterCancellationCase(registry, "Editor.TerrainLifecycle.PreviewCancelDiscardsDetachedWork", "preview cancelled");
		RegisterCancellationCase(registry, "Editor.TerrainLifecycle.ToolSwitchDiscardsDetachedWork", "tool changed");
		RegisterCancellationCase(registry, "Editor.TerrainLifecycle.SceneSwitchDiscardsDetachedWork", "scene switched");
		RegisterCancellationCase(registry, "Editor.TerrainLifecycle.ProjectSwitchDiscardsDetachedWork", "project switched");
		RegisterCancellationCase(registry, "Editor.TerrainLifecycle.PlayEntryDiscardsDetachedWork", "play mode entered");
		RegisterCancellationCase(registry, "Editor.TerrainLifecycle.DeviceLossDiscardsDetachedWork", "device lost");
		RegisterCancellationCase(registry, "Editor.TerrainLifecycle.StaleCompletionIsCancelledBeforePublication", "stale worker completion");
		registry.Add("Editor.TerrainLifecycle.BoundedGarbageCollectionPreservesLiveRoot", [](TestContext& context) {
			LifecycleFixture fixture;
			const std::vector<std::uint8_t> orphanBytes{0xCAU, 0xFEU};
			const auto orphanHash = TerraEngine::Terrain::HashTerrainBytes(orphanBytes);
			fixture.store->Blobs().PutVerified(orphanHash, orphanBytes);
			TerraEngine::Terrain::TerrainGarbageCollector collector(*fixture.store);
			const auto receipt = collector.Collect({{}, 1U});
			context.Expect(!receipt.cancelled && receipt.removed.size() == 1U && receipt.removed.front() == orphanHash
				&& fixture.store->Blobs().Resolve(orphanHash) == std::nullopt,
				"A bounded GC action must remove only unreachable immutable blobs while the live root stays protected.");
		});
	}
}
