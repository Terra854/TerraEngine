// Static cache destruction runs only under the protected device boundary.
// Logical invalidation never substitutes for completion of native image users.
#include "VulkanResourceSource.h"

void VulkanContext::Impl::CaptureStaticShadowAllocations(TerraEngine::Renderer::RendererEfficiencyStaticShadowExecution& execution) const
{
	// Called only for disposable completed-evidence staging. Production does not
	// copy this inventory, and these values authorize neither reuse nor release.
	for (const auto& receipt : staticShadowLifetime.CaptureAllocations()) {
		const auto& owner = receipt.owner;
		TerraEngine::Renderer::RendererEfficiencyStaticShadowAllocation allocation;
		allocation.sessionGeneration = owner.sessionGeneration; allocation.backendGeneration = owner.backendGeneration;
		allocation.allocationIdentity = owner.allocationIdentity; allocation.allocationGeneration = owner.allocationGeneration;
		allocation.actualBytes = owner.actualBytes; allocation.depthFormat = owner.depthFormat;
		allocation.layers = owner.layers; allocation.extent = owner.extent; allocation.retiring = receipt.retiring;
		for (const auto& use : receipt.uses) allocation.uses.push_back({use.commandSerial, use.submitSerial, use.frameSlot});
		execution.allocations.push_back(std::move(allocation));
	}
	for (auto receipt : staticShadowCompletedSubmissions) if (receipt) execution.completedSubmissions.push_back(receipt);
	for (const auto& accepted : staticShadowAcceptedSubmissions)
		if (accepted.commandSerial && accepted.submitSerial) execution.acceptedSubmissions.push_back(accepted);
	for (auto command : staticShadowDiscardedCommands) if (command) execution.discardedCommands.push_back(command);
	const auto unique = [](auto& values) { std::sort(values.begin(), values.end()); values.erase(std::unique(values.begin(), values.end()), values.end()); };
	unique(execution.completedSubmissions); unique(execution.discardedCommands);
}

void VulkanContext::Impl::RetireStaticShadowEntries() noexcept
{
	staticShadowPublicationLedger.Reset(renderSessionGeneration);
	for (auto& entry : staticShadowEntries)
	{
		entry->retiring = true;
		staticShadowPublicationLedger.InvalidatePhysicalRegion(entry->staticView);
		staticShadowLifetime.RequestRetirement(entry->owner);
	}
}

void VulkanContext::Impl::ReleaseCompletedStaticShadowEntries() noexcept
{
	// Erasing an owner destroys framebuffers before views/images through RAII.
	// References are exact command receipts, so an older completed frame cannot
	// free storage also referenced by another recorded or submitted primary.
	std::erase_if(staticShadowEntries, [&](const auto& entry) {
		if (!entry->retiring || !staticShadowLifetime.CanRelease(entry->owner)) return false;
		staticShadowPublicationLedger.InvalidatePhysicalRegion(entry->staticView);
		return staticShadowLifetime.ReleaseCompleted(entry->owner);
	});
}

bool VulkanContext::Impl::ReleaseStaticShadowEntriesBeforeColorReplacement() noexcept
{
	RetireStaticShadowEntries();
	// Called only after the pre-existing successful device idle. Submitted users
	// have now completed; unsubmitted recorded commands must still block release.
	for (std::uint32_t frame = 0; frame < shadowSubmitReceiptsByFrame.size(); ++frame)
		CompleteShadowWrites(frame);
	ReleaseCompletedStaticShadowEntries();
	return staticShadowEntries.empty();
}

void VulkanContext::Impl::ReleaseStaticShadowEntriesForTeardown(bool lost) noexcept
{
	// Cleanup has abandoned all pending primaries and proven terminal idle/loss.
	// This terminal receipt must never be used for ordinary budget lowering.
	using Proof = TerraEngine::Renderer::VulkanStaticShadow::StaticShadowTerminalProof;
	for (auto& entry : staticShadowEntries)
		staticShadowLifetime.TerminalRelease(entry->owner, lost ? Proof::DeviceLost : Proof::DeviceIdle);
	staticShadowEntries.clear();
	staticShadowPublicationLedger.Reset(renderSessionGeneration);
	if (device != VK_NULL_HANDLE && staticShadowLoadRenderPass != VK_NULL_HANDLE)
		vkDestroyRenderPass(device, staticShadowLoadRenderPass, nullptr);
	staticShadowLoadRenderPass = VK_NULL_HANDLE;
}
