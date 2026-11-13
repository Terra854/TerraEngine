// TerrainPersistenceRoundTripTests.cpp -- Real descriptor and fresh-root persistence regressions.
#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainCreationService.h"
#include "Terrain/TerrainAssetStore.h"
#include "Editor/TerraEngine-Editor/Windows/TerrainWindow.h"
#include "Physics/PhysicsSystem.h"
#include "ThreadPool/ThreadPool.h"

#include <chrono>
#include <filesystem>
#include <future>

// Supply only fixture inputs; all cancellation, asset switching and collision
// capture below execute production window methods and the real worker cook.
struct TerrainCollisionPreparationReviewAccess
{
	static void SeedPending(TerrainWindow& window, std::future<TerraEngine::Physics::Terrain::TerrainCollisionCandidate> future) {
		window.automaticCollisionFuture = std::move(future);
		window.automaticCollisionStarted = std::chrono::steady_clock::now();
	}
	static bool HasPending(const TerrainWindow& window) { return window.automaticCollisionFuture.valid() || window.automaticCollisionReady.has_value(); }
	static void CancelThroughAutomation(TerrainWindow& window) { (void)window.ExecuteAutomationCommand("terrain.collision-cancel", nlohmann::json::object()); }
	static void Initialize(TerrainWindow& window, TerraEngine::UUID asset, const std::filesystem::path& destination = {}) {
		window.destinationPath = destination;
		TerraEngine::AssetPipeline::TerrainHeightfield heightfield;
		heightfield.width = 512; heightfield.height = 256; heightfield.heightsMm.assign(512u * 256u, 0);
		window.InitializeAuthoringTerrain(asset, heightfield);
	}
	static TerraEngine::Physics::Terrain::TerrainCollisionCandidate TakeCook(TerrainWindow& window) {
		if(!window.automaticCollisionFuture.valid()
			|| window.automaticCollisionFuture.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
			throw TerraEngine::EditorTests::TestFailure("The real automatic collision cook did not finish.");
		return window.automaticCollisionFuture.get();
	}
};

namespace TerraEngine::EditorTests
{
	namespace
	{
		// Each case owns its files even when a fatal prerequisite throws.
		struct PersistenceWorkspace
		{
			std::filesystem::path path = std::filesystem::temp_directory_path()
				/ ("TerrainPersistenceReview-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			~PersistenceWorkspace() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
		};
	}

	void RegisterTerrainPersistenceRoundTripTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainPersistence.CancelRevokesPendingAutomaticCollision", [](TestContext& context) {
			TerrainWindow window;
			std::promise<TerraEngine::Physics::Terrain::TerrainCollisionCandidate> promise;
			TerrainCollisionPreparationReviewAccess::SeedPending(window, promise.get_future());
			TerrainCollisionPreparationReviewAccess::CancelThroughAutomation(window);
			promise.set_value({}); // A late detached completion cannot regain an observation slot.
			context.Expect(!TerrainCollisionPreparationReviewAccess::HasPending(window),
				"Successful automation cancellation must revoke pending worker observation as well as staged physics work.");
		});
		registry.Add("Editor.TerrainPersistence.AssetSwitchRevokesPreviousCollisionCook", [](TestContext& context) {
			auto& pool = ThreadPool::instance(); const bool ownsPool = pool.getThreadCount() == 0;
			if(ownsPool) pool.initialize(2);
			struct PoolScope { bool owns; ~PoolScope() { if(owns) ThreadPool::instance().shutdown(); } } poolScope{ownsPool};
			TerrainWindow window;
			std::promise<TerraEngine::Physics::Terrain::TerrainCollisionCandidate> previous;
			TerrainCollisionPreparationReviewAccess::SeedPending(window, previous.get_future());
			TerrainCollisionPreparationReviewAccess::Initialize(window, TerraEngine::GenerateUUID());
			context.Expect(!TerrainCollisionPreparationReviewAccess::HasPending(window),
				"Inactive-world asset initialization must revoke the previous unresolved cook without waiting for its deadline.");
			previous.set_value({});
		});
		registry.Add("Editor.TerrainPersistence.InitialCollisionUsesRehydratedPersistedAreas", [](TestContext& context) {
			using namespace TerraEngine::Terrain;
			PersistenceWorkspace workspace;
			auto root = std::make_shared<TerrainAssetRoot>(); root->assetUuid = TerraEngine::GenerateUUID();
			root->publicationGeneration = 1; root->rootContentHash = {81, 82}; root->authoredBounds = {0,0,256,256};
			root->sampleSpacingMetres = 1.0; root->defaultHeightMm = 7000; root->defaultLayerId = 1; root->layerStackGeneration = 1;
			TerrainAssetStore persisted(root); std::shared_ptr<const TerrainAssetRoot> resized;
			context.Expect(persisted.BuildResizeCandidate(*root, {0,0,512,256}, {}, {}, resized).Succeeded()
				&& persisted.PublishCandidate(root, resized).Succeeded(), "Build a real multi-area persisted root with nonzero implicit heights.");
			const auto destination = workspace.path / "Persisted.teterrain";
			context.Expect(persisted.PersistAtomically({destination.string() + ".data"}).Succeeded(), "Persist the complete multi-area fixture.");
			auto& pool = ThreadPool::instance(); const bool ownsPool = pool.getThreadCount() == 0;
			if(ownsPool) pool.initialize(2);
			auto& physics = TerraEngine::Physics::PhysicsSystem::Instance();
			const auto previousSettings = physics.ConfiguredSettings(); physics.SetEditorLifecycleControlled(true); physics.Initialize();
			struct RuntimeScope { bool owns; TerraEngine::Physics::PhysicsSettings settings; ~RuntimeScope() {
				auto& p = TerraEngine::Physics::PhysicsSystem::Instance(); (void)p.StopWorld();
				(void)p.StartWorld(settings); (void)p.StopWorld(); p.Shutdown(); p.SetEditorLifecycleControlled(false);
				if(owns) ThreadPool::instance().shutdown();
			} } runtimeScope{ownsPool, previousSettings};
			context.Expect(physics.StartWorld(TerraEngine::Physics::PhysicsSettings::Defaults()), "Opening while playing needs a real active physics world.");
			TerrainWindow window;
			TerrainCollisionPreparationReviewAccess::Initialize(window, root->assetUuid, destination);
			const auto cooked = TerrainCollisionPreparationReviewAccess::TakeCook(window);
			context.Expect(cooked.complete && !cooked.recipe.heightMillimetres.empty()
				&& cooked.recipe.heightMillimetres.front() == 7000 && cooked.recipe.heightMillimetres.back() == 7000,
				"The first cook must use persisted selected-area and neighbor-seam samples, never the supplied stale flat overview.");
		});
		registry.Add("Editor.TerrainPersistence.FractionalDescriptorReopensExactRoot", [](TestContext& context) {
			// These float inputs promote to doubles that require more than six digits.
			// Comparing the persisted root to the promoted input catches decimal loss.
			for(const float spacing : {0.123456789F, 1.0F / 3.0F})
			{
				PersistenceWorkspace workspace;
				const auto destination = workspace.path / "Fractional.teterrain";
				TerraEngine::AssetPipeline::TerrainImportRequest request;
				request.assetId = TerraEngine::GenerateUUID();
				request.sampleSpacingMetres = spacing;
				TerraEngine::Editor::Terrain::TerrainCreationService service;
				const auto created = service.CreateOrImport(request, destination);
				context.Expect(created.succeeded, "Fractional terrain must create before exercising its production reopen path.");
				const auto reopened = service.OpenExisting(destination);
				context.Expect(reopened.succeeded && reopened.store != nullptr,
					"Fractional descriptor spacing must exactly match the persisted root on reopen.");
				const auto capture = reopened.store->CaptureRoot();
				context.Expect(capture.IsValid() && capture.root->assetUuid == request.assetId
					&& capture.root->sampleSpacingMetres == static_cast<double>(spacing)
					&& reopened.sampleSpacingMetres == spacing
					&& reopened.heightfield.width == created.heightfield.width
					&& reopened.heightfield.height == created.heightfield.height
					&& reopened.heightfield.heightsMm == created.heightfield.heightsMm,
					"Creation and reopen must preserve root identity, exact spacing, dimensions, and heights.");
			}
		});

