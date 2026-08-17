// TerrainHistoryTests.cpp -- Exact deferred replay and bounded terrain history.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Subsystems/UndoRedoSystem.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainHistoryCommand.h"

#include "Terrain/TerrainPublication.h"

namespace TerraEngine::EditorTests
{
	namespace
	{
		bool SameChannelIdentity(const Terrain::TerrainAreaDescriptor& left, const Terrain::TerrainAreaDescriptor& right)
		{
			return left.channels.heightHash == right.channels.heightHash
				&& left.channels.holeHash == right.channels.holeHash
				&& left.channels.weightGroupHashes == right.channels.weightGroupHashes;
		}

		bool SameDescriptor(const Terrain::TerrainAreaDescriptor& left, const Terrain::TerrainAreaDescriptor& right)
		{
			return left.coordinate == right.coordinate && left.editGeneration == right.editGeneration
				&& SameChannelIdentity(left, right) && left.minHeightMm == right.minHeightMm
				&& left.maxHeightMm == right.maxHeightMm && left.layerPresence == right.layerPresence
				&& left.authoredContentHash == right.authoredContentHash;
		}

		class BudgetCommand final : public ICommand
		{
		public:
			explicit BudgetCommand(std::size_t bytes) : m_Bytes(bytes) {}
			void Undo() override {}
			void Redo() override {}
			std::string GetName() const override { return "Budget Probe"; }
			std::size_t GetHistoryBytes() const override { return m_Bytes; }
		private:
			std::size_t m_Bytes = 0U;
		};

		struct TerrainHistoryFixture
		{
			TerrainHistoryFixture()
			{
				root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>();
				root->assetUuid = TerraEngine::GenerateUUID();
				root->publicationGeneration = 1U;
				root->persistedRootGeneration = 1U;
				root->rootContentHash = {1U, 2U};
				root->authoredBounds = {0, 0, 256, 256};
				root->layerStackGeneration = 1U;
				const std::vector<std::uint8_t> initialHeightBytes{0x10U, 0x11U};
				const std::vector<std::uint8_t> initialHoleBytes{0x20U, 0x21U};
				const std::vector<std::uint8_t> initialWeightBytes{0x30U, 0x31U};
				const auto initialHeightHash = TerraEngine::Terrain::HashTerrainBytes(initialHeightBytes);
				const auto initialHoleHash = TerraEngine::Terrain::HashTerrainBytes(initialHoleBytes);
				const auto initialWeightHash = TerraEngine::Terrain::HashTerrainBytes(initialWeightBytes);
				TerraEngine::Terrain::TerrainAreaDescriptor descriptor;
				descriptor.coordinate = {0, 0};
				descriptor.editGeneration = 1U;
				descriptor.authoredContentHash = initialHeightHash;
				descriptor.channels.heightHash = initialHeightHash;
				descriptor.channels.holeHash = initialHoleHash;
				descriptor.channels.weightGroupHashes.emplace(1U, initialWeightHash);
				TerraEngine::Terrain::TerrainDirectoryShard shard;
				shard.areaBounds = root->authoredBounds;
				shard.identity = {9U, 10U};
				shard.descriptors.emplace(descriptor.coordinate, descriptor);
				root->directoryShards.push_back(std::move(shard));
				const std::vector<std::uint8_t> unrelatedBytes{0x40U, 0x41U};
				const auto unrelatedHash = TerraEngine::Terrain::HashTerrainBytes(unrelatedBytes);
				Terrain::TerrainAreaDescriptor unrelated;
				unrelated.coordinate = {1, 0};
				unrelated.editGeneration = 7U;
				unrelated.authoredContentHash = unrelatedHash;
				unrelated.channels.heightHash = unrelatedHash;
				root->directoryShards.front().descriptors.emplace(unrelated.coordinate, unrelated);
				store = std::make_unique<TerraEngine::Terrain::TerrainAssetStore>(root);
				store->Blobs().PutVerified(initialHeightHash, initialHeightBytes);
				store->Blobs().PutVerified(initialHoleHash, initialHoleBytes);
				store->Blobs().PutVerified(initialWeightHash, initialWeightBytes);
				store->Blobs().PutVerified(unrelatedHash, unrelatedBytes);
				publication = std::make_unique<TerraEngine::Terrain::TerrainPublication>(*store);
			}

