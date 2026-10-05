// AnimationProgramPublisherTests.cpp - Deferred graph compilation publication contracts.

#include "TestHarness.h"

#include "Animation/Graph/AnimationProgramPublisher.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace TerraEngine::Tests {
namespace {
using namespace TerraEngine::Animation;

class ManualCompileExecutor final : public AnimationProgramCompileExecutor {
public:
	void Enqueue(std::function<void()> work) override {
		if (rejectNext_) {
			rejectNext_ = false;
			throw std::runtime_error("Controlled compiler enqueue failure");
		}
		work_.push_back(std::move(work));
	}

	void RejectNext() noexcept { rejectNext_ = true; }

	[[nodiscard]] std::size_t PendingCount() const noexcept {
		return work_.size();
	}

	void RunNext() {
		std::function<void()> work = std::move(work_.front());
		work_.erase(work_.begin());
		work();
	}

	void RunLast() {
		std::function<void()> work = std::move(work_.back());
		work_.pop_back();
		work();
	}

	// Real Task::run retains an escaped worker exception in its future. This
	// manual executor models that discarded-future boundary for deterministic
	// ordering tests; the separate pool case exercises the actual Task path.
	void RunNextAsTask() { try { RunNext(); } catch (...) {} }
	void RunLastAsTask() { try { RunLast(); } catch (...) {} }

private:
	std::vector<std::function<void()>> work_;
	bool rejectNext_ = false;
};

AnimationGuid Guid(std::uint8_t seed) {
	AnimationGuid value;
	for (std::size_t index = 0U; index < value.value.size(); ++index)
		value.value[index] = static_cast<std::uint8_t>(seed + index);
	return value;
}

AnimationControllerAsset MakeController(std::uint8_t seed = 1U) {
	AnimationControllerAsset controller;
	controller.assetId = Guid(seed);
	controller.skeletonAssetId = Guid(40U);
	controller.compatibilityFingerprint = 0x4545U;
	controller.nodes = {{1U, AnimationControllerNodeKind::ProceduralPose, 0U, {}, 0U, 0U}};
	controller.states = {{2U, 1U, 0U, true, false, false}};
	return controller;
}

AnimationProgramCompileRequest MakeRequest(AnimationGeneration sourceGeneration = 1U) {
	AnimationProgramCompileRequest request;
	request.controller = MakeController();
	request.sourceGeneration = sourceGeneration;
	return request;
}

AnimationProgramGenerationSnapshot SnapshotFor(const AnimationProgramCompileRequest &request) {
	return {request.controller.assetId, request.sourceGeneration, request.dependencyGenerations};
}

struct WorkerFaultState {
	std::atomic<bool> compilerFailure{false};
	std::atomic<bool> stagingFailure{false};
	std::atomic<std::size_t> compilerCalls{0U};
	std::atomic<std::size_t> stagingCalls{0U};
};

AnimationProgramPublisher::WorkerDependencies FaultDependencies(const std::shared_ptr<WorkerFaultState>& fault) {
	AnimationProgramPublisher::WorkerDependencies dependencies;
	dependencies.compile = [fault](const AnimationProgramCompileRequest& request, EvaluationProgram& output) {
		++fault->compilerCalls;
		if (fault->compilerFailure.load()) throw std::runtime_error("Controlled inside-compiler failure");
		return CompileAnimationControllerGraph(request.controller, {request.availableControllers}, output);
	};
	dependencies.beforeResultStaging = [fault] {
		++fault->stagingCalls;
		if (fault->stagingFailure.load()) throw std::runtime_error("Controlled compiled-result staging failure");
	};
	return dependencies;
}

AnimationProgramPublishPollResult WaitForTerminal(AnimationProgramPublisher& publisher,
	const AnimationProgramCompileRequest& request) {
	// Poll remains non-blocking in production. This bounded test-side wait gives
	// actual workers time to finish without turning publisher polling into a join.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	AnimationProgramPublishPollResult result;
	do {
		result = publisher.PollAndPublish(SnapshotFor(request));
		if (result.status != AnimationStatus::NotReady) return result;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	} while (std::chrono::steady_clock::now() < deadline);
	return result;
}
} // namespace