		registry.Add("Editor.TerrainPersistence.FreshResizeEncodesPersistsAndReopens", [](TestContext& context) {
			using namespace TerraEngine::Terrain;
			PersistenceWorkspace workspace;
			auto root = std::make_shared<TerrainAssetRoot>();
			root->assetUuid = TerraEngine::GenerateUUID();
			root->publicationGeneration = 1u;
			root->rootContentHash = {1u, 2u};
			root->authoredBounds = {0, 0, 256, 256};
			root->sampleSpacingMetres = 1.0;
			root->defaultLayerId = 1u;
			root->layerStackGeneration = 1u;
			// A fresh in-memory root intentionally has no persisted generation yet.
			TerrainAssetStore store(root);
			std::shared_ptr<const TerrainAssetRoot> candidate;
			const auto prepared = store.BuildResizeCandidate(*root, {0, 0, 512, 256}, {}, {}, candidate);
			context.Expect(prepared.Succeeded() && candidate && candidate->persistedRootGeneration != 0u,
				"Resizing a fresh root must establish an encodable persistence identity.");
			context.Expect(root->persistedRootGeneration == 0u && store.CaptureRoot().root == root,
				"Candidate preparation must leave the observed root and live visibility unchanged.");
			const auto encoded = EncodeTerrainRootManifest(*candidate);
			context.Expect(encoded.Succeeded(), "The prepared resize must pass the production root encoder.");
			context.Expect(store.PublishCandidate(root, candidate).Succeeded(),
				"The isolated store's explicit publication boundary must adopt the complete resize.");
			TerrainPersistenceRequest request;
			request.dataDirectory = workspace.path / "Fresh.data";
			context.Expect(store.PersistAtomically(request).Succeeded(), "The resized root must persist a complete CURRENT generation.");
			TerrainAssetStore reopened(std::make_shared<TerrainAssetRoot>());
			context.Expect(reopened.OpenPersistedRoot(request.dataDirectory).Succeeded(),
				"The persisted fresh-root resize must reopen through the production reader.");
			const auto actual = reopened.CaptureRoot();
			context.Expect(actual.IsValid() && actual.root->assetUuid == root->assetUuid
				&& actual.root->authoredBounds == candidate->authoredBounds
				&& actual.root->persistedRootGeneration != 0u,
				"Reopen must retain resized coverage, asset identity, and valid persistence identity.");
		});
	}
}
