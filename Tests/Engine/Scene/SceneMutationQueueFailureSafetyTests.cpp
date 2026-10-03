#include "TestHarness.h"
#include "Runtime/CorrectnessTestSupport.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

namespace TerraEngine::Tests {
	namespace {
		// Each case owns a clean scene and restores callbacks before destroying it.
		// Workers enqueue copied requests only; all live writes belong to Apply.
		struct QueueSceneFixture {
			ObjectManager& objects = ObjectManager::instance();
			Object root;
			TerraEngine::UUID id;
			QueueSceneFixture() : root([&] { objects.clear(); return objects.createObject("Before"); }()), id(objects.getUUID(root)) {}
			~QueueSceneFixture() { objects.SetOnModifiedCallback({}); objects.clear(); }
		};

		TerraEngine::Scene::SceneMutationRequest NameRequest(const char* requestId, const TerraEngine::UUID& id, const char* name) {
			TerraEngine::Scene::SceneMutationRequest request;
			request.requestId = requestId;
			request.operation = TerraEngine::Scene::SceneMutationOperation::SetObjectName;
			request.targetObjectId = id;
			request.metadataValue = name;
			return request;
		}

		void ExpectNoReplay(TestContext& context, TerraEngine::Scene::SceneMutationQueue& queue, ObjectManager& objects) {
			const auto empty = queue.Apply(objects, "Repeat Boundary");
			context.Expect(queue.Empty() && empty.results.empty() && empty.transactionResults.empty()
				&& empty.publicationGroupResults.empty() && !empty.applied,
				"A terminal decision must not execute or return a second receipt at the next boundary.");
		}
	}

