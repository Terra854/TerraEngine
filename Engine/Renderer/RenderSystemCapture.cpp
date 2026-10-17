// RenderSystem capture facade, preserving existing protected ownership and publication boundaries.
#include "Frame/RenderFrameState.h"
#include "Renderer/RHI/RHIContext.h"
#include "ThreadPool/ThreadPool.h"
#include "Renderer/RenderCaptureEncoding.h"
#include "Renderer/SyntheticCapture.h"
#include "Resource/ResourceManager.h"
#include "Window/WindowSystem.h"
#include "Core/Logger/Logger.h"
#include "Profiling/ProfilerScope.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "Frame/RenderCaptureOperationState.h"

namespace {
	// Polling shares the existing frame/lifecycle gate, but a busy protected
	// boundary is normal contention rather than a recursive public entry error.
	class CapturePollingBoundary final {
	public:
		explicit CapturePollingBoundary(std::atomic_flag& flag) noexcept
			: flag_(flag), owns_(!flag.test_and_set(std::memory_order_acquire)) {}
		~CapturePollingBoundary() { if(owns_) flag_.clear(std::memory_order_release); }
		bool OwnsLock() const noexcept { return owns_; }
		CapturePollingBoundary(const CapturePollingBoundary&) = delete;
		CapturePollingBoundary& operator=(const CapturePollingBoundary&) = delete;
	private:
		std::atomic_flag& flag_;
		bool owns_;
	};
}

TerraEngine::Renderer::RenderCaptureLimits RenderSystem::GetCaptureLimits() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return captureLimits;
}

TerraEngine::Renderer::RenderCaptureQueueState RenderSystem::GetCaptureQueueState() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return captureQueueState;
}

TerraEngine::Renderer::RenderCaptureAvailability RenderSystem::GetCaptureAvailability() const
{
	TerraEngine::Renderer::RenderCaptureAvailability availability;
	availability.rendererInitialized = initialized && rhi != nullptr;
	if (rhi)
	{
		const TerraEngine::Renderer::BackendCapabilityProfile profile = rhi->GetBackendCapabilityProfile();
		availability.backendId = profile.backendId;
		availability.backendReadbackAvailable = profile.supportsImageReadback;
	}
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		for (const TerraEngine::Renderer::RenderFrameViewRequest& request : latestRenderFrameSnapshot.viewRequests)
		{
			if (request.viewType == RenderViewType::Game)
			{
				availability.gameViewAvailable = true;
				break;
			}
		}
	}
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		availability.limits = captureLimits;
		availability.queueState = captureQueueState;
		availability.queueFull = captureQueueState.pendingCaptureCount >= captureLimits.maxQueuedCaptures;
		availability.overBudget = false;
		if (hasLatestGameViewCamera)
		{
			availability.gameViewAvailable = true;
		}
	}
	if (availability.queueFull)
	{
		availability.available = false;
		availability.state = "queueFull";
		availability.message = "Capture queue is full.";
		availability.retryable = true;
	}
	else if (!availability.rendererInitialized && !availability.deterministicFallbackAvailable)
	{
		availability.available = false;
		availability.state = "rendererUnavailable";
		availability.message = "Renderer is not initialized and no deterministic capture fallback is available.";
		availability.retryable = true;
	}
	if (!availability.backendReadbackAvailable)
	{
		availability.notes.push_back("Backend image readback is unavailable; captures use the deterministic PNG fallback.");
	}
	if (!availability.gameViewAvailable)
	{
		availability.notes.push_back("No completed Game View frame is currently published.");
	}
	return availability;
}

std::string RenderSystem::QueueCaptureRequest(TerraEngine::Renderer::RenderCaptureRequest request, std::string& errorMessage)
{
	std::string admissionId;
	return QueueCaptureRequestInternal(std::move(request), errorMessage, admissionId);
}