			TerraEngine::Terrain::TerrainEditTransaction MakeEdit(std::uint32_t index)
			{
				const auto capture = store->CaptureRoot();
				const auto before = store->FindDescriptor(*capture.root, {0, 0});
				const std::vector<std::uint8_t> nextBytes{
					static_cast<std::uint8_t>(index),
					static_cast<std::uint8_t>(index >> 8U),
					static_cast<std::uint8_t>(index % 3U),
					0x5AU};
				const TerraEngine::Terrain::ContentHash next = TerraEngine::Terrain::HashTerrainBytes(nextBytes);
				store->Blobs().PutVerified(next, nextBytes);
				TerraEngine::Terrain::TerrainPatch forward;
				forward.replacement = *before;
				forward.preparedBlobHash = next;
				forward.replacement.editGeneration++;
				forward.replacement.authoredContentHash = next;
				// Rotate real height, holes, and a weight group so the test covers
				// mixed channels without bypassing TerrainPublication.
				if(index % 3U == 0U) { forward.kind = TerraEngine::Terrain::TerrainPatchKind::ReplaceHeight; forward.replacement.channels.heightHash = next; }
				else if(index % 3U == 1U)
				{
					forward.kind = TerraEngine::Terrain::TerrainPatchKind::ReplaceHoles;
					const auto previousHash = before->channels.holeHash.value();
					const auto previousBytes = store->Blobs().Resolve(previousHash);
					forward.replacement.channels.holeHash = next;
					// Hole geometry must replay exact masks; history is not allowed to
					// regenerate them from a brush after collision or render data changes.
					TerraEngine::Terrain::TerrainExactSpan beforeSpan;
					beforeSpan.channel = TerraEngine::Terrain::TerrainChannelKind::Holes;
					beforeSpan.uncompressedByteCount = previousBytes ? previousBytes->size() : 0U;
					beforeSpan.compressedBytes = previousBytes.value_or(std::vector<std::uint8_t>{});
					beforeSpan.compressedContentHash = previousHash;
					beforeSpan.immutableBlobHash = previousHash;
					TerraEngine::Terrain::TerrainExactSpan afterSpan;
					afterSpan.channel = TerraEngine::Terrain::TerrainChannelKind::Holes;
					afterSpan.uncompressedByteCount = nextBytes.size();
					afterSpan.compressedBytes = nextBytes;
					afterSpan.compressedContentHash = next;
					afterSpan.immutableBlobHash = next;
					forward.beforeSpans = {beforeSpan};
					forward.afterSpans = {afterSpan};
				}
				else
				{
					forward.kind = TerraEngine::Terrain::TerrainPatchKind::ReplaceWeightGroup;
					const auto previousHash = before->channels.weightGroupHashes.at(1U);
					const auto previousBytes = store->Blobs().Resolve(previousHash);
					forward.replacement.channels.weightGroupHashes[1U] = next;
					TerraEngine::Terrain::TerrainExactSpan beforeSpan;
					beforeSpan.channel = TerraEngine::Terrain::TerrainChannelKind::WeightGroup;
					beforeSpan.weightGroup = 1U;
					beforeSpan.uncompressedByteCount = previousBytes ? previousBytes->size() : 0U;
					beforeSpan.compressedBytes = previousBytes.value_or(std::vector<std::uint8_t>{});
					beforeSpan.compressedContentHash = previousHash;
					beforeSpan.immutableBlobHash = previousHash;
					TerraEngine::Terrain::TerrainExactSpan afterSpan;
					afterSpan.channel = TerraEngine::Terrain::TerrainChannelKind::WeightGroup;
					afterSpan.weightGroup = 1U;
					afterSpan.uncompressedByteCount = nextBytes.size();
					afterSpan.compressedBytes = nextBytes;
					afterSpan.compressedContentHash = next;
					afterSpan.immutableBlobHash = next;
					forward.beforeSpans = {beforeSpan};
					forward.afterSpans = {afterSpan};
				}
				forward.retainedBlobHashes = {next, before->authoredContentHash};
				TerraEngine::Terrain::TerrainPatch inverse = forward;
				inverse.replacement = *before;
				inverse.preparedBlobHash = before->authoredContentHash;
				inverse.beforeSpans = forward.afterSpans;
				inverse.afterSpans = forward.beforeSpans;
				TerraEngine::Terrain::TerrainEditTransaction transaction;
				transaction.transactionId = TerraEngine::GenerateUUID();
				transaction.targetAssetUuid = capture.root->assetUuid;
				transaction.inputRootHash = capture.root->rootContentHash;
				transaction.expectedLayerStackGeneration = capture.root->layerStackGeneration;
				transaction.affectedBounds = capture.root->authoredBounds;
				transaction.budget.maximumAreas = 1U;
				transaction.readSet = {{before->coordinate, before->editGeneration, before->authoredContentHash}};
				transaction.patches = {std::move(forward)};
				transaction.inversePatches = {std::move(inverse)};
				return transaction;
			}

