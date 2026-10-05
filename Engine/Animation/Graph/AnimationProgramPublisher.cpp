// AnimationProgramPublisher.cpp - Generation-checked asynchronous program publication.

#include "Animation/Graph/AnimationProgramPublisher.h"

#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <type_traits>
#include <utility>

namespace TerraEngine::Animation {
namespace {
class ThreadPoolAnimationProgramCompileExecutor final : public AnimationProgramCompileExecutor {
public:
	explicit ThreadPoolAnimationProgramCompileExecutor(ThreadPool &pool)
		: pool_(pool) {}

	void Enqueue(std::function<void()> work) override {
		// The task owns its function, so the pool never keeps a pointer into the
		// publisher or caller stack after this method returns.
		auto task = pool_.createTask(std::move(work));
		pool_.enqueue(task);
	}

private:
	ThreadPool &pool_;
};

void Canonicalize(std::vector<AnimationProgramDependencyGeneration> &dependencies) {
	std::sort(dependencies.begin(), dependencies.end(), [](const auto &left, const auto &right) {
		return left.assetId != right.assetId ? left.assetId < right.assetId : left.generation < right.generation;
	});
}

bool IsCanonicalUniqueDependencyList(const std::vector<AnimationProgramDependencyGeneration> &dependencies) noexcept {
	if (!std::is_sorted(dependencies.begin(), dependencies.end(), [](const auto &left, const auto &right) {
		return left.assetId != right.assetId ? left.assetId < right.assetId : left.generation < right.generation;
	}))
		return false;
	if (!std::all_of(dependencies.begin(), dependencies.end(), [](const auto &dependency) {
			return dependency.assetId.IsValid() && dependency.generation != 0U;
	}))
		return false;
	return std::adjacent_find(dependencies.begin(), dependencies.end(), [](const auto &left, const auto &right) {
		return left.assetId == right.assetId;
	}) == dependencies.end();
}

std::optional<std::vector<AnimationProgramDependencyGeneration>> CollectDependencies(const AnimationControllerAsset &controller) {
	std::vector<AnimationProgramDependencyGeneration> dependencies;
	dependencies.reserve(controller.subgraphDependencies.size() + controller.nodes.size() + controller.layers.size());
	for (const AnimationControllerSubgraphDependency &dependency : controller.subgraphDependencies)
		dependencies.push_back({dependency.controllerAssetId, dependency.generation});
	for (const AnimationControllerNode &node : controller.nodes)
		if (node.dependencyAssetId.IsValid())
			dependencies.push_back({node.dependencyAssetId, node.dependencyGeneration});
	for (const AnimationControllerLayer &layer : controller.layers)
		if (layer.maskAssetId.IsValid())
			dependencies.push_back({layer.maskAssetId, layer.maskGeneration});
	Canonicalize(dependencies);

	// Authored graphs may sample one clip, mask, or subgraph from several nodes.
	// The publication snapshot names each asset once, so coalesce equal repeated
	// references but reject a single asset that claims two incompatible versions.
	std::vector<AnimationProgramDependencyGeneration> coalesced;
	coalesced.reserve(dependencies.size());
	for (const AnimationProgramDependencyGeneration &dependency : dependencies) {
		if (!dependency.assetId.IsValid() || dependency.generation == 0U)
			return std::nullopt;
		if (coalesced.empty() || coalesced.back().assetId != dependency.assetId) {
			coalesced.push_back(dependency);
			continue;
		}
		if (coalesced.back().generation != dependency.generation)
			return std::nullopt;
	}
	return coalesced;
}

bool MatchesSnapshot(const AnimationProgramCompileRequest &request, const AnimationProgramGenerationSnapshot &current) {
	if (request.controller.assetId != current.controllerAssetId || request.sourceGeneration != current.sourceGeneration)
		return false;
	try {
		// Asset owners are free to enumerate their immutable generations in any
		// order. Canonicalizing this local copy keeps the protected boundary
		// deterministic without retaining caller-owned mutable storage.
		std::vector<AnimationProgramDependencyGeneration> canonicalCurrent = current.dependencyGenerations;
		Canonicalize(canonicalCurrent);
		return IsCanonicalUniqueDependencyList(canonicalCurrent) && request.dependencyGenerations == canonicalCurrent;
	} catch (...) {
		// A failed temporary snapshot allocation must not replace last-known-good
		// output or let a partially inspected generation pass validation.
		return false;
	}
}

AnimationStatus ValidateRequest(AnimationProgramCompileRequest &request) {
	if (!request.controller.assetId.IsValid() || request.sourceGeneration == 0U)
		return AnimationStatus::InvalidArgument;
	Canonicalize(request.dependencyGenerations);
	const std::optional<std::vector<AnimationProgramDependencyGeneration>> collected = CollectDependencies(request.controller);
	if (!IsCanonicalUniqueDependencyList(request.dependencyGenerations) || !collected.has_value() ||
		request.dependencyGenerations != *collected)
		return AnimationStatus::InvalidArgument;
	return AnimationStatus::Ok;
}
} // namespace

struct AnimationProgramPublisher::SharedState {
	struct Job {
		std::uint64_t requestSequence = 0U;
		AnimationProgramCompileRequest request{};
		bool cancelled = false;
		bool completed = false;
		AnimationGraphCompilationResult compilation{};
		EvaluationProgram program{};
	};