std::string RenderSystem::QueueCaptureRequestInternal(TerraEngine::Renderer::RenderCaptureRequest request,
	std::string& errorMessage, std::string& admissionId)
{
	try { (void)TerraEngine::Renderer::ValidateRgba8PngLayout(request.targetSize.x, request.targetSize.y); }
	catch (const std::exception& exception) { errorMessage = exception.what(); return {}; }
	try {
	auto operation = std::make_shared<CaptureOperation>();
	std::lock_guard lock(statsMutex);
	if ((captureLimits.maxWidth && request.targetSize.x > *captureLimits.maxWidth)
		|| (captureLimits.maxHeight && request.targetSize.y > *captureLimits.maxHeight)) {
		errorMessage = "Capture target size exceeds configured limits."; return {};
	}
	const auto operationCount = captureOperations.size();
	const bool logicalCapacityFull = operationCount >= captureLimits.maxQueuedCaptures
		&& operationCount - captureLimits.maxQueuedCaptures >= captureLimits.maxConcurrentCaptures;
	if (pendingCaptureRequests.size() >= captureLimits.maxQueuedCaptures || logicalCapacityFull) {
		errorMessage = "Capture queue is full."; return {};
	}
	if (nextCaptureAdmissionSequence == std::numeric_limits<std::uint64_t>::max()) {
		errorMessage = "Capture admission identity exhausted."; return {};
	}
	if (request.requestId.empty()) request.requestId = "render-capture-" + std::to_string(nextCaptureRequestSequence++);
	if (request.timeoutMs == 0) request.timeoutMs = captureLimits.defaultTimeoutMs;
	// Publication capacity and the timeout record are prepared before any live
	// admission. Shared-pointer moves into this bounded vector cannot allocate.
	if (nextSyntheticCaptureCheckpoint && nextSyntheticCaptureCheckpoint->fault == TerraEngine::Renderer::SyntheticCaptureFault::DeliveryReservation) {
		nextSyntheticCaptureCheckpoint.reset();
		throw std::bad_alloc();
	}
	completedCaptureResults.reserve(captureLimits.maxQueuedCaptures);
	operation->original = request;
	operation->publicResultId = request.requestId;
	operation->admissionId = "render-capture-admission-" + std::to_string(nextCaptureAdmissionSequence++);
	request.requestId = operation->admissionId;
	operation->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(request.timeoutMs);
	operation->timeoutResult.requestId = operation->admissionId;
	operation->timeoutResult.sourceKind = request.sourceKind;
	operation->timeoutResult.sourceName = request.sourceName;
	operation->timeoutResult.size = request.targetSize;
	operation->timeoutResult.freshness = request.freshness;
	operation->timeoutResult.message = "Capture request exceeded its admitted deadline.";
	operation->timeoutResult.errorCategory = "timeout";
	// Keep public and backend correlation separate. A repeated public ID can
	// never let an old worker result satisfy a newer synchronous admission.
	// Prepare all caller delivery strings before publishing either queue owner.
	std::string publicId = operation->original.requestId;
	admissionId = operation->admissionId;
	captureOperations.emplace(operation->admissionId, operation);
	try { pendingCaptureRequests.push_back(std::move(request)); }
	catch (...) { captureOperations.erase(operation->admissionId); throw; }
	captureQueueState.pendingCaptureCount = static_cast<std::uint32_t>(pendingCaptureRequests.size());
	errorMessage.clear();
	return publicId;
	} catch (const std::exception& exception) { errorMessage = exception.what(); return {}; }
}

std::optional<TerraEngine::Renderer::RenderCaptureResult> RenderSystem::PollCaptureResult(std::string_view requestId)
{
	std::shared_ptr<CaptureOperation> retired;
	std::lock_guard lock(statsMutex);
	for (auto it = completedCaptureResults.begin(); it != completedCaptureResults.end(); ++it) {
		if ((*it)->delivery.requestId != requestId) continue;
		auto result = std::move((*it)->delivery);
		retired = std::move(*it);
		completedCaptureResults.erase(it);
		captureQueueState.completedResultCount = static_cast<std::uint32_t>(completedCaptureResults.size());
		return result;
	}
	return {};
}

std::optional<TerraEngine::Renderer::RenderCaptureResult> RenderSystem::PollCaptureAdmission(std::string_view admissionId)
{
	std::shared_ptr<CaptureOperation> retired;
	std::lock_guard lock(statsMutex);
	for (auto it = completedCaptureResults.begin(); it != completedCaptureResults.end(); ++it) {
		if ((*it)->admissionId != admissionId) continue;
		auto result = std::move((*it)->delivery);
		retired = std::move(*it);
		completedCaptureResults.erase(it);
		captureQueueState.completedResultCount = static_cast<std::uint32_t>(completedCaptureResults.size());
		return result;
	}
	return {};
}

