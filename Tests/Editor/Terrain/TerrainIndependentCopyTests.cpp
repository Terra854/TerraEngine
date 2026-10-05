// Durable copy identity and authored-content acceptance through real providers.
#include "TerrainIndependentCopyTestFixtures.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainCopySourceMetadata.h"
#include "Editor/TerraEngine-Editor/Core/EditorSchedulerDeclaration.h"

namespace TerraEngine::EditorTests {
	using namespace TerrainCopyFixtures;
	void RegisterTerrainIndependentCopyTests(TestRegistry& registry) {
		registry.Add("Editor.TerrainIndependentCopy.DurableFreshIdentity", [](TestContext& context) {
			Fixture fixture;
			const auto originalResource = ResourceManager::Instance().GetRegisteredAssetPath(fixture.sourceId, AssetType::Terrain);
			const auto request = fixture.service->Begin(fixture.Capture());
			context.Expect(request.accepted && fixture.Await(request.operationId), "Independent durable preparation must settle.");
			fixture.Flush(); const auto destination = fixture.CopyPath(request.operationId);
			context.Expect(fixture.Status(request.operationId).state == Copy::TerrainCopyState::Committed, "Independent copy must own a committed rebind receipt.");
			const auto found = AssetDatabase::Instance().GetAssets().find(destination);
			context.Expect(found != AssetDatabase::Instance().GetAssets().end(), "Copy must have a real durable catalog registration.");
			if (found == AssetDatabase::Instance().GetAssets().end()) return;
			context.Expect(found->second.guid != fixture.sourceId && Open(destination)->CaptureRoot().root->assetUuid == found->second.guid,
				"Independent descriptor/root/catalog must own a fresh consistent identity.");
			context.Expect(ResourceManager::Instance().GetRegisteredAssetPath(fixture.sourceId, AssetType::Terrain) == originalResource,
				"Copy import must never redirect the original resource alias.");
			context.Expect(ResourceManager::Instance().GetRegisteredAssetPath(found->second.guid, AssetType::Terrain) == destination,
				"Copy resource alias must resolve only its private descriptor path.");
		});
		registry.Add("Editor.TerrainIndependentCopy.PreservesEveryAuthoredChannelAndLayer", [](TestContext& context) {
			Fixture fixture; const auto captured = fixture.Capture(); const auto submission = fixture.service->Begin(captured);
			context.Expect(submission.accepted && fixture.Await(submission.operationId), "Genuine durable terrain preparation must complete.");
			context.Expect(fixture.objects.GetComponent<TerrainComponent>(fixture.selected)->terrainAssetId == fixture.sourceId,
				"Selected binding must remain old before the protected transaction flush.");
			fixture.Flush(); const auto status = fixture.Status(submission.operationId);
			context.Expect(status.state == Copy::TerrainCopyState::Committed, "Only a committed scene receipt completes the copy.");
			const auto destination = fixture.CopyPath(submission.operationId); auto source = Open(fixture.source); auto copied = Open(destination);
			const auto original = source->CaptureRoot().root; const auto clone = copied->CaptureRoot().root;
			context.Expect(original->assetUuid != clone->assetUuid && clone->assetUuid == status.freshAssetId, "Both durable roots must have independent identity.");
			context.Expect(original->authoredBounds == clone->authoredBounds && original->sampleSpacingMetres == clone->sampleSpacingMetres
				&& original->defaultHeightMm == clone->defaultHeightMm && original->layerStackGeneration == clone->layerStackGeneration,
				"Copy must preserve authored bounds, spacing, defaults and layer-stack revision.");
			context.Expect(original->layerStack->layers.size() == clone->layerStack->layers.size(), "Complete layer stack must survive.");
			for (std::size_t index = 0U; index < original->layerStack->layers.size(); ++index) {
				const auto& before = original->layerStack->layers[index]; const auto& after = clone->layerStack->layers[index];
				context.Expect(before.stableId == after.stableId && before.name == after.name && before.materialAssetUuid == after.materialAssetUuid
					&& before.mapping == after.mapping && before.enabled == after.enabled && before.allowFallback == after.allowFallback,
					"Layer IDs, ordering, mapping, material references and enabled/fallback metadata must remain authored values.");
			}
			const auto before = original->directoryShards.front().descriptors.begin()->second;
			const auto after = clone->directoryShards.front().descriptors.begin()->second;
			const auto compare = [&](Authored::ContentHash oldHash, Authored::ContentHash newHash) {
				Authored::DecodedTerrainArea oldArea, newArea;
				const auto oldBytes = source->Blobs().Resolve(oldHash); const auto newBytes = copied->Blobs().Resolve(newHash);
				context.Expect(oldBytes && newBytes && Authored::DecodeTerrainArea(*oldBytes, oldArea).Succeeded() && Authored::DecodeTerrainArea(*newBytes, newArea).Succeeded(), "Both complete channel envelopes must decode.");
				context.Expect(oldArea.canonicalPayload == newArea.canonicalPayload && oldArea.header.area == newArea.header.area
					&& oldArea.header.channelKind == newArea.header.channelKind && oldArea.header.weightGroup == newArea.header.weightGroup,
					"All authored height/hole/weight bytes and channel coordinates must be preserved.");
				context.Expect(oldHash != newHash && newArea.header.assetUuid == status.freshAssetId, "Every identity-bearing envelope must be reencoded for the new asset.");
			};
			compare(*before.channels.heightHash, *after.channels.heightHash); compare(*before.channels.holeHash, *after.channels.holeHash);
			for (const auto& [group, hash] : before.channels.weightGroupHashes) compare(hash, after.channels.weightGroupHashes.at(group));
			context.Expect(nlohmann::json::parse(Read(destination))["preservedCustomMetadata"] == nlohmann::json::parse(captured.descriptorBytes)["preservedCustomMetadata"], "Unrelated descriptor metadata must survive verbatim values.");
			context.Expect(fixture.objects.GetComponent<TerrainComponent>(fixture.other)->terrainAssetId == fixture.sourceId
				&& fixture.objects.GetComponent<TerrainComponent>(fixture.selected)->terrainAssetId == status.freshAssetId, "Only the selected instance may rebind at commit.");
		});
		registry.Add("Editor.TerrainIndependentCopy.TwoCopiesRemainIndependentAfterSaveAndReopen", [](TestContext& context) {
			Fixture fixture; std::array<Copy::TerrainCopyStatus, 2U> copies; std::array<std::filesystem::path, 2U> paths;
			for (unsigned index = 0U; index < 2U; ++index) {
				TerraEngine::Scene::TransactionalMutationBuilder reset("copy-source-reset-" + std::to_string(index), TerraEngine::Scene::TransactionOrigin::Editor, "fixture");
				reset.UpdateComponentProperty(fixture.objects.getUUID(fixture.selected), "TerrainComponent", "terrainAssetId", TerraEngine::Reflection::Variant{fixture.sourceId});
				(void)TerraEngine::Scene::CommitTransaction(fixture.objects, reset.Build());
				const auto request = fixture.service->Begin(fixture.Capture()); context.Expect(request.accepted && fixture.Await(request.operationId), "Each requested independent copy must prepare.");
				fixture.Flush(); copies[index] = fixture.Status(request.operationId); paths[index] = fixture.CopyPath(request.operationId);
			}
			context.Expect(copies[0].freshAssetId != copies[1].freshAssetId && copies[0].freshAssetId != fixture.sourceId && copies[1].freshAssetId != fixture.sourceId
				&& paths[0] != paths[1], "Original and two copies require three durable IDs and private paths.");
			const auto originalHash = Open(fixture.source)->CaptureRoot().root->rootContentHash;
			const auto otherHash = Open(paths[1])->CaptureRoot().root->rootContentHash;
			auto edited = Open(paths[0]); const auto base = edited->CaptureRoot().root;
			auto area = base->directoryShards.front().descriptors.begin()->second;
			Authored::DecodedTerrainArea decoded; (void)Authored::DecodeTerrainArea(*edited->Blobs().Resolve(*area.channels.heightHash), decoded);
			decoded.canonicalPayload[0] = 19U; Authored::EncodedTerrainArea encoded;
			context.Expect(Authored::EncodeTerrainArea(decoded.header, decoded.canonicalPayload, encoded).Succeeded(), "Copied height edit must cook independently.");
			area.channels.heightHash = area.authoredContentHash = encoded.header.envelopeHash;
			context.Expect(edited->Blobs().PutVerified(encoded.header.envelopeHash, std::move(encoded.bytes)), "Edited copy owns its new channel blob.");
			std::shared_ptr<const Authored::TerrainAssetRoot> candidate;
			context.Expect(edited->BuildCandidateRoot(*base, {area}, {}, candidate).Succeeded() && edited->PublishCandidate(base, candidate).Succeeded()
				&& edited->PersistAtomically({paths[0].string() + ".data"}).Succeeded(), "Independent copy edit must persist atomically.");
			context.Expect(Open(fixture.source)->CaptureRoot().root->rootContentHash == originalHash && Open(paths[1])->CaptureRoot().root->rootContentHash == otherHash,
				"Editing one copy must leave the original and other copy unchanged.");
			AssetDatabase::Instance().Shutdown(); AssetDatabase::Instance().Initialize(fixture.project); AssetDatabase::Instance().PauseWatcherForTests();
			for (const auto& path : {fixture.source, paths[0], paths[1]}) {
				AssetDatabase::Instance().ImportAsset(path, true); const auto id = Open(path)->CaptureRoot().root->assetUuid;
				context.Expect(AssetDatabase::Instance().FindAssetByGuid(id) != nullptr && ResourceManager::Instance().GetRegisteredAssetPath(id, AssetType::Terrain) == path,
					"Save/reimport/reopen must retain each durable catalog/resource identity and path.");
			}
		});
		registry.Add("Editor.TerrainIndependentCopy.CapturesRealSavedCurrentPointer", [](TestContext& context) {
			Fixture fixture; const auto pointer = std::filesystem::path(fixture.source.string() + ".data/CURRENT");
			const auto initial = Open(fixture.source)->CaptureRoot().root->rootContentHash;
			context.Expect(Read(pointer) == Authored::ToHexString(initial) + "\n-\n"
				&& Copy::ReadTerrainCopyRootHash(fixture.source) == initial,
				"Owner capture must accept the real initial durable pointer with no predecessor.");
			auto store = Open(fixture.source); const auto base = store->CaptureRoot().root;
			std::shared_ptr<const Authored::TerrainAssetRoot> candidate;
			context.Expect(store->BuildCandidateRoot(*base, {}, {}, candidate).Succeeded() && store->PublishCandidate(base, candidate).Succeeded()
				&& store->PersistAtomically({fixture.source.string() + ".data"}).Succeeded(), "Second genuine store save must publish a durable predecessor.");
			const auto current = store->CapturePersistedRootState().savedRootHash; const auto saved = Read(pointer);
			context.Expect(saved.size() == 66U && saved == Authored::ToHexString(current) + "\n" + Authored::ToHexString(initial) + "\n"
				&& Copy::ReadTerrainCopyRootHash(fixture.source) == current && current != initial,
				"Owner capture must select the real current hash from a two-hash saved pointer.");
			std::string crlf; for (const char byte : saved) { if (byte == '\n') crlf += '\r'; crlf += byte; }
			{ std::ofstream output(pointer, std::ios::binary | std::ios::trunc); output << crlf; }
			context.Expect(crlf.size() == 68U && Copy::ReadTerrainCopyRootHash(fixture.source) == current,
				"Bounded owner capture must also accept canonical two-line CRLF pointers.");
			std::ofstream(pointer, std::ios::binary | std::ios::trunc) << saved;
		});
		registry.Add("Editor.TerrainIndependentCopy.RejectsMalformedCurrentPointers", [](TestContext& context) {
			Fixture fixture; const auto pointer = std::filesystem::path(fixture.source.string() + ".data/CURRENT");
			const auto saved = Read(pointer); const auto hash = Authored::ToHexString(Open(fixture.source)->CaptureRoot().root->rootContentHash);
			for (const auto& malformed : std::vector<std::string>{hash + "\n", hash + "\ninvalid\n", hash + "\n-\ntrailing",
				hash + "\n-\n\n", hash + "\n-", std::string(32U, 'A') + "\n-\n", std::string(32U, '0') + "\n-\n",
				hash + "\n" + std::string(33U, 'a') + "\n", hash + "\r\r\n-\n"}) {
				{ std::ofstream output(pointer, std::ios::binary | std::ios::trunc); output << malformed; }
				bool rejected = false; try { (void)Copy::ReadTerrainCopyRootHash(fixture.source); } catch (const std::runtime_error&) { rejected = true; }
				context.Expect(rejected, "Owner capture must reject malformed, ambiguous or oversized durable pointers.");
			}
			std::ofstream(pointer, std::ios::binary | std::ios::trunc) << saved;
		});
		registry.Add("Editor.TerrainIndependentCopy.SchedulerDeclaresProtectedOwnership", [](TestContext& context) {
			const auto declaration = TerraEngine::Editor::MakeEditorSchedulerDeclaration();
			// Query the actual production contract, independent of source placement.
			context.Expect(declaration.phase == "PostFrame" && declaration.phaseKind == TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit,
				"Terrain publication must remain at the protected editor boundary.");
			for (const auto* name : {"TerrainCopyWorkerResults", "TerrainDurableRootMetadata", "EditorTransactionReceipts"})
				context.Expect(std::find(declaration.reads.begin(), declaration.reads.end(), name) != declaration.reads.end(), "Editor must read terrain copy preparation and receipts.");
			for (const auto* name : {"TerrainCopyOperationState", "TerrainCopyAssetRegistration", "TerrainCopyPrivateOutputs", "SceneMutationQueue"})
				context.Expect(std::find(declaration.writes.begin(), declaration.writes.end(), name) != declaration.writes.end(), "Editor must declare protected terrain copy ownership writes.");
			context.Expect(declaration.protectedCommit.find("sole") != std::string::npos && declaration.safeSubwork.find("staging") != std::string::npos,
				"Scheduler declaration must explain worker-owned staging and sole receipt settlement.");
			const auto owner = Read(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.cpp");
			context.Expect(owner.find("DrainCompletedEditorTransactions") != std::string::npos, "Existing sole receipt consumer remains the editor settlement path.");
			context.Expect(owner.find("DrainTransactions(") == std::string::npos && owner.find("FlushSceneMutations(") == std::string::npos,
				"Terrain copy owner must queue its rebind and preserve the existing protected flush boundary.");
		});
	}
}

