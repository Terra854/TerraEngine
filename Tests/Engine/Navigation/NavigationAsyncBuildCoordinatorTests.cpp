// NavigationAsyncBuildCoordinatorTests.cpp - deterministic lifetime tests for async navigation work.

#include "TestHarness.h"

#include "Navigation/NavigationAsyncPreparation.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <future>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Navigation;

		[[nodiscard]] NavigationAsyncBuildIdentity Identity(std::uint64_t sequence) {
			NavigationAsyncBuildIdentity identity{};
			identity.world = {{sequence}, 1U};
			identity.lifecycleEpoch = 1U;
			identity.groupSequence = sequence;
			identity.profileBuildKey = 100U + sequence;
			return identity;
		}

		[[nodiscard]] std::shared_ptr<NavigationBudget> Budget(std::size_t buildScratchBytes) {
			NavigationBudgetLimits limits{};
			limits.buildScratchBytes = buildScratchBytes;
			return std::make_shared<NavigationBudget>(limits);
		}

		// Coordinator work is deliberately dispatched through the real singleton.
		// The focused navigation process may arrive after a test that shut it down,
		// so own only a missing worker pool and leave an already-running host pool
		// untouched for neighbouring tests.
		class AsyncWorkerPoolSession final {
		public:
			AsyncWorkerPoolSession() {
				if(ThreadPool::instance().getThreadCount() == 0U) {
					ThreadPool::instance().initialize(1U);
					ownsPool_ = true;
				}
			}

			~AsyncWorkerPoolSession() {
				if(ownsPool_) ThreadPool::instance().shutdown();
			}

			AsyncWorkerPoolSession(const AsyncWorkerPoolSession&) = delete;
			AsyncWorkerPoolSession& operator=(const AsyncWorkerPoolSession&) = delete;

		private:
			bool ownsPool_ = false;
		};

		// This gate controls worker progress without sleeping for a performance target.
		// The test waits only for a semantic event: worker entry or caller release.
		struct WorkerGate {
			std::shared_ptr<std::promise<void>> started = std::make_shared<std::promise<void>>();
			std::future<void> startedFuture = started->get_future();
			std::shared_ptr<std::promise<void>> release = std::make_shared<std::promise<void>>();
			std::shared_future<void> releaseFuture = release->get_future().share();
			std::shared_ptr<std::atomic_bool> completed = std::make_shared<std::atomic_bool>(false);

			[[nodiscard]] NavigationAsyncBuildCoordinator::WorkFunction Work() const {
				return [started = started, releaseFuture = releaseFuture, completed = completed] {
					started->set_value();
					releaseFuture.wait();
					completed->store(true, std::memory_order_release);
				};
			}

			[[nodiscard]] bool WaitUntilStarted() {
				if(startedFuture.wait_for(std::chrono::seconds(3)) != std::future_status::ready) return false;
				startedFuture.get();
				return true;
			}

			void Release() const { release->set_value(); }
		};

		// Polling is deliberately bounded. It waits for the coordinator's real future
		// rather than treating a wall-clock latency threshold as a routing requirement.
		[[nodiscard]] std::vector<NavigationAsyncBuildCompletion> PollUntilCompleted(
			NavigationAsyncBuildCoordinator& coordinator) {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while(std::chrono::steady_clock::now() < deadline) {
				std::vector<NavigationAsyncBuildCompletion> completed = coordinator.PollCompleted();
				if(!completed.empty()) return completed;
				std::this_thread::yield();
			}
			return {};
		}

		[[nodiscard]] std::vector<NavigationAsyncBuildCompletion> PollUntilCount(
			NavigationAsyncBuildCoordinator& coordinator, std::size_t expectedCount) {
			std::vector<NavigationAsyncBuildCompletion> completed;
			completed.reserve(expectedCount);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while(completed.size() < expectedCount && std::chrono::steady_clock::now() < deadline) {
				std::vector<NavigationAsyncBuildCompletion> batch = coordinator.PollCompleted();
				completed.insert(completed.end(),
					std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
				if(completed.size() < expectedCount) std::this_thread::yield();
			}
			return completed;
		}
	}

	void RegisterNavigationAsyncBuildCoordinatorTests(TestRegistry& registry) {
		registry.Add("Navigation.Dynamic.AsyncBuild.RetiredWorkRetainsCapacityAndBudgetUntilPolled", [](TestContext& context) {
			AsyncWorkerPoolSession workers;
			auto budget = Budget(64U);
			NavigationAsyncBuildCoordinator coordinator(1U);
			WorkerGate gate{};
			std::string rejection;
			const bool admitted = coordinator.TrySubmit(Identity(1U), budget, 64U, gate.Work(), rejection);
			const bool started = admitted && gate.WaitUntilStarted();
			coordinator.RetireCurrent();
			const NavigationAsyncBuildStatus retired = coordinator.Status();
			const std::size_t chargedBeforeRelease = budget->UsedBytes(NavigationBudgetCategory::BuildScratch);

			std::string secondRejection;
			const bool replacementRejected = !coordinator.TrySubmit(
				Identity(2U), budget, 1U, [] {}, secondRejection);
			gate.Release();
			const std::vector<NavigationAsyncBuildCompletion> completions = PollUntilCompleted(coordinator);
			const NavigationAsyncBuildStatus drained = coordinator.Status();

			const bool retiredCompletion = completions.size() == 1U && completions.front().identity == Identity(1U)
				&& completions.front().retired && completions.front().succeeded;
			context.Expect(started && retired.currentCount == 0U && retired.retiredCount == 1U
				&& retired.chargedBytes == 64U && retired.peakChargedBytes == 64U
				&& chargedBeforeRelease == 64U
				&& replacementRejected && !secondRejection.empty() && retiredCompletion
				&& drained.currentCount == 0U && drained.retiredCount == 0U && drained.chargedBytes == 0U
				&& drained.completedCount == 1U && drained.staleDiscardCount == 1U,
				"Retiring a world must retain its worker permit until polling observes completion, then release capacity and report one stale discard.");
		});

		registry.Add("Navigation.Dynamic.AsyncBuild.RejectsBudgetPressureWithoutDisturbingCurrentWork", [](TestContext& context) {
			AsyncWorkerPoolSession workers;
			auto budget = Budget(96U);
			NavigationAsyncBuildCoordinator coordinator(2U);
			WorkerGate gate{};
			std::string firstRejection;
			const bool firstAdmitted = coordinator.TrySubmit(Identity(3U), budget, 64U, gate.Work(), firstRejection);
			const bool started = firstAdmitted && gate.WaitUntilStarted();
			std::string pressureRejection;
			const bool pressureRejected = !coordinator.TrySubmit(Identity(4U), budget, 64U, [] {}, pressureRejection);
			const NavigationAsyncBuildStatus underPressure = coordinator.Status();
			gate.Release();
			const std::vector<NavigationAsyncBuildCompletion> firstCompletion = PollUntilCompleted(coordinator);

			std::string retryRejection;
			const bool retryAdmitted = coordinator.TrySubmit(Identity(5U), budget, 64U, [] {}, retryRejection);
			const std::vector<NavigationAsyncBuildCompletion> retryCompletion = PollUntilCompleted(coordinator);
			const NavigationAsyncBuildStatus final = coordinator.Status();
			context.Expect(started && pressureRejected && !pressureRejection.empty()
				&& underPressure.currentCount == 1U && underPressure.chargedBytes == 64U
				&& underPressure.admissionRejectedCount == 1U && firstCompletion.size() == 1U
				&& retryAdmitted && retryCompletion.size() == 1U && retryCompletion.front().succeeded
				&& final.currentCount == 0U && final.retiredCount == 0U && final.chargedBytes == 0U
				&& budget->UsedBytes(NavigationBudgetCategory::BuildScratch) == 0U,
				"Budget rejection must leave the current copied build intact, and a completed release must reopen the exact same bounded admission capacity.");
		});

		registry.Add("Navigation.Dynamic.AsyncBuild.RepeatedRetireAndReinitializeStaysBounded", [](TestContext& context) {
			AsyncWorkerPoolSession workers;
			auto budget = Budget(48U);
			bool everyIterationDrained = true;
			for(std::uint64_t iteration = 1U; iteration <= 16U; ++iteration) {
				NavigationAsyncBuildCoordinator coordinator(1U);
				WorkerGate gate{};
				std::string rejection;
				const bool admitted = coordinator.TrySubmit(Identity(100U + iteration), budget, 48U, gate.Work(), rejection);
				const bool started = admitted && gate.WaitUntilStarted();
				coordinator.RetireCurrent();
				gate.Release();
				const std::vector<NavigationAsyncBuildCompletion> completed = PollUntilCompleted(coordinator);
				const NavigationAsyncBuildStatus status = coordinator.Status();
				everyIterationDrained = everyIterationDrained && started && completed.size() == 1U
					&& completed.front().retired && status.currentCount == 0U && status.retiredCount == 0U
					&& status.chargedBytes == 0U && status.peakChargedBytes == 48U
					&& budget->UsedBytes(NavigationBudgetCategory::BuildScratch) == 0U;
			}
			context.Expect(everyIterationDrained,
				"Repeated world retirement and coordinator reinitialization must drain every worker permit instead of accumulating former-world capacity.");
		});

		registry.Add("Navigation.Dynamic.AsyncBuild.LateCompletionAfterCoordinatorDestructionIsSafe", [](TestContext& context) {
			AsyncWorkerPoolSession workers;
			auto budget = Budget(64U);
			WorkerGate gate{};
			bool admitted = false;
			{
				NavigationAsyncBuildCoordinator coordinator(1U);
				std::string rejection;
				admitted = coordinator.TrySubmit(Identity(200U), budget, 64U, gate.Work(), rejection);
				if(admitted && gate.WaitUntilStarted()) coordinator.RetireCurrent();
				else admitted = false;
			}
			// The coordinator is gone before the worker resumes. Its task captures only
			// copied permit/work values, so late completion cannot dereference it.
			gate.Release();
			for(std::size_t attempt = 0U; attempt < 10'000U && !gate.completed->load(std::memory_order_acquire); ++attempt)
				std::this_thread::yield();
			context.Expect(admitted && gate.completed->load(std::memory_order_acquire),
				"A worker completing after coordinator destruction must remain lifetime-safe and must not require a live navigation world.");
		});

		registry.Add("Navigation.Dynamic.AsyncBuild.WorkFailureProducesNoSuccessfulCompletion", [](TestContext& context) {
			AsyncWorkerPoolSession workers;
			auto budget = Budget(64U);
			NavigationAsyncBuildCoordinator coordinator(1U);
			auto entered = std::make_shared<std::promise<void>>();
			std::future<void> enteredFuture = entered->get_future();
			std::string rejection;
			const bool admitted = coordinator.TrySubmit(Identity(300U), budget, 64U,
				[entered] {
					entered->set_value();
					throw std::runtime_error("AsyncBuildFailureSentinel");
				}, rejection);
			const bool enteredWorker = admitted
				&& enteredFuture.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
			if(enteredWorker) enteredFuture.get();
			const std::vector<NavigationAsyncBuildCompletion> completions = PollUntilCompleted(coordinator);
			const NavigationAsyncBuildStatus statusAfterFailure = coordinator.Status();
			const bool failedOnly = completions.size() == 1U && !completions.front().succeeded
				&& !completions.front().retired
				&& completions.front().failureDetail.find("AsyncBuildFailureSentinel") != std::string::npos;
			// A failed future must release the coordinator entry before the next protected
			// admission. Submit real follow-up work to prove this is capacity recovery,
			// not merely a zeroed diagnostic counter.
			std::string retryRejection;
			const bool retryAdmitted = coordinator.TrySubmit(Identity(301U), budget, 64U, [] {}, retryRejection);
			const std::vector<NavigationAsyncBuildCompletion> retryCompletion = PollUntilCompleted(coordinator);
			const NavigationAsyncBuildStatus finalStatus = coordinator.Status();
			const bool retryCompleted = retryCompletion.size() == 1U && retryCompletion.front().succeeded
				&& !retryCompletion.front().retired;
			context.Expect(enteredWorker && failedOnly && statusAfterFailure.completedCount == 1U
				&& statusAfterFailure.currentCount == 0U && statusAfterFailure.retiredCount == 0U
				&& statusAfterFailure.chargedBytes == 0U
				&& retryAdmitted && retryRejection.empty() && retryCompleted
				&& finalStatus.completedCount == 2U && finalStatus.currentCount == 0U
				&& finalStatus.retiredCount == 0U && finalStatus.chargedBytes == 0U
				&& budget->UsedBytes(NavigationBudgetCategory::BuildScratch) == 0U,
				"A failed copied worker packet must expose one failed completion, release its permit, and reopen the same bounded admission capacity without a partial successful group or receipt.");
		});

		registry.Add("Navigation.Dynamic.AsyncBuild.RetiredWorkFailureReleasesStalePermit", [](TestContext& context) {
			AsyncWorkerPoolSession workers;
			auto budget = Budget(64U);
			NavigationAsyncBuildCoordinator coordinator(1U);
			WorkerGate gate{};
			std::string rejection;
			const bool admitted = coordinator.TrySubmit(Identity(400U), budget, 64U,
				[started = gate.started, releaseFuture = gate.releaseFuture] {
					started->set_value();
					releaseFuture.wait();
					throw std::runtime_error("RetiredAsyncBuildFailureSentinel");
				}, rejection);
			const bool started = admitted && gate.WaitUntilStarted();
			coordinator.RetireCurrent();
			const NavigationAsyncBuildStatus retired = coordinator.Status();
			gate.Release();
			const std::vector<NavigationAsyncBuildCompletion> completions = PollUntilCompleted(coordinator);
			const NavigationAsyncBuildStatus drained = coordinator.Status();
			const bool retiredFailure = completions.size() == 1U && completions.front().identity == Identity(400U)
				&& completions.front().retired && !completions.front().succeeded
				&& completions.front().failureDetail.find("RetiredAsyncBuildFailureSentinel") != std::string::npos;
			context.Expect(started && retired.currentCount == 0U && retired.retiredCount == 1U
				&& retired.chargedBytes == 64U && retiredFailure
				&& drained.currentCount == 0U && drained.retiredCount == 0U && drained.chargedBytes == 0U
				&& drained.completedCount == 1U && drained.staleDiscardCount == 1U
				&& budget->UsedBytes(NavigationBudgetCategory::BuildScratch) == 0U,
				"A retired worker exception must remain a stale failed completion until polling, then release its charged permit without reviving former-world work.");
		});

		registry.Add("Navigation.Dynamic.AsyncBuild.ReportsGroundAndDynamicTimingByPacketKind", [](TestContext& context) {
			AsyncWorkerPoolSession workers;
			auto budget = Budget(128U);
			NavigationAsyncBuildCoordinator coordinator(2U);
			NavigationAsyncBuildIdentity groundIdentity = Identity(500U);
			groundIdentity.dynamicRebuild = false;
			NavigationAsyncBuildIdentity dynamicIdentity = Identity(501U);
			dynamicIdentity.dynamicRebuild = true;
			std::string groundRejection;
			std::string dynamicRejection;
			const bool groundAdmitted = coordinator.TrySubmit(
				groundIdentity, budget, 32U, [] {}, groundRejection);
			const bool dynamicAdmitted = coordinator.TrySubmit(
				dynamicIdentity, budget, 32U, [] {}, dynamicRejection);
			const std::vector<NavigationAsyncBuildCompletion> completions = PollUntilCount(coordinator, 2U);
			const NavigationAsyncBuildStatus status = coordinator.Status();
			const bool identitiesObserved = completions.size() == 2U
				&& std::any_of(completions.begin(), completions.end(), [&groundIdentity](const auto& completion) {
					return completion.identity == groundIdentity && completion.succeeded;
				})
				&& std::any_of(completions.begin(), completions.end(), [&dynamicIdentity](const auto& completion) {
					return completion.identity == dynamicIdentity && completion.succeeded;
				});
			// Timing counters are cumulative observations, not performance promises. The
			// only invariant asserted here is that aggregate duration contains its
			// corresponding maximum; zero is valid for a very short worker on a coarse
			// platform clock, so no wall-clock threshold or sleep is used.
			const bool timingConsistent = status.totalWorkerNanoseconds >= status.maxWorkerNanoseconds
				&& status.totalQueueWaitNanoseconds >= status.maxQueueWaitNanoseconds
				&& status.totalPublicationWaitNanoseconds >= status.maxPublicationWaitNanoseconds;
			context.Expect(groundAdmitted && dynamicAdmitted && groundRejection.empty() && dynamicRejection.empty()
				&& identitiesObserved && status.completedCount == 2U
				&& status.groundCompletedCount == 1U && status.dynamicCompletedCount == 1U
				&& status.currentCount == 0U && status.retiredCount == 0U
				&& status.chargedBytes == 0U && timingConsistent,
				"Ground and dynamic packets must complete as real coordinator jobs and contribute classified, internally consistent cumulative timing counters.");
		});

		registry.Add("Navigation.Dynamic.AsyncBuild.BatchAdmissionIsAllOrNoneForCapacityAndBudget", [](TestContext& context) {
			AsyncWorkerPoolSession workers;
			using Submission = NavigationAsyncBuildCoordinator::Submission;
			const auto makeSubmission = [](std::uint64_t sequence, bool dynamicRebuild,
				std::shared_ptr<NavigationBudget> budget, std::size_t chargedBytes) {
				NavigationAsyncBuildIdentity identity = Identity(sequence);
				identity.dynamicRebuild = dynamicRebuild;
				return Submission{identity, std::move(budget), chargedBytes, [] {}};
			};

			NavigationAsyncBuildCoordinator successfulCoordinator(2U);
			auto successfulBudget = Budget(96U);
			std::string successfulRejection;
			std::vector<Submission> successfulBatch;
			successfulBatch.push_back(makeSubmission(600U, false, successfulBudget, 32U));
			successfulBatch.push_back(makeSubmission(601U, true, successfulBudget, 32U));
			const bool successfulAdmission = successfulCoordinator.TrySubmitBatch(
				std::move(successfulBatch), successfulRejection);
			const NavigationAsyncBuildStatus admitted = successfulCoordinator.Status();
			const std::vector<NavigationAsyncBuildCompletion> successfulCompletions =
				PollUntilCount(successfulCoordinator, 2U);
			const NavigationAsyncBuildStatus drained = successfulCoordinator.Status();

			NavigationAsyncBuildCoordinator capacityCoordinator(1U);
			auto capacityBudget = Budget(96U);
			std::string capacityRejection;
			std::vector<Submission> capacityBatch;
			capacityBatch.push_back(makeSubmission(610U, false, capacityBudget, 32U));
			capacityBatch.push_back(makeSubmission(611U, true, capacityBudget, 32U));
			const bool capacityAdmission = capacityCoordinator.TrySubmitBatch(
				std::move(capacityBatch), capacityRejection);
			const NavigationAsyncBuildStatus afterCapacityReject = capacityCoordinator.Status();

			NavigationAsyncBuildCoordinator budgetCoordinator(2U);
			auto limitedBudget = Budget(48U);
			std::string budgetRejection;
			std::vector<Submission> budgetBatch;
			budgetBatch.push_back(makeSubmission(620U, false, limitedBudget, 32U));
			budgetBatch.push_back(makeSubmission(621U, true, limitedBudget, 32U));
			const bool budgetAdmission = budgetCoordinator.TrySubmitBatch(
				std::move(budgetBatch), budgetRejection);
			const NavigationAsyncBuildStatus afterBudgetReject = budgetCoordinator.Status();

			NavigationAsyncBuildCoordinator emptyCoordinator(2U);
			std::string emptyRejection;
			std::vector<Submission> emptyBatch;
			const bool emptyAdmission = emptyCoordinator.TrySubmitBatch(std::move(emptyBatch), emptyRejection);
			const NavigationAsyncBuildStatus afterEmptyReject = emptyCoordinator.Status();

			NavigationAsyncBuildCoordinator invalidCoordinator(2U);
			auto invalidBudget = Budget(96U);
			std::string invalidRejection;
			std::vector<Submission> invalidBatch;
			invalidBatch.push_back(makeSubmission(625U, false, invalidBudget, 32U));
			// An invalid child must be rejected during preflight, before the valid
			// sibling can reserve bytes or enqueue work.
			invalidBatch.push_back(Submission{Identity(626U), invalidBudget, 32U,
				NavigationAsyncBuildCoordinator::WorkFunction{}});
			const bool invalidAdmission = invalidCoordinator.TrySubmitBatch(
				std::move(invalidBatch), invalidRejection);
			const NavigationAsyncBuildStatus afterInvalidReject = invalidCoordinator.Status();

			const bool classifiedCompletions = successfulCompletions.size() == 2U
				&& std::any_of(successfulCompletions.begin(), successfulCompletions.end(), [](const auto& completion) {
					return !completion.identity.dynamicRebuild;
				})
				&& std::any_of(successfulCompletions.begin(), successfulCompletions.end(), [](const auto& completion) {
					return completion.identity.dynamicRebuild;
				});
			context.Expect(successfulAdmission && successfulRejection.empty()
				&& admitted.currentCount == 2U && admitted.admittedCount == 2U
				&& classifiedCompletions && drained.completedCount == 2U
				&& drained.groundCompletedCount == 1U && drained.dynamicCompletedCount == 1U
				&& drained.currentCount == 0U && drained.chargedBytes == 0U
				&& !capacityAdmission && !capacityRejection.empty()
				&& afterCapacityReject.currentCount == 0U && afterCapacityReject.admittedCount == 0U
				&& afterCapacityReject.chargedBytes == 0U
				&& !budgetAdmission && !budgetRejection.empty()
				&& afterBudgetReject.currentCount == 0U && afterBudgetReject.admittedCount == 0U
				&& afterBudgetReject.chargedBytes == 0U && limitedBudget->UsedBytes(NavigationBudgetCategory::BuildScratch) == 0U
				&& !emptyAdmission && !emptyRejection.empty()
				&& afterEmptyReject.currentCount == 0U && afterEmptyReject.admittedCount == 0U
				&& !invalidAdmission && !invalidRejection.empty()
				&& afterInvalidReject.currentCount == 0U && afterInvalidReject.admittedCount == 0U
				&& afterInvalidReject.chargedBytes == 0U
				&& invalidBudget->UsedBytes(NavigationBudgetCategory::BuildScratch) == 0U,
				"Batch admission must reserve every child before enqueue, release all reservations on capacity or budget failure, and classify both ground and dynamic completions.");
		});

		registry.Add("Navigation.Dynamic.AsyncBuild.BatchFailureAndRetirementKeepPerChildPermitsBounded", [](TestContext& context) {
			AsyncWorkerPoolSession workers;
			using Submission = NavigationAsyncBuildCoordinator::Submission;
			auto makeSubmission = [](std::uint64_t sequence, bool dynamicRebuild,
				std::shared_ptr<NavigationBudget> budget, std::size_t chargedBytes,
				NavigationAsyncBuildCoordinator::WorkFunction work) {
				NavigationAsyncBuildIdentity identity = Identity(sequence);
				identity.dynamicRebuild = dynamicRebuild;
				return Submission{identity, std::move(budget), chargedBytes, std::move(work)};
			};

			NavigationAsyncBuildCoordinator failureCoordinator(2U);
			auto failureBudget = Budget(96U);
			std::string failureRejection;
			std::vector<Submission> failureBatch;
			failureBatch.push_back(makeSubmission(630U, false, failureBudget, 32U, [] {}));
			failureBatch.push_back(makeSubmission(631U, true, failureBudget, 32U, [] {
				throw std::runtime_error("BatchChildFailureSentinel");
			}));
			const bool failureAdmission = failureCoordinator.TrySubmitBatch(std::move(failureBatch), failureRejection);
			const std::vector<NavigationAsyncBuildCompletion> failureCompletions = PollUntilCount(failureCoordinator, 2U);
			const NavigationAsyncBuildStatus failureStatus = failureCoordinator.Status();
			const bool oneSucceededOneFailed = failureCompletions.size() == 2U
				&& std::any_of(failureCompletions.begin(), failureCompletions.end(), [](const auto& completion) {
					return !completion.identity.dynamicRebuild && completion.succeeded;
				})
				&& std::any_of(failureCompletions.begin(), failureCompletions.end(), [](const auto& completion) {
					return completion.identity.dynamicRebuild && !completion.succeeded
						&& completion.failureDetail.find("BatchChildFailureSentinel") != std::string::npos;
				});

			NavigationAsyncBuildCoordinator retireCoordinator(2U);
			auto retireBudget = Budget(96U);
			WorkerGate firstGate{};
			WorkerGate secondGate{};
			std::string retireRejection;
			std::vector<Submission> retireBatch;
			retireBatch.push_back(makeSubmission(640U, false, retireBudget, 32U, firstGate.Work()));
			retireBatch.push_back(makeSubmission(641U, true, retireBudget, 32U, secondGate.Work()));
			const bool retireAdmission = retireCoordinator.TrySubmitBatch(std::move(retireBatch), retireRejection);
			// Retire the complete batch after the first child starts.  This remains
			// deterministic even when the shared worker pool has only one worker:
			// the second child may not begin until the first gate is released.
			const bool firstStarted = retireAdmission && firstGate.WaitUntilStarted();
			retireCoordinator.RetireCurrent();
			const NavigationAsyncBuildStatus retired = retireCoordinator.Status();
			std::string blockedRejection;
			const bool blockedWhileRetired = !retireCoordinator.TrySubmit(
				Identity(642U), retireBudget, 1U, [] {}, blockedRejection);
			firstGate.Release();
			const bool secondStarted = retireAdmission && secondGate.WaitUntilStarted();
			secondGate.Release();
			const std::vector<NavigationAsyncBuildCompletion> retiredCompletions =
				PollUntilCount(retireCoordinator, 2U);
			const NavigationAsyncBuildStatus drained = retireCoordinator.Status();

			context.Expect(failureAdmission && failureRejection.empty() && oneSucceededOneFailed
				&& failureStatus.completedCount == 2U && failureStatus.groundCompletedCount == 1U
				&& failureStatus.dynamicCompletedCount == 1U
				&& firstStarted && secondStarted && retired.retiredCount == 2U && retired.chargedBytes == 64U
				&& blockedWhileRetired && !blockedRejection.empty()
				&& retiredCompletions.size() == 2U
				&& std::all_of(retiredCompletions.begin(), retiredCompletions.end(), [](const auto& completion) {
					return completion.retired;
				})
				&& drained.completedCount == 2U && drained.staleDiscardCount == 2U
				&& drained.currentCount == 0U && drained.retiredCount == 0U
				&& drained.chargedBytes == 0U && retireBudget->UsedBytes(NavigationBudgetCategory::BuildScratch) == 0U,
				"A batch child failure must remain independently observable, while retiring a batch keeps every child permit charged and capacity-blocking until both completions are polled.");
		});
	}
}
