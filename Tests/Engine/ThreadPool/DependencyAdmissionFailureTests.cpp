// DependencyAdmissionFailureTests.cpp — Observe actual ready-parent admission,
// ancestor settlement, and captured-input lifetime under a targeted native fault.
#include "../TestHarness.h"
#include "../Runtime/CorrectnessTestSupport.h"
#include "ThreadPool/ThreadPool.h"

#include <atomic>
#include <cstdlib>
#include <sstream>
#include <string_view>

namespace TerraEngine::Tests {
	namespace {
		// Exception objects may be copied by a standard-library rethrow. An exact
		// final type, full message, and shared cause token survive those copies and
		// distinguish the initiating infrastructure error from every sibling error.
		struct AdmissionFailure final : std::runtime_error {
			static constexpr std::uint64_t kCause = 0x066ad115510fULL;
			std::shared_ptr<const std::uint64_t> cause;
			explicit AdmissionFailure(std::shared_ptr<const std::uint64_t> identity)
				: std::runtime_error("injected dependency admission failure"), cause(std::move(identity)) {}
		};

		struct OriginalCauseFacts {
			bool exactType = true;
			bool fullMessage = true;
			bool identityToken = true;
			bool Preserved() const { return exactType && fullMessage && identityToken; }
		};
		struct AdmissionFacts {
			std::atomic<unsigned> failedParentBodies{0};
			std::atomic<unsigned> ancestorBodies{0};
			std::atomic<unsigned> settledSiblings{0};
			std::atomic<unsigned> inputDestroyed{0};
			std::atomic<unsigned> inputReadsBeforeGates{0};
			std::atomic<unsigned> inputReadsAfterGates{0};
			std::atomic<bool> inputSentinelsValid{true};
		};

		// Every real graph callable owns this payload. The sentinel is read on
		// both sides of each held worker boundary; destruction is observed only
		// after physical drain and explicit retirement of all graph handles.
		struct CapturedInput {
			static constexpr std::uint64_t kSentinel = 0x51a7d04e9b3c2f68ULL;
			std::shared_ptr<AdmissionFacts> facts;
			const std::uint64_t sentinel = kSentinel;
			explicit CapturedInput(std::shared_ptr<AdmissionFacts> value) : facts(std::move(value)) {}
			~CapturedInput() { ++facts->inputDestroyed; }
		};

		std::size_t GenericWorkerCount()
		{
			const char* text = std::getenv("TERRAENGINE_RUNTIME_MEMORY_SAFETY_WORKERS");
			if(!text) return 1;
			const std::string_view value(text);
			if(value == "1") return 1;
			if(value == "2") return 2;
			if(value == "8") return 8;
			throw std::invalid_argument("Runtime memory safety workers must be exactly 1, 2, or 8");
		}

		// This existing public graph representation deliberately preserves actual
		// identity. addDependency clones, which would target a different parent.
		void Attach(const std::shared_ptr<ITask>& parent, const std::shared_ptr<ITask>& child)
		{
			parent->dependenciesList.push_back(child);
			child->setParentTask(parent);
		}

		// Declare before gates: they cancel first on unwind, then physical drain
		// finishes before the quiescent native checkpoint is cleared.
		struct FaultScope {
			ThreadPool& pool;
			~FaultScope() {
				pool.shutdown();
				pool.SetDependencyAdmissionFailureForTesting(nullptr, {});
			}
		};

		template<typename Result>
		bool ConsumeOriginal(std::future<Result>& future,
			const std::shared_ptr<const std::uint64_t>& cause, OriginalCauseFacts& facts)
		{
			try { (void)future.get(); }
			catch(const AdmissionFailure& error) {
				const bool message = std::string_view(error.what()) == "injected dependency admission failure";
				const bool token = error.cause == cause && error.cause && *error.cause == AdmissionFailure::kCause;
				facts.fullMessage = facts.fullMessage && message;
				facts.identityToken = facts.identityToken && token;
				return message && token;
			}
			catch(...) { facts.exactType = false; return false; }
			facts.exactType = false;
			return false;
		}

		struct Scenario {
			std::size_t workers;
			unsigned ancestorCount;
			bool holdAncestors;
			bool secondaryBodyFailure = false;
			bool leafBodyFailure = false;
		};

