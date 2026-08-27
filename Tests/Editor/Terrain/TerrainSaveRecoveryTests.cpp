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
				// Persistence accepts the complete canonical payload for each channel;
				// tiny sentinel blobs would bypass the real .tearea durability contract.
				initialBytes.assign(TerraEngine::Terrain::kTerrainAreaSampleCount * sizeof(std::int32_t), 0u);
				initialBytes[0u] = 0x01u;
				initialBytes[1u] = 0x02u;
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
				std::vector<std::uint8_t> nextBytes(TerraEngine::Terrain::kTerrainAreaSampleCount * sizeof(std::int32_t), 0u);
				nextBytes[0u] = 0x99u; nextBytes[1u] = 0x88u;
				const auto nextHash = TerraEngine::Terrain::HashTerrainBytes(nextBytes); store->Blobs().PutVerified(nextHash, nextBytes);
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
		registry.Add("Editor.TerrainSaveRecovery.TERRAROOT1AllFailurePointsRetainPriorCurrent", [](TestContext& context) {
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
					"Every injected save failure must leave the prior CURRENT root recoverable. point="
						+ std::to_string(static_cast<int>(point)) + " failed='" + failed.message
						+ "' recovered='" + recovered.message + "'.");
			}
		});

		registry.Add("Editor.TerrainSaveRecovery.TERRAROOT1CorruptCurrentFallsBackToPredecessor", [](TestContext& context) {
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
			TerraEngine::Terrain::DecodedTerrainArea decodedHole;
			const auto durableHole = reloadedDescriptor && reloadedDescriptor->channels.holeHash
				? reloaded.Blobs().Resolve(*reloadedDescriptor->channels.holeHash) : std::nullopt;
			const auto decodedHoleResult = durableHole
				? TerraEngine::Terrain::DecodeTerrainArea(*durableHole, decodedHole)
				: TerraEngine::Terrain::TerrainCodecResult{TerraEngine::Terrain::TerrainResultCode::CorruptData, "missing durable hole"};
			context.Expect(first.Succeeded() && committed.outcome == TerraEngine::Terrain::TerrainEditOutcome::Committed && second.Succeeded()
				&& recovered.Succeeded() && recovered.usedPredecessor && recovered.savedRootHash == first.savedRootHash
				&& opened.Succeeded() && reloadedDescriptor && decodedHoleResult.Succeeded()
				&& decodedHole.header.channelKind == TerraEngine::Terrain::TerrainChannelKind::Holes
				&& decodedHole.canonicalPayload == fixture.initialHoleBytes,
				"Recovery must choose the complete predecessor and decode its canonical .tearea hole envelope without losing masks. first='"
					+ first.message + "' second='" + second.message + "' recovered='" + recovered.message
					+ "' opened='" + opened.message + "' decoded='" + decodedHoleResult.message + "'.");
		});
	}
}