void RenderSystem::PollSyntheticCaptures()
{
	std::vector<std::shared_ptr<CaptureOperation>> operations;
	{
		std::lock_guard lock(statsMutex);
		for (const auto& [id, operation] : captureOperations) operations.push_back(operation);
	}
	for (const auto& operation : operations) {
		if (!operation->clientTerminal && !operation->logicalRevoked && std::chrono::steady_clock::now() >= operation->deadline) {
			const bool noPhysicalOwner = !operation->syntheticStarted && !operation->backendPending;
			if (noPhysicalOwner) {
				std::lock_guard lock(statsMutex);
				std::erase_if(pendingCaptureRequests, [&operation](const auto& pending) { return pending.requestId == operation->admissionId; });
				std::erase_if(submittedCaptureReadbackRequests, [&operation](const auto& pending) { return pending.requestId == operation->admissionId; });
				captureQueueState.pendingCaptureCount = static_cast<std::uint32_t>(pendingCaptureRequests.size());
			}
			CompleteCapture(std::move(operation->timeoutResult), noPhysicalOwner);
		}
		if (operation->syntheticStarted) {
			if (auto result = operation->synthetic.TakeResult()) CompleteCapture(std::move(*result), false);
		}
		if (!operation->syntheticStarted || !operation->synthetic.IsPhysicallySettled()) continue;
		std::lock_guard lock(statsMutex);
		if (!operation->clientTerminal) continue;
		if (operation->activeCharged) {
			--captureQueueState.activeCaptureCount;
			operation->activeCharged = false;
		}
		captureOperations.erase(operation->admissionId);
	}
	// The vector retains each job until outside statsMutex, including fast jobs
	// whose last callable capture disappeared while publishing its future.
}

void RenderSystem::RetireCaptureJobs()
{
	std::vector<std::shared_ptr<CaptureOperation>> retired;
	decltype(completedCaptureResults) retiredResults;
	std::shared_ptr<TerraEngine::Renderer::SyntheticCaptureCheckpoint> retiredCheckpoint;
	{
		std::lock_guard lock(statsMutex);
		retired.reserve(captureOperations.size());
		retiringCaptures = true;
		retiredResults.swap(completedCaptureResults);
		retiredCheckpoint = std::move(nextSyntheticCaptureCheckpoint);
		for (auto iterator = captureOperations.begin(); iterator != captureOperations.end();) {
			const auto& operation = iterator->second;
			operation->logicalRevoked = true;
			// An accepted GPU owner survives logical scene/session revocation.
			// Only its original backend's Released proof can release its charge.
			if (operation->backendPending && !operation->physicalSettled) { ++iterator; continue; }
			retired.push_back(operation);
			iterator = captureOperations.erase(iterator);
		}
		pendingCaptureRequests.clear();
		submittedCaptureReadbackRequests.clear();
		captureQueueState.pendingCaptureCount = 0;
		captureQueueState.completedResultCount = 0;
	}
	// CPU owners settle outside queue/diagnostic locks while the executor is
	// alive. Charges remain visible until their last captured input has died.
	for (auto& operation : retired) operation->synthetic.Join();
	{
		std::lock_guard lock(statsMutex);
		for (auto& operation : retired) if (operation->activeCharged) {
			--captureQueueState.activeCaptureCount;
			operation->activeCharged = false;
		}
		retiringCaptures = false;
	}
}

void RenderSystem::PollCaptureBackendOwnership()
{
	if (!rhi) return;
	std::vector<std::shared_ptr<CaptureOperation>> operations;
	{
		std::lock_guard lock(statsMutex);
		for (const auto& [id, operation] : captureOperations) {
			if (operation->backendPending && operation->backendOwner == rhi.get()) operations.push_back(operation);
		}
	}
	for (const auto& operation : operations) {
		// Unsupported is never release evidence, and an owner from a different
		// backend/session is never queried against this replacement instance.
		if (rhi->GetCaptureReadbackOwnership(operation->admissionId) != TerraEngine::Renderer::CaptureReadbackOwnership::Released) continue;
		std::lock_guard lock(statsMutex);
		operation->physicalSettled = true;
		if (operation->activeCharged) {
			--captureQueueState.activeCaptureCount;
			operation->activeCharged = false;
		}
		if (operation->clientTerminal || operation->logicalRevoked) captureOperations.erase(operation->admissionId);
	}
}

bool RenderSystem::TryReleaseCaptureBackend()
{
	if (!rhi) return true;
	rhi->Shutdown();
	DrainCaptureReadbacks();
	if (!rhi->IsShutdownComplete()) return false;
	{
		std::lock_guard lock(statsMutex);
		for (const auto& [id, operation] : captureOperations) {
			if (operation->backendPending && operation->backendOwner == rhi.get() && !operation->physicalSettled) return false;
		}
	}
	// Reset only after both device teardown and per-admission native retirement
	// are proven. No later result can consult this old backend instance.
	rhi.reset();
	return true;
}