		template<typename RootResult>
		void CheckAdmission(TestContext& context, std::string_view suffix, Scenario scenario)
		{
			auto facts = std::make_shared<AdmissionFacts>();
			auto input = std::make_shared<CapturedInput>(facts);
			std::weak_ptr<CapturedInput> inputObserver = input;
			auto pool = ThreadPool::CreateOwned(scenario.workers);
			const auto actualWorkers = pool->getThreadCount();
			FaultScope faultScope{*pool};
			std::vector<std::unique_ptr<Correctness::ControlledGate>> holds;
			std::vector<std::future<void>> siblingFutures;
			std::vector<std::shared_ptr<Task<void>>> chain;
			std::vector<std::future<void>> chainFutures;
			auto root = pool->createTask([facts, input, hasAncestors = scenario.ancestorCount != 0]() -> RootResult {
				if(input->sentinel != CapturedInput::kSentinel) facts->inputSentinelsValid.store(false);
				if(hasAncestors) ++facts->ancestorBodies;
				else ++facts->failedParentBodies;
				if constexpr(!std::is_void_v<RootResult>) return 42;
			});
			auto rootFuture = root->getFuture();
			std::shared_ptr<ITask> target = root;
			// chain[0] is the failed parent; higher entries and root are ancestors.
			for(unsigned index = 0; index < scenario.ancestorCount; ++index) {
				auto node = pool->createTask([facts, input, index] {
					if(input->sentinel != CapturedInput::kSentinel) facts->inputSentinelsValid.store(false);
					if(index == 0) ++facts->failedParentBodies;
					else ++facts->ancestorBodies;
				});
				chainFutures.push_back(node->getFuture());
				if(index != 0) Attach(node, chain.back());
				chain.push_back(std::move(node));
			}
			if(!chain.empty()) {
				target = chain.front();
				Attach(root, chain.back());
			}
			Attach(target, pool->createTask([facts, input, fail = scenario.leafBodyFailure] {
				if(input->sentinel != CapturedInput::kSentinel) facts->inputSentinelsValid.store(false);
				if(fail) throw std::runtime_error("ordinary leaf body exception");
			}));

			// Add held siblings after their failing branch, preserving leaf-first
			// traversal and preventing a held gate from blocking a sole worker early.
			if(scenario.holdAncestors) {
				for(unsigned index = 0; index < scenario.ancestorCount; ++index) {
					holds.push_back(std::make_unique<Correctness::ControlledGate>());
					auto sibling = pool->createTask([facts, input, wait = holds.back()->MakeWaiter(),
						fail = scenario.secondaryBodyFailure] {
						if(input->sentinel != CapturedInput::kSentinel) facts->inputSentinelsValid.store(false);
						++facts->inputReadsBeforeGates;
						if(wait() != Correctness::GateResult::Released) throw std::runtime_error("sibling gate did not release");
						if(input->sentinel != CapturedInput::kSentinel) facts->inputSentinelsValid.store(false);
						++facts->inputReadsAfterGates;
						++facts->settledSiblings;
						if(fail) throw std::runtime_error("secondary sibling body exception");
					});
					siblingFutures.push_back(sibling->getFuture());
					if(index + 1 < scenario.ancestorCount) Attach(chain[index + 1], sibling);
					else Attach(root, sibling);
				}
			}

			const auto cause = std::make_shared<const std::uint64_t>(AdmissionFailure::kCause);
			const auto original = std::make_exception_ptr(AdmissionFailure(cause));
			pool->SetDependencyAdmissionFailureForTesting(target.get(), original);
			pool->enqueue(root);
			const auto deadline = std::chrono::steady_clock::now() + Correctness::kDefaultTimeout;
			while(pool->DependencyAdmissionFailureCountForTesting() == 0
				&& std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
			context.Expect(pool->DependencyAdmissionFailureCountForTesting() == 1,
				"The actual final-dependency ready-parent checkpoint must be reached exactly once.");
			bool heldAncestorsPending = true;
			bool inputAliveWhileHeld = true;
			for(std::size_t index = 0; index < holds.size(); ++index) {
				const bool arrived = holds[index]->WaitForArrivals(1);
				bool pending = rootFuture.wait_for(std::chrono::seconds(0)) == std::future_status::timeout;
				for(std::size_t ancestor = index + 1; ancestor < chainFutures.size(); ++ancestor) {
					pending = pending && chainFutures[ancestor].wait_for(std::chrono::seconds(0)) == std::future_status::timeout;
				}
				heldAncestorsPending = heldAncestorsPending && arrived && pending;
				inputAliveWhileHeld = inputAliveWhileHeld && !inputObserver.expired()
					&& facts->inputDestroyed.load() == 0 && input.use_count() > 1;
				holds[index]->Release();
			}
			context.Expect(rootFuture.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready,
				"The owning root must reach a terminal failure after all admitted siblings settle.");
			unsigned terminalAncestors = 1;
			OriginalCauseFacts originalFacts;
			bool originalPreserved = ConsumeOriginal(rootFuture, cause, originalFacts);
			for(auto& future : chainFutures) {
				const bool ready = future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
				if(ready) { ++terminalAncestors; originalPreserved = ConsumeOriginal(future, cause, originalFacts) && originalPreserved; }
				else originalPreserved = false;
			}
			bool siblingOutcomes = true;
			for(auto& future : siblingFutures) {
				try { future.get(); siblingOutcomes = siblingOutcomes && !scenario.secondaryBodyFailure; }
				catch(const std::runtime_error& error) {
					siblingOutcomes = siblingOutcomes && scenario.secondaryBodyFailure
						&& std::string_view(error.what()) == "secondary sibling body exception";
				}
			}
			// This task is outside the failed graph and must execute on the same
			// still-open executor; clearing the fault is unnecessary for its identity.
			auto healthy = pool->createTask([] { return 73; });
			auto healthyFuture = healthy->getFuture();
			pool->enqueue(healthy);
			const bool healthyReady = healthyFuture.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready;
			const bool independentCompleted = healthyReady && healthyFuture.get() == 73;
			pool->shutdown();
			const bool drainComplete = pool->getThreadCount() == 0;
			const auto checkpoints = pool->DependencyAdmissionFailureCountForTesting();
			const bool inputAliveAtDrain = !inputObserver.expired() && facts->inputDestroyed.load() == 0;
			// No worker can retain queued task ownership after shutdown returns.
			// Retire every graph owner before releasing the final external input.
			target.reset();
			root.reset();
			chain.clear();
			const bool graphOwnershipBalanced = input.use_count() == 1;
			input.reset();
			const bool inputExpiredAfterRetirement = inputObserver.expired();
			// Only the settled test thread formats evidence. Publish one complete
			// line so asynchronous log prefixes cannot split individual JSON fields.
			std::ostringstream observation;
			observation << "\nRMS_OBSERVATION {\"caseId\":\"Engine.RuntimeMemorySafety.DependencyAdmission."
				<< suffix << "\",\"actualWorkers\":" << actualWorkers
				<< ",\"failureCheckpointCount\":" << checkpoints
				<< ",\"originalExceptionPreserved\":" << (originalPreserved ? "true" : "false")
				<< ",\"originalErrorTypePreserved\":" << (originalFacts.exactType ? "true" : "false")
				<< ",\"originalErrorMessagePreserved\":" << (originalFacts.fullMessage ? "true" : "false")
				<< ",\"originalErrorTokenPreserved\":" << (originalFacts.identityToken ? "true" : "false")
				<< ",\"terminalAncestors\":" << terminalAncestors
				<< ",\"expectedTerminalAncestors\":" << scenario.ancestorCount + 1
				<< ",\"heldSiblingCount\":" << holds.size()
				<< ",\"settledSiblingCount\":" << facts->settledSiblings.load()
				<< ",\"heldAncestorsPending\":" << (heldAncestorsPending ? "true" : "false")
				<< ",\"failedParentBodyCalls\":" << facts->failedParentBodies.load()
				<< ",\"ancestorBodyCalls\":" << facts->ancestorBodies.load()
				<< ",\"independentWorkCompleted\":" << (independentCompleted ? "true" : "false")
				<< ",\"inputAliveWhileHeld\":" << (inputAliveWhileHeld ? "true" : "false")
				<< ",\"inputAliveAtDrain\":" << (inputAliveAtDrain ? "true" : "false")
				<< ",\"inputReadsBeforeGates\":" << facts->inputReadsBeforeGates.load()
				<< ",\"inputReadsAfterGates\":" << facts->inputReadsAfterGates.load()
				<< ",\"inputSentinelsValid\":" << (facts->inputSentinelsValid.load() ? "true" : "false")
				<< ",\"graphOwnershipBalanced\":" << (graphOwnershipBalanced ? "true" : "false")
				<< ",\"inputDestructionCount\":" << facts->inputDestroyed.load()
				<< ",\"inputExpiredAfterRetirement\":" << (inputExpiredAfterRetirement ? "true" : "false")
				<< ",\"drainComplete\":" << (drainComplete ? "true" : "false") << "}\n";
			std::cout << observation.str();
			context.Expect(actualWorkers == scenario.workers && checkpoints == 1 && originalPreserved && originalFacts.Preserved()
				&& terminalAncestors == scenario.ancestorCount + 1 && heldAncestorsPending
				&& facts->settledSiblings.load() == holds.size() && siblingOutcomes
				&& facts->failedParentBodies.load() == 0 && facts->ancestorBodies.load() == 0
				&& independentCompleted && drainComplete && inputAliveWhileHeld && inputAliveAtDrain
				&& facts->inputReadsBeforeGates.load() == holds.size()
				&& facts->inputReadsAfterGates.load() == holds.size() && facts->inputSentinelsValid.load()
				&& graphOwnershipBalanced && facts->inputDestroyed.load() == 1 && inputExpiredAfterRetirement,
				"Exactly-once infrastructure settlement must retain original failure and physical sibling ownership.");
		}
	}

