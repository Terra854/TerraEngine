#include "TestHarness.h"

#include "Animation/AnimationPresentationSystem.h"
#include "Physics/IncrementalBodyStateStore.h"

#include <memory>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace TerraEngine::Animation;

		std::shared_ptr<const AnimationAuthorityFrame> Authority(std::uint64_t generation)
		{
			auto sidecars = std::make_shared<AnimationFrameSidecars>();
			// An empty immutable root is valid and isolates generation linkage from GPU
			// planning details that belong to the presentation-system test suite.
			(void)sidecars->ValidateAndOrder(generation);
			auto authority = std::make_shared<AnimationAuthorityFrame>();
			authority->frameId = generation;
			authority->sessionGeneration = 3U;
			authority->lifecycleGeneration = 4U;
			authority->authorityGeneration = generation;
			authority->pages = std::make_shared<const std::vector<std::shared_ptr<const AnimationAuthorityPage>>>();
			authority->sidecars = std::move(sidecars);
			authority->receipt.outcome = AnimationAuthorityPublicationOutcome::Published;
			authority->receipt.authorityGeneration = generation;
			return authority;
		}

		UUID Owner(std::uint8_t value)
		{
			// Tests use small, stable UUIDs so the lookup assertions describe the
			// published owner identity instead of depending on a scene fixture.
			UUID owner{};
			owner[0U] = value;
			return owner;
		}

		std::shared_ptr<const AnimationAuthorityFrame> HeterogeneousAuthority(
			std::uint64_t generation)
		{
			auto sidecars = std::make_shared<AnimationFrameSidecars>();
			(void)sidecars->ValidateAndOrder(generation);

			auto page = std::make_shared<AnimationAuthorityPage>();
			page->capacity = static_cast<std::uint32_t>(kAnimationAuthorityPageCapacity128);
			page->changedMasks.assign(2U, 0U);
			auto addRow = [generation, &page](std::uint32_t slot, UUID owner,
				AnimationEffectiveExecutionMode mode, std::uint64_t poseGeneration) {
				AnimationAuthorityRow row;
				row.key = {slot, slot + 10U, 3U};
				row.ownerObjectId = owner;
				row.componentGeneration = 1U;
				row.sourceGeneration = generation;
				row.poseGeneration = poseGeneration;
				row.authorityPublicationGeneration = generation;
				row.effectiveExecutionMode = mode;
				row.active = true;
				row.safeFallback = mode == AnimationEffectiveExecutionMode::CpuDeterministicFallback;
				page->rows.push_back(std::move(row));
			};
			// These two rows deliberately use different evaluation paths. Both still
			// belong to the same immutable authority generation at presentation time.
			addRow(3U, Owner(11U), AnimationEffectiveExecutionMode::GpuVisual, 101U);
			addRow(8U, Owner(22U), AnimationEffectiveExecutionMode::CpuDeterministicFallback, 202U);

			auto pages = std::make_shared<const std::vector<std::shared_ptr<const AnimationAuthorityPage>>>(
				std::vector<std::shared_ptr<const AnimationAuthorityPage>>{page});
			auto authority = std::make_shared<AnimationAuthorityFrame>();
			authority->frameId = generation;
			authority->sessionGeneration = 3U;
			authority->lifecycleGeneration = 4U;
			authority->authorityGeneration = generation;
			authority->pages = std::move(pages);
			authority->sidecars = std::move(sidecars);
			authority->receipt.outcome = AnimationAuthorityPublicationOutcome::Published;
			authority->receipt.authorityGeneration = generation;
			return authority;
		}

		std::shared_ptr<const PublishedGpuAnimationPresentationPlan> Plan(
			std::uint64_t presentationGeneration, std::uint64_t authorityGeneration, bool safeFallback)
		{
			auto plan = std::make_shared<PublishedGpuAnimationPresentationPlan>();
			plan->presentationGeneration = presentationGeneration;
			plan->sourceGeneration = authorityGeneration;
			plan->safeFallback = safeFallback;
			return plan;
		}

		AnimationPostPhysicsBoundary PublishedBoundary(std::uint64_t schedulerGeneration)
		{
			auto snapshot = std::make_shared<Physics::IncrementalBodyStateSnapshot>();
			snapshot->world = {Physics::PhysicsWorldId{1U}, 41U};
			snapshot->tick = 57U;
			snapshot->storeGeneration = 73U;
			return {AnimationPostPhysicsBoundaryKind::PublishedSnapshot,
				schedulerGeneration, snapshot->world.worldGeneration, snapshot->tick,
				snapshot->storeGeneration, std::move(snapshot)};
		}
	}

	void RegisterAnimationPresentationPublicationTests(TestRegistry& registry)
	{
		registry.Add("Animation.PresentationPublication.LinksAuthorityPhysicsAndSafePresentation", [](TestContext& context)
		{
			const auto authority = Authority(71U);
			GpuAnimationPresentationFrame physicsInput;
			physicsInput.authority = authority;
			physicsInput.sourceGeneration = authority->authorityGeneration;
			AnimationPresentationFrame presentation;
			presentation.presentationGeneration = 101U;
			presentation.postPhysicsBoundary = PublishedBoundary(93U);
			presentation.authority = physicsInput.authority;
			presentation.gpuPlan = Plan(presentation.presentationGeneration,
				physicsInput.sourceGeneration, true);

			context.Expect(authority->IsValid() && physicsInput.sourceGeneration == authority->authorityGeneration
				&& presentation.IsValid() && presentation.gpuPlan->safeFallback
				&& presentation.postPhysicsBoundary.snapshot != nullptr
				&& presentation.postPhysicsBoundary.kind
					== AnimationPostPhysicsBoundaryKind::PublishedSnapshot,
				"A safe presentation must retain the exact authority root through the physics-to-presentation handoff.");
		});

		registry.Add("Animation.PresentationPublication.SuppressesIncompleteAuthorityHandoff", [](TestContext& context)
		{
			AnimationPresentationFrame presentation;
			presentation.presentationGeneration = 8U;
			presentation.postPhysicsBoundary = PublishedBoundary(9U);
			presentation.gpuPlan = Plan(8U, 7U, true);
			context.Expect(!presentation.IsValid(),
				"Presentation publication must be suppressed when the authority root is absent, even with a safe fallback plan.");
		});

		registry.Add("Animation.PresentationPublication.RejectsIndependentGenerationAssembly", [](TestContext& context)
		{
			AnimationPresentationFrame presentation;
			presentation.presentationGeneration = 23U;
			presentation.postPhysicsBoundary = PublishedBoundary(17U);
			presentation.authority = Authority(16U);
			presentation.gpuPlan = Plan(24U, 15U, false);
			context.Expect(!presentation.IsValid(),
				"Presentation must reject a plan assembled from different authority and presentation generations.");
		});

		registry.Add("Animation.PresentationPublication.AcceptsExplicitNoWorldBoundary", [](TestContext& context)
		{
			const auto authority = Authority(31U);
			AnimationPresentationFrame presentation;
			presentation.presentationGeneration = 32U;
			presentation.postPhysicsBoundary = {
				AnimationPostPhysicsBoundaryKind::NoWorld, 33U, 0U, 0U, 0U, {}};
			presentation.authority = authority;
			presentation.gpuPlan = Plan(32U, 31U, false);
			context.Expect(presentation.IsValid(),
				"Visual-only animation must retain an explicit no-world boundary rather than fabricate a physics generation.");
		});

		registry.Add("Animation.PresentationPublication.RejectsActiveWorldWithoutSnapshot", [](TestContext& context)
		{
			const auto authority = Authority(41U);
			AnimationPresentationFrame presentation;
			presentation.presentationGeneration = 42U;
			// A live world without a published snapshot must remain distinguishable from
			// a valid provider publication and therefore cannot validate this root.
			presentation.postPhysicsBoundary = {
				AnimationPostPhysicsBoundaryKind::ActiveWorldWithoutSnapshot,
				43U, 44U, 0U, 0U, {}};
			presentation.authority = authority;
			presentation.gpuPlan = Plan(42U, 41U, false);
			context.Expect(!presentation.IsValid(),
				"An active physics world without a current immutable snapshot must fail closed.");
		});

		registry.Add("Animation.PresentationPublication.RejectsStaleSnapshotBoundary", [](TestContext& context)
		{
			const auto authority = Authority(51U);
			AnimationPresentationFrame presentation;
			presentation.presentationGeneration = 52U;
			AnimationPostPhysicsBoundary boundary = PublishedBoundary(53U);
			boundary.snapshotGeneration += 1U;
			presentation.postPhysicsBoundary = std::move(boundary);
			presentation.authority = authority;
			presentation.gpuPlan = Plan(52U, 51U, false);
			context.Expect(!presentation.IsValid(),
				"A root must reject a snapshot boundary whose copied generation no longer matches its retained immutable snapshot.");
		});

		registry.Add("Animation.PresentationPublication.HeterogeneousRowsRetainOneExactAuthorityRoot", [](TestContext& context)
		{
			const auto authority = HeterogeneousAuthority(61U);
			AnimationPresentationFrame presentation;
			presentation.presentationGeneration = 62U;
			presentation.postPhysicsBoundary = PublishedBoundary(63U);
			presentation.authority = authority;
			presentation.gpuPlan = Plan(62U, 61U, true);
			presentation.ownerBindings = std::make_shared<const std::vector<
				AnimationPresentationFrame::OwnerBinding>>(
				std::vector<AnimationPresentationFrame::OwnerBinding>{
					{Owner(11U), {3U, 13U, 3U}},
					{Owner(22U), {8U, 18U, 3U}}});

			// A render consumer acquires this one frame and resolves every mixed-mode
			// row through it. It must never pair a GPU row with a CPU row from another
			// authority generation.
			const AnimationAuthorityRow* gpuRow = presentation.FindAuthorityRowForOwner(Owner(11U));
			const AnimationAuthorityRow* cpuRow = presentation.FindAuthorityRowForOwner(Owner(22U));
			context.Expect(presentation.IsValid() && gpuRow != nullptr && cpuRow != nullptr
				&& gpuRow->effectiveExecutionMode == AnimationEffectiveExecutionMode::GpuVisual
				&& cpuRow->effectiveExecutionMode
					== AnimationEffectiveExecutionMode::CpuDeterministicFallback
				&& gpuRow->authorityPublicationGeneration == authority->authorityGeneration
				&& cpuRow->authorityPublicationGeneration == authority->authorityGeneration,
				"Heterogeneous CPU and GPU rows must resolve from one exact immutable authority root.");

			presentation.authority = HeterogeneousAuthority(64U);
			context.Expect(!presentation.IsValid(),
				"Replacing only the authority root must reject the presentation instead of mixing generations.");
		});
	}
}