void RegisterAnimationProgramPublisherTests(TestRegistry &registry) {
	registry.Add("Animation.ProgramPublisher.WorkerCompilerExceptionsReclaimCapacity", [](TestContext& context) {
		constexpr std::size_t capacity = 2U;
		auto executor = std::make_shared<ManualCompileExecutor>();
		auto fault = std::make_shared<WorkerFaultState>();
		AnimationProgramPublisher publisher(executor, capacity, FaultDependencies(fault));
		const auto initial = MakeRequest();
		context.Expect(publisher.RequestCompile(initial) == AnimationStatus::Queued, "A real valid compiler result must establish the initial publication.");
		executor->RunNext();
		context.Expect(publisher.PollAndPublish(SnapshotFor(initial)).published, "The initial program must publish before injected worker failures.");
		const auto lastGood = publisher.GetPublishedRecord();
		fault->compilerFailure = true;
		for (std::size_t round = 0U; round < 2U; ++round) {
			const auto older = MakeRequest(2U + round * capacity);
			const auto newer = MakeRequest(3U + round * capacity);
			context.Expect(publisher.RequestCompile(older) == AnimationStatus::Queued && publisher.RequestCompile(newer) == AnimationStatus::Queued
				&& publisher.RequestCompile(newer) == AnimationStatus::Rejected, "Each failed batch must fill exactly the configured pending capacity.");
			executor->RunNextAsTask(); executor->RunNextAsTask();
			const auto terminal = publisher.PollAndPublish(SnapshotFor(newer));
			context.Expect(terminal.status == AnimationStatus::InternalError && !terminal.published && publisher.GetPublishedRecord() == lastGood,
				"Exceptions inside compilation must settle the newest failure and preserve the complete last-good record.");
			context.Expect(publisher.PollAndPublish(SnapshotFor(newer)).status == AnimationStatus::NotReady,
				"A terminal failed batch must be consumed once instead of retaining pending capacity.");
		}
		context.Expect(fault->compilerCalls == 1U + 2U * capacity && fault->stagingCalls == 1U,
			"At least twice capacity must fail inside compiler execution, independently of result staging or enqueue.");
		fault->compilerFailure = false;
		const auto recovered = MakeRequest(10U);
		context.Expect(publisher.RequestCompile(recovered) == AnimationStatus::Queued, "Failed compiler jobs must release capacity for a later valid request.");
		executor->RunNext();
		context.Expect(publisher.PollAndPublish(SnapshotFor(recovered)).published && publisher.GetPublishedRecord()->programGeneration == 2U,
			"Successful recovery must publish exactly the next last-good program generation.");
	});

	registry.Add("Animation.ProgramPublisher.WorkerStagingExceptionsPreserveLastGood", [](TestContext& context) {
		constexpr std::size_t capacity = 2U;
		auto executor = std::make_shared<ManualCompileExecutor>();
		auto fault = std::make_shared<WorkerFaultState>();
		AnimationProgramPublisher publisher(executor, capacity, FaultDependencies(fault));
		const auto initial = MakeRequest();
		context.Expect(publisher.RequestCompile(initial) == AnimationStatus::Queued, "The staging fixture must admit a valid initial graph.");
		executor->RunNext();
		context.Expect(publisher.PollAndPublish(SnapshotFor(initial)).published, "The staging fixture must start with an actual last-good program.");
		const auto lastGood = publisher.GetPublishedRecord();
		fault->stagingFailure = true;
		for (std::size_t index = 0U; index < 2U * capacity; ++index) {
			const auto request = MakeRequest(2U + index);
			context.Expect(publisher.RequestCompile(request) == AnimationStatus::Queued, "Every prior staging failure must relinquish capacity.");
			executor->RunNextAsTask();
			const auto terminal = publisher.PollAndPublish(SnapshotFor(request));
			context.Expect(terminal.status == AnimationStatus::InternalError && !terminal.published && publisher.GetPublishedRecord() == lastGood,
				"A result-staging exception after real compilation must settle without replacing last-good output.");
		}
		context.Expect(fault->compilerCalls == 1U + 2U * capacity && fault->stagingCalls == 1U + 2U * capacity,
			"The staging fault must be reached after each real compiler execution, not injected by enqueue.");
		fault->stagingFailure = false;
		const auto recovered = MakeRequest(10U);
		context.Expect(publisher.RequestCompile(recovered) == AnimationStatus::Queued, "Repeated staging failures must not exhaust the owner.");
		executor->RunNext();
		context.Expect(publisher.PollAndPublish(SnapshotFor(recovered)).published && publisher.GetPublishedRecord()->programGeneration == 2U,
			"A valid staged result must recover with no skipped published generation.");
	});

	registry.Add("Animation.ProgramPublisher.ExceptionalNewestRequestPreventsOlderPublication", [](TestContext& context) {
		auto executor = std::make_shared<ManualCompileExecutor>();
		AnimationProgramPublisher::WorkerDependencies dependencies;
		dependencies.compile = [](const AnimationProgramCompileRequest& request, EvaluationProgram& output) {
			if (request.sourceGeneration == 3U) throw std::runtime_error("Newest request compiler failed");
			return CompileAnimationControllerGraph(request.controller, {request.availableControllers}, output);
		};
		AnimationProgramPublisher publisher(executor, 2U, std::move(dependencies));
		const auto initial = MakeRequest();
		context.Expect(publisher.RequestCompile(initial) == AnimationStatus::Queued, "The ordering fixture must admit its initial program.");
		executor->RunNext(); context.Expect(publisher.PollAndPublish(SnapshotFor(initial)).published, "The ordering fixture must establish last-good output.");
		const auto lastGood = publisher.GetPublishedRecord();
		const auto older = MakeRequest(2U); const auto newest = MakeRequest(3U);
		context.Expect(publisher.RequestCompile(older) == AnimationStatus::Queued && publisher.RequestCompile(newest) == AnimationStatus::Queued,
			"Both source generations must be admitted before reversing their physical completion order.");
		executor->RunLastAsTask();
		context.Expect(publisher.PollAndPublish(SnapshotFor(newest)).status == AnimationStatus::InternalError && publisher.GetPublishedRecord() == lastGood,
			"A newest compiler exception must become terminal while retaining last-good output.");
		executor->RunNext();
		context.Expect(publisher.PollAndPublish(SnapshotFor(newest)).status == AnimationStatus::StaleHandle && publisher.GetPublishedRecord() == lastGood,
			"A later physical completion from an older request must not publish through the failed newest request.");
		const auto recovered = MakeRequest(4U);
		context.Expect(publisher.RequestCompile(recovered) == AnimationStatus::Queued, "Ordering rejection must also reclaim capacity.");
		executor->RunNext(); context.Expect(publisher.PollAndPublish(SnapshotFor(recovered)).published, "A newer valid request must recover after inverted exceptional completion.");
	});

	registry.Add("Animation.ProgramPublisher.CancelledAndRetiredWorkersCannotResurrect", [](TestContext& context) {
		auto executor = std::make_shared<ManualCompileExecutor>();
		auto fault = std::make_shared<WorkerFaultState>();
		AnimationProgramPublisher publisher(executor, 1U, FaultDependencies(fault));
		const auto cancelled = MakeRequest();
		context.Expect(publisher.RequestCompile(cancelled) == AnimationStatus::Queued, "A worker must be admitted before cancellation.");
		publisher.CancelPending();
		context.Expect(publisher.PollAndPublish(SnapshotFor(cancelled)).status == AnimationStatus::Cancelled,
			"Cancellation must be logically consumed without waiting for physical execution.");
		const auto recovered = MakeRequest(2U);
		context.Expect(publisher.RequestCompile(recovered) == AnimationStatus::Queued, "A logically consumed cancellation must immediately release capacity.");
		fault->stagingFailure = true; executor->RunNextAsTask(); fault->stagingFailure = false;
		context.Expect(publisher.PollAndPublish(SnapshotFor(recovered)).status == AnimationStatus::NotReady && !publisher.GetPublishedRecord(),
			"A late cancelled worker's staging exception cannot resurrect a removed job or publish an old program.");
		executor->RunNext(); context.Expect(publisher.PollAndPublish(SnapshotFor(recovered)).published, "The replacement request must remain eligible after late cancellation settlement.");
		const auto lastGood = publisher.GetPublishedRecord();
		auto lifetime = std::make_shared<int>(1); const std::weak_ptr<int> observedLifetime = lifetime;
		AnimationProgramPublisher::WorkerDependencies retiredDependencies;
		retiredDependencies.compile = [lifetime](const AnimationProgramCompileRequest&, EvaluationProgram&) -> AnimationGraphCompilationResult {
			throw std::runtime_error("Retired owner compiler failure");
		};
		{
			AnimationProgramPublisher retired(executor, 1U, std::move(retiredDependencies));
			context.Expect(retired.RequestCompile(MakeRequest(3U)) == AnimationStatus::Queued, "The retirement fixture must own an admitted worker.");
		}
		lifetime.reset();
		context.Expect(!observedLifetime.expired(), "The queued worker must retain its immutable dependencies after publisher destruction.");
		executor->RunNextAsTask();
		context.Expect(observedLifetime.expired() && publisher.GetPublishedRecord() == lastGood,
			"Late retired compilation must release owned captures without accessing a destroyed publisher or changing another owner's publication.");
	});

	registry.Add("Animation.ProgramPublisher.EnqueueFailureSettlesOnce", [](TestContext& context) {
		auto executor = std::make_shared<ManualCompileExecutor>();
		auto fault = std::make_shared<WorkerFaultState>();
		AnimationProgramPublisher publisher(executor, 1U, FaultDependencies(fault));
		const auto request = MakeRequest(); executor->RejectNext();
		context.Expect(publisher.RequestCompile(request) == AnimationStatus::Queued && executor->PendingCount() == 0U && fault->compilerCalls == 0U,
			"An enqueue exception must be distinct from an admitted worker compiler exception.");
		context.Expect(publisher.PollAndPublish(SnapshotFor(request)).status == AnimationStatus::InternalError
			&& publisher.PollAndPublish(SnapshotFor(request)).status == AnimationStatus::NotReady,
			"Enqueue failure must settle and reclaim the request exactly once.");
		context.Expect(publisher.RequestCompile(request) == AnimationStatus::Queued, "An executor admission failure must leave capacity available for retry.");
		executor->RunNext(); context.Expect(publisher.PollAndPublish(SnapshotFor(request)).published, "A valid retry must publish after enqueue rejection.");
	});

	registry.Add("Animation.ProgramPublisher.ThreadPoolWorkerFailuresRecoverCapacity", [](TestContext& context) {
		constexpr std::size_t capacity = 2U;
		// This executor is private to the fixture; global engine worker admission
		// and profiler state remain owned by the surrounding harness.
		auto pool = ThreadPool::CreateOwned(2U);
		auto fault = std::make_shared<WorkerFaultState>();
		AnimationProgramPublisher publisher(CreateThreadPoolAnimationProgramCompileExecutor(*pool), capacity, FaultDependencies(fault));
		const auto initial = MakeRequest();
		context.Expect(publisher.RequestCompile(initial) == AnimationStatus::Queued, "The real pool must accept initial immutable compiler work.");
		context.Expect(WaitForTerminal(publisher, initial).published, "The production compiler must publish through the real ThreadPool adapter.");
		const auto lastGood = publisher.GetPublishedRecord();
		fault->compilerFailure = true;
		for (std::size_t index = 0U; index < 2U * capacity; ++index) {
			const auto request = MakeRequest(2U + index);
			context.Expect(publisher.RequestCompile(request) == AnimationStatus::Queued, "A terminal failure must reclaim real worker capacity before the next request.");
			const auto terminal = WaitForTerminal(publisher, request);
			context.Expect(terminal.status == AnimationStatus::InternalError && !terminal.published && publisher.GetPublishedRecord() == lastGood,
				"An exception retained by a discarded Task future must still settle publisher ownership and preserve last-good output.");
		}
		context.Expect(fault->compilerCalls == 1U + 2U * capacity, "Actual pool workers must execute at least twice capacity inside-compiler failures.");
		fault->compilerFailure = false;
		const auto recovered = MakeRequest(10U);
		context.Expect(publisher.RequestCompile(recovered) == AnimationStatus::Queued && WaitForTerminal(publisher, recovered).published,
			"Real worker exceptions must not leave jobs permanently pending or prevent a valid recovery publication.");
		context.Expect(publisher.GetPublishedRecord()->programGeneration == 2U, "Only successful real-pool publications advance program generation.");
	});

	registry.Add("Animation.ProgramPublisher.PublishesOnlyCurrentImmutablePrograms", [](TestContext &context) {
		auto executor = std::make_shared<ManualCompileExecutor>();
		AnimationProgramPublisher publisher(executor, 2U);
		const AnimationProgramCompileRequest first = MakeRequest(4U);

		context.Expect(publisher.RequestCompile(first) == AnimationStatus::Queued && executor->PendingCount() == 1U &&
					   publisher.PollAndPublish(SnapshotFor(first)).status == AnimationStatus::NotReady,
					   "The protected publish boundary must never wait for worker compilation.");
		executor->RunNext();
		const AnimationProgramPublishPollResult result = publisher.PollAndPublish(SnapshotFor(first));
		const auto record = publisher.GetPublishedRecord();
		context.Expect(result.status == AnimationStatus::Ok && result.published && record.has_value() &&
					   record->controllerAssetId == first.controller.assetId && record->sourceGeneration == 4U &&
					   record->programGeneration == 1U && record->program != nullptr &&
					   record->programHash == record->program->stableHash,
					   "A completed current request must atomically publish one immutable, generation-labelled program record.");
	});

	registry.Add("Animation.ProgramPublisher.PreservesLastGoodForFailuresStaleAndLateWorkers", [](TestContext &context) {
		auto executor = std::make_shared<ManualCompileExecutor>();
		AnimationProgramPublisher publisher(executor, 3U);
		AnimationProgramCompileRequest good = MakeRequest(1U);
		context.Expect(publisher.RequestCompile(good) == AnimationStatus::Queued, "The initial valid request should queue.");
		executor->RunNext();
		context.Expect(publisher.PollAndPublish(SnapshotFor(good)).published, "The initial program establishes last-known-good state.");
		const auto lastGood = publisher.GetPublishedRecord();

		AnimationProgramCompileRequest invalid = MakeRequest(2U);
		invalid.controller.states.front().entry = false;
		context.Expect(publisher.RequestCompile(invalid) == AnimationStatus::Queued, "Invalid authored input remains a worker compile request.");
		executor->RunNext();
		context.Expect(publisher.PollAndPublish(SnapshotFor(invalid)).status == AnimationStatus::Rejected &&
					   publisher.GetPublishedRecord() == lastGood,
					   "Compiler failures must leave the entire prior published record untouched.");

		AnimationProgramCompileRequest stale = MakeRequest(3U);
		context.Expect(publisher.RequestCompile(stale) == AnimationStatus::Queued, "A new source generation should queue.");
		executor->RunNext();
		AnimationProgramGenerationSnapshot newerSnapshot = SnapshotFor(stale);
		++newerSnapshot.sourceGeneration;
		context.Expect(publisher.PollAndPublish(newerSnapshot).status == AnimationStatus::StaleHandle &&
					   publisher.GetPublishedRecord() == lastGood,
					   "A source-generation mismatch must reject without replacing last-known-good output.");

		AnimationProgramCompileRequest dependencyStale = MakeRequest(4U);
		dependencyStale.controller.subgraphDependencies = {{9U, Guid(90U), 7U, dependencyStale.controller.compatibilityFingerprint}};
		dependencyStale.dependencyGenerations = {{Guid(90U), 7U}};
		dependencyStale.availableControllers = {MakeController(90U)};
		dependencyStale.availableControllers.front().assetId = Guid(90U);
		dependencyStale.availableControllers.front().compatibilityFingerprint = dependencyStale.controller.compatibilityFingerprint;
		context.Expect(publisher.RequestCompile(dependencyStale) == AnimationStatus::Queued,
					   "A request should preserve the dependency generation it was compiled against.");
		executor->RunNext();
		AnimationProgramGenerationSnapshot changedDependency = SnapshotFor(dependencyStale);
		++changedDependency.dependencyGenerations.front().generation;
		context.Expect(publisher.PollAndPublish(changedDependency).status == AnimationStatus::StaleHandle &&
					   publisher.GetPublishedRecord() == lastGood,
					   "A dependency-generation mismatch must reject without replacing last-known-good output.");

		AnimationProgramCompileRequest older = MakeRequest(5U);
		AnimationProgramCompileRequest newer = MakeRequest(6U);
		context.Expect(publisher.RequestCompile(older) == AnimationStatus::Queued && publisher.RequestCompile(newer) == AnimationStatus::Queued,
					   "Two source generations should be independently queued for late-completion ordering coverage.");
		executor->RunLast();
		context.Expect(publisher.PollAndPublish(SnapshotFor(newer)).published, "The newest completed request should publish.");
		const auto newerRecord = publisher.GetPublishedRecord();
		executor->RunNext();
		context.Expect(publisher.PollAndPublish(SnapshotFor(newer)).status == AnimationStatus::StaleHandle &&
					   publisher.GetPublishedRecord() == newerRecord,
					   "An older late completion must not roll back a newer published generation.");
	});

	registry.Add("Animation.ProgramPublisher.ValidatesDependenciesCancellationAndCapacity", [](TestContext &context) {
		auto executor = std::make_shared<ManualCompileExecutor>();
		AnimationProgramPublisher publisher(executor, 1U);
		AnimationProgramCompileRequest dependencyRequest = MakeRequest(1U);
		dependencyRequest.controller.subgraphDependencies = {{9U, Guid(90U), 7U, dependencyRequest.controller.compatibilityFingerprint}};
		dependencyRequest.dependencyGenerations = {{Guid(90U), 7U}};
		dependencyRequest.availableControllers = {MakeController(90U)};
		dependencyRequest.availableControllers.front().assetId = Guid(90U);
		dependencyRequest.availableControllers.front().compatibilityFingerprint = dependencyRequest.controller.compatibilityFingerprint;
		context.Expect(publisher.RequestCompile(dependencyRequest) == AnimationStatus::Queued &&
					   publisher.RequestCompile(dependencyRequest) == AnimationStatus::Rejected,
					   "The publisher must bound outstanding worker work instead of accumulating unbounded requests.");
		publisher.CancelPending();
		executor->RunNext();
		context.Expect(publisher.PollAndPublish(SnapshotFor(dependencyRequest)).status == AnimationStatus::Cancelled &&
					   !publisher.GetPublishedRecord().has_value(),
					   "Cancellation must be safe after enqueue and preserve the absence of a last-known-good program.");

		auto snapshotExecutor = std::make_shared<ManualCompileExecutor>();
		AnimationProgramPublisher snapshotPublisher(snapshotExecutor, 1U);
		context.Expect(snapshotPublisher.RequestCompile(dependencyRequest) == AnimationStatus::Queued,
					   "A valid dependency generation snapshot should queue for publication validation.");
		snapshotExecutor->RunNext();
		AnimationProgramGenerationSnapshot duplicateSnapshot = SnapshotFor(dependencyRequest);
		duplicateSnapshot.dependencyGenerations.push_back(duplicateSnapshot.dependencyGenerations.front());
		context.Expect(snapshotPublisher.PollAndPublish(duplicateSnapshot).status == AnimationStatus::StaleHandle &&
					   !snapshotPublisher.GetPublishedRecord().has_value(),
					   "Protected publication must reject duplicate dependency asset IDs even when their generations match.");

		auto unorderedExecutor = std::make_shared<ManualCompileExecutor>();
		AnimationProgramPublisher unorderedPublisher(unorderedExecutor, 1U);
		AnimationProgramCompileRequest unorderedRequest = MakeRequest(2U);
		unorderedRequest.controller.nodes.push_back(
			{3U, AnimationControllerNodeKind::Clip, 0U, Guid(94U), 2U, unorderedRequest.controller.compatibilityFingerprint});
		unorderedRequest.controller.layers = {{12U, 2U, Guid(93U), 1U, unorderedRequest.controller.compatibilityFingerprint, false, 1.0F}};
		unorderedRequest.dependencyGenerations = {{Guid(94U), 2U}, {Guid(93U), 1U}};
		context.Expect(unorderedPublisher.RequestCompile(unorderedRequest) == AnimationStatus::Queued,
					   "Request canonicalization should accept dependencies supplied in a non-canonical caller order.");
		unorderedExecutor->RunNext();
		AnimationProgramGenerationSnapshot reversedSnapshot = SnapshotFor(unorderedRequest);
		std::reverse(reversedSnapshot.dependencyGenerations.begin(), reversedSnapshot.dependencyGenerations.end());
		context.Expect(unorderedPublisher.PollAndPublish(reversedSnapshot).published &&
					   unorderedPublisher.GetPublishedRecord()->dependencyGenerations.size() == 2U,
					   "Publication re-canonicalizes caller snapshot order while retaining one entry per dependency asset.");

		AnimationProgramCompileRequest mismatchedDependency = MakeRequest(2U);
		mismatchedDependency.controller.subgraphDependencies = {{10U, Guid(91U), 2U, mismatchedDependency.controller.compatibilityFingerprint}};
		mismatchedDependency.dependencyGenerations = {{Guid(91U), 3U}};
		context.Expect(publisher.RequestCompile(mismatchedDependency) == AnimationStatus::InvalidArgument,
					   "Dependency snapshots must exactly copy the generations recorded by the immutable controller input.");

		AnimationProgramCompileRequest duplicateDependency = MakeRequest(3U);
		duplicateDependency.controller.subgraphDependencies = {{11U, Guid(92U), 2U, duplicateDependency.controller.compatibilityFingerprint}};
		duplicateDependency.controller.nodes.push_back(
			{3U, AnimationControllerNodeKind::Clip, 0U, Guid(92U), 2U, duplicateDependency.controller.compatibilityFingerprint});
		duplicateDependency.dependencyGenerations = {{Guid(92U), 2U}};
		context.Expect(publisher.RequestCompile(duplicateDependency) == AnimationStatus::Queued,
					   "Identical authored references must coalesce into the one canonical dependency snapshot entry.");
		duplicateDependency.controller.nodes.back().dependencyGeneration = 3U;
		context.Expect(publisher.RequestCompile(duplicateDependency) == AnimationStatus::InvalidArgument,
					   "Conflicting dependency generations for one asset must be rejected before worker work is admitted.");

		auto oversizedExecutor = std::make_shared<ManualCompileExecutor>();
		AnimationProgramPublisher oversizedPublisher(oversizedExecutor, kAnimationProgramPublisherMaximumPendingJobs + 1U);
		bool allWithinHardCeilingQueued = true;
		for (std::size_t index = 0U; index < kAnimationProgramPublisherMaximumPendingJobs; ++index) {
			AnimationProgramCompileRequest queued = MakeRequest(static_cast<AnimationGeneration>(index + 1U));
			allWithinHardCeilingQueued = allWithinHardCeilingQueued && oversizedPublisher.RequestCompile(queued) == AnimationStatus::Queued;
		}
		context.Expect(allWithinHardCeilingQueued &&
					   oversizedPublisher.RequestCompile(MakeRequest(static_cast<AnimationGeneration>(kAnimationProgramPublisherMaximumPendingJobs + 1U))) ==
						   AnimationStatus::Rejected,
					   "Oversized caller configuration must still be clamped to the fixed pending-work hard ceiling.");
	});
}
} // namespace TerraEngine::Tests
