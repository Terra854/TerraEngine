// TerrainSaveRecoveryTests.cpp -- Durable terrain save failure and recovery checks.

#include "TestHarness.h"

#include "Terrain/TerrainPublication.h"

#include <filesystem>

namespace TerraEngine::EditorTests
{
	namespace
	{
		struct SaveFixture
		{
			SaveFixture()
			{
				initialBytes = {0x01U, 0x02U, 0x03U};
				initialHash = TerraEngine::Terrain::HashTerrainBytes(initialBytes);
				initialHoleBytes.assign(TerraEngine::Terrain::kTerrainAreaSampleCount / 8u, 0u);
				initialHoleBytes[3u] = 0x40u; initialHoleHash = TerraEngine::Terrain::HashTerrainBytes(initialHoleBytes);
				auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>();
				root->assetUuid = TerraEngine::GenerateUUID(); root->publicationGeneration = 1U; root->persistedRootGeneration = 1U;
				root->rootContentHash = TerraEngine::Terrain::HashTerrainBytes(std::vector<std::uint8_t>{0x77U}); root->authoredBounds = {0, 0, 256, 256}; root->layerStackGeneration = 1U;
				TerraEngine::Terrain::TerrainAreaDescriptor descriptor; descriptor.coordinate = {0, 0}; descriptor.editGeneration = 1U; descriptor.authoredContentHash = initialHash; descriptor.channels.heightHash = initialHash; descriptor.channels.holeHash = initialHoleHash;
				TerraEngine::Terrain::TerrainDirectoryShard shard; shard.areaBounds = root->authoredBounds; shard.identity = TerraEngine::Terrain::HashTerrainBytes(std::vector<std::uint8_t>{0x66U}); shard.descriptors.emplace(descriptor.coordinate, descriptor); root->directoryShards.push_back(std::move(shard));
				store = std::make_unique<TerraEngine::Terrain::TerrainAssetStore>(root);
				store->Blobs().PutVerified(initialHash, initialBytes); store->Blobs().PutVerified(initialHoleHash, initialHoleBytes);
				publication = std::make_unique<TerraEngine::Terrain::TerrainPublication>(*store);
				directory = std::filesystem::temp_directory_path() / ("terraengine-terrain-save-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID()));
			}
			~SaveFixture() { std::error_code error; std::filesystem::remove_all(directory, error); }

			TerraEngine::Terrain::TerrainTransactionReceipt CommitSecondRoot()
			{
				const auto capture = store->CaptureRoot(); const auto before = store->FindDescriptor(*capture.root, {0, 0});
				const std::vector<std::uint8_t> nextBytes = {0x99U, 0x88U}; const auto nextHash = TerraEngine::Terrain::HashTerrainBytes(nextBytes); store->Blobs().PutVerified(nextHash, nextBytes);
				TerraEngine::Terrain::TerrainPatch patch; patch.kind = TerraEngine::Terrain::TerrainPatchKind::ReplaceHeight; patch.preparedBlobHash = nextHash; patch.replacement = *before; patch.replacement.authoredContentHash = nextHash; patch.replacement.channels.heightHash = nextHash; patch.replacement.editGeneration++;
				TerraEngine::Terrain::TerrainEditTransaction edit; edit.transactionId = TerraEngine::GenerateUUID(); edit.targetAssetUuid = capture.root->assetUuid; edit.inputRootHash = capture.root->rootContentHash; edit.expectedLayerStackGeneration = capture.root->layerStackGeneration; edit.affectedBounds = capture.root->authoredBounds; edit.budget.maximumAreas = 1U; edit.readSet = {{before->coordinate, before->editGeneration, before->authoredContentHash}}; edit.patches = {patch};
				return publication->Publish(std::move(edit));
			}

			std::vector<std::uint8_t> initialBytes;
			TerraEngine::Terrain::ContentHash initialHash{};
			std::vector<std::uint8_t> initialHoleBytes;
			TerraEngine::Terrain::ContentHash initialHoleHash{};
			std::unique_ptr<TerraEngine::Terrain::TerrainAssetStore> store;
			std::unique_ptr<TerraEngine::Terrain::TerrainPublication> publication;
			std::filesystem::path directory;
		};
	}

	void RegisterTerrainSaveRecoveryTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainSaveRecovery.AllFailurePointsRetainPriorCurrent", [](TestContext& context) {
			SaveFixture fixture;
			const auto saved = fixture.store->PersistAtomically({fixture.directory});
			context.Expect(saved.Succeeded(), "The initial terrain root must persist before save-failure recovery is tested.");
			for(const auto point : {TerraEngine::Terrain::TerrainPersistenceFailurePoint::BlobWrite, TerraEngine::Terrain::TerrainPersistenceFailurePoint::DirectoryWrite,
				TerraEngine::Terrain::TerrainPersistenceFailurePoint::ManifestWrite, TerraEngine::Terrain::TerrainPersistenceFailurePoint::DurableFlush,
				TerraEngine::Terrain::TerrainPersistenceFailurePoint::CurrentReplacement})
			{
				const auto failed = fixture.store->PersistAtomically({fixture.directory, nullptr, point});
				const auto recovered = fixture.store->RecoverPersistedRoot(fixture.directory);
				context.Expect(!failed.Succeeded() && recovered.Succeeded() && recovered.savedRootHash == saved.savedRootHash,
					"Every injected save failure must leave the prior CURRENT root recoverable.");
			}
		});

		registry.Add("Editor.TerrainSaveRecovery.CorruptCurrentFallsBackToPredecessor", [](TestContext& context) {
			SaveFixture fixture;
			const auto first = fixture.store->PersistAtomically({fixture.directory});
			const auto committed = fixture.CommitSecondRoot();
			const auto second = fixture.store->PersistAtomically({fixture.directory});
			std::error_code error;
			std::filesystem::remove(fixture.directory / "roots" / (TerraEngine::Terrain::ToHexString(second.savedRootHash) + ".root"), error);
			const auto recovered = fixture.store->RecoverPersistedRoot(fixture.directory);
			TerraEngine::Terrain::TerrainAssetStore reloaded(fixture.store->CaptureRoot().root);
			const auto opened = reloaded.OpenPersistedRoot(fixture.directory);
			const auto reloadedDescriptor = reloaded.FindDescriptor(*reloaded.CaptureRoot().root, {0, 0});
			context.Expect(first.Succeeded() && committed.outcome == TerraEngine::Terrain::TerrainEditOutcome::Committed && second.Succeeded()
				&& recovered.Succeeded() && recovered.usedPredecessor && recovered.savedRootHash == first.savedRootHash
				&& opened.Succeeded() && reloadedDescriptor && reloadedDescriptor->channels.holeHash == fixture.initialHoleHash,
				"Recovery must choose the complete predecessor and re-open its authored hole identity without losing masks.");
		});
	}
}
