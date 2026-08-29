// TerrainAutomationParityTests.cpp -- Shared typed-boundary parity evidence.

#include "TestHarness.h"
#include "TerrainResizeTestFixtures.h"

#include "Editor/TerraEngine-Editor/Terrain/TerrainAutomationAdapter.h"
#include "Terrain/TerrainPublication.h"

#include <chrono>
#include <memory>
#include <thread>
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

		std::optional<TerraEngine::Editor::Terrain::TerrainPreparedResize> WaitForResizePreview(
			TerraEngine::Editor::Terrain::TerrainResizeJobs& jobs)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while(std::chrono::steady_clock::now() < deadline)
			{
				if(auto ready = jobs.TakeReadyPreview()) return ready;
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			return std::nullopt;
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

		registry.Add("Editor.TerrainAutomation.ResizePreviewAndPublishMatchVisibleEditor", [](TestContext& context) {
			using namespace TerraEngine;
			const auto root = TerraEngine::Tests::TerrainResizeFixtures::MakeRoot();
			auto visibleRequest = TerraEngine::Tests::TerrainResizeEditorFixtures::MakeExpansionRequest(
				root, {-256, 0, 512, 256});
			visibleRequest.delta.expansionPolicy = Terrain::TerrainExpansionPolicy::FlatDefault;
			visibleRequest.delta.holePolicy = Terrain::TerrainBoundaryHolePolicy::CloseAtFormerEdge;
			visibleRequest.holePolicyExplicit = true;
			visibleRequest.sourceHeight = [](Terrain::TerrainSampleCoordinate) {
				return std::optional<std::int32_t>{2400};
			};
			auto automatedRequest = visibleRequest;
			Editor::Terrain::TerrainResizeJobs visibleJobs;
			Editor::Terrain::TerrainResizeJobs automatedJobs;
			std::string visibleError;
			std::string automatedError;
			const bool visibleStarted = visibleJobs.Start(std::move(visibleRequest), visibleError);
			const bool automatedStarted = automatedJobs.Start(std::move(automatedRequest), automatedError);
			auto visiblePreview = WaitForResizePreview(visibleJobs);
			auto automatedPreview = WaitForResizePreview(automatedJobs);

			Terrain::TerrainAssetStore visibleStore(root);
			Terrain::TerrainAssetStore automatedStore(root);
			auto visibleTransaction = visiblePreview
				? visibleJobs.BuildTransaction(visibleStore, *visiblePreview, false, visibleError) : std::nullopt;
			auto automatedTransaction = automatedPreview
				? automatedJobs.BuildTransaction(automatedStore, *automatedPreview, false, automatedError) : std::nullopt;
			Terrain::TerrainPublication visiblePublication(visibleStore);
			Terrain::TerrainPublication automatedPublication(automatedStore);
			const auto visibleReceipt = visibleTransaction
				? visiblePublication.Publish(std::move(*visibleTransaction)) : Terrain::TerrainTransactionReceipt{};
			const auto automatedReceipt = automatedTransaction
				? automatedPublication.Publish(std::move(*automatedTransaction)) : Terrain::TerrainTransactionReceipt{};
			const auto visibleRoot = visibleStore.CaptureRoot();
			const auto automatedRoot = automatedStore.CaptureRoot();

			context.Expect(visibleStarted && automatedStarted && visiblePreview && automatedPreview
				&& visiblePreview->preview.proposalFingerprint == automatedPreview->preview.proposalFingerprint
				&& visiblePreview->preview.warnings == automatedPreview->preview.warnings
				&& visiblePreview->preview.discontinuityWarning
				&& visiblePreview->preview.delta.proposedBounds == automatedPreview->preview.delta.proposedBounds
				&& visibleReceipt.outcome == Terrain::TerrainEditOutcome::Committed
				&& automatedReceipt.outcome == Terrain::TerrainEditOutcome::Committed
				&& visibleReceipt.publishedRootHash == automatedReceipt.publishedRootHash
				&& visibleRoot.root->authoredBounds == automatedRoot.root->authoredBounds
				&& visibleRoot.root->rootContentHash == automatedRoot.root->rootContentHash,
				"Equivalent visible-editor and automation expansion inputs must return identical fingerprints, warnings, receipts, authored bounds, and hashes. visible='"
					+ visibleError + "' automated='" + automatedError + "'.");

			// Crop parity uses the same typed preview and confirmation values. The
			// proposal remains detached, and both producers must report the same loss.
			auto cropRoot = TerraEngine::Tests::TerrainResizeFixtures::MakeRoot({0, 0, 512, 256});
			Editor::Terrain::TerrainResizeJobRequest visibleCrop{};
			visibleCrop.observedRoot = {cropRoot, cropRoot->publicationGeneration};
			visibleCrop.delta.kind = Terrain::TerrainResizeKind::Crop;
			visibleCrop.delta.previousBounds = cropRoot->authoredBounds;
			visibleCrop.delta.proposedBounds = {0, 0, 128, 256};
			auto automatedCrop = visibleCrop;
			Editor::Terrain::TerrainResizeJobs visibleCropJobs;
			Editor::Terrain::TerrainResizeJobs automatedCropJobs;
			const bool visibleCropStarted = visibleCropJobs.Start(std::move(visibleCrop), visibleError);
			const bool automatedCropStarted = automatedCropJobs.Start(std::move(automatedCrop), automatedError);
			auto visibleCropPreview = WaitForResizePreview(visibleCropJobs);
			auto automatedCropPreview = WaitForResizePreview(automatedCropJobs);
			context.Expect(visibleCropStarted && automatedCropStarted && visibleCropPreview && automatedCropPreview
				&& visibleCropPreview->preview.requiresDataLossConfirmation
				&& visibleCropPreview->preview.proposalFingerprint == automatedCropPreview->preview.proposalFingerprint
				&& visibleCropPreview->preview.cropLoss.discardedSamples
					== automatedCropPreview->preview.cropLoss.discardedSamples
				&& visibleCropPreview->preview.warnings == automatedCropPreview->preview.warnings,
				"Equivalent visible-editor and automation crop inputs must return the same approval fingerprint, loss summary, warnings, and confirmation requirement.");
		});
	}
}
