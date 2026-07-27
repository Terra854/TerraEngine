// AnimationProgramPublisherTests.cpp - Deferred graph compilation publication contracts.

#include "TestHarness.h"

#include "Animation/Graph/AnimationProgramPublisher.h"

#include <algorithm>
#include <functional>
#include <vector>

namespace TerraEngine::Tests {
namespace {
using namespace TerraEngine::Animation;

class ManualCompileExecutor final : public AnimationProgramCompileExecutor {
public:
	void Enqueue(std::function<void()> work) override {
		work_.push_back(std::move(work));
	}

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

private:
	std::vector<std::function<void()>> work_;
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
} // namespace

void RegisterAnimationProgramPublisherTests(TestRegistry &registry) {
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
