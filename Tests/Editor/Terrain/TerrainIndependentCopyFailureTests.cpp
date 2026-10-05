// Owned preparation/publication failures preserve original terrain and bindings.
#include "TerrainIndependentCopyTestFixtures.h"
#include <condition_variable>
#include <mutex>

namespace TerraEngine::EditorTests {
	using namespace TerrainCopyFixtures;
	void RegisterTerrainIndependentCopyFailureTests(TestRegistry& registry) {
		for (const auto& [name, boundary] : std::array<std::pair<const char*, Copy::TerrainIndependentCopyService::Boundary>, 3U>{{
			{"Editor.TerrainIndependentCopyFailure.PreparationFailure", Copy::TerrainIndependentCopyService::Boundary::BeforePreparation},
			{"Editor.TerrainIndependentCopyFailure.PersistenceFailure", Copy::TerrainIndependentCopyService::Boundary::BeforePersistence},
			{"Editor.TerrainIndependentCopyFailure.EnqueueFailure", Copy::TerrainIndependentCopyService::Boundary::BeforeEnqueue}}}) {
			registry.Add(name, [boundary](TestContext& context) {
				Fixture fixture; const auto original = Read(fixture.source); const auto hash = Open(fixture.source)->CaptureRoot().root->rootContentHash;
				fixture.service->SetTestHook([boundary, &fixture](auto observed) {
					if (observed != boundary) return;
					if (boundary == Copy::TerrainIndependentCopyService::Boundary::BeforePersistence) {
						// Obstruct the actual atomic writer's data-directory creation. This
						// tests a storage failure rather than merely throwing before it.
						for (const auto& entry : std::filesystem::directory_iterator(fixture.project / ".terrain-copy-staging"))
							std::ofstream(entry.path() / "copy.teterrain.data") << "blocked directory";
						return;
					}
					throw std::runtime_error("Controlled terrain copy boundary failure.");
				});
				const auto request = fixture.service->Begin(fixture.Capture()); context.Expect(request.accepted && fixture.Await(request.operationId), "Controlled failed copy must settle its worker ownership.");
				const auto status = fixture.Status(request.operationId);
				context.Expect(status.state == Copy::TerrainCopyState::Failed && AssetDatabase::Instance().FindAssetByGuid(status.freshAssetId) == nullptr, "Failed copy must not retain a provisional registration.");
				context.Expect(Read(fixture.source) == original && Open(fixture.source)->CaptureRoot().root->rootContentHash == hash
					&& fixture.objects.GetComponent<TerrainComponent>(fixture.selected)->terrainAssetId == fixture.sourceId, "Preparation/persistence/enqueue failure must preserve original authored identity and binding.");
				context.Expect(!std::filesystem::exists(fixture.project / "Assets" / "Authored Copy 1.teterrain.data"), "Failed copy must remove owned private outputs.");
			});
		}
		registry.Add("Editor.TerrainIndependentCopyFailure.AdmissionMarkerRecovery", [](TestContext& context) {
			// Exercise missing, partial, and changed markers before any worker exists.
			for (unsigned mode = 0U; mode < 3U; ++mode) {
				Fixture fixture; std::filesystem::path stage;
				fixture.service->SetTestHook([&](auto boundary) {
					if (boundary != (mode == 0U ? Copy::TerrainIndependentCopyService::Boundary::AfterStageReservation
						: Copy::TerrainIndependentCopyService::Boundary::AfterOwnerMarkerCreation)) return;
					stage = std::filesystem::directory_iterator(fixture.project / ".terrain-copy-staging")->path();
					if (mode != 0U) std::ofstream(stage / "OWNER", std::ios::trunc) << (mode == 1U ? stage.filename().string().substr(0U, 8U) : "changed owner");
					throw std::runtime_error("Controlled partial terrain admission.");
				});
				const auto request = fixture.service->Begin(fixture.Capture());
				context.Expect(!request.accepted && request.operationId != TerraEngine::EMPTY_UUID && fixture.registrations == 0U,
					"Failed admission must return its tracked operation identity without launching or registering.");
				if (mode < 2U) {
					context.Expect(fixture.Status(request.operationId).state == Copy::TerrainCopyState::Failed && !std::filesystem::exists(stage),
						"Exclusively reserved empty/partial marker admission must clean without an invisible retry loop.");
				} else {
					context.Expect(fixture.Status(request.operationId).state == Copy::TerrainCopyState::CleanupPending && std::filesystem::exists(stage / "OWNER"),
						"A changed owner marker must remain untouched with a visible recovery obligation.");
					std::ofstream(stage / "OWNER", std::ios::trunc) << TerraEngine::ToHexString(request.operationId);
					fixture.service->RetryCleanup(fixture.epoch);
					context.Expect(fixture.Status(request.operationId).state == Copy::TerrainCopyState::Failed && !std::filesystem::exists(stage),
						"Restoring the same captured marker file permits explicit owner recovery.");
				}
			}
		});
		registry.Add("Editor.TerrainIndependentCopyFailure.ImportFailure", [](TestContext& context) {
			Fixture fixture; fixture.rejectRegistration = true; const auto request = fixture.service->Begin(fixture.Capture());
			context.Expect(request.accepted && fixture.Await(request.operationId), "Failed provisional import must settle.");
			context.Expect(fixture.Status(request.operationId).state == Copy::TerrainCopyState::Failed && fixture.rollbacks == 1U
				&& !std::filesystem::exists(fixture.project / "Assets" / "Authored Copy 1.teterrain"), "Import failure must conditionally unregister and remove only installed copy outputs.");
		});
		registry.Add("Editor.TerrainIndependentCopyFailure.SourceChangedBeforePublication", [](TestContext& context) {
			Fixture fixture; const auto capture = fixture.Capture(); fixture.rejectValidation = true;
			const auto request = fixture.service->Begin(capture); context.Expect(request.accepted && fixture.Await(request.operationId), "Stale source preparation must physically settle.");
			context.Expect(fixture.Status(request.operationId).state == Copy::TerrainCopyState::Failed && fixture.registrations == 0U
				&& fixture.objects.GetComponent<TerrainComponent>(fixture.selected)->terrainAssetId == fixture.sourceId, "Stale source observation must reject before any registration or rebind.");
		});
		registry.Add("Editor.TerrainIndependentCopyFailure.SceneConflictRollsBackCopy", [](TestContext& context) {
			Fixture fixture; const auto request = fixture.service->Begin(fixture.Capture()); context.Expect(request.accepted && fixture.Await(request.operationId), "Copy must reach pending rebind before conflict.");
			const auto path = fixture.CopyPath(request.operationId); fixture.objects.setObjectName(fixture.selected, "Changed after copy observation"); fixture.Flush();
			context.Expect(fixture.Status(request.operationId).state == Copy::TerrainCopyState::Failed && !std::filesystem::exists(path)
				&& !std::filesystem::exists(path.string() + ".data") && fixture.objects.GetComponent<TerrainComponent>(fixture.selected)->terrainAssetId == fixture.sourceId,
				"Rejected observed scene receipt must remove copy registration/private data without a partial binding.");
		});
		registry.Add("Editor.TerrainIndependentCopyFailure.CleanupRetryRemainsVisible", [](TestContext& context) {
			Fixture fixture; fixture.rejectRegistration = true; fixture.retryRollback = true;
			const auto request = fixture.service->Begin(fixture.Capture()); context.Expect(request.accepted && fixture.Await(request.operationId), "Copy cleanup failure must remain owned after worker settlement.");
			context.Expect(fixture.Status(request.operationId).state == Copy::TerrainCopyState::CleanupPending
				&& std::filesystem::exists(fixture.project / "Assets" / "Authored Copy 1.teterrain"), "Retry obligation must remain visible rather than claiming completed rollback.");
			fixture.retryRollback = false; fixture.service->RetryCleanup(fixture.epoch);
			context.Expect(fixture.Status(request.operationId).state == Copy::TerrainCopyState::Failed
				&& !std::filesystem::exists(fixture.project / "Assets" / "Authored Copy 1.teterrain.data"), "A later protected cleanup retry must settle owned private output removal.");
		});
		registry.Add("Editor.TerrainIndependentCopyFailure.OwnershipLostRefusesDeletion", [](TestContext& context) {
			Fixture fixture; const auto request = fixture.service->Begin(fixture.Capture()); context.Expect(request.accepted && fixture.Await(request.operationId), "Owned copy must reach registration.");
			const auto path = fixture.CopyPath(request.operationId); fixture.loseOwnership = true; fixture.service->Cancel(request.operationId);
			context.Expect(std::filesystem::exists(path) && std::filesystem::exists(path.string() + ".data")
				&& fixture.Status(request.operationId).state == Copy::TerrainCopyState::Failed, "Conditional ownership loss must refuse deletion of possibly rebound output.");
		});
		registry.Add("Editor.TerrainIndependentCopyFailure.PendingCancellationFencesRebind", [](TestContext& context) {
			Fixture fixture; const auto request = fixture.service->Begin(fixture.Capture()); context.Expect(request.accepted && fixture.Await(request.operationId), "Copy must await existing protected flush.");
			fixture.service->Cancel(request.operationId); fixture.Flush();
			context.Expect(fixture.Status(request.operationId).state == Copy::TerrainCopyState::Cancelled
				&& fixture.objects.GetComponent<TerrainComponent>(fixture.selected)->terrainAssetId == fixture.sourceId, "Canceled queued transaction must never rebind later after copy cleanup.");
		});
		registry.Add("Editor.TerrainIndependentCopyFailure.LateCancellationWaitsForCommittedReceipt", [](TestContext& context) {
			Fixture fixture; const auto request = fixture.service->Begin(fixture.Capture()); context.Expect(request.accepted && fixture.Await(request.operationId), "Copy must enqueue an observed rebind.");
			const auto path = fixture.CopyPath(request.operationId);
			(void)TerraEngine::Scene::SceneMutationQueue::Instance().DrainTransactions(fixture.objects);
			fixture.service->Cancel(request.operationId);
			context.Expect(fixture.Status(request.operationId).state == Copy::TerrainCopyState::AwaitingReceipt && std::filesystem::exists(path), "False queue cancellation must retain output while a committed receipt may exist.");
			for (const auto& completed : TerraEngine::Scene::SceneMutationQueue::Instance().DrainCompletedEditorTransactions()) (void)fixture.service->HandleTransactionResult(completed.result);
			context.Expect(fixture.Status(request.operationId).state == Copy::TerrainCopyState::Committed && std::filesystem::exists(path), "Known committed receipt must finalize retained copy despite late cancellation.");
		});
		registry.Add("Editor.TerrainIndependentCopyFailure.ProjectRetirementBeforePreparationSettles", [](TestContext& context) {
			Fixture fixture;
			struct Gate { std::mutex mutex; std::condition_variable changed; bool entered = false, released = false; };
			auto gate = std::make_shared<Gate>();
			struct Release { std::shared_ptr<Gate> gate; ~Release() { std::lock_guard lock(gate->mutex); gate->released = true; gate->changed.notify_all(); } } release{gate};
			fixture.service->SetTestHook([gate](auto boundary) { if (boundary == Copy::TerrainIndependentCopyService::Boundary::BeforePreparation) {
				std::unique_lock lock(gate->mutex); gate->entered = true; gate->changed.notify_all(); gate->changed.wait(lock, [&] { return gate->released; });
			} });
			const auto request = fixture.service->Begin(fixture.Capture());
			{ std::unique_lock lock(gate->mutex); context.Expect(gate->changed.wait_for(lock, std::chrono::seconds(10), [&] { return gate->entered; }), "Worker must hold only immutable old-project preparation."); }
			fixture.service->RetireProject(fixture.epoch); ++fixture.epoch;
			{ std::lock_guard lock(gate->mutex); gate->released = true; gate->changed.notify_all(); }
			context.Expect(fixture.Await(request.operationId) && fixture.Status(request.operationId).state == Copy::TerrainCopyState::Cancelled && fixture.registrations == 0U,
				"Retired old-project job must physically settle without registering into the next epoch.");
		});
		registry.Add("Editor.TerrainIndependentCopyFailure.ParallelReservationsStayDistinct", [](TestContext& context) {
			Fixture fixture; const auto first = fixture.service->Begin(fixture.Capture()); auto secondCapture = fixture.Capture();
			TerraEngine::Scene::TransactionalMutationBuilder observe("other-terrain-observation", TerraEngine::Scene::TransactionOrigin::Editor, "fixture");
			observe.ObserveScene(fixture.objects).ObserveObject(fixture.objects, fixture.objects.getUUID(fixture.other));
			secondCapture.selectedObjectId = fixture.objects.getUUID(fixture.other); secondCapture.observedReadSet = observe.Build().readSet;
			const auto second = fixture.service->Begin(secondCapture);
			context.Expect(first.accepted && second.accepted && fixture.Await(first.operationId) && fixture.Await(second.operationId), "Independent parallel preparations must finish without frame waits.");
			context.Expect(fixture.Status(first.operationId).freshAssetId != fixture.Status(second.operationId).freshAssetId
				&& fixture.CopyPath(first.operationId) != fixture.CopyPath(second.operationId), "Reservations must claim unique UUIDs and destination paths before workers start.");
			fixture.Flush(); context.Expect(fixture.Status(first.operationId).state == Copy::TerrainCopyState::Committed && fixture.Status(second.operationId).state == Copy::TerrainCopyState::Committed,
				"Observed independent selected instances must commit their separate copy bindings.");
		});
		registry.Add("Editor.TerrainIndependentCopyFailure.UndoRedoRetainsCompletedAsset", [](TestContext& context) {
			Fixture fixture; const auto request = fixture.service->Begin(fixture.Capture()); context.Expect(request.accepted && fixture.Await(request.operationId), "Undo fixture copy must prepare.");
			(void)TerraEngine::Scene::SceneMutationQueue::Instance().DrainTransactions(fixture.objects);
			auto receipts = TerraEngine::Scene::SceneMutationQueue::Instance().DrainCompletedEditorTransactions();
			context.Expect(receipts.size() == 1U && receipts.front().result.inverseDeltas.size() == 1U, "Sole editor receipt must retain the normal typed inverse for undo.");
			if (receipts.size() != 1U || receipts.front().result.inverseDeltas.empty()) return;
			(void)fixture.service->HandleTransactionResult(receipts.front().result); const auto path = fixture.CopyPath(request.operationId);
			TerraEngine::Scene::Transaction undo; undo.id = "copy-undo"; undo.origin = TerraEngine::Scene::TransactionOrigin::Editor;
			undo.writeSet.operations.push_back(receipts.front().result.inverseDeltas.front().operation);
			context.Expect(TerraEngine::Scene::CommitTransaction(fixture.objects, undo).status == TerraEngine::Scene::TransactionStatus::Committed
				&& fixture.objects.GetComponent<TerrainComponent>(fixture.selected)->terrainAssetId == fixture.sourceId && std::filesystem::exists(path), "Undo must restore binding while retaining the completed independent asset.");
			TerraEngine::Scene::TransactionalMutationBuilder redo("copy-redo", TerraEngine::Scene::TransactionOrigin::Editor, "fixture");
			redo.UpdateComponentProperty(fixture.objects.getUUID(fixture.selected), "TerrainComponent", "terrainAssetId", TerraEngine::Reflection::Variant{fixture.Status(request.operationId).freshAssetId});
			context.Expect(TerraEngine::Scene::CommitTransaction(fixture.objects, redo.Build()).status == TerraEngine::Scene::TransactionStatus::Committed && std::filesystem::exists(path), "Redo must reuse the durable independent identity.");
		});
		registry.Add("Editor.TerrainIndependentCopyFailure.ConditionalUnregisterRefusesOriginalIdentity", [](TestContext& context) {
			Fixture fixture; std::string error; const auto originalPath = ResourceManager::Instance().GetRegisteredAssetPath(fixture.sourceId, AssetType::Terrain);
			context.Expect(!AssetDatabase::Instance().UnregisterProvisionalTerrainCopy(fixture.source, TerraEngine::GenerateUUID(), error)
				&& AssetDatabase::Instance().FindAssetByGuid(fixture.sourceId) != nullptr
				&& ResourceManager::Instance().GetRegisteredAssetPath(fixture.sourceId, AssetType::Terrain) == originalPath,
				"Real conditional unregister must refuse a mismatched expected UUID without touching original aliases/files.");
		});
	}
}
