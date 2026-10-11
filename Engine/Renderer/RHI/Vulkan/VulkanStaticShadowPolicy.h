// Pure copied-value policy. Vulkan allocation and command recording stay with
// the protected backend; these receipts never manufacture GPU completion.
#pragma once
#include "Renderer/ShadowWorkPlan.h"

namespace TerraEngine::Renderer::VulkanStaticShadow
{
	struct StaticShadowAllocationOwner
	{
		std::uint64_t sessionGeneration = 0, backendGeneration = 0;
		std::uint64_t allocationIdentity = 0, allocationGeneration = 0, actualBytes = 0;
		std::uint32_t depthFormat = 0, layers = 0;
		std::array<std::uint32_t, 2> extent{};
		bool operator==(const StaticShadowAllocationOwner&) const = default;
	};
	// Diagnostics copy the exact charged owners and outstanding native uses.
	// These values are observations only, never authority to reuse or release.
	struct StaticShadowUseReceipt
	{
		std::uint64_t commandSerial = 0, submitSerial = 0;
		std::uint32_t frameSlot = 0;
		bool operator==(const StaticShadowUseReceipt&) const = default;
	};
	struct StaticShadowAllocationReceipt
	{
		StaticShadowAllocationOwner owner;
		bool retiring = false;
		std::vector<StaticShadowUseReceipt> uses;
	};
	enum class StaticShadowTerminalProof : std::uint8_t
	{
		DeviceLost,
		DeviceIdle // Terminal teardown only: all recorded commands are abandoned.
	};
	class TERRAENGINE_API StaticShadowLifetimeTracker
	{
	public:
		// These two preparation calls may allocate; finish them before native use.
		bool RegisterAllocation(const StaticShadowAllocationOwner&);
		bool ReserveRecordedUse(const StaticShadowAllocationOwner&, std::uint64_t commandSerial, std::uint32_t frameIndex);
		bool PreserveSubmittedUse(std::uint64_t commandSerial, std::uint64_t submitSerial) noexcept;
		void DiscardUnsubmitted(std::uint64_t commandSerial) noexcept;
		void CompleteSubmission(std::uint64_t submitSerial) noexcept;
		bool RequestRetirement(const StaticShadowAllocationOwner&) noexcept;
		bool CanRelease(const StaticShadowAllocationOwner&) const noexcept;
		std::uint64_t ChargedBytes() const noexcept;
		std::uint64_t ResidentBytes() const noexcept;
		std::uint64_t RetiringBytes() const noexcept;
		// Validation capture may allocate, but leaves lifetime state unchanged.
		std::vector<StaticShadowAllocationReceipt> CaptureAllocations() const;
		bool ReleaseCompleted(const StaticShadowAllocationOwner&) noexcept;
		bool TerminalRelease(const StaticShadowAllocationOwner&, StaticShadowTerminalProof) noexcept;
	private:
		struct Use { std::uint64_t command = 0, submit = 0; std::uint32_t frame = 0; };
		struct Allocation
		{
			StaticShadowAllocationOwner owner;
			std::vector<Use> uses;
			std::uint64_t lastCommand = 0, lastSubmit = 0, lastCompletedSubmit = 0;
			bool retiring = false;
		};
		std::vector<Allocation> allocations;
	};
	enum class StaticShadowCompositionRoute : std::uint8_t { CombinedFallback, CacheCopyOverlay };
	struct StaticShadowCompositionRequest
	{
		ShadowViewKey staticView{}, finalView{};
		std::uint64_t staticContentToken = 0;
		std::uint64_t sourceRecordedCommandSerial = 0, currentCommandSerial = 0;
		StaticShadowAllocationOwner source{}, destination{};
		std::uint8_t requiredFaceMask = 0, sourceFaceMask = 0;
		bool sourceTransferSupported = false, destinationTransferSupported = false;
		bool sourceTransferUsage = false, destinationTransferUsage = false;
		bool sameShadowPipeline = false, lessDepthComparison = false;
		bool colorWritesDisabled = false, cutoutBehaviorPreserved = false;
	};
	struct StaticShadowCompositionPlan
	{
		StaticShadowCompositionRoute route = StaticShadowCompositionRoute::CombinedFallback;
		std::uint8_t requiredFaceMask = 0;
	};
	TERRAENGINE_API StaticShadowCompositionPlan BuildStaticShadowCompositionPlan(
		const StaticShadowCompositionRequest&, const ShadowCachePublicationLedger&) noexcept;
}
