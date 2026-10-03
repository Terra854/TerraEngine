#include "TestHarness.h"
#include "Scene/TransactionalMutation.h"
#include "Scene/TransactionalMutationTestFixtures.h"

#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/SystemScheduler.h"
#include "Core/SystemBase.h"
#include "Runtime/CorrectnessTestSupport.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"

#include <array>
#include <future>

namespace {
	// This real protected consumer checks the same declared publication edges
	// used by Engine. Producers retain typed copies and never touch live storage.
	class MutationVisibilityConsumer final : public SystemsBase {
	public:
		std::function<void()> observe;
		void Initialize() override {}
		void Shutdown() override {}
		void Update(float) override { observe(); }
		std::string SystemName() override { return "MutationVisibilityConsumer"; }
		TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override {
			TerraEngine::Scheduling::SystemDeclaration declaration;
			declaration.name = SystemName(); declaration.phase = "Render";
			declaration.reads = {"ObjectManager"}; declaration.mainThread = true;
			declaration.requiresFlushedSceneMutations = true; declaration.usesDefaultMetadata = false;
			declaration.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit;
			declaration.boundaryClassification = TerraEngine::Scheduling::HardBoundaryClassification::Protected;
			declaration.safeSubwork = "Copied producer transactions are admitted by fixture-owned worker tasks.";
			declaration.protectedCommit = "Read live authored state only after the scheduler's declared mutation flush.";
			declaration.protectedReason = declaration.protectedCommit;
			return declaration;
		}
	};
}

