// AnimatorComponentBindingCoordinator.h - protected authored/runtime bridge.
//
// Scene components are durable authoring records, while AnimationSystem owns
// transient evaluator instances. This coordinator is called only from the
// scheduler's protected capture boundary to keep those two lifetimes aligned.

#pragma once

#include "Core/Export.h"

#include <cstddef>
#include <memory>

class ObjectManager;
class ResourceManager;

namespace TerraEngine::Animation {
	class AnimationSystem;

	struct AnimatorComponentSynchronizationSummary {
		std::size_t authoredActiveCount = 0U;
		std::size_t runtimeActiveCount = 0U;
		std::size_t createdCount = 0U;
		std::size_t reboundCount = 0U;
		std::size_t invalidatedCount = 0U;
		std::size_t failedCount = 0U;
		// Streaming counts are copied at the protected synchronization boundary.
		// They expose only bounded aggregate state; no mutable payload is leaked.
		std::size_t requestedStreamPayloadCount = 0U;
		std::size_t residentStreamPayloadCount = 0U;
		std::size_t retainedStreamPayloadCount = 0U;
		std::size_t failedStreamPayloadCount = 0U;
		std::size_t cancelledStreamPayloadCount = 0U;
		std::size_t streamedMotionDatabaseCount = 0U;
		std::size_t fallbackMotionDatabaseCount = 0U;
	};

	class TERRAENGINE_API AnimatorComponentBindingCoordinator {
	public:
		AnimatorComponentBindingCoordinator();
		~AnimatorComponentBindingCoordinator();

		AnimatorComponentBindingCoordinator(const AnimatorComponentBindingCoordinator&) = delete;
		AnimatorComponentBindingCoordinator& operator=(const AnimatorComponentBindingCoordinator&) = delete;

		// Synchronize must run on AnimationSystem's coordinator thread after scene
		// mutations have flushed. It never exposes ObjectManager data to workers.
		[[nodiscard]] AnimatorComponentSynchronizationSummary Synchronize(
			AnimationSystem& animation,
			ObjectManager& objects,
			ResourceManager& resources) noexcept;

		// Edit/Play clock changes restart authored playback at the next protected
		// synchronization without discarding compiled controllers or the last
		// complete pose that remains visible until that replacement is published.
		void RequestPlaybackRestart() noexcept;

		// Shutdown invalidates handles through their owning AnimationSystem so a
		// later engine session cannot revive an old scene/component identity.
		void InvalidateAll(AnimationSystem& animation) noexcept;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};
}