	void RegisterSceneMutationQueueFailureSafetyTests(TestRegistry& registry) {
		using namespace TerraEngine::Scene;
		registry.Add("Engine.SceneMutationFailureSafety.Queue.InvalidGenericDoesNotBlockIndependentNative", [](TestContext& context) {
			QueueSceneFixture fixture;
			SceneMutationQueue queue;
			queue.Enqueue(NameRequest("rejected-name", fixture.id, "Unpublished"));
			queue.Enqueue({"missing-delete", SceneMutationOperation::DeleteObject, TerraEngine::GenerateUUID()});
			queue.EnqueueTransaction(TransactionalMutationBuilder("independent-tag", TransactionOrigin::Editor)
				.SetUndoGroup("Independent Native").SetTag(fixture.id, "Native Committed").Build());
			context.Expect(fixture.objects.getObjectName(fixture.root) == "Before", "Admission cannot expose authored writes.");
			const auto batch = queue.Apply(fixture.objects);
			context.Expect(fixture.objects.getObjectName(fixture.root) == "Before"
				&& fixture.objects.getObjectTag(fixture.root) == "Native Committed", "Rejected generic state remains unchanged while independent native work commits.");
			context.Expect(batch.results.size() == 2U && !batch.results[0].accepted && !batch.results[1].accepted,
				"Every rejected generic request receives one terminal receipt.");
			const auto history = queue.DrainCompletedEditorTransactions();
			context.Expect(history.size() == 1U && history.front().result.status == TransactionStatus::Committed, "Independent editor work receives one committed history receipt.");
			context.Expect(queue.DrainCompletedEditorTransactions().empty(), "History receipts are consumed once.");
			ExpectNoReplay(context, queue, fixture.objects);
			queue.Enqueue(NameRequest("later-valid", fixture.id, "Later"));
			const auto later = queue.Apply(fixture.objects);
			context.Expect(later.results.size() == 1U && later.results.front().accepted
				&& fixture.objects.getObjectName(fixture.root) == "Later", "Later valid submissions progress after rejection.");
		});

		registry.Add("Engine.SceneMutationFailureSafety.Queue.SupersededCoalescingReturnsEveryIdentity", [](TestContext& context) {
			QueueSceneFixture fixture;
			SceneMutationQueue queue;
			auto earlier = NameRequest("superseded", fixture.id, "Earlier");
			earlier.coalescingKey = "same-name"; earlier.sourceFrameOrder = 1U;
			auto latest = NameRequest("latest", fixture.id, "Latest");
			latest.coalescingKey = "same-name"; latest.sourceFrameOrder = 2U;
			queue.EnqueueFromWorker(1U, std::move(earlier)); queue.EnqueueFromWorker(2U, std::move(latest));
			const auto batch = queue.Apply(fixture.objects);
			context.Expect(batch.inputMutationCount == 2U && batch.coalescedMutationCount == 1U
				&& batch.acceptedMutationCount == 1U, "Accounting preserves admitted identities but counts only the committed coalesced write.");
			const auto replaced = std::find_if(batch.results.begin(), batch.results.end(), [](const auto& receipt) { return receipt.requestId == "superseded"; });
			const auto committed = std::find_if(batch.results.begin(), batch.results.end(), [](const auto& receipt) { return receipt.requestId == "latest"; });
			context.Expect(batch.results.size() == 2U && replaced != batch.results.end() && committed != batch.results.end(),
				"Both superseded and selected producers receive terminal identities.");
			context.Expect(!replaced->accepted && replaced->appliedWriteCount == 0U && !replaced->diagnostics.empty()
				&& committed->accepted && committed->appliedWriteCount == 1U, "Superseded receipts cannot claim an applied write.");
			context.Expect(fixture.objects.getObjectName(fixture.root) == "Latest", "Only the latest coalesced value is published.");
			ExpectNoReplay(context, queue, fixture.objects);
		});

		registry.Add("Engine.SceneMutationFailureSafety.Queue.InvalidExplicitGroupDoesNotBlockIndependentGroup", [](TestContext& context) {
			QueueSceneFixture fixture;
			SceneMutationQueue queue;
			TransactionPublicationGroup invalid;
			invalid.groupId = "invalid-group";
			invalid.chunks.push_back(TransactionalMutationBuilder("invalid-earlier", TransactionOrigin::Test).SetName(fixture.id, "Unpublished").Build());
			invalid.chunks.push_back(TransactionalMutationBuilder("invalid-later", TransactionOrigin::Test).DeleteObject(TerraEngine::GenerateUUID()).Build());
			TransactionPublicationGroup valid;
			valid.groupId = "valid-group";
			valid.chunks.push_back(TransactionalMutationBuilder("valid-tag", TransactionOrigin::Test).SetTag(fixture.id, "Independent").Build());
			queue.EnqueuePublicationGroup(std::move(invalid)); queue.EnqueuePublicationGroup(std::move(valid));
			const auto batch = queue.Apply(fixture.objects);
			context.Expect(batch.publicationGroupResults.size() == 2U && !batch.publicationGroupResults[0].Committed()
				&& batch.publicationGroupResults[1].Committed(), "Explicit groups receive independent terminal decisions at one boundary.");
			context.Expect(batch.publicationGroupResults[0].chunkReceipts.size() == 2U
				&& batch.publicationGroupResults[0].appliedWriteCount == 0U, "Every rejected chunk is settled without partial write counts.");
			context.Expect(fixture.objects.getObjectName(fixture.root) == "Before"
				&& fixture.objects.getObjectTag(fixture.root) == "Independent", "The invalid group rolls back completely while the valid group remains visible.");
			context.Expect(queue.TakeCompletedPublicationGroupResults().empty(), "Normal group receipts must not also be delivered by the fatal-only channel.");
			ExpectNoReplay(context, queue, fixture.objects);
		});

		registry.Add("Engine.SceneMutationFailureSafety.Queue.ProducerDuringCommitWaitsForNextBoundary", [](TestContext& context) {
			QueueSceneFixture fixture;
			auto queue = std::make_shared<SceneMutationQueue>();
			queue->Enqueue(NameRequest("first", fixture.id, "First Boundary"));
			Correctness::ControlledGate start;
			Correctness::CompletionCounter completed;
			ThreadPool& pool = ThreadPool::instance();
			const bool ownsPool = pool.getThreadCount() == 0U;
			if (ownsPool) pool.initialize(1U);
			// Pool shutdown joins our task on exceptional test exits. Existing harness
			// pools remain owned by their fixture; the shared queue outlives the task.
			struct PoolOwner { ThreadPool& pool; bool owns; ~PoolOwner() { if (owns) pool.shutdown(); } } poolOwner{pool, ownsPool};
			auto task = pool.createTask([queue, id = fixture.id, wait = start.MakeWaiter(), completed] {
				if (wait() != Correctness::GateResult::Released) { completed.Increment(); return false; }
				queue->Enqueue(NameRequest("later-generic", id, "Second Boundary"));
				queue->EnqueueTransaction(TransactionalMutationBuilder("later-native", TransactionOrigin::Editor).SetUndoGroup("Later Native").SetTag(id, "Second Boundary Tag").Build());
				TransactionPublicationGroup group; group.groupId = "later-group";
				group.chunks.push_back(TransactionalMutationBuilder("later-layer", TransactionOrigin::Test).SetLayer(id, "Second Boundary Layer").Build());
				queue->EnqueuePublicationGroup(std::move(group));
				NavigationSourceMutationIntent intent; intent.intentId = "later-navigation"; intent.transactionId = "later-native";
				intent.sourceId = id; intent.operation = NavigationSourceMutationOperation::Create;
				intent.newBounds = {{{0, 0, 0}, {-1.0, 0.0, -1.0}}, {{0, 0, 0}, {1.0, 1.0, 1.0}}, true};
				intent.committedSourceRevision = 1U; intent.physicalReadyRevision = 1U; intent.newEnabled = true;
				if (!queue->EnqueueNavigationSourceMutationIntent(intent)) { completed.Increment(); return false; }

				completed.Increment(); return true;
			});
			auto future = task->getFuture(); pool.enqueue(task);
			struct JoinProducer { Correctness::ControlledGate& gate; std::future<bool>& future; ~JoinProducer() { gate.Cancel(); if (future.valid()) (void)future.wait_for(Correctness::kDefaultTimeout); } } join{start, future};
			context.Expect(start.WaitForArrivals(1U), "The copied producer reached the controlled gate.");
			bool published = false;
			fixture.objects.SetOnModifiedCallback([&] {
				if (published) return;
				published = true; start.Release();
				if (!completed.WaitForAtLeast(1U)) throw std::runtime_error("Producer failed to enqueue at the publication boundary.");
			});
			const auto first = queue->Apply(fixture.objects);
			fixture.objects.SetOnModifiedCallback({});
			context.Expect(future.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready && future.get(), "The producer completed before its fixture is reclaimed.");
			context.Expect(completed.Value() == 1U && first.results.size() == 1U, "The first detached cohort owns only its admitted request.");
			context.Expect(fixture.objects.getObjectName(fixture.root) == "First Boundary"
				&& fixture.objects.getObjectTag(fixture.root) != "Second Boundary Tag"
				&& fixture.objects.getObjectLayer(fixture.root) != "Second Boundary Layer"
				&& first.publicationGroupResults.empty() && first.navigationSourceMutationReceipts.empty(), "Every new producer channel remains invisible until the next declared flush.");
			context.Expect(!queue->Empty() && queue->DrainCompletedEditorTransactions().empty(), "Later native work is pending rather than prematurely acknowledged.");
			const auto second = queue->Apply(fixture.objects);
			context.Expect(second.results.size() == 1U && second.results.front().requestId == "later-generic"
				&& fixture.objects.getObjectName(fixture.root) == "Second Boundary"
				&& fixture.objects.getObjectTag(fixture.root) == "Second Boundary Tag"
				&& fixture.objects.getObjectLayer(fixture.root) == "Second Boundary Layer"
				&& second.publicationGroupResults.size() == 1U && second.navigationSourceMutationReceipts.size() == 1U
				&& second.navigationSourceMutationReceipts.front().accepted, "The next boundary commits every preserved submission.");
			context.Expect(queue->DrainCompletedEditorTransactions().size() == 1U, "The native producer receives exactly one committed completion.");
			ExpectNoReplay(context, *queue, fixture.objects);
		});
	}
}

