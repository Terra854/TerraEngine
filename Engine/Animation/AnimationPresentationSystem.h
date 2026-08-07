// AnimationPresentationSystem.h - post-physics visual handoff scheduler owner.

#pragma once

#include "Animation/Gpu/GpuAnimationPlanner.h"
#include "Animation/Runtime/AnimationCancellation.h"
#include "Animation/Runtime/AnimatorInstanceStore.h"
#include "Core/SystemBase.h"

#include <cstddef>
#include <cstdint>
#include <limits>
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
		// The producer leaves this lane invalid because packet order is decided by
		// deterministic presentation lowering. The published mapping fills it with
		// the exact packet index so RenderSystem does not rebuild a crowd-sized
		// stable-instance hash table every frame.
		std::uint32_t workPacketIndex = std::numeric_limits<std::uint32_t>::max();

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return ownerObjectId != EMPTY_UUID && stableInstanceId != 0U
				&& instanceSlot != kInvalidAnimatorInstanceSlot
				&& instanceGeneration != 0U && componentGeneration != 0U
				&& stableInstanceId == ((static_cast<std::uint64_t>(instanceSlot) << 32U)
					| static_cast<std::uint64_t>(instanceGeneration));
		}

		[[nodiscard]] bool HasPublishedPacketIndex() const noexcept
		{
			return IsComplete()
				&& workPacketIndex != std::numeric_limits<std::uint32_t>::max();
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
		bool topologyReused = false;
		double topologyBuildMs = 0.0;
		double topologyRefreshMs = 0.0;
		GpuAnimationPlan plan{};
		// Compact visual presentation consumes this retained topology instead of
		// rebuilding owner and dispatch structure from a previous frame plan.
		// The dynamic sidecar is frame-owned and must match every topology key.
		GpuAnimationTopology topology{};
		GpuAnimationTopologyDynamicState topologyDynamicState{};
		// This contains only mappings whose stable packet survived lowering. Keeping
		// it in the envelope makes packet/object matching one atomic publication.
		std::vector<GpuAnimationOwnerObjectMapping> ownerObjectMappings;
		// Presentation builds this compact open-addressed table once before atomic
		// publication. Render collection can then resolve thousands of authored
		// owners in expected O(1) time without rebuilding a frame-local hash table.
		// Each slot stores an index into the sorted ownerObjectMappings vector.
		std::vector<std::uint32_t> ownerObjectLookupIndices;
		std::uint32_t ownerObjectLookupMask = 0U;

		// BuildOwnerObjectLookup preserves the sorted-vector fallback if allocation
		// fails. FindFirstOwnerObjectMapping returns the first mapping for an owner,
		// allowing the renderer to retain its adjacent-duplicate ambiguity check.
		void BuildOwnerObjectLookup() noexcept;
		[[nodiscard]] const GpuAnimationOwnerObjectMapping*
			FindFirstOwnerObjectMapping(const UUID& ownerObjectId) const noexcept;
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
		// The next protected capture uses this exact source-generation check to
		// force complete processing after a failed, stale, or incomplete GPU handoff.
		// It reads only an immutable published envelope and never waits for a fence.

		[[nodiscard]] std::size_t PublishedAnimatorCount() const noexcept;
		[[nodiscard]] std::uint64_t PresentationPublicationGeneration() const noexcept;

	private:
		AnimationPresentationSystem();
		~AnimationPresentationSystem() override;

		struct Impl;
		std::unique_ptr<Impl> impl_;
	};
}