void RenderSystem::ProcessQueuedCaptureRequests()
{
	DrainCaptureReadbacks();
	std::deque<TerraEngine::Renderer::RenderCaptureRequest> requests;
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		while (!submittedCaptureReadbackRequests.empty())
		{
			requests.push_back(std::move(submittedCaptureReadbackRequests.front()));
			submittedCaptureReadbackRequests.pop_front();
		}
		captureQueueState.pendingCaptureCount = static_cast<std::uint32_t>(pendingCaptureRequests.size());
	}

	if (requests.empty())
	{
		const bool dedicatedBackendAvailable = initialized && rhi
			&& rhi->GetBackendCapabilityProfile().supportsImageReadback;
		std::lock_guard<std::mutex> lock(statsMutex);
		// Only an existing viewport readback may bypass render preparation. Custom,
		// camera-object, and dedicated next-frame Game captures must remain pending
		// until RenderSystem builds their view, seals its immutable publication, and
		// moves the exact request to submittedCaptureReadbackRequests. Draining those
		// requests here raced the renderer under heavy workloads and copied a cleared
		// capture target before its commands had even been submitted.
		for (auto iterator = pendingCaptureRequests.begin();
			iterator != pendingCaptureRequests.end()
			&& captureQueueState.activeCaptureCount < captureLimits.maxConcurrentCaptures;)
		{
			const bool requiresDedicatedRender =
				iterator->sourceKind != TerraEngine::Renderer::RenderCaptureSourceKind::GameView
				|| iterator->dedicatedRenderTarget;
			if (requiresDedicatedRender && dedicatedBackendAvailable)
			{
				++iterator;
				continue;
			}
			if (auto found = captureOperations.find(iterator->requestId); found != captureOperations.end()) found->second->activeCharged = true;
			requests.push_back(std::move(*iterator));
			iterator = pendingCaptureRequests.erase(iterator);
			++captureQueueState.activeCaptureCount;
		}
		captureQueueState.pendingCaptureCount = static_cast<std::uint32_t>(pendingCaptureRequests.size());
	}

	for (const TerraEngine::Renderer::RenderCaptureRequest& request : requests)
	{
		{
			std::lock_guard lock(statsMutex);
			auto found = captureOperations.find(request.requestId);
			// A frame continuation may carry a descriptor across admission expiry
			// or Clear. Never start CPU/native work after its logical owner retired.
			if (found == captureOperations.end() || found->second->clientTerminal || found->second->logicalRevoked) continue;
		}
		TerraEngine::Renderer::RenderCaptureResult result;
		result.requestId = request.requestId;
		result.sourceKind = request.sourceKind;
		result.sourceName = request.sourceName;
		result.size = request.targetSize;
		// Game View captures report the last frame that actually submitted a
		// Game/runtime view; custom offscreen captures report the frame boundary
		// that drained the queue. Both values are read-only diagnostics.
		const std::uint64_t captureFrameId = request.sourceKind == TerraEngine::Renderer::RenderCaptureSourceKind::GameView
			&& !request.dedicatedRenderTarget
			&& latestGameViewFrameId != 0u
			? latestGameViewFrameId
			: activeRenderFrameId;
		result.frameId = captureFrameId;
		result.freshness = request.freshness;
		result.overlaysIncluded = request.includeOverlays;

		// A normal Game View readback needs the last frame published by the dock.
		// A dedicated request already contains an immutable authored-camera
		// snapshot, so it remains valid even when the dock has never rendered.
		if (request.sourceKind == TerraEngine::Renderer::RenderCaptureSourceKind::GameView
			&& !request.dedicatedRenderTarget
			&& !hasLatestGameViewCamera)
		{
			result.message = "No active Game View camera is available.";
			result.errorCategory = "missing_target";
		}
		else if (request.format != "image/png")
		{
			result.message = "Only image/png capture output is currently supported.";
			result.errorCategory = "unsupported_readback";
		}
		else if (initialized && rhi && rhi->GetBackendCapabilityProfile().supportsImageReadback)
		{
			// Stamp the immutable request copy with the exact frame whose output
			// the backend is about to copy. The caller-owned descriptor remains
			// unchanged and can be reused safely.
			TerraEngine::Renderer::RenderCaptureRequest backendRequest = request;
			backendRequest.submittedFrameId = captureFrameId;
			if (rhi->GetCaptureReadbackOwnership(request.requestId) == TerraEngine::Renderer::CaptureReadbackOwnership::Unsupported) {
				result.errorCategory = "backend_readback_unavailable";
				result.message = "Backend readback ownership reporting is unavailable; native admission was refused.";
			} else {
				result = rhi->ReadbackCapture(backendRequest);
			}
			result.requestId = request.requestId;
			result.sourceKind = request.sourceKind;
			result.sourceName = request.sourceName;
			if (result.size.x == 0u || result.size.y == 0u)
			{
				result.size = request.targetSize;
			}
			if (result.frameId == 0u)
			{
				result.frameId = captureFrameId;
			}
			result.freshness = request.freshness;
		}

		const bool backendFallbackEligible = result.errorCategory == "internal"
			|| result.errorCategory == "backend_readback_unavailable"
			|| result.errorCategory == "backend_error";
		if (!result.pendingAsync && !result.ok && backendFallbackEligible && request.allowBackendFallback)
		{
			std::shared_ptr<CaptureOperation> operation;
			std::shared_ptr<TerraEngine::Renderer::SyntheticCaptureCheckpoint> checkpoint;
			{
				std::lock_guard lock(statsMutex);
				auto found = captureOperations.find(request.requestId);
				if (found != captureOperations.end()) operation = found->second;
				checkpoint = std::move(nextSyntheticCaptureCheckpoint);
				if (operation) operation->timeoutResult.frameId = captureFrameId;
			}
			std::string error;
			if (operation && operation->synthetic.Start(request, captureFrameId, &ThreadPool::instance(), std::move(checkpoint), error)) {
				operation->syntheticStarted = true;
				continue;
			}
			result.message = error.empty() ? "Synthetic capture admission failed." : std::move(error);
			result.errorCategory = "capture_admission_failed";
			result.base64Data.clear();
		}

		if (result.pendingAsync) {
			std::lock_guard lock(statsMutex);
			if (auto found = captureOperations.find(request.requestId); found != captureOperations.end()) {
				found->second->backendPending = true;
				found->second->backendOwner = rhi.get();
				found->second->timeoutResult.frameId = result.frameId;
				found->second->timeoutResult.backendTarget = result.backendTarget;
			}
		}
		if (!result.pendingAsync)
		{
			CompleteCapture(std::move(result));
		}
	}
}

