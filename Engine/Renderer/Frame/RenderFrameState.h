// Stable renderer lifetime and coroutine ownership shared by compiled stages.
#pragma once
#include "RenderFrameCaches.h"
#include "RenderFrameAlgorithms.h"
#include "Renderer/Graph/RenderFrameScratch.h"
#include "Renderer/RHI/Vulkan/VulkanRendererWorkCache.h"
#include <coroutine>
#include <atomic>
#include <stdexcept>
#include <exception>
#include <mutex>
#include <utility>

namespace TerraEngine::Renderer::Frame {
	// Every resume, synchronous capture and lifecycle retirement admits through
	// the same stable RenderSystem-owned atomic gate before touching borrowers.
	class RenderExecutionBoundary final {
	public:
		explicit RenderExecutionBoundary(std::atomic_flag& flag) : flag_(flag) {
			if(flag_.test_and_set(std::memory_order_acquire))
				throw std::logic_error("Render frame execution is already active");
		}
		~RenderExecutionBoundary() { flag_.clear(std::memory_order_release); }
		RenderExecutionBoundary(const RenderExecutionBoundary&) = delete;
		RenderExecutionBoundary& operator=(const RenderExecutionBoundary&) = delete;
	private:
		std::atomic_flag& flag_;
	};
}

using namespace TerraEngine::Renderer::Frame;

struct RenderFrameExecution
{
	struct promise_type
	{
		std::exception_ptr failure;

		RenderFrameExecution get_return_object()
		{
			return RenderFrameExecution{
				std::coroutine_handle<promise_type>::from_promise(*this)};
		}
		std::suspend_always initial_suspend() const noexcept { return {}; }
		std::suspend_always final_suspend() const noexcept { return {}; }
		void return_void() const noexcept {}
		void unhandled_exception() noexcept { failure = std::current_exception(); }
	};

	using Handle = std::coroutine_handle<promise_type>;
	Handle handle{};

	RenderFrameExecution() = default;
	explicit RenderFrameExecution(Handle value) : handle(value) {}
	RenderFrameExecution(const RenderFrameExecution&) = delete;
	RenderFrameExecution& operator=(const RenderFrameExecution&) = delete;
	RenderFrameExecution(RenderFrameExecution&& other) noexcept
		: handle(std::exchange(other.handle, {})) {}
	RenderFrameExecution& operator=(RenderFrameExecution&& other) noexcept
	{
		if(this == &other) return *this;
		if(handle) handle.destroy();
		handle = std::exchange(other.handle, {});
		return *this;
	}
	~RenderFrameExecution()
	{
		if(handle) handle.destroy();
	}

	// Returns true when another scheduler stage remains. Exceptions are rethrown
	// at the owning callback so the scheduler reports the correct failed phase.
	bool Resume()
	{
		if(!handle || handle.done()) return false;
		handle.resume();
		if(handle.promise().failure) std::rethrow_exception(handle.promise().failure);
		return !handle.done();
	}
};

// One implementation-owned aggregate replaces every former function-static
// cache. Project switches and renderer shutdown can now release all retained
// roots and scratch capacity together without leaking state across sessions.
struct RenderSystemLifetimeState
{
	struct FramePhaseState
	{
		float deltaTime = 0.0F;
		std::uint64_t captureSequence = 0U;
		std::unique_ptr<RenderFrameExecution> execution;
		bool captureReady = false;
		bool routeReady = false;
		bool virtualGeometryReady = false;
		bool backendComplete = false;
		bool publicationComplete = false;
	};
	// Scheduler callbacks may cross worker and protected executors. This mutex
	// guards the small immutable-root handoff only; no live ECS or GPU work runs
	// while it is held.
	std::mutex framePhaseMutex;
	// Declared before the continuation: reverse destruction retires its vectors
	// and strong lease before this lifetime can release the stable scratch owner.
	std::shared_ptr<TerraEngine::Renderer::RenderFrameScratch> frameScratch;
	FramePhaseState framePhase;
	RetainedRenderTopologyState retainedRenderTopology;
	GroupedSubmissionMembershipCacheState groupedSubmissionMembershipCache;
	TerraEngine::Renderer::Vulkan::VulkanRendererPreparationCache rendererPreparationCache;
	VisibleCommandCacheState visibleCommandCache;
	ShadowDrawListCacheState shadowDrawListCache;
	SpatialPreparationCacheState spatialPreparationCache;
	RenderableDrawWorkingSet renderableDrawWorkingSet;
	DrawCommandCacheState drawCommandCache;
	RenderableFrameIdentityCacheState renderableFrameIdentityCache;
	LightCandidateCacheState lightCandidateCache;
	// View commands are fully lowered into immutable selection and presentation
	// records before the next view starts. Retaining this temporary vector avoids
	// allocating crowd-sized command storage again for every view and frame.
	std::vector<RenderItemRecord> viewCommandScratch;
};
// Clear retires the continuation before replacing the caches declared above.