			std::shared_ptr<TerraEngine::Terrain::TerrainAssetRoot> root;
			std::unique_ptr<TerraEngine::Terrain::TerrainAssetStore> store;
			TerraEngine::Terrain::TerrainMutationQueue queue;
			std::unique_ptr<TerraEngine::Terrain::TerrainPublication> publication;
		};
	}

	void RegisterTerrainHistoryTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainHistory.ExactMixedChannel1000StrokeUndoRedo", [](TestContext& context) {
			TerrainHistoryFixture fixture;
			UndoRedoSystem::Instance().Clear();
			UndoRedoSystem::Instance().SetHistoryBudget(1000U, 512U * 1024U * 1024U);
			const auto initialRoot = fixture.store->CaptureRoot();
			const auto initialEdited = fixture.store->FindDescriptor(*initialRoot.root, {0, 0});
			const auto initialUnrelated = fixture.store->FindDescriptor(*initialRoot.root, {1, 0});
			for(std::uint32_t index = 0U; index < 1000U; ++index)
			{
				const auto receipt = fixture.publication->Publish(fixture.MakeEdit(index));
				context.Expect(receipt.outcome == TerraEngine::Terrain::TerrainEditOutcome::Committed,
					"Every bounded mixed-channel stroke must commit before it enters history.");
				if(!receipt.historyPayload) continue;
				const auto capture = fixture.store->CaptureRoot();
				TerraEngine::Terrain::TerrainEditTransaction forward;
				forward.targetAssetUuid = capture.root->assetUuid;
				forward.inputRootHash = capture.root->rootContentHash;
				forward.expectedLayerStackGeneration = capture.root->layerStackGeneration;
				forward.affectedBounds = capture.root->authoredBounds;
				forward.budget.maximumAreas = receipt.historyPayload->forwardPatches.size();
				forward.patches = receipt.historyPayload->forwardPatches;
				TerraEngine::Terrain::TerrainEditTransaction inverse = forward;
				inverse.patches = receipt.historyPayload->inversePatches;
				UndoRedoSystem::Instance().PushCommand(std::make_unique<TerraEngine::Editor::Terrain::TerrainHistoryCommand>(
					*fixture.store, fixture.queue, std::move(forward), std::move(inverse), receipt.historyPayload->retainedByteCount));
			}
			context.Expect(UndoRedoSystem::Instance().GetUndoDepth() == 1000U,
				"Terrain history must retain exactly the configured 1000 mixed-channel strokes.");
			context.Expect(UndoRedoSystem::Instance().GetHistoryBytes() > 0U
				&& UndoRedoSystem::Instance().GetHistoryBytes() <= UndoRedoSystem::Instance().GetHistoryByteBudget(),
				"Exact terrain history must account retained bytes without exceeding its configured byte budget.");
			const auto committedRoot = fixture.store->CaptureRoot();
			const auto committedEdited = fixture.store->FindDescriptor(*committedRoot.root, {0, 0});
			UndoRedoSystem::Instance().SetSavePoint();
			context.Expect(UndoRedoSystem::Instance().IsAtSavePoint(), "The committed 1,000-stroke state must be recorded as the exact save point.");
			for(std::uint32_t index = 0U; index < 1000U; ++index)
			{
				UndoRedoSystem::Instance().Undo();
				for(const auto& receipt : fixture.publication->PublishQueued(fixture.queue))
					context.Expect(UndoRedoSystem::Instance().CompleteTerrainReplay(receipt), "Undo replay receipts must advance exactly one deferred terrain cursor.");
			}
			context.Expect(UndoRedoSystem::Instance().GetUndoDepth() == 0U && UndoRedoSystem::Instance().GetRedoDepth() == 1000U,
				"Undo must preserve every exact terrain command for a later redo.");
			const auto undoneRoot = fixture.store->CaptureRoot();
			const auto undoneEdited = fixture.store->FindDescriptor(*undoneRoot.root, {0, 0});
			const auto undoneUnrelated = fixture.store->FindDescriptor(*undoneRoot.root, {1, 0});
			context.Expect(undoneEdited && initialEdited && SameChannelIdentity(*undoneEdited, *initialEdited),
				"Undoing all strokes must restore the exact initial mixed-channel identities.");
			context.Expect(undoneUnrelated && initialUnrelated && SameDescriptor(*undoneUnrelated, *initialUnrelated),
				"Undo must never modify an unrelated terrain area.");
			context.Expect(!UndoRedoSystem::Instance().IsAtSavePoint(), "Moving before the saved terrain state must mark the history cursor unsaved.");
			for(std::uint32_t index = 0U; index < 1000U; ++index)
			{
				UndoRedoSystem::Instance().Redo();
				for(const auto& receipt : fixture.publication->PublishQueued(fixture.queue))
					context.Expect(UndoRedoSystem::Instance().CompleteTerrainReplay(receipt), "Redo replay receipts must advance exactly one deferred terrain cursor.");
			}
			context.Expect(UndoRedoSystem::Instance().GetUndoDepth() == 1000U && UndoRedoSystem::Instance().GetRedoDepth() == 0U,
				"Redo must return the cursor to the saved 1000-stroke terrain state.");
			const auto redoneRoot = fixture.store->CaptureRoot();
			const auto redoneEdited = fixture.store->FindDescriptor(*redoneRoot.root, {0, 0});
			const auto redoneUnrelated = fixture.store->FindDescriptor(*redoneRoot.root, {1, 0});
			context.Expect(redoneEdited && committedEdited && SameChannelIdentity(*redoneEdited, *committedEdited),
				"Redoing all strokes must restore the exact saved mixed-channel identities.");
			context.Expect(redoneUnrelated && initialUnrelated && SameDescriptor(*redoneUnrelated, *initialUnrelated),
				"Redo must preserve the unrelated terrain area byte-for-byte.");
			context.Expect(UndoRedoSystem::Instance().IsAtSavePoint(), "Redoing to the saved state must return the exact save-point identity.");
			UndoRedoSystem::Instance().Clear();
		});

		registry.Add("Editor.TerrainHistory.ConfigurableCountAndByteBudgetsPruneOldest", [](TestContext& context) {
			UndoRedoSystem::Instance().Clear();
			UndoRedoSystem::Instance().SetHistoryBudget(2U, 150U);
			UndoRedoSystem::Instance().PushCommand(std::make_unique<BudgetCommand>(100U));
			UndoRedoSystem::Instance().PushCommand(std::make_unique<BudgetCommand>(100U));
			context.Expect(UndoRedoSystem::Instance().GetUndoDepth() == 1U && UndoRedoSystem::Instance().GetHistoryBytes() == 100U,
				"The byte budget must prune the oldest command even before the count budget is reached.");
			UndoRedoSystem::Instance().PushCommand(std::make_unique<BudgetCommand>(25U));
			UndoRedoSystem::Instance().PushCommand(std::make_unique<BudgetCommand>(25U));
			context.Expect(UndoRedoSystem::Instance().GetUndoDepth() == 2U && UndoRedoSystem::Instance().GetHistoryBytes() == 50U,
				"The count budget must retain only the newest commands and update retained-byte accounting.");
			UndoRedoSystem::Instance().Clear();
			UndoRedoSystem::Instance().SetHistoryBudget(1000U, 512U * 1024U * 1024U);
		});
	}
}