namespace TerraEngine::Tests {
	void RegisterTransactionalMutationStressTests(TestRegistry& registry)
	{
		registry.Add("Engine.TransactionalMutation.CommitsLargeSimpleBatch", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::TransactionalMutationBuilder builder("tx-large", TerraEngine::Scene::TransactionOrigin::Test, "stress");
			for (int index = 1; index <= 1000; ++index)
			{
				builder.CreateObject(index, "BatchObject");
			}

			const TerraEngine::Scene::TransactionResult result = TerraEngine::Scene::CommitTransaction(fixture.objects, builder.Build());

			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Committed, "Large simple batch should commit.");
			context.ExpectEqual(static_cast<std::size_t>(1000), result.appliedWriteCount, "Every batch write should apply.");
		});

		registry.Add("Engine.TransactionalMutation.SmallScenePreflightOverheadDoesNotMutate", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::Transaction transaction = TerraEngine::Scene::TransactionalMutationBuilder("tx-preflight", TerraEngine::Scene::TransactionOrigin::Test, "stress")
				.SetMode(TerraEngine::Scene::TransactionMode::PreflightOnly)
				.SetName(fixture.rootId, "PreflightOnly")
				.Build();

			const TerraEngine::Scene::TransactionResult result = TerraEngine::Scene::PreflightTransaction(fixture.objects, transaction);

			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::PreflightAccepted, "Valid preflight should be accepted.");
			context.ExpectEqual(std::string("TransactionRoot"), fixture.objects.getObjectName(fixture.root), "Preflight should not mutate.");
		});

		registry.Add("Engine.TransactionalMutation.ConcurrentAdmissionsRejectIndependentlyAtDeclaredFlush", [](TestContext& context) {
			using namespace TerraEngine::Scene;
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			auto queue = std::make_shared<SceneMutationQueue>();
			std::vector<Object> targets;
			std::array<Transaction, 4> copied;
			for(std::size_t worker = 0; worker < copied.size(); ++worker) {
				TransactionalMutationBuilder builder("concurrent-producer-" + std::to_string(worker), TransactionOrigin::Test);
				for(unsigned index = 0; index < 64U; ++index) {
					Object object = fixture.objects.createObject("Concurrent Target");
					builder.SetTag(fixture.objects.getUUID(object), "Committed At Boundary");
					targets.push_back(object);
				}
				copied[worker] = builder.Build();
			}
			SceneMutationRequest earlier;
			earlier.requestId = "generic-before-rejection"; earlier.operation = SceneMutationOperation::SetObjectName;
			earlier.targetObjectId = fixture.rootId; earlier.metadataValue = "Must Roll Back";
			queue->Enqueue(std::move(earlier));
			queue->Enqueue({"invalid-generic", SceneMutationOperation::DeleteObject, TerraEngine::GenerateUUID()});
			auto rollback = TransactionalMutationBuilder("protected-rollback", TransactionOrigin::Test)
				.SetName(fixture.rootId, "Must Also Roll Back").SetTag(fixture.rootId, "Aborted Tag").Build();
			rollback.injectedFailureOperationIndex = 1U;
			queue->EnqueueTransaction(std::move(rollback));
			auto conflict = TransactionalMutationBuilder("independent-conflict", TransactionOrigin::Test)
				.DeleteObject(TerraEngine::GenerateUUID()).Build();
			queue->EnqueueTransaction(std::move(conflict));
			auto& pool = ThreadPool::instance();
			const bool ownsPool = pool.getThreadCount() == 0U;
			if(ownsPool) pool.initialize(4U);
			struct PoolOwner { ThreadPool& pool; bool owns; ~PoolOwner() { if(owns) pool.shutdown(); } } poolOwner{pool, ownsPool};
			Correctness::ControlledGate gate;
			std::vector<std::future<bool>> futures;
			// Release gates before joining even when an assertion throws. Every job
			// owns the queue and its request, so no callback retains fixture references.
			struct JoinTasks {
				Correctness::ControlledGate& gate; std::vector<std::future<bool>>& futures;
				~JoinTasks() { gate.Cancel(); for(auto& future : futures) if(future.valid()) future.wait(); }
			} join{gate, futures};
			for(auto& transaction : copied) {
				auto task = pool.createTask([queue, request = std::move(transaction), wait = gate.MakeWaiter()]() mutable {
					if(wait() != Correctness::GateResult::Released) return false;
					queue->EnqueueTransaction(std::move(request)); return true;
				});
				futures.push_back(task->getFuture()); pool.enqueue(task);
			}
			context.Expect(gate.WaitForArrivals(std::min(std::size_t{4}, pool.getThreadCount())), "Available worker producers reached their controlled admission gate.");
			gate.Release();
			for(auto& future : futures) context.Expect(future.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready && future.get(), "Every copied worker admission must settle.");
			for(const auto& object : targets) context.Expect(fixture.objects.getObjectTag(object) != "Committed At Boundary", "Admission alone cannot publish live authored state.");
			bool observed = false;
			MutationVisibilityConsumer consumer;
			consumer.observe = [&] {
				for(const auto& object : targets) context.Expect(fixture.objects.getObjectTag(object) == "Committed At Boundary", "Same-frame consumer sees every independent write after the declared flush.");
				context.Expect(fixture.objects.getObjectName(fixture.root) == "TransactionRoot", "Both generic and injected typed failures leave prior authored state intact.");
				observed = true;
			};
			TerraEngine::Scheduling::SystemScheduler scheduler;
			scheduler.RegisterSystem(consumer);
			std::size_t committed = 0, rejected = 0, genericRejected = 0;
			const auto& summary = scheduler.ExecuteFrame(1.0F / 60.0F, [&](const TerraEngine::Scheduling::MutationFlushPoint& point) {
				const auto result = queue->Apply(fixture.objects, point.name);
				for(const auto& receipt : result.transactionResults) {
					// Generic requests also expose their single lowered cohort receipt.
					// Count it separately so this check proves both native failures settle
					// once without mistaking truthful generic diagnostics for a duplicate.
					if(receipt.status == TransactionStatus::Committed) ++committed;
					else if(receipt.transactionId.starts_with("generic-cohort:")) ++genericRejected;
					else ++rejected;
				}
				return TerraEngine::Scheduling::MutationFlushRuntimeState{true, queue->Size(), {}};
			});
			context.Expect(summary.valid && observed && queue->Empty(), "The existing protected schedule completes with no replayable rejected work.");
			context.ExpectEqual(std::size_t{4}, committed, "Four independent producer transactions commit once.");
			context.ExpectEqual(std::size_t{2}, rejected, "Conflict and mid-commit rollback each receive one rejected typed receipt.");
			context.ExpectEqual(std::size_t{1}, genericRejected, "The rejected generic cohort exposes exactly one lowered receipt.");
		});
	}
}
