// MutationSubmissionOrderTests.cpp - generated identities and protected flush order.
#include "TestHarness.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"

#include <set>
#include <algorithm>
#include <string>
#include <vector>
#include <thread>
#include <iostream>
#include <stdexcept>

namespace TerraEngine::Tests {
	namespace {
		struct MutationOrderWorld {
			ObjectManager& objects = ObjectManager::instance();
			MutationOrderWorld() { objects.clear(); ComponentRegistry::RegisterAllComponents(); }
			~MutationOrderWorld() { objects.clear(); }
		};
		Scene::SceneMutationRequest CreateNamed(std::string name) {
			Scene::SceneMutationRequest request;
			request.operation = Scene::SceneMutationOperation::CreateObject;
			request.displayName = std::move(name);
			return request;
		}
		void VerifyFlush(TestContext& context, Scene::SceneMutationQueue& queue, ObjectManager& objects,
			const std::vector<std::string>& expected) {
			const auto pending = queue.PendingRequests();
			std::set<std::string> identities;
			std::vector<std::string> observed;
			for (const auto& request : pending) {
				identities.insert(request.requestId);
				observed.push_back(request.displayName);
			}
			// No object is visible during worker-safe ingress; Apply is the only
			// protected visibility boundary used by this regression.
			const bool invisibleBeforeFlush = objects.getRootObjects().empty();
			const auto result = queue.Apply(objects, "Review submission-order protected boundary");
			context.Expect(invisibleBeforeFlush && result.applied && result.results.size() == expected.size(),
				"Ingress must remain invisible until one successful protected flush publishes all requests.");
			context.Expect(identities.size() == expected.size(), "Every generated main/worker request identity must be unique.");
			context.Expect(observed == expected, "Numeric producer submission order must survive snapshot/flush sorting beyond request ten.");
			for (std::size_t index = 0; index < expected.size(); ++index) {
				const auto object = objects.getObject(result.results[index].createdObjectId);
				context.Expect(object.has_value(), "Each accepted creation must have a live object at the flush boundary.");
				context.Expect(objects.getObjectName(*object) == expected[index], "Flush receipts must retain exact producer order.");
			}
		}
		Scene::SceneMutationRequest CoalescedName(const TerraEngine::UUID& target, const char* id, const char* value) {
			Scene::SceneMutationRequest request;
			request.requestId = id; request.operation = Scene::SceneMutationOperation::SetObjectName;
			request.targetObjectId = target; request.metadataValue = value;
			request.coalescingKey = "owned-name"; request.sourceFrameOrder = 7U;
			return request;
		}
		void VerifyCoalescing(TestContext& context,
			std::optional<Scene::SceneMutationLogicalOrder> firstKey,
			std::optional<Scene::SceneMutationLogicalOrder> secondKey,
			bool firstWorker, bool secondWorker, bool reverse, bool swapWorkers, const char* expected) {
			MutationOrderWorld world;
			const auto root = world.objects.createObject("Before");
			Scene::SceneMutationQueue queue;
			auto first = CoalescedName(world.objects.getUUID(root), "first", "First");
			auto second = CoalescedName(world.objects.getUUID(root), "second", "Second");
			first.logicalCoalescingOrder = firstKey; second.logicalCoalescingOrder = secondKey;
			// Deliberately forge the opposite marker: admission must overwrite it.
			first.ingressWorker = !firstWorker; second.ingressWorker = !secondWorker;
			auto pool = ThreadPool::CreateOwned(2U);
			const auto owner = std::this_thread::get_id();
			auto submit = [&](Scene::SceneMutationRequest request, bool worker, unsigned index) {
				if(!worker) { queue.Enqueue(std::move(request)); return; }
				auto task = pool->createTask([&queue, request = std::move(request), index, owner] () mutable {
					if(std::this_thread::get_id() == owner) throw std::runtime_error("Expected actual worker ingress");
					queue.EnqueueFromWorker(index, std::move(request));
				});
				auto done = task->getFuture(); pool->enqueue(task); done.get();
			};
			// Waiting for each producer establishes opposite, known arrival orders;
			// worker indices also swap independently of stable logical identities.
			if(reverse) { submit(second, secondWorker, swapWorkers ? 1U : 0U); submit(first, firstWorker, swapWorkers ? 0U : 1U); }
			else { submit(first, firstWorker, swapWorkers ? 0U : 1U); submit(second, secondWorker, swapWorkers ? 1U : 0U); }
			queue.Enqueue(CreateNamed("Independent generic create"));
			const auto pending = queue.PendingRequests();
			const auto validation = queue.Validate(Scene::BuildSceneGraphFromObjectManager(world.objects));
			context.Expect(world.objects.getObjectName(root) == "Before" && world.objects.getRootObjects().size() == 1U,
				"Ingress, inspection, and validation cannot expose worker-authored writes.");
			if(!expected) context.Expect(pending.size() == 3U && validation.acceptedMutationCount == 0U
				&& validation.rejectedMutationCount == 3U, "Ambiguous inspection must retain every identity and validation must reject the complete generic cohort.");
			else context.Expect(pending.size() == 2U && validation.rejectedMutationCount == 0U,
				"Valid inspection and validation must share the same selected coalesced requests.");
			const auto batch = queue.Apply(world.objects, "Logical coalescing protected boundary");
			context.Expect(batch.results.size() == 3U, "Selected, superseded, or rejected contributors each need a terminal receipt.");
			if(expected) context.Expect(batch.applied && world.objects.getObjectName(root) == expected
				&& world.objects.getRootObjects().size() == 2U && batch.acceptedMutationCount == 2U,
				"The selected value and unrelated generic creation must publish at the protected boundary.");
			else {
				context.Expect(!batch.applied && batch.acceptedMutationCount == 0U && batch.rejectedMutationCount == 3U
					&& world.objects.getObjectName(root) == "Before" && world.objects.getRootObjects().size() == 1U,
					"Ambiguity must reject atomically, including an otherwise valid unrelated generic creation.");
				for(const auto& receipt : batch.results) context.Expect(!receipt.accepted && receipt.appliedWriteCount == 0U
					&& !receipt.diagnostics.empty(), "Rejected identities require diagnostics and cannot claim applied writes.");
			}
			context.Expect(queue.Empty() && queue.Apply(world.objects).results.empty(), "A terminal coalescing decision must not replay.");
		}

	}