	void RegisterDependencyAdmissionFailureTests(TestRegistry& registry)
	{
		registry.Add("Engine.RuntimeMemorySafety.DependencyAdmission.VoidParent", [](TestContext& context) {
			CheckAdmission<void>(context, "VoidParent", {GenericWorkerCount(), 0, false});
		});
		registry.Add("Engine.RuntimeMemorySafety.DependencyAdmission.ValueParent", [](TestContext& context) {
			CheckAdmission<int>(context, "ValueParent", {GenericWorkerCount(), 0, false});
		});
		registry.Add("Engine.RuntimeMemorySafety.DependencyAdmission.HeldAncestorSibling", [](TestContext& context) {
			CheckAdmission<void>(context, "HeldAncestorSibling", {GenericWorkerCount(), 1, true});
		});
		registry.Add("Engine.RuntimeMemorySafety.DependencyAdmission.MultipleAncestors", [](TestContext& context) {
			CheckAdmission<int>(context, "MultipleAncestors", {GenericWorkerCount(), 2, true});
		});
		registry.Add("Engine.RuntimeMemorySafety.DependencyAdmission.FirstFailurePreserved", [](TestContext& context) {
			CheckAdmission<int>(context, "FirstFailurePreserved", {GenericWorkerCount(), 1, true, true});
		});
		registry.Add("Engine.RuntimeMemorySafety.DependencyAdmission.ParentBodySuppressed", [](TestContext& context) {
			CheckAdmission<void>(context, "ParentBodySuppressed", {GenericWorkerCount(), 2, false, false, true});
		});
		registry.Add("Engine.RuntimeMemorySafety.DependencyAdmission.IndependentWorkSurvives", [](TestContext& context) {
			CheckAdmission<int>(context, "IndependentWorkSurvives", {GenericWorkerCount(), 1, false});
		});
		registry.Add("Engine.RuntimeMemorySafety.DependencyAdmission.OneWorkerHeldSibling", [](TestContext& context) {
			CheckAdmission<void>(context, "OneWorkerHeldSibling", {1, 1, true});
		});
		registry.Add("Engine.RuntimeMemorySafety.DependencyAdmission.TwoWorkerHeldSibling", [](TestContext& context) {
			CheckAdmission<int>(context, "TwoWorkerHeldSibling", {2, 1, true});
		});
		registry.Add("Engine.RuntimeMemorySafety.DependencyAdmission.EightWorkerHeldSibling", [](TestContext& context) {
			CheckAdmission<int>(context, "EightWorkerHeldSibling", {8, 1, true});
		});
	}
}