	mutable std::mutex mutex;
	bool shuttingDown = false;
	std::size_t maxPendingJobs = 0U;
	AnimationGuid controllerAssetId{};
	std::uint64_t newestRequestSequence = 0U;
	AnimationGeneration nextProgramGeneration = 1U;
	std::vector<std::shared_ptr<Job>> jobs;
	std::optional<AnimationProgramPublishRecord> published;
	// Keep injected immutable dependencies behind the existing state pointer so
	// adding native worker seams does not enlarge the public publisher object.
	WorkerDependencies workerDependencies;

	// The caller holds mutex for every terminal transition. A cancelled job may
	// already have been consumed by polling while its physical worker continues;
	// neither that worker nor a late executor exception can revive its ownership.
	[[nodiscard]] bool CanSettle(const Job& job) const noexcept {
		return !shuttingDown && !job.cancelled && !job.completed;
	}

	void SettleInternalError(Job& job) noexcept {
		if (!CanSettle(job)) return;
		// Failure must not allocate another diagnostic while handling an exception.
		// This scalar result is sufficient for protected polling to reclaim the job
		// and preserve the newest-request barrier and last-known-good publication.
		job.compilation.status = AnimationStatus::InternalError;
		job.completed = true;
	}
};

std::shared_ptr<AnimationProgramCompileExecutor> CreateThreadPoolAnimationProgramCompileExecutor(ThreadPool &pool) {
	return std::make_shared<ThreadPoolAnimationProgramCompileExecutor>(pool);
}

AnimationProgramPublisher::AnimationProgramPublisher(std::shared_ptr<AnimationProgramCompileExecutor> executor, std::size_t maxPendingJobs)
	: AnimationProgramPublisher(std::move(executor), maxPendingJobs, {}) {}

AnimationProgramPublisher::AnimationProgramPublisher(std::shared_ptr<AnimationProgramCompileExecutor> executor,
	std::size_t maxPendingJobs, WorkerDependencies workerDependencies)
	: state_(std::make_shared<SharedState>()), executor_(std::move(executor)) {
	state_->maxPendingJobs = std::min(maxPendingJobs, kAnimationProgramPublisherMaximumPendingJobs);
	state_->workerDependencies = std::move(workerDependencies);
	if (!state_->workerDependencies.compile) {
		state_->workerDependencies.compile = [](const AnimationProgramCompileRequest& request, EvaluationProgram& output) {
			return CompileAnimationControllerGraph(request.controller, {request.availableControllers}, output);
		};
	}
}

AnimationProgramPublisher::~AnimationProgramPublisher() {
	CancelPending();
	std::scoped_lock lock(state_->mutex);
	state_->shuttingDown = true;
}

AnimationStatus AnimationProgramPublisher::RequestCompile(const AnimationProgramCompileRequest &input) {
	if (executor_ == nullptr || state_->maxPendingJobs == 0U)
		return AnimationStatus::InvalidArgument;
	AnimationProgramCompileRequest request = input;
	const AnimationStatus validation = ValidateRequest(request);
	if (validation != AnimationStatus::Ok)
		return validation;

	std::shared_ptr<SharedState::Job> job;
	try {
		job = std::make_shared<SharedState::Job>();
	} catch (...) {
		return AnimationStatus::InternalError;
	}
	{
		std::scoped_lock lock(state_->mutex);
		if (state_->shuttingDown)
			return AnimationStatus::Cancelled;
		if (state_->jobs.size() >= state_->maxPendingJobs)
			return AnimationStatus::Rejected;
		if (state_->controllerAssetId.IsValid() && state_->controllerAssetId != request.controller.assetId)
			return AnimationStatus::InvalidArgument;
		if (state_->newestRequestSequence == std::numeric_limits<std::uint64_t>::max())
			return AnimationStatus::RangeOverflow;
		job->requestSequence = state_->newestRequestSequence + 1U;
		job->request = std::move(request);
		try {
			state_->jobs.push_back(job);
		} catch (...) {
			return AnimationStatus::InternalError;
		}
		state_->controllerAssetId = job->request.controller.assetId;
		state_->newestRequestSequence = job->requestSequence;
	}

	// Only captured shared ownership crosses the asynchronous boundary. The
	// publisher object itself may disappear immediately after RequestCompile.
	const std::shared_ptr<SharedState> state = state_;
	try {
		executor_->Enqueue([state, job, dependencies = state->workerDependencies]() {
			EvaluationProgram staged;
			AnimationGraphCompilationResult compilation;
			try {
				// Compilation and all fallible result preparation belong to this worker.
				// No task future is needed to carry terminal ownership back to polling.
				compilation = dependencies.compile(job->request, staged);
				if (dependencies.beforeResultStaging) dependencies.beforeResultStaging();
			} catch (...) {
				std::scoped_lock lock(state->mutex);
				state->SettleInternalError(*job);
				return;
			}
			std::scoped_lock lock(state->mutex);
			if (!state->CanSettle(*job))
				return;
			// Default-allocator vectors transfer their storage without allocation.
			// Assert that property here rather than copying diagnostics under mutex
			// and risking an exception before the job becomes terminal.
			static_assert(std::is_nothrow_move_assignable_v<AnimationGraphCompilationResult>);
			static_assert(std::is_nothrow_move_assignable_v<EvaluationProgram>);
			job->compilation = std::move(compilation);
			if (job->compilation.status == AnimationStatus::Ok)
				job->program = std::move(staged);
			job->completed = true;
		});
	} catch (...) {
		// An executor failure is treated like a completed failed compile. This
		// preserves ordering: an older job cannot publish through this request.
		std::scoped_lock lock(state_->mutex);
		state_->SettleInternalError(*job);
	}
	return AnimationStatus::Queued;
}

void AnimationProgramPublisher::CancelPending() noexcept {
	if (state_ == nullptr)
		return;
	std::scoped_lock lock(state_->mutex);
	for (const std::shared_ptr<SharedState::Job> &job : state_->jobs)
		job->cancelled = true;
}

AnimationProgramPublishPollResult AnimationProgramPublisher::PollAndPublish(const AnimationProgramGenerationSnapshot &current) {
	AnimationProgramPublishPollResult result{};
	std::scoped_lock lock(state_->mutex);
	for (auto iterator = state_->jobs.begin(); iterator != state_->jobs.end();) {
		const std::shared_ptr<SharedState::Job> &job = *iterator;
		if (!job->completed && !job->cancelled) {
			++iterator;
			continue;
		}

		if (job->cancelled) {
			result.status = AnimationStatus::Cancelled;
		} else if (job->requestSequence != state_->newestRequestSequence) {
			// A later request was accepted, so this completion is stale even if it
			// finished first. This deterministic rule prevents rollback by timing.
			result.status = AnimationStatus::StaleHandle;
		} else if (!MatchesSnapshot(job->request, current)) {
			result.status = AnimationStatus::StaleHandle;
		} else if (job->compilation.status != AnimationStatus::Ok) {
			result.status = job->compilation.status;
		} else if (state_->nextProgramGeneration == 0U ||
			state_->nextProgramGeneration == std::numeric_limits<AnimationGeneration>::max()) {
			result.status = AnimationStatus::InternalError;
		} else {
			// Allocate every fallible part of the next immutable snapshot before
			// changing the last-good record or consuming a program generation.
			try {
				AnimationProgramPublishRecord staged{};
				staged.dependencyGenerations = job->request.dependencyGenerations;
				staged.program = std::make_shared<const EvaluationProgram>(std::move(job->program));
				staged.controllerAssetId = job->request.controller.assetId;
				staged.sourceGeneration = job->request.sourceGeneration;
				staged.programGeneration = state_->nextProgramGeneration;
				staged.programHash = staged.program->stableHash;
				// Moving the fully staged record is non-allocating, so this is the
				// atomic last-known-good replacement point.
				state_->published = std::move(staged);
				++state_->nextProgramGeneration;
				result.status = AnimationStatus::Ok;
				result.published = true;
			} catch (...) {
				result.status = AnimationStatus::InternalError;
			}
		}
		iterator = state_->jobs.erase(iterator);
	}
	return result;
}

std::optional<AnimationProgramPublishRecord> AnimationProgramPublisher::GetPublishedRecord() const {
	std::scoped_lock lock(state_->mutex);
	return state_->published;
}
} // namespace TerraEngine::Animation
