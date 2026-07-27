// AnimationPresentationSystem.h - post-physics visual handoff scheduler owner.

#pragma once

#include "Animation/Gpu/GpuAnimationPlanner.h"
#include "Animation/Runtime/AnimationCancellation.h"
#include "Animation/Runtime/AnimatorInstanceStore.h"
#include "Core/SystemBase.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace TerraEngine::Animation {
	// This immutable record connects one GPU packet to the AnimatorComponent
	// owner that produced it.  Render collection consumes this copied evidence
	// instead of reopening AnimatorInstanceStore or guessing from a globally
	// unique mesh/generation tuple.
	struct GpuAnimationOwnerObjectMapping {
		UUID ownerObjectId = EMPTY_UUID;
		std::uint64_t stableInstanceId = 0U;
		std::uint32_t instanceSlot = kInvalidAnimatorInstanceSlot;
		std::uint32_t instanceGeneration = 0U;
		std::uint32_t componentGeneration = 0U;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return ownerObjectId != EMPTY_UUID && stableInstanceId != 0U
				&& instanceSlot != kInvalidAnimatorInstanceSlot
				&& instanceGeneration != 0U && componentGeneration != 0U
				&& stableInstanceId == ((static_cast<std::uint64_t>(instanceSlot) << 32U)
					| static_cast<std::uint64_t>(instanceGeneration));
		}
	};

	// Producers submit this immutable value after AnimationSystem has captured
	// program, visual, and effective-mode decisions. It intentionally carries no
	// live scene or renderer state, so presentation workers may retain it safely.
	struct GpuAnimationPresentationFrame {
		std::uint64_t sourceGeneration = 0U;
		std::uint32_t planGeneration = 0U;
		std::uint64_t scratchArenaCapacityBytes = 64U * 1024U * 1024U;
		std::uint64_t paletteArenaCapacityBytes = 64U * 1024U * 1024U;
		GpuAnimationPlanLimits planLimits{};
		std::vector<GpuAnimationLoweringInput> inputs;
		// Each mapping is captured from the same protected AnimationSystem frame as
		// inputs. Presentation filters it against the lowered plan before publish.
		std::vector<GpuAnimationOwnerObjectMapping> ownerObjectMappings;
	};

	// Render consumes this one immutable envelope, rather than separately loading
	// a plan and its validation status. That prevents a failed update mixing a new
	// diagnostic generation with last frame's GPU work.
	struct PublishedGpuAnimationPresentationPlan {
		std::uint64_t presentationGeneration = 0U;
		std::uint64_t sourceGeneration = 0U;
		GpuAnimationLoweringStatus loweringStatus = GpuAnimationLoweringStatus::InvalidArgument;
		GpuAnimationPlanStatus planStatus = GpuAnimationPlanStatus::InvalidArgument;
		bool safeFallback = true;
		GpuAnimationPlan plan{};
		// This contains only mappings whose stable packet survived lowering. Keeping
		// it in the envelope makes packet/object matching one atomic publication.
		std::vector<GpuAnimationOwnerObjectMapping> ownerObjectMappings;
	};

	// Presentation is intentionally a separate system from controller authority.
	// It can consume the current physics step for ragdoll visuals without creating
	// an Animation-to-Physics-to-Animation dependency cycle in one scheduler phase.
	class TERRAENGINE_API AnimationPresentationSystem final : public SystemsBase {
	public:
		static AnimationPresentationSystem& Instance();

		AnimationPresentationSystem(const AnimationPresentationSystem&) = delete;
		AnimationPresentationSystem& operator=(const AnimationPresentationSystem&) = delete;

		void Initialize() override;
		void Update(float deltaTime) override;
		void Shutdown() override;
		std::string SystemName() override;
		TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override;
		std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareFrameWorkloads(float deltaTime) override;

		// Submission copies shared immutable ownership only. It is safe for the
		// Animation worker-side producer and never reads ObjectManager or a renderer.
		[[nodiscard]] AnimationStatus SubmitGpuAnimationPresentationFrame(
			std::shared_ptr<const GpuAnimationPresentationFrame> frame) noexcept;
		// Renderer, device, scene, asset, and Play Mode owners invalidate GPU work
		// through the same protected lifecycle identity used by CPU evaluation.
		[[nodiscard]] AnimationStatus NotifyLifecycleInvalidation(
			AnimationLifecycleBoundary boundary) noexcept;
		[[nodiscard]] std::shared_ptr<const PublishedGpuAnimationPresentationPlan>
			LoadPublishedGpuAnimationPresentationPlan() const noexcept;

		[[nodiscard]] std::size_t PublishedAnimatorCount() const noexcept;
		[[nodiscard]] std::uint64_t PresentationPublicationGeneration() const noexcept;

	private:
		AnimationPresentationSystem();
		~AnimationPresentationSystem() override;

		struct Impl;
		std::unique_ptr<Impl> impl_;
	};
}
