#include "TestHarness.h"
#include "VulkanSourceTestUtils.h"
#include "Renderer/RHI/Vulkan/VulkanEmptyFramePolicy.h"
#include "Renderer/RHI/Vulkan/VulkanPresentationLifecycle.h"

#include <new>
#include <vector>

namespace TerraEngine::Tests
{
	void RegisterVulkanPresentationLifecycleTests(TestRegistry& registry)
	{
		registry.Add("Renderer.VulkanPresentationLifecycle.MaintenanceLineageCompatibility", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Vulkan;
			context.Expect(SelectMaintenanceExtensionLineage(true, true) == MaintenanceExtensionLineage::Ext,
				"The EXT lineage must win when both aliases exist so older validation layers accept the pNext chain.");
			context.Expect(SelectMaintenanceExtensionLineage(false, true) == MaintenanceExtensionLineage::Khr,
				"KHR-only drivers must retain presentation-fence support through the promoted lineage.");
			context.Expect(SelectMaintenanceExtensionLineage(false, false) == MaintenanceExtensionLineage::None,
				"Missing maintenance extensions must select the bounded unextended fallback.");
		});

		registry.Add("Renderer.VulkanPresentationLifecycle.ImageCountsAndGenerations", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Vulkan;
			for (std::uint32_t imageCount = 2; imageCount <= 8; ++imageCount) {
				PresentationLifecycle lifecycle;
				context.Expect(lifecycle.Configure(3, 2, 2, imageCount), "Supported image counts should configure successfully.");
				for (std::uint32_t image = 0; image < imageCount; ++image) {
					context.Expect(lifecycle.Validate({ image % 3, image, lifecycle.Generation() }), "Every actual image index should validate.");
				}
				context.Expect(!lifecycle.Validate({ 0, imageCount, lifecycle.Generation() }), "The first out-of-range image must be rejected.");
				const std::uint64_t oldGeneration = lifecycle.Generation();
				lifecycle.PublishRecreation(PresentationRecreationOutcome::Success, 2, 2, imageCount);
				context.Expect(!lifecycle.Validate({ 0, 0, oldGeneration }), "A pre-recreation acquisition must be stale.");
			}
		});

		registry.Add("Renderer.VulkanPresentationLifecycle.AllCountTransitions", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Vulkan;
			for (std::uint32_t oldCount = 2; oldCount <= 8; ++oldCount) {
				for (std::uint32_t newCount = 2; newCount <= 8; ++newCount) {
					PresentationLifecycle lifecycle;
					lifecycle.Configure(3, 2, 2, oldCount);
					lifecycle.RequestRecreation();
					lifecycle.PublishRecreation(PresentationRecreationOutcome::Success, 2, 2, newCount);
					context.ExpectEqual(newCount, lifecycle.ActualImages(), "Recreation must publish the complete candidate image count.");
				}
			}
		});

		registry.Add("Renderer.VulkanPresentationLifecycle.ZeroExtentAndPresentRetirement", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Vulkan;
			PresentationLifecycle lifecycle;
			lifecycle.Configure(3, 2, 2, 3);
			const auto generation = lifecycle.Generation();
			context.Expect(!lifecycle.CanReleasePresentSynchronization(generation - 1), "A render fence alone must not retire present waits.");
			context.Expect(lifecycle.CanReleasePresentSynchronization(generation), "A presentation completion proof may retire image synchronization.");
			lifecycle.PublishRecreation(PresentationRecreationOutcome::DeferredZeroExtent, 2, 2, 3);
			context.Expect(lifecycle.State() == PresentationLifecycleState::DeferredZeroExtent, "Minimize should defer without blocking.");
		});

		registry.Add("Renderer.VulkanPresentationLifecycle.CoalescesResizeAndAdvisorySuboptimal", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Vulkan;
			PresentationRecreationCoordinator coordinator;
			coordinator.SetActiveExtent({ 1600u, 900u });
			coordinator.RequestFramebufferExtent({ 1920u, 1080u });
			coordinator.RequestFramebufferExtent({ 1920u, 1080u });
			const auto request = coordinator.Claim();
			context.Expect(request.has_value() && request->revision == 1u, "Repeated native extent callbacks should coalesce into one recreation revision.");
			context.Expect(!coordinator.Claim().has_value(), "A claimed resize revision must not rebuild periodically before a new event.");
			coordinator.PublishSuccess({ 1920u, 1080u });
			context.Expect(!coordinator.RequestSuboptimalIfExtentChanged(), "SUBOPTIMAL is advisory while the active presentation extent still matches.");
			coordinator.RequestFramebufferExtent({ 0u, 0u });
			context.Expect(coordinator.IsSuspendedForZeroExtent() && !coordinator.Claim().has_value(), "Minimize should suspend instead of claiming periodic zero-extent rebuilds.");
			coordinator.RequestFramebufferExtent({ 1280u, 720u });
			context.Expect(coordinator.Claim().has_value(), "Restore with a new nonzero extent should create exactly one fresh request.");
		});

		registry.Add("Renderer.VulkanPresentationLifecycle.ReservationFailureRetriesWithoutResize", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Vulkan;
			// Fault only the production reservation's host container. No global
			// allocator override, native device, or window callback is needed.
			struct Retired { int resources = 0; std::uint64_t generation = 0; };
			struct RetirementQueue {
				std::vector<Retired> entries;
				bool fail = true;
				void emplace_back() { if(fail) throw std::bad_alloc(); entries.emplace_back(); }
				Retired& back() { return entries.back(); }
				void pop_back() { entries.pop_back(); }
			} retired;
			PresentationLifecycle lifecycle;
			context.Expect(lifecycle.Configure(3, 2, 2, 3), "Initial presentation owners must be usable.");
			const auto originalGeneration = lifecycle.Generation();
			const PresentationExtent extent{1280, 720};
			PresentationRecreationCoordinator coordinator;
			coordinator.SetActiveExtent(extent);
			coordinator.RequestOutOfDate();
			EmptyFrameRecoveryState recovery;
			int activeBundle = 55;
			std::uint64_t previousRevision = 0;
			// Each iteration represents a later protected BeginFrame. Repeated
			// failures must preserve ownership and permit exactly one next attempt
			// without a new extent, OUT_OF_DATE result, or retirement-fence event.
			for(unsigned attempt = 0; attempt < 3; ++attempt) {
				const auto request = coordinator.Claim();
				context.Expect(request && request->extent == extent && request->revision > previousRevision,
					"Reservation failure must re-arm recovery at the unchanged window extent.");
				previousRevision = request->revision;
				context.Expect(!coordinator.Claim(), "A frame boundary must not claim the same retry twice.");
				bool rejected = false;
				try { EmptyFrameRetirementReservation reservation(retired); }
				catch(const std::bad_alloc&) {
					rejected = true;
					recovery.Suspend(lifecycle.Generation());
					coordinator.PublishReservationFailure();
				}
				context.Expect(rejected && activeBundle == 55 && retired.entries.empty(),
					"Failed reservation must retain the complete active bundle without partial retirement.");
				context.Expect(recovery.Suspended() && coordinator.IsRecoveryPending()
					&& !coordinator.OldSwapchainWasConsumed() && !coordinator.IsWaitingForRetirementProof()
					&& lifecycle.Generation() == originalGeneration,
					"Host allocation failure must suspend frame reuse without inventing a native handoff or completion proof.");
			}
			const auto retry = coordinator.Claim();
			context.Expect(retry && retry->extent == extent && retry->reason == PresentationRecreationReason::CandidateFailure,
				"The next frame must retry automatically after host memory becomes available.");
			retired.fail = false;
			{
				EmptyFrameRetirementReservation reservation(retired);
				context.Expect(lifecycle.Configure(3, 2, 2, 3), "Complete replacement owners must publish a newer generation.");
				reservation.Commit(activeBundle, originalGeneration);
			}
			coordinator.PublishSuccess(extent);
			context.Expect(recovery.CompleteReplacement(lifecycle.Generation(), true)
				&& !recovery.Suspended() && !coordinator.IsRecoveryPending() && !coordinator.Claim(),
				"Successful replacement must resume normal frame admission and stop scheduling retries.");
			context.Expect(activeBundle == 0 && retired.entries.size() == 1 && retired.back().resources == 55
				&& ValidateEmptyFrameAcquisition(lifecycle, {0, 0, lifecycle.Generation()}, {3, 3, 3}),
				"Only the completed replacement may resume acquisition with the old bundle retained for retirement.");
			// Keep the deterministic policy test connected to the actual native
			// exception boundary; reverting only its call site must also fail.
			const auto source = VulkanSourceContracts::ReadSources({"Engine/Renderer/RHI/Vulkan/VulkanContextInit.cpp"});
			const auto reserve = source.find("retirementReservation.emplace(retiredPresentationBundles)");
			const auto detach = source.find("PresentationResourceBundle activeBundle = TakePresentationResourceBundle()", reserve);
			context.Expect(reserve != std::string::npos && detach != std::string::npos
				&& source.substr(reserve, detach - reserve).find("presentationRecreation.PublishReservationFailure()") != std::string::npos,
				"Native reservation rejection must use the tested automatic retry transition before detaching owners.");
		});

		registry.Add("Renderer.VulkanPresentationLifecycle.FailureAndPresentFenceRetirement", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Vulkan;
			PresentationRecreationCoordinator coordinator;
			coordinator.SetActiveExtent({ 1280u, 720u });
			coordinator.RequestOutOfDate();
			context.Expect(coordinator.Claim().has_value(), "OUT_OF_DATE must force a recovery request even with an unchanged extent.");
			coordinator.PublishRetirementBlocked(true);
			context.Expect(coordinator.OldSwapchainWasConsumed() && !coordinator.Claim().has_value(), "A failed oldSwapchain handoff must suspend rather than restore or reattempt the retired chain.");
			context.Expect(coordinator.RequestRetryAfterRetirement(), "Explicit retirement proof should publish one fresh recovery revision.");
			context.Expect(coordinator.Claim().has_value(), "The fresh post-retirement revision should permit one bounded recovery attempt.");
			context.Expect(!coordinator.Claim().has_value(), "Post-retirement recovery must not retry repeatedly without another event.");

			PresentationRetirementTracker fences;
			fences.BeginRetirement(9u, 3u);
			fences.RecordPresentFence(9u, 0u, true);
			fences.RecordPresentFence(9u, 1u, true);
			context.Expect(!fences.CanDestroyRetiredBundle(9u), "A retired bundle must retain image-owned waits until every present completion fence signals.");
			fences.RecordPresentFence(8u, 2u, true);
			context.Expect(!fences.CanDestroyRetiredBundle(9u), "A stale generation completion may not release the current retired bundle.");
			fences.RecordPresentFence(9u, 2u, true);
			context.Expect(fences.CanDestroyRetiredBundle(9u), "All matching-generation present fences prove retirement ordering.");

			PresentationRetirementTracker partialCoverage;
			partialCoverage.BeginRetirement(10u, 2u);
			partialCoverage.RecordPresentFence(10u, 0u, true);
			partialCoverage.RecordPresentFence(10u, 1u, true);
			partialCoverage.RecordUnfencedPresent(10u);
			context.Expect(!partialCoverage.CanDestroyRetiredBundle(10u),
				"Reusable signaled fences cannot release a generation after one later present lost completion coverage.");

			PresentationRecreationCoordinator backlog;
			backlog.SetActiveExtent({ 1280u, 720u });
			backlog.RequestFramebufferExtent({ 1600u, 900u });
			context.Expect(backlog.Claim().has_value(), "A changed extent should claim the initial bounded recreation attempt.");
			backlog.PublishRetirementBlocked(false);
			context.Expect(!backlog.RequestSuboptimalIfExtentChanged(),
				"Advisory SUBOPTIMAL results must not spin new revisions while acquire-history proof is pending.");
			context.Expect(!backlog.Claim().has_value(), "A full retirement backlog must keep presenting instead of reclaiming the same revision.");
			context.Expect(backlog.RequestRetryAfterRetirement(), "Released fallback resources should explicitly re-arm the deferred resize.");
			context.Expect(backlog.Claim().has_value(), "The re-armed fallback revision should be claimable exactly once.");
		});

		registry.Add("Renderer.VulkanPresentationLifecycle.UnextendedAcquireProofRetiresLatestChain", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Vulkan;
			UnextendedPresentationRetirementTracker tracker;
			tracker.BeginGeneration(4u, true);
			context.Expect(!tracker.NeedsAcquireProof(), "A retired chain must remain intact until the new chain has its first present.");
			tracker.RecordPresent(4u, 2u);
			context.Expect(tracker.NeedsAcquireProof(), "The first new-chain present should request a matching completed acquire proof.");
			context.Expect(!tracker.RecordCompletedAcquire(3u, 2u), "A stale-generation acquire must not release retired bundles.");
			context.Expect(!tracker.RecordCompletedAcquire(4u, 1u), "An acquire of another image must not release the first-present anchor.");
			context.Expect(tracker.RecordCompletedAcquire(4u, 2u), "The matching completed acquire proves the retired presentation chain can be released.");
			context.Expect(!tracker.NeedsAcquireProof(), "A completed proof should release retired bundles exactly once.");
			context.Expect(!tracker.RecordCompletedAcquire(4u, 2u), "A consumed acquire proof must not release the same bundle twice.");
		});

		registry.Add("Renderer.VulkanPresentationLifecycle.UnextendedRecreationFoldsAndSeparatesMaintenance", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Vulkan;
			UnextendedPresentationRetirementTracker fallback;
			fallback.BeginGeneration(10u, true);
			// A zero-extent pause records no present/acquire and therefore leaves
			// the old bundle pending rather than inventing completion evidence.
			context.Expect(!fallback.NeedsAcquireProof(), "No presentation events during a zero-extent pause may release retired resources.");
			fallback.BeginGeneration(11u, true);
			fallback.RecordPresent(10u, 0u);
			context.Expect(!fallback.NeedsAcquireProof(), "A superseded generation may not become the release anchor.");
			fallback.RecordPresent(11u, 1u);
			context.Expect(fallback.NeedsAcquireProof(), "Multiple recreations should fold old bundles under the newest first present.");
			context.Expect(fallback.RecordCompletedAcquire(11u, 1u), "The newest matching acquire should release every folded retired bundle.");

			PresentationRetirementTracker maintenance;
			maintenance.BeginRetirement(11u, 1u);
			context.Expect(!maintenance.CanDestroyRetiredBundle(11u), "The explicit maintenance-fence path must remain independent from acquire-history proof.");
			maintenance.RecordPresentFence(11u, 0u, true);
			context.Expect(maintenance.CanDestroyRetiredBundle(11u), "Maintenance mode releases only through its own explicit present fence.");
		});
	}
}
