#pragma once
#include "Renderer/RHI/Vulkan/VulkanEmptyFramePolicy.h"
#include <nlohmann/json.hpp>
#include <optional>
namespace TerraEngine::Validation::RuntimeMemorySafety {
using Json = nlohmann::json;
// Actual producer formatter consumes an independently observed post-operation
// ledger value. Missing checkpoints stay null/unavailable, never copied Before.
inline nlohmann::json FormatNativeAcquireReceipt(
	const TerraEngine::Renderer::Vulkan::EmptyFrameValidationReceipt& value,
	std::optional<bool> observedAcquireWaitConsumedAfter)
{
		return {{"sequence", value.sequence}, {"correlationId", value.correlationId}, {"generation", value.generation},
			{"frameSlot", value.frameSlot}, {"imageIndex", value.imageIndex}, {"acquired", value.acquired},
			{"swapchainImageCount", value.swapchainImageCount}, {"frameSlotCount", value.frameSlotCount},
			{"forcedEmpty", value.forcedEmpty}, {"commandBufferCount", value.commandBufferCount},
			{"recordEnded", value.recordEnded}, {"acquireWaitConsumedBefore", value.acquireWaitConsumedBefore},
			{"acquireWaitConsumed", observedAcquireWaitConsumedAfter ? Json(*observedAcquireWaitConsumedAfter) : Json(nullptr)},
			{"acquireWaitConsumedAfterObserved", observedAcquireWaitConsumedAfter.has_value()},
			{"acquireWaitConsumedAfterUnavailableReason", observedAcquireWaitConsumedAfter ? Json(nullptr) : Json("No post-operation acquisition ledger checkpoint was observed")}, {"fallbackCommandSubmitted", value.forcedEmpty && value.submitAccepted && value.commandBufferCount == 1U},
			{"resetAttempted", value.resetAttempted}, {"resetResult", value.resetAttempted ? Json(value.resetResult) : Json(nullptr)},
			{"fenceResetAccepted", value.fenceResetAccepted}, {"submitAttempted", value.submitAttempted},
			{"submitResult", value.submitAttempted ? Json(value.submitResult) : Json(nullptr)}, {"submitAccepted", value.submitAccepted},
			{"presentAttempted", value.presentAttempted}, {"presentResult", value.presentAttempted ? Json(value.presentResult) : Json(nullptr)},
			{"waitIdleAttempted", value.waitIdleAttempted}, {"waitIdleResult", value.waitIdleAttempted ? Json(value.waitIdleResult) : Json(nullptr)},
			{"waitIdleCompletionObserved", value.waitIdleCompletionObserved}, {"ordinarySubmitAccepted", value.ordinarySubmitAccepted},
			{"ordinarySubmissionSerial", value.ordinarySubmissionSerial}, {"ordinaryCompletionObserved", value.ordinaryCompletionObserved},
			{"acquisitionDrainSubmitted", value.acquisitionDrainSubmitted}, {"acquisitionDrainCompleted", value.acquisitionDrainCompleted},
			{"recoverySuspended", value.recoverySuspended}, {"rejectedCheckpoint", static_cast<unsigned>(value.rejectedCheckpoint)},
			{"candidateRejected", value.candidateRejected}, {"candidateCheckpoint", static_cast<unsigned>(value.candidateCheckpoint)},
			{"candidateFallbackPassCreated", value.candidateFallbackPassCreated}, {"candidatePartialCleanupCompleted", value.candidatePartialCleanupCompleted},
			{"candidateOldSwapchainConsumed", value.candidateOldSwapchainConsumed}, {"maintenanceEnabled", value.maintenanceEnabled},
			{"validationErrorsBefore", value.validationErrorsBefore}, {"validationErrorsAfter", value.validationErrorsAfter}};
	}
}
