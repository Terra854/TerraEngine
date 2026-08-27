// TerrainEditLifecycleTests.cpp -- Cancellation and root-identity dirty checks.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Terrain/TerrainAuthoringSystem.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainDirtyState.h"

#include "Core/Scheduling/JobGraph.h"
#include "Terrain/TerrainPublication.h"
#include "Terrain/TerrainGarbageCollector.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::string ReadTerrainLifecycleSource(const std::filesystem::path& relativePath)
		{
			// Lifecycle source checks describe the missing invalidation routing without
			// requiring a live renderer or ImGui context in the editor unit-test binary.
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath, std::ios::binary);
			std::ostringstream contents;
			contents << input.rdbuf();
			return contents.str();
		}

		struct LifecycleFixture
		{
			LifecycleFixture()
			{
				const std::vector<std::uint8_t> bytes{0x12U, 0x34U};
				const auto hash = TerraEngine::Terrain::HashTerrainBytes(bytes);
				auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>();
				root->assetUuid = TerraEngine::GenerateUUID(); root->publicationGeneration = 1U; root->rootContentHash = {21U, 22U}; root->authoredBounds = {0, 0, 256, 256}; root->layerStackGeneration = 1U;
				TerraEngine::Terrain::TerrainLayerStack layers;
				layers.generation = 1U;
				layers.layers = {{1U, "Ground"}};
				root->defaultLayerId = 1U;
				root->layerStack = std::make_shared<const TerraEngine::Terrain::TerrainLayerStack>(layers);
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

			bool QueueMappingEdit(std::string& outError)
			{
				const auto capture = store->CaptureRoot();
				TerraEngine::Terrain::TerrainLayer replacement = capture.root->layerStack->layers.front();
				replacement.mapping.tileSizeXMetres = 8.0;
				replacement.mapping.rotationRadians = 0.5;
				TerraEngine::Terrain::TerrainLayerPatch forward;
				forward.kind = TerraEngine::Terrain::TerrainLayerPatchKind::Replace;
				forward.expectedGeneration = capture.root->layerStackGeneration;
				forward.targetStableId = replacement.stableId;
				forward.layer = replacement;
				TerraEngine::Terrain::TerrainLayerPatch inverse = forward;
				inverse.expectedGeneration = 0U;
				inverse.layer = capture.root->layerStack->layers.front();

				TerraEngine::Terrain::TerrainEditTransaction transaction;
				transaction.transactionId = TerraEngine::GenerateUUID();
				transaction.origin = TerraEngine::Terrain::TerrainEditOrigin::VisibleEditor;
				transaction.targetAssetUuid = capture.root->assetUuid;
				transaction.historyGroup = "Terrain mapping edit";
				transaction.inputRootHash = capture.root->rootContentHash;
				transaction.expectedLayerStackGeneration = capture.root->layerStackGeneration;
				transaction.affectedBounds = capture.root->authoredBounds;
				transaction.layerStackPatch = forward;
				transaction.inverseLayerStackPatch = inverse;
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
		registry.Add("Editor.TerrainLifecycle.PredeclaredPublishNoOpsAfterProjectTeardown", [](TestContext& context) {
			// Reproduce the frame ordering that caused the live-editor exception: the
			// scheduler owns a callback declared while the terrain session is active,
			// then EditorSystemUpdate closes the project before that callback runs.
			auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>();
			root->assetUuid = TerraEngine::GenerateUUID();
			root->publicationGeneration = 1U;
			root->rootContentHash = {31U, 32U};
			auto store = std::make_unique<TerraEngine::Terrain::TerrainAssetStore>(root);
			auto queue = std::make_unique<TerraEngine::Terrain::TerrainMutationQueue>();
			auto publication = std::make_unique<TerraEngine::Terrain::TerrainPublication>(*store);
			TerraEngine::Editor::Terrain::TerrainAuthoringSystem authoring;
			authoring.Configure(*queue, *publication);
			authoring.Initialize();

			auto workloads = authoring.DeclareFrameWorkloads(0.0F);
			const auto publish = std::find_if(workloads.begin(), workloads.end(), [](const auto& workload) {
				return workload.stableDescriptorId == "TerrainAuthoringSystem/PostFrame/TerrainAuthoringPublish";
			});
			context.Expect(publish != workloads.end() && publish->active && static_cast<bool>(publish->work),
				"An active terrain session must declare its protected publication callback.");
			if(publish == workloads.end() || !publish->work) return;

			// The owner must be free to destroy both services after Shutdown returns.
			// Invoking the stale declaration afterward must only revalidate and return.
			authoring.Shutdown();
			publication.reset();
			queue.reset();
			TerraEngine::Scheduling::JobGraphContext jobContext;
			publish->work(jobContext);
			std::string error;
			context.Expect(!authoring.EnqueueDetachedCommit({}, error) && !error.empty(),
				"Project teardown must leave terrain authoring detached and unable to admit new work.");
		});

		registry.Add("Editor.TerrainLifecycle.WindowDetachesAuthoringBeforeOwnedServices", [](TestContext& context) {
			// TerrainWindow needs the same ownership order as the isolated regression:
			// detach the consumer before releasing the services it references.
			const std::string window = ReadTerrainLifecycleSource(
				"Editor/TerraEngine-Editor/Windows/TerrainWindow.cpp");
			const std::size_t resetStart = window.find("void TerrainWindow::ResetSceneTerrainState");
			const std::size_t resetEnd = window.find(
				"TerrainAutomationCommandResult TerrainWindow::ExecuteAutomationCommand", resetStart);
			const std::string reset = resetStart != std::string::npos && resetEnd != std::string::npos
				? window.substr(resetStart, resetEnd - resetStart) : std::string{};
			const std::size_t shutdown = reset.find("authoringSystem.Shutdown()");
			const std::size_t queueReset = reset.find("terrainQueue.reset()");
			const std::size_t publicationReset = reset.find("terrainPublication.reset()");
			context.Expect(shutdown != std::string::npos && queueReset != std::string::npos
				&& publicationReset != std::string::npos && shutdown < queueReset
				&& shutdown < publicationReset,
				"TerrainWindow must detach authoring before destroying its queue and publication services.");
		});

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

		registry.Add("Editor.TerrainLifecycle.MappingPublicationReturnsUndoReceiptWithin100Milliseconds", [](TestContext& context) {
			LifecycleFixture fixture;
			std::string error;
			const bool queued = fixture.QueueMappingEdit(error);
			const auto started = std::chrono::steady_clock::now();
			fixture.authoring.PublishAtProtectedBoundary();
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started);
			const auto receipts = fixture.authoring.DrainReceipts();
			const auto published = fixture.store->CaptureRoot();
			context.Expect(queued && receipts.size() == 1U
				&& receipts.front().outcome == TerraEngine::Terrain::TerrainEditOutcome::Committed
				&& receipts.front().historyPayload
				&& receipts.front().historyPayload->forwardLayerStackPatch
				&& receipts.front().historyPayload->inverseLayerStackPatch,
				"A mapping Apply must produce one committed receipt carrying the exact undo pair.");
			context.Expect(elapsed < std::chrono::milliseconds(100)
				&& published.root->layerStack->layers.front().mapping.tileSizeXMetres == 8.0,
				"The protected mapping publication must become authoritative within the 100 ms editor target.");
		});

		registry.Add("Editor.TerrainLifecycle.MaterialOnlyReceiptNeverSchedulesGeometryRebuild", [](TestContext& context) {
			const std::string transaction = ReadTerrainLifecycleSource(
				"Engine/Terrain/TerrainEditTransaction.h");
			const std::string window = ReadTerrainLifecycleSource(
				"Editor/TerraEngine-Editor/Windows/TerrainWindow.cpp");
			const std::size_t boundaryStart = window.find("void TerrainWindow::ProcessProtectedAuthoringBoundary");
			const std::size_t workloadStart = window.find(
				"std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration>", boundaryStart);
			const std::string boundary = boundaryStart != std::string::npos && workloadStart != std::string::npos
				? window.substr(boundaryStart, workloadStart - boundaryStart) : std::string{};

			context.Expect(transaction.find("GeometryAndCollision") != std::string::npos
				&& transaction.find("ControlTextures") != std::string::npos
				&& transaction.find("MaterialBindings") != std::string::npos,
				"Terrain receipts must classify derived invalidation so material mapping edits cannot masquerade as geometry edits.");
			context.Expect(!boundary.empty()
				&& boundary.find("receipt.invalidation") != std::string::npos
				&& boundary.find("MaterialBindings") != std::string::npos
				&& boundary.find("GeometryAndCollision") != std::string::npos
				&& boundary.find("if(committedRenderableChange)") == std::string::npos,
				"The editor must route a material-only receipt to material publication and reserve ScheduleRenderProxyBuild for geometry invalidation.");
		});
	}
}
