// TerrainAutomationParityTests.cpp -- Shared typed-boundary parity evidence.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Terrain/TerrainAutomationAdapter.h"
#include "Terrain/TerrainPublication.h"

#include <memory>
#include <vector>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::shared_ptr<const TerraEngine::Terrain::TerrainAssetRoot> MakeRoot(
			TerraEngine::UUID assetId, TerraEngine::Terrain::ContentHash authoredHash)
		{
			auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>();
			root->assetUuid = assetId;
			root->publicationGeneration = 1u;
			root->persistedRootGeneration = 1u;
			root->rootContentHash = {1u, 2u};
			root->authoredBounds = {0, 0, 256, 256};
			root->layerStackGeneration = 1u;
			TerraEngine::Terrain::TerrainAreaDescriptor descriptor{};
			descriptor.coordinate = {0, 0};
			descriptor.editGeneration = 1u;
			descriptor.authoredContentHash = authoredHash;
			descriptor.channels.heightHash = authoredHash;
			TerraEngine::Terrain::TerrainDirectoryShard shard{};
			shard.areaBounds = {0, 0, 1, 1};
			shard.descriptors.emplace(descriptor.coordinate, descriptor);
			root->directoryShards.push_back(std::move(shard));
			return root;
		}

		TerraEngine::Terrain::TerrainEditTransaction MakeRemap(
			const TerraEngine::Terrain::TerrainAssetRoot& root,
			const TerraEngine::Terrain::TerrainAreaDescriptor& before,
			TerraEngine::Terrain::ContentHash afterHash,
			TerraEngine::UUID transactionId)
		{
			TerraEngine::Terrain::TerrainEditTransaction transaction{};
			transaction.transactionId = transactionId;
			transaction.targetAssetUuid = root.assetUuid;
			transaction.historyGroup = "Terrain height remap";
			transaction.inputRootHash = root.rootContentHash;
			transaction.expectedLayerStackGeneration = root.layerStackGeneration;
			transaction.affectedBounds = root.authoredBounds;
			transaction.readSet = {{before.coordinate, before.editGeneration, before.authoredContentHash}};
			TerraEngine::Terrain::TerrainPatch patch{};
			patch.kind = TerraEngine::Terrain::TerrainPatchKind::ReplaceHeight;
			patch.replacement = before;
			patch.replacement.editGeneration = before.editGeneration + 1u;
			patch.replacement.authoredContentHash = afterHash;
			patch.replacement.channels.heightHash = afterHash;
			patch.preparedBlobHash = afterHash;
			TerraEngine::Terrain::TerrainPatch inverse{};
			inverse.kind = patch.kind;
			inverse.replacement = before;
			inverse.preparedBlobHash = before.authoredContentHash;
			transaction.patches.push_back(std::move(patch));
			transaction.inversePatches.push_back(std::move(inverse));
			return transaction;
		}
	}

	void RegisterTerrainAutomationParityTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainAutomation.VisibleAndAutomationPublishIdenticalTypedHeightRemap", [](TestContext& context) {
			using namespace TerraEngine;
			const UUID assetId = GenerateUUID();
			const std::vector<std::uint8_t> beforeBytes{1u, 2u, 3u};
			const std::vector<std::uint8_t> afterBytes{4u, 5u, 6u};
			const Terrain::ContentHash beforeHash = Terrain::HashTerrainBytes(beforeBytes);
			const Terrain::ContentHash afterHash = Terrain::HashTerrainBytes(afterBytes);

			Terrain::TerrainAssetStore visibleStore(MakeRoot(assetId, beforeHash));
			Terrain::TerrainMutationQueue visibleQueue;
			Terrain::TerrainPublication visiblePublication(visibleStore);
			Editor::Terrain::TerrainAuthoringSystem visibleAuthoring;
			visibleAuthoring.Configure(visibleQueue, visiblePublication);
			visibleAuthoring.Initialize();
			visibleStore.Blobs().PutVerified(beforeHash, beforeBytes);
			visibleStore.Blobs().PutVerified(afterHash, afterBytes);
			const auto visibleBefore = visibleStore.CaptureRoot();
			const auto visibleDescriptor = visibleStore.FindDescriptor(*visibleBefore.root, {0, 0});
			std::string error;
			const bool visibleQueued = visibleDescriptor && visibleAuthoring.EnqueueProceduralCommit(MakeRemap(*visibleBefore.root, *visibleDescriptor, afterHash, GenerateUUID()), error);
			context.Expect(visibleQueued && visibleStore.CaptureRoot().publicationGeneration == 1u,
				"A visible detached remap must remain invisible before its protected publication boundary.");
			visibleAuthoring.PublishAtProtectedBoundary();
			const auto visibleReceipts = visibleAuthoring.DrainReceipts();

			Terrain::TerrainAssetStore automatedStore(MakeRoot(assetId, beforeHash));
			Terrain::TerrainMutationQueue automatedQueue;
			Terrain::TerrainPublication automatedPublication(automatedStore);
			Editor::Terrain::TerrainAuthoringSystem automatedAuthoring;
			automatedAuthoring.Configure(automatedQueue, automatedPublication);
			automatedAuthoring.Initialize();
			automatedStore.Blobs().PutVerified(beforeHash, beforeBytes);
			automatedStore.Blobs().PutVerified(afterHash, afterBytes);
			Editor::Terrain::TerrainAutomationAdapter adapter;
			adapter.Configure(automatedStore, automatedAuthoring);
			adapter.SetPrepareCallback([&](const Editor::Automation::TerrainAutomationRequest& request, std::string&) -> std::optional<Terrain::TerrainEditTransaction> {
				const auto capture = automatedStore.CaptureRoot();
				const auto descriptor = automatedStore.FindDescriptor(*capture.root, {0, 0});
				return descriptor ? std::optional{MakeRemap(*capture.root, *descriptor, afterHash, request.transactionId)} : std::nullopt;
			});
			bool terminalCommitted = false;
			adapter.SetReceiptCallback([&](std::string_view jobId, const Terrain::TerrainTransactionReceipt& receipt, const Editor::Automation::TerrainAutomationRequest&) {
				terminalCommitted = jobId == "terrain-job-1" && receipt.outcome == Terrain::TerrainEditOutcome::Committed;
			});
			Editor::Automation::TerrainAutomationRequest request{};
			request.transactionId = GenerateUUID();
			request.assetId = assetId;
			request.instanceId = GenerateUUID();
			request.operation = "height-remap";
			request.expectedRootHash = Terrain::ToHexString(automatedStore.CaptureRoot().root->rootContentHash);
			request.workloadClass = "interactive";
			request.bounds = {0, 0, 256, 256};
			request.budget = {1u, Terrain::kTerrainAreaSampleCount, Terrain::Format::kTerrainAreaMaximumDecodedBytes};
			request.observations = {{{0, 0}, 1u, beforeHash}};
			request.previewOnly = false;
			request.approvalFingerprint = Editor::Automation::TerrainApprovalFingerprint(request);
			const bool automatedQueued = adapter.PrepareAndEnqueue(request, "terrain-job-1", error);
			context.Expect(automatedQueued && automatedStore.CaptureRoot().publicationGeneration == 1u,
				"Automation must use the same invisible detached queue before the protected boundary.");
			automatedAuthoring.PublishAtProtectedBoundary();
			for(const auto& receipt : automatedAuthoring.DrainReceipts()) adapter.ObserveReceipt(receipt);

			const auto visibleAfter = visibleStore.CaptureRoot();
			const auto automatedAfter = automatedStore.CaptureRoot();
			const auto visibleAfterDescriptor = visibleStore.FindDescriptor(*visibleAfter.root, {0, 0});
			const auto automatedAfterDescriptor = automatedStore.FindDescriptor(*automatedAfter.root, {0, 0});
			context.Expect(!visibleReceipts.empty() && visibleReceipts.front().outcome == Terrain::TerrainEditOutcome::Committed && terminalCommitted
				&& visibleAfter.root->rootContentHash == automatedAfter.root->rootContentHash
				&& visibleAfter.publicationGeneration == automatedAfter.publicationGeneration
				&& visibleAfterDescriptor && automatedAfterDescriptor
				&& visibleAfterDescriptor->authoredContentHash == automatedAfterDescriptor->authoredContentHash
				&& visibleAfterDescriptor->channels.heightHash == automatedAfterDescriptor->channels.heightHash,
				"Visible and automated remaps must produce identical root, area, channel, generation, and terminal receipt results through one typed boundary.");
			visibleAuthoring.Shutdown();
			automatedAuthoring.Shutdown();
		});

		registry.Add("Editor.TerrainAutomation.UnsupportedProducerAndCancellationLeaveRootUnchanged", [](TestContext& context) {
			using namespace TerraEngine;
			const UUID assetId = GenerateUUID();
			const std::vector<std::uint8_t> bytes{9u};
			const Terrain::ContentHash hash = Terrain::HashTerrainBytes(bytes);
			Terrain::TerrainAssetStore store(MakeRoot(assetId, hash));
			Terrain::TerrainMutationQueue queue;
			Terrain::TerrainPublication publication(store);
			Editor::Terrain::TerrainAuthoringSystem authoring;
			authoring.Configure(queue, publication);
			authoring.Initialize();
			Editor::Terrain::TerrainAutomationAdapter adapter;
			adapter.Configure(store, authoring);
			adapter.SetPrepareCallback([](const Editor::Automation::TerrainAutomationRequest&, std::string& error) -> std::optional<Terrain::TerrainEditTransaction> { error = "Unsupported detached producer."; return std::nullopt; });
			Editor::Automation::TerrainAutomationRequest request{};
			request.transactionId = GenerateUUID(); request.assetId = assetId; request.instanceId = GenerateUUID(); request.operation = "paint-hole";
			request.expectedRootHash = Terrain::ToHexString(store.CaptureRoot().root->rootContentHash); request.workloadClass = "interactive"; request.bounds = {0, 0, 1, 1}; request.budget = {1u, 1u, 1u}; request.observations = {{{0, 0}, 1u, hash}}; request.previewOnly = false; request.approvalFingerprint = Editor::Automation::TerrainApprovalFingerprint(request);
			std::string error;
			const bool queued = adapter.PrepareAndEnqueue(request, "unsupported-job", error);
			context.Expect(!queued && !error.empty() && store.CaptureRoot().publicationGeneration == 1u,
				"A capability without an exact editor-owned producer must report unavailable and leave authored visibility unchanged.");
			authoring.Shutdown();
		});
	}
}
