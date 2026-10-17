#pragma once

#include "VulkanReadbackLifecycle.h"
#include <array>
#include <string>

namespace TerraEngine::Renderer::Vulkan
{
	enum class ReadbackValidationEventKind : std::uint8_t
	{
		GateCreated, SubmitAccepted, FenceObserved, ClientTerminal, HostReleased, NativeRetired, GateDestroyed, IdleObserved,
		SourceImageDestroyed, CommandPoolDestroyed, PayloadAllocationFault
	};
	struct ReadbackValidationEvent
	{
		std::uint64_t sequence = 0;
		ReadbackValidationEventKind kind = ReadbackValidationEventKind::GateCreated;
		std::int32_t nativeResult = 0;
	};
	// Native-only bounded observations. Owner order is buffer, allocation,
	// command buffer, fence; commandPool is a dependency, never operation-owned.
	struct ReadbackLifetimeValidationSnapshot
	{
		std::string operationId;
		std::uint64_t renderSessionGeneration = 0;
		bool initialized = false;
		bool validationActuallyEnabled = false;
		bool validationCallbackInstalled = false;
		std::uint64_t validationErrorsBefore = 0;
		std::uint64_t validationErrorsAfter = 0;
		bool timelineSemaphoreEnabled = false;
		bool gateTimelineSemaphore = false;
		std::uint64_t gateInitialValue = 0;
		std::uint64_t gateWaitValue = 0;
		std::uint64_t gateSignalValue = 0;
		std::uint32_t gateWaitStageMask = 0;
		bool gateRecorded = false;
		bool submitAccepted = false;
		bool hostReleased = false;
		bool sourceImageBlocked = false;
		bool eventLogOverflow = false;
		bool payloadAllocationFaultArmed = false;
		bool payloadAllocationFaultTriggered = false;
		ReadbackClientState clientState = ReadbackClientState::Pending;
		ReadbackNativeState nativeState = ReadbackNativeState::Prepared;
		std::int32_t lastFenceResult = 0;
		bool idleObserved = false;
		std::int32_t idleResult = 0;
		std::array<std::uint32_t, 4> ownerCreated{};
		std::array<std::uint32_t, 4> ownerDestroyed{};
		std::uint32_t commandPoolDependencyReleases = 0;
		bool commandPoolAliveAtRetirement = false;
		bool commandPoolDestroyedBeforeRetirement = false;
		bool sourceImageDestroyObserved = false;
		bool sourceImageDestroyedBeforeRetirement = false;
		std::uint32_t sourceBlockerReleases = 0;
		std::uint32_t gateCreated = 0;
		std::uint32_t gateSignaled = 0;
		std::uint32_t gateDestroyed = 0;
		std::uint32_t terminalDeliveries = 0;
		std::array<ReadbackValidationEvent, 128> events{};
		std::uint32_t eventCount = 0;
	};
}
