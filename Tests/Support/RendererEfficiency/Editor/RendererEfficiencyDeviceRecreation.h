#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

// Validation requests carry the exact owner and backend lifetime they intend
// to replace. A queued request is never evidence of device recreation.
struct RendererEfficiencyDeviceRecreationRequest
{
	std::string operationId, fixtureStepId, fixtureId, fixtureHash;
	std::filesystem::path projectRoot;
	std::uint64_t projectGeneration = 0, expectedRendererSessionGeneration = 0, expectedBackendResourceGeneration = 0;
};
enum class RendererEfficiencyDeviceRecreationStatus { Pending, Rejected, Completed, Unavailable };
struct RendererEfficiencyDeviceRecreationReceipt
{
	RendererEfficiencyDeviceRecreationRequest request;
	RendererEfficiencyDeviceRecreationStatus status = RendererEfficiencyDeviceRecreationStatus::Rejected;
	std::uint64_t oldRendererSessionGeneration = 0, newRendererSessionGeneration = 0;
	std::uint64_t oldBackendResourceGeneration = 0, newBackendResourceGeneration = 0;
	bool idleProven = false, rebound = false;
	std::string reason;
};

// These pure checks reject incomplete/stale requests before any native handles
// are released. Actual project ownership is additionally checked by the service.
inline bool IsCompleteRendererEfficiencyDeviceRecreationRequest(const RendererEfficiencyDeviceRecreationRequest& request)
{
	return !request.operationId.empty() && !request.fixtureStepId.empty() && !request.fixtureId.empty() && !request.fixtureHash.empty()
		&& !request.projectRoot.empty() && request.projectGeneration && request.expectedRendererSessionGeneration && request.expectedBackendResourceGeneration;
}
inline bool MatchesRendererEfficiencyDeviceRecreationBackend(const RendererEfficiencyDeviceRecreationRequest& request,
	std::uint64_t session, std::uint64_t backend)
{
	return IsCompleteRendererEfficiencyDeviceRecreationRequest(request)
		&& request.expectedRendererSessionGeneration == session && request.expectedBackendResourceGeneration == backend;
}
inline bool HasCompletedRendererEfficiencyDeviceRecreation(const RendererEfficiencyDeviceRecreationReceipt& receipt)
{
	return receipt.status == RendererEfficiencyDeviceRecreationStatus::Completed && receipt.idleProven && receipt.rebound
		&& MatchesRendererEfficiencyDeviceRecreationBackend(receipt.request, receipt.oldRendererSessionGeneration, receipt.oldBackendResourceGeneration)
		&& receipt.newRendererSessionGeneration > receipt.oldRendererSessionGeneration && receipt.newBackendResourceGeneration > receipt.oldBackendResourceGeneration;
}

// The protected editor owner supplies the actual platform backend operations.
// Keeping the window explicit lets the same lifecycle be checked with bundled
// GLFW/ImGui in a native regression without constructing a Vulkan device.
template<typename Window, typename Shutdown, typename Initialize, typename Validate>
bool RebindRendererEfficiencyPlatformBackend(Window* expectedWindow, Shutdown&& shutdown, Initialize&& initialize, Validate&& validate)
{
	// A missing application window cannot authorize releasing a live backend.
	// Initialization and readiness are separate: an allocated platform owner
	// alone does not prove that its main viewport points at the original window.
	if (!expectedWindow) return false;
	shutdown();
	if (!initialize()) return false;
	return validate();
}

