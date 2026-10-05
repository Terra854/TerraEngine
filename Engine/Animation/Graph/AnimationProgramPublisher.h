// AnimationProgramPublisher.h - Non-blocking publication of worker-compiled graph programs.

#pragma once

#include "Animation/Graph/AnimationGraphCompiler.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class ThreadPool;

namespace TerraEngine::Animation {
// This fixed ceiling is independent of caller configuration. It keeps a burst
// of hot-reload requests from retaining more than a bounded amount of copied
// authored graph data when compiler workers are unavailable.
inline constexpr std::size_t kAnimationProgramPublisherMaximumPendingJobs = 64U;

// This compact pair names one external authored dependency and the immutable
// generation that the compiler was allowed to read. It is retained with the
// result so publication can reject a source changed while work was running.
struct AnimationProgramDependencyGeneration {
	AnimationGuid assetId{};
	AnimationGeneration generation = 0U;
	auto operator<=>(const AnimationProgramDependencyGeneration &) const = default;
};

// A request owns every graph document used by its worker. The worker therefore
// never dereferences live asset storage or caller-owned vectors after enqueue.
struct AnimationProgramCompileRequest {
	AnimationControllerAsset controller{};
	AnimationGeneration sourceGeneration = 0U;
	std::vector<AnimationControllerAsset> availableControllers;
	std::vector<AnimationProgramDependencyGeneration> dependencyGenerations;
};

// The protected publication boundary receives a fresh, immutable generation
// snapshot from the asset owner. No publishing decision reads mutable assets.
struct AnimationProgramGenerationSnapshot {
	AnimationGuid controllerAssetId{};
	AnimationGeneration sourceGeneration = 0U;
	std::vector<AnimationProgramDependencyGeneration> dependencyGenerations;
};

// A published record is copied by value, while its program remains immutable
// behind a shared pointer. Readers may retain a record without blocking a later
// publisher update or exposing mutable compiler data.
struct AnimationProgramPublishRecord {
	AnimationGuid controllerAssetId{};
	AnimationGeneration sourceGeneration = 0U;
	AnimationGeneration programGeneration = 0U;
	std::uint64_t programHash = 0U;
	std::vector<AnimationProgramDependencyGeneration> dependencyGenerations;
	std::shared_ptr<const EvaluationProgram> program;
	auto operator<=>(const AnimationProgramPublishRecord &) const = default;
};

struct AnimationProgramPublishPollResult {
	AnimationStatus status = AnimationStatus::NotReady;
	bool published = false;
};

// Executors are injected so tests and tools can drive completion deterministically.
// The production adapter below lowers the same work onto TerraEngine's ThreadPool.
class AnimationProgramCompileExecutor {
public:
	virtual ~AnimationProgramCompileExecutor() = default;
	virtual void Enqueue(std::function<void()> work) = 0;
};

TERRAENGINE_API std::shared_ptr<AnimationProgramCompileExecutor> CreateThreadPoolAnimationProgramCompileExecutor(ThreadPool &pool);

class AnimationProgramPublisher {
public:
	// Native dependencies exercise the same worker boundary as the real compiler.
	// Requests and prepared output are owned by that worker; the staging callback
	// runs after compilation, before its result crosses into shared owner state.
	struct WorkerDependencies {
		using Compiler = std::function<AnimationGraphCompilationResult(
			const AnimationProgramCompileRequest&, EvaluationProgram&)>;
		Compiler compile;
		std::function<void()> beforeResultStaging;
	};

	// maxPendingJobs is clamped to kAnimationProgramPublisherMaximumPendingJobs,
	// preventing a changing asset from producing unbounded retained documents.
	TERRAENGINE_API explicit AnimationProgramPublisher(std::shared_ptr<AnimationProgramCompileExecutor> executor,
		std::size_t maxPendingJobs = 16U);
	TERRAENGINE_API AnimationProgramPublisher(std::shared_ptr<AnimationProgramCompileExecutor> executor,
		std::size_t maxPendingJobs, WorkerDependencies workerDependencies);
	TERRAENGINE_API ~AnimationProgramPublisher();

	AnimationProgramPublisher(const AnimationProgramPublisher &) = delete;
	AnimationProgramPublisher &operator=(const AnimationProgramPublisher &) = delete;

	// Queues a copied input document. It returns immediately and never compiles on
	// the caller/frame thread; capacity and malformed snapshot rejection are local.
	TERRAENGINE_API AnimationStatus RequestCompile(const AnimationProgramCompileRequest &request);

	// Cancels queued results without joining worker threads. Their lambdas own a
	// shared state block, so destroying this publisher cannot leave a raw-this use.
	TERRAENGINE_API void CancelPending() noexcept;

	// This is the protected, non-blocking boundary. It only consumes completed
	// work, revalidates generations, and atomically swaps a last-known-good record.
	TERRAENGINE_API AnimationProgramPublishPollResult PollAndPublish(const AnimationProgramGenerationSnapshot &current);

	[[nodiscard]] TERRAENGINE_API std::optional<AnimationProgramPublishRecord> GetPublishedRecord() const;

private:
	struct SharedState;
	std::shared_ptr<SharedState> state_;
	std::shared_ptr<AnimationProgramCompileExecutor> executor_;
};
} // namespace TerraEngine::Animation
