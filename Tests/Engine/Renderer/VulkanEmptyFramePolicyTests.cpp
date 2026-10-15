#include "TestHarness.h"
#include "VulkanSourceTestUtils.h"
#include "Renderer/RHI/Vulkan/VulkanEmptyFramePolicy.h"

namespace TerraEngine::Tests
{
	void RegisterVulkanEmptyFramePolicyTests(TestRegistry& registry)
	{
		using namespace TerraEngine::Renderer::Vulkan;
		registry.Add("Engine.RuntimeMemorySafety.EmptyFramePolicy.AcquiredGenerationAndIndexValidation", [](TestContext& context) {
			PresentationLifecycle lifecycle;
			context.Expect(lifecycle.Configure(3, 2, 2, 4), "Fixture lifecycle must configure.");
			const AcquiredPresentationImage image{2, 3, lifecycle.Generation()};
			context.Expect(ValidateEmptyFrameAcquisition(lifecycle, image, {3, 3, 4}), "Image and frame owners use distinct counts.");
			context.Expect(!ValidateEmptyFrameAcquisition(lifecycle, image, {2, 3, 4}), "Missing frame fence must reject before indexing.");
			context.Expect(!ValidateEmptyFrameAcquisition(lifecycle, image, {3, 2, 4}), "Missing acquisition semaphore must reject.");
			context.Expect(!ValidateEmptyFrameAcquisition(lifecycle, image, {3, 3, 3}), "Missing image signal must reject.");
			context.Expect(!ValidateEmptyFrameAcquisition(lifecycle, {2, 3, image.presentationGeneration + 1}, {3, 3, 4}), "Stale generation must reject.");
		});
		registry.Add("Engine.RuntimeMemorySafety.EmptyFramePolicy.ConditionalAcquireWait", [](TestContext& context) {
			context.Expect(EmptyFrameNeedsAcquireWait(false), "Outstanding binary acquisition must be consumed.");
			context.Expect(!EmptyFrameNeedsAcquireWait(true), "Completed descriptor-prefix acquisition cannot be waited twice.");
		});
		registry.Add("Engine.RuntimeMemorySafety.EmptyFramePolicy.RecordBeforeFenceReset", [](TestContext& context) {
			EmptyFrameSubmissionProgress progress;
			context.Expect(!progress.MayResetFence() && !progress.MaySubmit(), "Unrecorded work cannot reset a fence or submit.");
			progress.RecordEnded();
			context.Expect(progress.FenceResetAccepted() && progress.MaySubmit(), "Ended recording precedes reset and submission.");
			context.Expect(progress.SubmitAccepted() && !progress.MaySubmit(), "One-time primary cannot be resubmitted.");
			const auto source = VulkanSourceContracts::ReadContextSources();
			const auto record = source.find("RecordEmptyRuntimeFallback(");
			context.Expect(record != std::string::npos && source.find("emptyProgress.MayResetFence()", record) != std::string::npos,
				"Native empty path must use the record-before-reset progression.");
		});
		registry.Add("Engine.RuntimeMemorySafety.EmptyFramePolicy.BundleRollbackAndDestructionOrder", [](TestContext& context) {
			struct Retired { int resources = 0; std::uint64_t generation = 0; };
			struct Queue {
				std::vector<Retired> entries;
				bool fail = false;
				void emplace_back() { if (fail) throw std::bad_alloc(); entries.emplace_back(); }
				Retired& back() { return entries.back(); }
				void pop_back() { entries.pop_back(); }
			} queue;
			int active = 55;
			queue.fail = true;
			try { EmptyFrameRetirementReservation reservation(queue); context.Expect(false, "Injected reservation allocation must throw."); }
			catch (const std::bad_alloc&) {}
			context.Expect(active == 55 && queue.entries.empty(), "Storage allocation fails before detaching active owners.");
			queue.fail = false;
			{ EmptyFrameRetirementReservation reservation(queue); }
			context.Expect(queue.entries.empty(), "Unused pre-handoff reservation must cancel.");
			{ EmptyFrameRetirementReservation reservation(queue); reservation.Commit(active, 7); }
			context.Expect(active == 0 && queue.entries.size() == 1 && queue.entries.back().resources == 55
				&& queue.entries.back().generation == 7, "Reserved slot commits without another allocation or retained active owner.");
			std::vector<int> buffers{11, 12}, events;
			int pass = 99;
			context.Expect(CompleteEmptyFrameBundle(2, buffers, pass), "A complete candidate covers every image.");
			context.Expect(!CompleteEmptyFrameBundle(3, buffers, pass), "Partial candidates cannot publish.");
			DestroyEmptyFrameBundle(buffers, pass, [&](int handle) { events.push_back(handle); }, [&](int handle) { events.push_back(handle); });
			context.Expect(events == std::vector<int>({11, 12, 99}) && buffers.empty() && pass == 0, "Real retirement helper destroys framebuffer owners before pass and clears ownership.");
			DestroyEmptyFrameBundle(buffers, pass, [&](int handle) { events.push_back(handle); }, [&](int handle) { events.push_back(handle); });
			context.Expect(events.size() == 3, "An already retired/rolled-back bundle cannot double destroy.");
			buffers = {13, 0}; pass = 100;
			context.Expect(!CompleteEmptyFrameBundle(2, buffers, pass), "Failed framebuffer creation rejects the candidate.");
			DestroyEmptyFrameBundle(buffers, pass, [&](int handle) { events.push_back(handle); }, [&](int handle) { events.push_back(handle); });
			context.Expect(events == std::vector<int>({11, 12, 99, 13, 100}), "Partial rollback destroys only actual owners in the same safe order.");
			const auto source = VulkanSourceContracts::ReadSources({"Engine/Renderer/RHI/Vulkan/VulkanContextInit.cpp"});
			for (const auto token : {"bundle.emptyFrameRenderPass", "bundle.emptyFrameFramebuffers", "CreateEmptyFrameFallbackResources()"})
				context.Expect(source.find(token) != std::string::npos, "Every candidate/take/restore/cleanup path must carry empty-frame resources.");
			const auto destroy = source.find("void VulkanContext::Impl::DestroyPresentationResourceBundle");
			const auto framebuffers = source.find("bundle.emptyFrameFramebuffers", destroy);
			context.Expect(framebuffers < source.find("vkDestroyRenderPass", framebuffers), "Fallback framebuffers retire before their pass.");
		});
		registry.Add("Engine.RuntimeMemorySafety.EmptyFramePolicy.RecoveryBeforeFrameReuse", [](TestContext& context) {
			EmptyFrameRecoveryState recovery;
			recovery.Suspend(7);
			context.Expect(recovery.Suspended(), "Unproven submission must suspend reuse.");
			context.Expect(!recovery.CompleteReplacement(7, true) && !recovery.CompleteReplacement(8, false), "Old or partial synchronization cannot clear suspension.");
			context.Expect(recovery.CompleteReplacement(8, true) && !recovery.Suspended(), "Complete newer owners release suspension.");
			const auto source = VulkanSourceContracts::ReadSources({"Engine/Renderer/RHI/Vulkan/VulkanContext.cpp"});
			const auto begin = source.find("void VulkanContext::BeginFrame()");
			context.Expect(source.find("emptyFrameRecovery.Suspended()", begin) < source.find("vkWaitForFences", begin), "Protected recovery must precede ordinary slot waits.");
		});
		registry.Add("Engine.RuntimeMemorySafety.EmptyFramePolicy.NoSceneShadowHiZPublication", [](TestContext& context) {
			EmptyFrameSubmittedHiZState<int> submitted;
			submitted.Confirm(11);
			int laterUnsubmittedHistory = 22;
			context.Expect(submitted.DiscardUnsubmitted(true, laterUnsubmittedHistory) && laterUnsubmittedHistory == 11,
				"Dropping later unsubmitted records restores only the actual submitted prefix metadata and pending promotion.");
			submitted.Reset();
			laterUnsubmittedHistory = 33;
			context.Expect(!submitted.DiscardUnsubmitted(true, laterUnsubmittedHistory), "Unsubmitted-only history never earns promotion.");
			submitted.Confirm(44);
			context.Expect(!submitted.DiscardUnsubmitted(false, laterUnsubmittedHistory) && !submitted.pending,
				"Reset/discard without a protected submitted-prefix proof revokes pending state.");
			for (const bool prefix : {false, true}) {
				const auto plan = MakeEmptyFramePublicationPlan(prefix);
				context.Expect(plan.discardUnsubmittedShadow && plan.discardUnsubmittedHiZ, "Empty fallback revokes every unsubmitted scene-derived candidate.");
				context.Expect(!plan.publishScene && !plan.publishShadow && !plan.publishHiZ, "Fallback owns presentation only, never scene publication.");
				context.Expect(plan.preserveSubmittedPrefix == prefix, "Previously accepted prefix receipts survive the fallback decision.");
			}
			const auto source = VulkanSourceContracts::ReadSources({"Engine/Renderer/RHI/Vulkan/VulkanFrameSubmission.cpp"});
			const auto empty = source.find("if (cmds.empty())");
			const auto ordinary = source.find("VkSubmitInfo submitInfo", empty);
			const auto branch = source.substr(empty, ordinary - empty);
			context.Expect(branch.find("DiscardRecordedShadowWrites") != std::string::npos
				&& branch.find("DiscardRecordedHiZWrites") != std::string::npos, "Fallback revokes unsubmitted scene-derived candidates.");
			context.Expect(branch.find("ConfirmShadowWritesSubmitted") == std::string::npos
				&& branch.find("ConfirmHiZWritesSubmitted") == std::string::npos, "Fallback cannot publish scene ownership receipts.");
		});
		registry.Add("Engine.RuntimeMemorySafety.EmptyFramePolicy.ImageOwnedPresentationSignals", [](TestContext& context) {
			context.ExpectEqual(5u, EmptyFramePresentSignalIndex({1, 5, 9}), "Presentation signal belongs to image, not frame slot.");
			const auto source = VulkanSourceContracts::ReadContextSources();
			context.Expect(source.find("EmptyFramePresentSignalIndex(*impl->acquiredPresentationImage)") != std::string::npos,
				"Native path must use the tested signal owner.");
			context.Expect(source.find("ResolveUnenqueuedPresentationFence") != std::string::npos
				&& source.find("RecordUnextendedPresent") != std::string::npos, "Existing completion proofs remain connected.");
		});
		registry.Add("Engine.RuntimeMemorySafety.EmptyFramePolicy.ColorOnlyPersistentFallback", [](TestContext& context) {
			const auto plan = MakeEmptyFrameColorPassPlan();
			context.Expect(plan.colorAttachmentCount == 1 && plan.samples == 1 && !plan.hasDepth && !plan.needsTransferUsage,
				"The actual native-consumed pass plan uses exactly one single-sample color attachment.");
			context.Expect(plan.clear && plan.store && plan.discardInitialContents && plan.presentFinalLayout,
				"Clear/store and undefined-to-present layout apply to fresh and previously presented images.");
			context.Expect(plan.clearColor == std::array<float, 4>{0, 0, 0, 1}, "Empty fallback produces opaque black.");
			const auto source = VulkanSourceContracts::ReadSources({"Engine/Renderer/RHI/Vulkan/VulkanContextInit.cpp"});
			const auto start = source.find("bool VulkanContext::Impl::CreateEmptyFrameFallbackResources()");
			context.Expect(start != std::string::npos, "A dedicated fallback resource constructor is required.");
			const auto end = source.find("bool VulkanContext::Impl::CreateCommandPool", start);
			const auto body = source.substr(start, end - start);
			for (const auto token : {"VK_ATTACHMENT_LOAD_OP_CLEAR", "VK_ATTACHMENT_STORE_OP_STORE", "VK_IMAGE_LAYOUT_UNDEFINED", "VK_IMAGE_LAYOUT_PRESENT_SRC_KHR"})
				context.Expect(body.find(token) != std::string::npos, "Fallback pass must clear and establish presentation layout.");
			context.Expect(body.find("pDepthStencilAttachment") == std::string::npos && body.find("TRANSFER") == std::string::npos,
				"Empty-frame pass must not assume scene depth or swapchain transfer usage.");
			context.Expect(source.find("emptyFrameCommandBuffers.resize(MAX_FRAMES_IN_FLIGHT)") != std::string::npos, "Fallback primaries must be persistent per-slot pool owners.");
		});
	}
}