	void RegisterMutationSubmissionOrderTests(TestRegistry& registry) {
		registry.Add("Engine.Review.MutationOrder.MainGeneratedIdsPreserveNumericOrder", [](TestContext& context) {
			MutationOrderWorld world;
			Scene::SceneMutationQueue queue;
			std::vector<std::string> expected;
			for (unsigned index = 0; index < 14; ++index) {
				expected.push_back("Main " + std::to_string(index));
				queue.Enqueue(CreateNamed(expected.back()));
			}
			VerifyFlush(context, queue, world.objects, expected);
		});
		registry.Add("Engine.Review.MutationOrder.WorkerGeneratedIdsAreUniqueAndOrdered", [](TestContext& context) {
			const auto ownerThread = std::this_thread::get_id();
			MutationOrderWorld world;
			Scene::SceneMutationQueue queue;
			std::vector<std::string> expected;
			auto pool = ThreadPool::CreateOwned(2);
			for (unsigned worker = 0; worker < 2; ++worker) {
				std::vector<std::string> names;
				for (unsigned index = 0; index < 14; ++index) {
					names.push_back("Worker " + std::to_string(worker) + " request " + std::to_string(index));
					expected.push_back(names.back());
				}
				// Serialize the two producer episodes deliberately so the expected
				// cross-producer order is known; each enqueue still runs on a worker.
				std::thread::id producerBefore, producerAfter;
				auto task = pool->createTask([&queue, worker, names, &producerBefore, &producerAfter] {
					// The identities bracket the real queue ingress, not a dummy dispatch.
					producerBefore = std::this_thread::get_id();
					for (const auto& name : names) queue.EnqueueFromWorker(worker, CreateNamed(name));
					producerAfter = std::this_thread::get_id();
				});
				auto done = task->getFuture();
				pool->enqueue(task);
				done.get();
				std::cout << "Mutation ingress identities: owner=" << ownerThread << " producer-episode=" << worker
					<< " thread=" << producerBefore << ':' << producerAfter << '\n';
				context.Expect(producerBefore != std::thread::id{} && producerBefore != ownerThread
					&& producerAfter == producerBefore && std::this_thread::get_id() == ownerThread,
					"Actual mutation ingress must execute on an observed worker before the owner performs the protected flush.");
			}
			VerifyFlush(context, queue, world.objects, expected);
		});
		registry.Add("Engine.Review.MutationOrder.StableWorkerCoalescingIgnoresArrivalAndAssignment", [](TestContext& context) {
			for(bool reverse : {false, true}) for(bool swap : {false, true})
				VerifyCoalescing(context, Scene::SceneMutationLogicalOrder{2U, 8U}, Scene::SceneMutationLogicalOrder{3U, 1U},
					true, true, reverse, swap, "Second");
			// Within one logical producer, request ordinal selects its final value.
			VerifyCoalescing(context, Scene::SceneMutationLogicalOrder{2U, 8U}, Scene::SceneMutationLogicalOrder{2U, 9U},
				true, true, true, false, "Second");
		});
		registry.Add("Engine.Review.MutationOrder.AmbiguousWorkerCoalescingRejectsAtomicCohort", [](TestContext& context) {
			for(bool reverse : {false, true}) {
				VerifyCoalescing(context, {}, {}, true, true, reverse, false, nullptr);
				VerifyCoalescing(context, Scene::SceneMutationLogicalOrder{1U, 1U}, {}, true, true, reverse, false, nullptr);
				VerifyCoalescing(context, Scene::SceneMutationLogicalOrder{1U, 1U}, Scene::SceneMutationLogicalOrder{1U, 1U},
					true, true, reverse, false, nullptr);
			}
		});
		registry.Add("Engine.Review.MutationOrder.MixedIngressCoalescingRequiresEveryLogicalKey", [](TestContext& context) {
			for(bool reverse : {false, true}) {
				VerifyCoalescing(context, {}, {}, true, false, reverse, false, nullptr);
				VerifyCoalescing(context, Scene::SceneMutationLogicalOrder{4U, 1U}, {}, true, false, reverse, false, nullptr);
				VerifyCoalescing(context, Scene::SceneMutationLogicalOrder{4U, 1U}, Scene::SceneMutationLogicalOrder{3U, 2U},
					true, false, reverse, true, "First");
			}
		});
		registry.Add("Engine.Review.MutationOrder.MainCoalescingRetainsLegacyAndValidatesExplicitKeys", [](TestContext& context) {
			VerifyCoalescing(context, {}, {}, false, false, false, false, "Second");
			VerifyCoalescing(context, {}, {}, false, false, true, false, "First");
			VerifyCoalescing(context, Scene::SceneMutationLogicalOrder{4U, 1U}, Scene::SceneMutationLogicalOrder{3U, 2U},
				false, false, false, false, "First");
			VerifyCoalescing(context, Scene::SceneMutationLogicalOrder{4U, 1U}, {}, false, false, false, false, nullptr);
			VerifyCoalescing(context, Scene::SceneMutationLogicalOrder{4U, 1U}, Scene::SceneMutationLogicalOrder{4U, 1U},
				false, false, true, false, nullptr);
		});
		registry.Add("Engine.Review.MutationOrder.HigherFrameSupersedesEarlierAmbiguousWorkerTie", [](TestContext& context) {
			MutationOrderWorld world;
			const auto root = world.objects.createObject("Before");
			Scene::SceneMutationQueue queue;
			auto earlier = CoalescedName(world.objects.getUUID(root), "earlier", "Earlier");
			auto other = earlier; other.requestId = "other";
			auto latest = earlier; latest.requestId = "latest"; latest.metadataValue = "Latest"; latest.sourceFrameOrder = 8U;
			// The uncontested highest frame needs no logical key, even though the
			// superseded earlier frame contains a competing unkeyed worker pair.
			queue.EnqueueFromWorker(2U, latest); queue.EnqueueFromWorker(1U, earlier); queue.EnqueueFromWorker(2U, other);
			context.Expect(queue.PendingRequests().size() == 1U && queue.PendingRequests().front().requestId == "latest",
				"Highest frame selection must be independent of arrival and earlier ambiguity.");
			const auto batch = queue.Apply(world.objects);
			context.Expect(batch.applied && batch.acceptedMutationCount == 1U && batch.results.size() == 3U
				&& world.objects.getObjectName(root) == "Latest", "Higher-frame publication must settle all superseded identities.");
		});

		registry.Add("Engine.Review.MutationOrder.MixedIngressPreservesGlobalNumericAdmission", [](TestContext& context) {
			MutationOrderWorld world;
			Scene::SceneMutationQueue queue;
			auto pool = ThreadPool::CreateOwned(1U);
			std::vector<std::string> expected;
			for(unsigned index = 0U; index < 8U; ++index) {
				expected.push_back("Main " + std::to_string(index)); queue.Enqueue(CreateNamed(expected.back()));
				expected.push_back("Worker " + std::to_string(index));
				auto task = pool->createTask([&queue, name = expected.back()] { queue.EnqueueFromWorker(5U, CreateNamed(name)); });
				auto done = task->getFuture(); pool->enqueue(task); done.get();
			}
			VerifyFlush(context, queue, world.objects, expected);
		});
		registry.Add("Engine.Review.MutationOrder.AmbiguityRejectsTypedSpatialEnvelopeWithoutReplay", [](TestContext& context) {
			MutationOrderWorld world;
			const auto root = world.objects.createObject("Before");
			const auto id = world.objects.getUUID(root);
			const auto before = *world.objects.GetCanonicalWorldPose(root);
			auto requested = before; requested.position.offsetMetres.x += 7.0;
			Scene::SceneMutationQueue queue;
			queue.EnqueueFromWorker(1U, CoalescedName(id, "first", "First"));
			queue.EnqueueFromWorker(2U, CoalescedName(id, "second", "Second"));
			Scene::SceneMutationRequest spatial;
			spatial.requestId = "owned-spatial"; spatial.operation = Scene::SceneMutationOperation::SetWorldTransform;
			spatial.targetObjectId = id;
			spatial.typedSpatialTransaction = Scene::TransactionalMutationBuilder(spatial.requestId, Scene::TransactionOrigin::Test)
				.SetWorldTransform(world.objects, id, requested).Build();
			queue.Enqueue(spatial);
			const auto batch = queue.Apply(world.objects);
			context.Expect(!batch.applied && batch.rejectedMutationCount == 3U && world.objects.getObjectName(root) == "Before"
				&& world.objects.GetCanonicalWorldPose(root)->position.offsetMetres.x == before.position.offsetMetres.x,
				"Ambiguous coalescing cannot publish an otherwise valid typed spatial write.");
			const auto receipt = std::find_if(batch.transactionResults.begin(), batch.transactionResults.end(), [](const auto& value) {
				return value.transactionId == "owned-spatial";
			});
			context.Expect(receipt != batch.transactionResults.end() && receipt->status == Scene::TransactionStatus::Rejected
				&& receipt->appliedWriteCount == 0U && receipt->rejectedWriteCount == 1U && receipt->inverseDeltas.empty(),
				"The real typed producer must receive its terminal rejected logical-write receipt.");
			context.Expect(queue.Empty() && queue.Apply(world.objects).transactionResults.empty(), "Rejected spatial ownership must not replay.");
		});

	}
}
