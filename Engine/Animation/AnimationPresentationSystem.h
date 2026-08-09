// AnimationPresentationSystem.h - post-physics visual handoff scheduler owner.

#pragma once

#include "Animation/Gpu/GpuAnimationPlanner.h"
#include "Animation/Runtime/AnimationAuthorityPublication.h"
#include "Animation/Runtime/AnimationCancellation.h"
#include "Animation/Runtime/AnimatorInstanceStore.h"
#include "Core/SystemBase.h"
#include "Physics/IncrementalBodyStateStore.h"

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

	// A post-physics presentation root must distinguish a completed snapshot from
	// an active world that did not publish one and from a deliberately absent
	// world. These states have different rendering and ragdoll safety rules.
	enum class AnimationPostPhysicsBoundaryKind : std::uint8_t {
		NoWorld = 0U,
		ActiveWorldWithoutSnapshot = 1U,
		PublishedSnapshot = 2U
	};

	struct AnimationPostPhysicsBoundary {
		AnimationPostPhysicsBoundaryKind kind = AnimationPostPhysicsBoundaryKind::NoWorld;
		// This is minted by the scheduler-owned presentation declaration. It names
		// the Physics -> Presentation boundary even when no physics world exists.
		std::uint64_t schedulerGeneration = 0U;
		std::uint64_t physicsWorldGeneration = 0U;
		std::uint64_t publishedTick = 0U;
		std::uint64_t snapshotGeneration = 0U;
		// Retain the exact immutable provider publication used by presentation; no
		// consumer is allowed to reopen PhysicsSystem and substitute a newer value.
		std::shared_ptr<const Physics::IncrementalBodyStateSnapshot> snapshot;

		// Validation is defined out-of-line so this public header can retain the
		// immutable physics snapshot through a forward declaration instead of
		// exposing the complete Physics implementation to every animation consumer.
		[[nodiscard]] bool IsValid() const noexcept;
	};

	// Producers submit this immutable value after AnimationSystem has captured
	// program, visual, and effective-mode decisions. It intentionally carries no
	// live scene or renderer state, so presentation workers may retain it safely.
	struct GpuAnimationPresentationFrame {
		// Presentation must retain the exact pre-physics authority root it lowers.
		// A renderer can then acquire one root instead of correlating plan and pose
		// generations from separate atomics.
		std::shared_ptr<const AnimationAuthorityFrame> authority;
		std::uint64_t sourceGeneration = 0U;
		std::uint32_t planGeneration = 0U;
		// This signature covers the stable animator slot, lifetime, component, and
		// owner identities in source order. It changes with presentation membership,
		// but not with sample ticks, so the next worker can reuse sorted owner roots.
		std::uint64_t ownerMappingSignature = 0U;
		// AnimationSystem computes this while it is already moving the inputs into
		// the frame, avoiding a second cold traversal during presentation lowering.
		std::uint64_t topologyInputSignature = 0U;
		std::uint64_t scratchArenaCapacityBytes = 64U * 1024U * 1024U;
		std::uint64_t paletteArenaCapacityBytes = 64U * 1024U * 1024U;
		GpuAnimationPlanLimits planLimits{};
		std::vector<GpuAnimationLoweringInput> inputs;
		// AnimationSystem already visits every validated GPU input while sealing this
		// frame. It writes the three changing plan lanes during that mandatory pass,
		// so presentation can adopt them in O(1) instead of scanning the crowd again.
		// retainedStaticPlan always points at the original cold topology root, never
		// the previous frame shell, which prevents an unbounded ownership chain.
		std::shared_ptr<const GpuAnimationPlan> retainedStaticPlan;
		GpuAnimationPlanDynamicLanes preparedDynamicLanes;
		// This CPU-only proof lane verifies that owners which shared one evaluation
		// in the retained topology still have identical dynamic parameters and ticks.
		// It is discarded before publication and never expands the shader ABI.
		std::vector<std::uint64_t> preparedEvaluationFingerprints;
		bool preparedDynamicPlanComplete = false;
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
		std::uint64_t ownerMappingSignature = 0U;
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
		std::shared_ptr<const std::vector<GpuAnimationOwnerObjectMapping>>
			retainedOwnerObjectMappings;
		// Presentation builds this compact open-addressed table once before atomic
		// publication. Render collection can then resolve thousands of authored
		// owners in expected O(1) time without rebuilding a frame-local hash table.
		// Each slot stores an index into the sorted ownerObjectMappings vector.
		std::vector<std::uint32_t> ownerObjectLookupIndices;
		std::shared_ptr<const std::vector<std::uint32_t>>
			retainedOwnerObjectLookupIndices;
		std::uint32_t ownerObjectLookupMask = 0U;

		// BuildOwnerObjectLookup preserves the sorted-vector fallback if allocation
		// fails. FindFirstOwnerObjectMapping returns the first mapping for an owner,
		// allowing the renderer to retain its adjacent-duplicate ambiguity check.
		void BuildOwnerObjectLookup() noexcept;
		[[nodiscard]] const std::vector<GpuAnimationOwnerObjectMapping>&
			OwnerObjectMappings() const noexcept
		{
			return retainedOwnerObjectMappings
				? *retainedOwnerObjectMappings : ownerObjectMappings;
		}
		[[nodiscard]] const std::vector<std::uint32_t>&
			OwnerObjectLookupIndices() const noexcept
		{
			return retainedOwnerObjectLookupIndices
				? *retainedOwnerObjectLookupIndices : ownerObjectLookupIndices;
		}
		[[nodiscard]] const GpuAnimationOwnerObjectMapping*
			FindFirstOwnerObjectMapping(const UUID& ownerObjectId) const noexcept;
	};

	// This is the canonical post-physics consumer product.  It owns both the
	// exact authority frame and the backend-neutral lowered plan, so consumers
	// cannot accidentally pair a newer pose with an older presentation mapping.
	struct AnimationPresentationFrame {
		std::uint64_t presentationGeneration = 0U;
		std::uint64_t ownerMappingSignature = 0U;
		AnimationPostPhysicsBoundary postPhysicsBoundary;
		std::shared_ptr<const AnimationAuthorityFrame> authority;
		std::shared_ptr<const PublishedGpuAnimationPresentationPlan> gpuPlan;
		struct OwnerBinding {
			UUID ownerObjectId = EMPTY_UUID;
			AnimationSlotGenerationKey key{};
		};
		// Built during presentation preparation.  The sorted values avoid a
		// renderer-local crowd hash allocation and reject ambiguous owners.
		std::shared_ptr<const std::vector<OwnerBinding>> ownerBindings;

		[[nodiscard]] const AnimationAuthorityRow*
			FindAuthorityRowForOwner(const UUID& ownerObjectId) const noexcept;

		[[nodiscard]] bool IsValid() const noexcept
		{
			return presentationGeneration != 0U && postPhysicsBoundary.IsValid()
				&& authority != nullptr && authority->IsValid() && gpuPlan != nullptr
				&& gpuPlan->presentationGeneration == presentationGeneration
				&& gpuPlan->sourceGeneration == authority->authorityGeneration;
		}
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
		// Renderer and scripting acquire this one immutable product for an operation.
		// It is intentionally backend-neutral: backend lowering owns native handles.
		[[nodiscard]] std::shared_ptr<const AnimationPresentationFrame>
			LoadPublishedAnimationPresentationFrame() const noexcept;
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