void RenderSystem::CompleteCapture(TerraEngine::Renderer::RenderCaptureResult result, bool physicallySettled)
{
	std::shared_ptr<CaptureOperation> operation;
	std::shared_ptr<CaptureOperation> evicted;
	{
		std::lock_guard lock(statsMutex);
		auto found = captureOperations.find(result.requestId);
		if (found == captureOperations.end()) return;
		operation = found->second;
		operation->physicalSettled = operation->physicalSettled || physicallySettled;
		if (!operation->clientTerminal && !operation->logicalRevoked) {
			result.requestId.swap(operation->publicResultId);
			operation->delivery = std::move(result);
			// QueueCaptureRequestInternal reserved the full bound. Evict before
			// push so publication never grows storage or loses a terminal outcome.
			if (completedCaptureResults.size() == captureLimits.maxQueuedCaptures) {
				evicted = std::move(completedCaptureResults.front());
				completedCaptureResults.erase(completedCaptureResults.begin());
			}
			completedCaptureResults.push_back(operation);
			operation->clientTerminal = true;
			captureQueueState.completedResultCount = static_cast<std::uint32_t>(completedCaptureResults.size());
		}
		if (operation->physicalSettled) {
			if (operation->activeCharged) { --captureQueueState.activeCaptureCount; operation->activeCharged = false; }
			captureOperations.erase(found);
		}
	}
	// Diagnostic allocation follows committed delivery, and owns no task data.
	try {
		std::lock_guard lock(statsMutex);
		latestRenderGraphDiagnostics.captureSchedulers.push_back(TerraEngine::Renderer::BuildRenderCaptureSchedulerDiagnostics(
			"RenderSystem", "Render", TerraEngine::Renderer::ToString(operation->original.sourceKind),
			operation->delivery.backendReadback, operation->delivery.deterministicFallback));
		if (latestRenderGraphDiagnostics.captureSchedulers.size() > 16) latestRenderGraphDiagnostics.captureSchedulers.erase(latestRenderGraphDiagnostics.captureSchedulers.begin());
	} catch (...) { /* Diagnostic retention must not undo terminal publication. */ }
}

