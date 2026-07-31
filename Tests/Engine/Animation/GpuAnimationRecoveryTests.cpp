// GpuAnimationRecoveryTests.cpp - GPU animation recovery contract tests.
//
// ImplementationCheck will provide the production GPU-animation resource controller. Until it
// exists, the small fixture below documents its required state transitions in
// deterministic CPU-only tests. The fixture deliberately uses the public
// generation, renderer-recovery, planning, and retirement contracts instead of
// duplicating backend handles or Vulkan work.

#include "TestHarness.h"

#include "Animation/Gpu/GpuAnimationPlan.h"
#include "Renderer/RHI/Vulkan/VulkanRendererWorkCache.h"
#include "Renderer/RendererRecovery.h"

#include <array>
#include <cstdint>
#include <optional>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::Renderer;

		enum class GpuAnimationRecoveryOutcome : std::uint8_t {
			PublishedGpu,
			CpuFallbackRendererLost,
			CpuFallbackBudgetExhausted,
			CpuFallbackAllocationFailure,
			RejectedStaleCompletion
		};

		// This is intentionally a test-local description of ImplementationCheck's missing resource
		// controller. It makes the required atomic lifecycle observable without
		// pretending that a renderer handle can be safely recreated in a unit test.
		class GpuAnimationRecoveryContractFixture {
		public:
			explicit GpuAnimationRecoveryContractFixture(std::uint64_t gpuBudgetBytes)
				: gpuBudgetBytes_(gpuBudgetBytes) {}

			[[nodiscard]] GpuAnimationRecoveryOutcome Prepare(
				const GpuAnimationGenerationRecord& generations,
				std::uint64_t requestedBytes)
			{
				// A prepared request is a complete immutable record. A later completion
				// must match this entire value before it can become visible to rendering.
				pendingGenerations_ = generations;
				if(!CanSubmitNormalGpuWork(recovery_)) {
					return SetFallback(GpuAnimationRecoveryOutcome::CpuFallbackRendererLost);
				}
				if(generations.rendererGeneration != rendererGeneration_)
				{
					return SetFallback(GpuAnimationRecoveryOutcome::RejectedStaleCompletion);
				}
				if(failNextAllocation_) {
					failNextAllocation_ = false;
					return SetFallback(GpuAnimationRecoveryOutcome::CpuFallbackAllocationFailure);
				}
				if(requestedBytes > gpuBudgetBytes_ - gpuBytesInUse_) {
					return SetFallback(GpuAnimationRecoveryOutcome::CpuFallbackBudgetExhausted);
				}

				gpuBytesInUse_ += requestedBytes;
				lastOutcome_ = GpuAnimationRecoveryOutcome::PublishedGpu;
				return lastOutcome_;
			}

			[[nodiscard]] bool CompleteGpuWork(const GpuAnimationGenerationRecord& completion)
			{
				// Device loss or any changed generation invalidates the whole result.
				// The fixture never patches individual fields from an older completion.
				if(!CanSubmitNormalGpuWork(recovery_)
					|| !pendingGenerations_.has_value()
					|| !GpuAnimationGenerationsMatch(*pendingGenerations_, completion)
					|| completion.rendererGeneration != rendererGeneration_)
				{
					lastOutcome_ = GpuAnimationRecoveryOutcome::RejectedStaleCompletion;
					return false;
				}

				publishedGenerations_ = completion;
				return true;
			}

			void FailNextAllocation() noexcept
			{
				// Dependency injection makes allocation failure deterministic; relying on
				// process memory exhaustion would make this safety test flaky.
				failNextAllocation_ = true;
			}

			void RecordRendererLoss(std::uint64_t frameId)
			{
				recovery_ = RecordDeviceLossIncident(
					recovery_, "VK_ERROR_DEVICE_LOST", "submit GPU animation", frameId,
					rendererGeneration_, RendererRecoveryAffectedView::GameView);
				// Lost-device allocations are no longer reusable, but their byte budget is
				// not silently handed to new work until a new renderer generation exists.
				gpuBytesInUse_ = 0U;
			}

			void BeginRecoveredRenderer(std::uint32_t rendererGeneration)
			{
				// ImplementationCheck must create a distinct renderer generation after recovery. Reusing
				// the old number could allow an old queue completion to look current.
				rendererGeneration_ = rendererGeneration;
				recovery_ = MakeActiveRecoverySnapshot(rendererGeneration);
				pendingGenerations_.reset();
				gpuBytesInUse_ = 0U;
			}

			void RetireGpuResource(std::uint64_t resourceId, std::uint64_t retireAfterFrameId)
			{
				retirementCache_.EnqueueResourceForRetirement(
					{resourceId, retireAfterFrameId, "GPU animation allocation"});
			}

			[[nodiscard]] std::size_t RetireCompletedResources(std::uint64_t completedFrameId)
			{
				return retirementCache_.RetireCompletedResources(completedFrameId).size();
			}

			[[nodiscard]] const RendererRecoverySnapshot& Recovery() const noexcept { return recovery_; }
			[[nodiscard]] std::uint64_t GpuBytesInUse() const noexcept { return gpuBytesInUse_; }
			[[nodiscard]] std::size_t PendingRetirementCount() const noexcept { return retirementCache_.PendingRetirementCount(); }
			[[nodiscard]] GpuAnimationRecoveryOutcome LastOutcome() const noexcept { return lastOutcome_; }
			[[nodiscard]] const std::optional<GpuAnimationGenerationRecord>& PublishedGenerations() const noexcept { return publishedGenerations_; }

		private:
			[[nodiscard]] GpuAnimationRecoveryOutcome SetFallback(GpuAnimationRecoveryOutcome outcome) noexcept
			{
				// CPU fallback is published as a complete generation, not as a partial GPU
				// buffer update. This preserves the last-good output until a compatible CPU
				// pose is available at the normal publication boundary.
				lastOutcome_ = outcome;
				return outcome;
			}

			RendererRecoverySnapshot recovery_ = MakeActiveRecoverySnapshot();
			std::uint32_t rendererGeneration_ = 1U;
			std::uint64_t gpuBudgetBytes_ = 0U;
			std::uint64_t gpuBytesInUse_ = 0U;
			bool failNextAllocation_ = false;
			GpuAnimationRecoveryOutcome lastOutcome_ = GpuAnimationRecoveryOutcome::PublishedGpu;
			std::optional<GpuAnimationGenerationRecord> pendingGenerations_;
			std::optional<GpuAnimationGenerationRecord> publishedGenerations_;
			Vulkan::VulkanRendererPreparationCache retirementCache_;
		};

		[[nodiscard]] GpuAnimationGenerationRecord MakeGenerations(
			std::uint32_t rendererGeneration = 1U,
			std::uint32_t workGeneration = 1U)
		{
			GpuAnimationGenerationRecord generations{};
			generations.instanceSlot = 2U;
			generations.instanceGeneration = 3U;
			generations.componentGeneration = 4U;
			generations.workGeneration = workGeneration;
			generations.programGeneration = 6U;
			generations.controllerGeneration = 7U;
			generations.skeletonGeneration = 8U;
			generations.clipGeneration = 9U;
			generations.meshGeneration = 10U;
			generations.resourceGeneration = 11U;
			generations.rendererGeneration = rendererGeneration;
			generations.sceneGeneration = 12U;
			generations.frameIdLow = 13U;
			return generations;
		}

		[[nodiscard]] GpuAnimationWorkRequest MakeWorkRequest()
		{
			GpuAnimationWorkRequest request{};
			request.batchKey.programContentId = 100U;
			request.batchKey.skeletonTopologyId = 200U;
			request.batchKey.stageMask = GpuAnimationStageMask(GpuAnimationStage::Sample)
				| GpuAnimationStageMask(GpuAnimationStage::Blend)
				| GpuAnimationStageMask(GpuAnimationStage::Hierarchy)
				| GpuAnimationStageMask(GpuAnimationStage::Bounds)
				| GpuAnimationStageMask(GpuAnimationStage::Palette);
			request.batchKey.outputFlags = static_cast<std::uint32_t>(GpuAnimationOutputFlag::Pose)
				| static_cast<std::uint32_t>(GpuAnimationOutputFlag::Bounds)
				| static_cast<std::uint32_t>(GpuAnimationOutputFlag::Palette);
			request.generations = MakeGenerations();
			request.stableInstanceId = 55U;
			request.clipResourceIndex = 1U;
			request.programResourceIndex = 2U;
			request.skeletonResourceIndex = 3U;
			request.jointCount = 4U;
			request.scratchSizeBytes = 32U;
			request.paletteEntryCount = 4U;
			request.palettePartitionCount = 1U;
			request.flags = static_cast<std::uint32_t>(GpuAnimationWorkFlag::UseBindPoseFallback);
			return request;
		}
	}

	void RegisterGpuAnimationRecoveryTests(TestRegistry& registry)
	{
		registry.Add("Animation.GpuRecovery.RendererLossCancelsGpuAndSelectsCpuFallback", [](TestContext& context) {
			GpuAnimationRecoveryContractFixture fixture(256U);
			const GpuAnimationGenerationRecord generations = MakeGenerations();
			context.Expect(fixture.Prepare(generations, 64U) == GpuAnimationRecoveryOutcome::PublishedGpu,
				"An active renderer with available budget should prepare GPU animation work.");

			fixture.RecordRendererLoss(41U);
			context.Expect(!CanSubmitNormalGpuWork(fixture.Recovery()),
				"A latched renderer-loss snapshot must block ordinary GPU animation submission.");
			context.Expect(fixture.Prepare(generations, 64U) == GpuAnimationRecoveryOutcome::CpuFallbackRendererLost,
				"Renderer loss must select the conservative CPU fallback instead of publishing invalid GPU geometry.");
			context.ExpectEqual(std::uint64_t{0U}, fixture.GpuBytesInUse(),
				"Renderer loss must cancel the live GPU allocation accounting atomically.");
		});

		registry.Add("Animation.GpuRecovery.StaleCompletionCannotPublishAfterRecoveredRenderer", [](TestContext& context) {
			GpuAnimationRecoveryContractFixture fixture(256U);
			const GpuAnimationGenerationRecord oldGenerations = MakeGenerations(1U);
			static_cast<void>(fixture.Prepare(oldGenerations, 64U));
			fixture.RecordRendererLoss(42U);
			fixture.BeginRecoveredRenderer(2U);

			context.Expect(!fixture.CompleteGpuWork(oldGenerations),
				"A completion from the retired renderer generation must not publish after recovery.");
			context.Expect(fixture.LastOutcome() == GpuAnimationRecoveryOutcome::RejectedStaleCompletion,
				"Stale completion rejection should report its explicit lifecycle outcome.");
			context.Expect(!fixture.PublishedGenerations().has_value(),
				"Rejected GPU work must leave no partial animation publication visible.");
		});

		registry.Add("Animation.GpuRecovery.BudgetExhaustionAndArenaFailureUseFallbackWithoutPartialPlan", [](TestContext& context) {
			GpuAnimationArena scratch(GpuAnimationArenaKind::Scratch, 64U);
			GpuAnimationArena palette(GpuAnimationArenaKind::Palette, 64U);
			GpuAnimationPlan previousPlan{};
			previousPlan.generation = 77U;
			previousPlan.workPackets.push_back({});
			const GpuAnimationPlanStatus status = BuildGpuAnimationPlan(
				std::array{MakeWorkRequest()}, 2U, scratch, palette, previousPlan);
			context.Expect(status == GpuAnimationPlanStatus::ArenaCapacityExceeded,
				"A bounded GPU animation arena must reject work that cannot fit its allocation budget.");
			context.ExpectEqual(77U, previousPlan.generation,
				"A rejected arena plan must retain the caller's complete previous plan.");

			GpuAnimationRecoveryContractFixture fixture(96U);
			const GpuAnimationGenerationRecord generations = MakeGenerations();
			context.Expect(fixture.Prepare(generations, 64U) == GpuAnimationRecoveryOutcome::PublishedGpu,
				"The fixture should establish one bounded GPU allocation before budget pressure.");
			context.Expect(fixture.Prepare(MakeGenerations(1U, 2U), 64U)
				== GpuAnimationRecoveryOutcome::CpuFallbackBudgetExhausted,
				"Budget exhaustion must choose CPU fallback rather than overcommitting GPU memory.");
			context.ExpectEqual(std::uint64_t{64U}, fixture.GpuBytesInUse(),
				"A rejected budget request must not partially charge GPU allocation bytes.");
		});

		registry.Add("Animation.GpuRecovery.DeterministicAllocationFailureUsesCpuFallback", [](TestContext& context) {
			GpuAnimationRecoveryContractFixture fixture(256U);
			fixture.FailNextAllocation();
			context.Expect(fixture.Prepare(MakeGenerations(), 64U)
				== GpuAnimationRecoveryOutcome::CpuFallbackAllocationFailure,
				"Injected allocation failure must select the bounded CPU fallback deterministically.");
			context.ExpectEqual(std::uint64_t{0U}, fixture.GpuBytesInUse(),
				"An allocation failure must not expose a partially charged GPU allocation.");
		});

		registry.Add("Animation.GpuRecovery.DeferredRetirementWaitsForCompletedGpuFrame", [](TestContext& context) {
			GpuAnimationRecoveryContractFixture fixture(256U);
			fixture.RetireGpuResource(900U, 50U);
			context.ExpectEqual(std::size_t{1U}, fixture.PendingRetirementCount(),
				"A retired GPU animation resource must remain queued before completion proof.");
			context.ExpectEqual(std::size_t{0U}, fixture.RetireCompletedResources(49U),
				"An earlier completed frame must not release an in-flight animation allocation.");
			context.ExpectEqual(std::size_t{1U}, fixture.RetireCompletedResources(50U),
				"The matching completed frame should release the deferred animation allocation exactly once.");
		});

		registry.Add("Animation.GpuRecovery.NeverMixesOldCompletionIntoNewGeneration", [](TestContext& context) {
			GpuAnimationRecoveryContractFixture fixture(256U);
			const GpuAnimationGenerationRecord published = MakeGenerations(1U, 1U);
			static_cast<void>(fixture.Prepare(published, 64U));
			context.Expect(fixture.CompleteGpuWork(published),
				"A complete matching GPU generation should publish before the next update begins.");

			GpuAnimationGenerationRecord next = MakeGenerations(1U, 2U);
			++next.meshGeneration;
			static_cast<void>(fixture.Prepare(next, 64U));
			GpuAnimationGenerationRecord mixedCompletion = next;
			mixedCompletion.meshGeneration = published.meshGeneration;
			context.Expect(!fixture.CompleteGpuWork(mixedCompletion),
				"A completion with one old dependency generation must be rejected as a whole.");
			context.Expect(fixture.PublishedGenerations().has_value()
				&& GpuAnimationGenerationsMatch(published, *fixture.PublishedGenerations()),
				"Rejected mixed work must retain the previous complete publication rather than combining generations.");
		});
	}
}