void RenderSystem::DrainCaptureReadbacks()
{
	PollSyntheticCaptures();
	if (!rhi)
	{
		return;
	}
	// PollCaptureReadbacks never waits for the GPU. Each completed item is
	// committed through the same bounded queue path as synchronous fallbacks.
	for (TerraEngine::Renderer::RenderCaptureResult& result : rhi->PollCaptureReadbacks())
	{
		CompleteCapture(std::move(result), false);
	}
	PollCaptureBackendOwnership();
	DrainConventionalVisibilityIdentityReadbacks();
}

void RenderSystem::DrainConventionalVisibilityIdentityReadbacks()
{
	if (!rhi) return;
	for (const TerraEngine::Renderer::ConventionalVisibilityIdentityReadbackResult& result :
		rhi->PollConventionalVisibilityIdentityReadbacks()) {
		std::lock_guard<std::mutex> lock(statsMutex);
		std::optional<glm::uvec2>& pendingPixel = result.viewType == RenderViewType::Scene
			? pendingSceneAuthoritativeIdentityPixel : pendingGameAuthoritativeIdentityPixel;
		bool& pendingFromCapture = result.viewType == RenderViewType::Scene
			? pendingSceneAuthoritativeIdentityFromCapture : pendingGameAuthoritativeIdentityFromCapture;
		CaptureConventionalVisibilityIdentitySnapshot& pendingSnapshot =
			result.viewType == RenderViewType::Scene
				? pendingSceneIdentitySnapshot : pendingGameIdentitySnapshot;
		std::vector<GpuVirtualGeometryPacketRecord>& pendingVisibilityRecords =
			result.viewType == RenderViewType::Scene
				? pendingSceneIdentityVisibilityRecords
				: pendingGameIdentityVisibilityRecords;
		std::string& failureReason = result.viewType == RenderViewType::Scene
			? latestSceneAuthoritativeIdentityFailureReason : latestGameAuthoritativeIdentityFailureReason;
		// A later request may replace this pixel on the other target. Ignore that
		// stale completion instead of letting it satisfy the newer target selector.
		if (!pendingPixel || *pendingPixel != result.pixel
			|| pendingFromCapture != result.dedicatedCaptureTarget) continue;
		pendingPixel.reset();
		CaptureConventionalVisibilityIdentitySnapshot resolvedSnapshot =
			std::move(pendingSnapshot);
		std::vector<GpuVirtualGeometryPacketRecord> resolvedVisibilityRecords =
			std::move(pendingVisibilityRecords);
		pendingSnapshot = {};
		pendingVisibilityRecords.clear();
		if (!result.completed || !result.authoritative
			|| result.frameId == 0u || result.identity == glm::uvec4(0u)) {
			failureReason = "The Vulkan identity copy completed without an authoritative nonzero uint4 result.";
			continue;
		}
		if (!resolvedSnapshot.IsUsable()
			|| result.rendererGeneration != resolvedSnapshot.rendererGeneration) {
			failureReason = "The Vulkan identity copy belonged to a stale renderer generation.";
			continue;
		}
		if(result.targetGeneration != resolvedSnapshot.targetGeneration)
		{
			failureReason = "The Vulkan identity copy belonged to a resized or recreated target.";
			continue;
		}
		if (result.originGeneration != resolvedSnapshot.originGeneration
			|| result.spatialGeneration != resolvedSnapshot.spatialGeneration
			|| result.residencyGeneration != resolvedSnapshot.residencyGeneration)
		{
			failureReason = "The Vulkan identity copy belonged to a stale origin, spatial, or residency generation.";
			continue;
		}
		const std::vector<TerraEngine::Renderer::ConventionalVisibilityIdentityRecord>& identityTable =
			resolvedSnapshot.table;
		if (resolvedSnapshot.frameId != result.frameId || identityTable.empty()) {
			failureReason = "The completed Vulkan identity belonged to an expired immutable frame snapshot.";
			continue;
		}
		const auto identityRecord = std::find_if(
			identityTable.begin(),
			identityTable.end(),
			[&result](const TerraEngine::Renderer::ConventionalVisibilityIdentityRecord& candidate) {
				return candidate.index == result.identity.y
					&& candidate.objectId != TerraEngine::EMPTY_UUID
					&& candidate.meshId != TerraEngine::EMPTY_UUID;
			});
		if (identityRecord == identityTable.end()) {
			// Include the failed key so editor diagnostics can distinguish a missing
			// producer key from a nonzero key belonging to a different object table.
			failureReason = "The completed Vulkan identity key " + std::to_string(result.identity.y)
				+ " did not match the immutable frame object table ("
				+ std::to_string(identityTable.size()) + " entries).";
			continue;
		}
		VirtualGeometryVisibilityPick pick{};
		pick.objectId = identityRecord->objectId;
		pick.meshId = identityRecord->meshId;
		pick.materialId = identityRecord->materialId;
		pick.frameId = result.frameId;
		pick.originGeneration = result.originGeneration;
		pick.spatialGeneration = result.spatialGeneration;
		pick.residencyGeneration = result.residencyGeneration;
		pick.meshletId = result.identity.x;
		// VG packet metadata remains useful when it agrees with the same snapshot
		// object. Direct meshes intentionally leave these fields at their neutral
		// values rather than being rejected for lacking a packet.
		const std::vector<GpuVirtualGeometryPacketRecord>& records =
			resolvedVisibilityRecords;
		const auto packet = std::find_if(records.begin(), records.end(),
			[&result, &pick](const GpuVirtualGeometryPacketRecord& candidate) {
				return candidate.frameId == result.frameId
					&& candidate.payloadDescriptorIndex == result.identity.z
					&& candidate.objectId == pick.objectId
					&& candidate.meshId == pick.meshId;
			});
		if (packet != records.end()) {
			pick.packetId = packet->packetId;
			pick.clusterId = packet->clusterId;
			pick.selectedLod = packet->selectedLod;
			pick.debugIdentity = packet->debugIdentity;
			pick.fallback = (packet->flags & 0x1u) != 0u;
		}
		if (result.viewType == RenderViewType::Scene) {
			latestSceneAuthoritativeIdentityPick = pick;
			latestSceneAuthoritativeIdentityPixel = result.pixel;
			latestSceneAuthoritativeIdentityFromCapture = result.dedicatedCaptureTarget;
			latestSceneAuthoritativeIdentityFailureReason.clear();
		} else {
			latestGameAuthoritativeIdentityPick = pick;
			latestGameAuthoritativeIdentityPixel = result.pixel;
			latestGameAuthoritativeIdentityFromCapture = result.dedicatedCaptureTarget;
			latestGameAuthoritativeIdentityFailureReason.clear();
		}
	}
}

TerraEngine::Renderer::RenderCaptureResult RenderSystem::CaptureImage(const TerraEngine::Renderer::RenderCaptureRequest& request)
{
	// Admission precedes QueueCaptureRequest: rejected recursive capture must not
	// leave an orphan request. The intentional immediate update shares this gate.
	std::optional<RenderExecutionBoundary> boundary;
	boundary.emplace(frameExecutionBoundary);
	TerraEngine::Renderer::RenderCaptureRequest preparedRequest = request;
	const std::chrono::milliseconds timeout(preparedRequest.timeoutMs > 0u ? preparedRequest.timeoutMs : captureLimits.defaultTimeoutMs);
	if (preparedRequest.sourceKind == TerraEngine::Renderer::RenderCaptureSourceKind::GameView
		&& preparedRequest.freshness == "nextFrame"
		&& !preparedRequest.dedicatedRenderTarget
		&& hasLatestGameViewCamera)
	{
		// Synchronous MCP calls cannot wait for the normal editor loop without
		// blocking that same loop. Snapshot the latest published Game View camera
		// and render one dedicated frame so nextFrame has real, bounded semantics
		// even when the Game View dock tab is occluded.
		preparedRequest.viewMatrix = latestGameViewCamera.view;
		preparedRequest.projectionMatrix = latestGameViewCamera.proj;
		preparedRequest.cameraCanonicalPosition =
			latestGameViewCamera.canonicalPosition;
		preparedRequest.dedicatedRenderTarget = true;
	}
	std::string errorMessage;
	std::string admissionId;
	std::string requestId = QueueCaptureRequestInternal(preparedRequest, errorMessage, admissionId);
	if (requestId.empty())
	{
		TerraEngine::Renderer::RenderCaptureResult result;
		result.requestId = preparedRequest.requestId;
		result.sourceKind = preparedRequest.sourceKind;
		result.sourceName = preparedRequest.sourceName;
		result.size = preparedRequest.targetSize;
		result.message = errorMessage;
		result.errorCategory = errorMessage.find("queue") != std::string::npos ? "queue_full" : "over_budget";
		return result;
	}
	const bool shouldRenderDedicatedCaptureImmediately = (preparedRequest.sourceKind != TerraEngine::Renderer::RenderCaptureSourceKind::GameView
			|| preparedRequest.dedicatedRenderTarget)
		&& initialized
		&& rhi
		&& rhi->GetBackendCapabilityProfile().supportsImageReadback
		&& preparedRequest.format == "image/png";
	if (shouldRenderDedicatedCaptureImmediately)
	{
		// MCP tool calls are synchronous. One render update submits the dedicated
		// object/custom offscreen pass; subsequent iterations poll only its fence
		// instead of recursively rendering many frames.
		UpdateAtExecutionBoundary(0.0f);
	}
	else
	{
		ProcessQueuedCaptureRequests();
	}
	boundary.reset();

	const auto deadline = std::chrono::steady_clock::now() + timeout;
	const auto cancelledResult = [&] {
		TerraEngine::Renderer::RenderCaptureResult cancelled;
		cancelled.requestId = requestId;
		cancelled.sourceKind = preparedRequest.sourceKind;
		cancelled.sourceName = preparedRequest.sourceName;
		cancelled.freshness = preparedRequest.freshness;
		cancelled.size = preparedRequest.targetSize;
		cancelled.errorCategory = "cancelled";
		cancelled.message = "Capture admission was retired.";
		return cancelled;
	};
	const auto admissionRevoked = [&] {
		// Logical revocation may be observed while Clear/Shutdown owns the gate
		// and joins a held worker. This lookup never touches a job or backend.
		std::lock_guard lock(statsMutex);
		if(const auto found = captureOperations.find(admissionId); found != captureOperations.end())
			return found->second->logicalRevoked;
		// Physical retirement can remove a live operation after committing its
		// delivery. Keep that result available to the next protected poll.
		return std::none_of(completedCaptureResults.begin(), completedCaptureResults.end(),
			[&](const auto& completed) { return completed->admissionId == admissionId; });
	};
	do
	{
		if(admissionRevoked()) return cancelledResult();
		{
			CapturePollingBoundary pollBoundary(frameExecutionBoundary);
			if(pollBoundary.OwnsLock()) {
				DrainCaptureReadbacks();
				if(auto result = PollCaptureAdmission(admissionId)) return std::move(*result);
			}
		}
		std::this_thread::yield();
	} while(std::chrono::steady_clock::now() < deadline);

	// Never return a local timeout while the admitted owner can later publish a
	// second result. Safe handoff can exceed the polling deadline by a protected
	// boundary's duration; the facade itself never joins a running CPU/GPU owner.
	for(;;)
	{
		if(admissionRevoked()) return cancelledResult();
		CapturePollingBoundary finalBoundary(frameExecutionBoundary);
		if(!finalBoundary.OwnsLock()) { std::this_thread::yield(); continue; }
		// This final poll, flag inspection and terminal commit share lifecycle
		// ownership, serializing SyntheticCaptureJob::TakeResult against Join.
		DrainCaptureReadbacks();
		if(auto completed = PollCaptureAdmission(admissionId)) return std::move(*completed);

		TerraEngine::Renderer::RenderCaptureResult result;
		result.requestId = admissionId;
		result.sourceKind = preparedRequest.sourceKind;
		result.sourceName = preparedRequest.sourceName;
		result.size = preparedRequest.targetSize;
		result.freshness = preparedRequest.freshness;
		result.message = "Capture request did not complete before polling.";
		result.errorCategory = "timeout";
		bool settled = true;
		{
			std::lock_guard lock(statsMutex);
			if(auto found = captureOperations.find(admissionId); found != captureOperations.end()) {
				settled = !found->second->syntheticStarted && !found->second->backendPending;
				if(settled) {
					std::erase_if(pendingCaptureRequests, [&admissionId](const auto& pending) { return pending.requestId == admissionId; });
					std::erase_if(submittedCaptureReadbackRequests, [&admissionId](const auto& pending) { return pending.requestId == admissionId; });
				}
				captureQueueState.pendingCaptureCount = static_cast<std::uint32_t>(pendingCaptureRequests.size());
			}
		}
		CompleteCapture(std::move(result), settled);
		if(auto terminal = PollCaptureAdmission(admissionId)) return std::move(*terminal);
		return cancelledResult();
	}
}
