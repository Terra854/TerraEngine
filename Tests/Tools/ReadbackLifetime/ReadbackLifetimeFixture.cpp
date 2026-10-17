// Installed native readback evidence and independent PNG inspection.
// Native: --mode native --install-root <Build> --output <fresh.json>
//   --run-nonce <runner nonce> --iterations 300 (100 per lifecycle scenario).
// Decoder: --mode decode --input <actual transmitted PNG> --output <fresh.json>.
// The owning runner supplies watchdog/install pins after observing process exit.
#include <Windows.h>
#include <bcrypt.h>

#include "Core/Logger/Logger.h"
#include "Renderer/Graph/RenderFramePublication.h"
#include "Renderer/RHI/Vulkan/VulkanContext.h"
#include "Resource/ResourceManager.h"
#include "ThreadPool/ThreadPool.h"
#include "Window/GLFW/GlfwWindowBackend.h"
#include "Window/WindowSystem.h"
#include "DecodedCaptureAssertions.h"

#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
	using Json = nlohmann::json;
	using namespace TerraEngine::Renderer;
	using namespace TerraEngine::Renderer::Vulkan;
	using Snapshot = ReadbackLifetimeValidationSnapshot;
	using EventKind = ReadbackValidationEventKind;
	constexpr auto kWaitBudget = std::chrono::seconds(5);

	struct MissingPrerequisite final : std::runtime_error { using std::runtime_error::runtime_error; };
	void Require(bool value, std::string_view message)
	{
		if (!value) throw std::runtime_error(std::string(message));
	}
	void Prerequisite(bool value, std::string_view message)
	{
		if (!value) throw MissingPrerequisite(std::string(message));
	}
	std::filesystem::path Path(std::string_view text)
	{
		return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
	}
	std::string ExecutablePath()
	{
		std::array<wchar_t, 32768> path{};
		const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
		Require(length > 0 && length < path.size(), "Cannot identify the running installed executable.");
		return std::filesystem::path(std::wstring_view(path.data(), length)).generic_string();
	}
	void WriteFresh(const std::filesystem::path& path, const Json& output)
	{
		// CREATE_NEW makes freshness an atomic filesystem condition. A second
		// process cannot win the earlier Parse check and have its evidence truncated.
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		Require(file != INVALID_HANDLE_VALUE, "Cannot exclusively create fresh fixture output.");
		struct FileOwner { HANDLE file; ~FileOwner() { CloseHandle(file); } } owner{file};
		const std::string bytes = output.dump(2) + '\n';
		std::size_t offset = 0;
		while (offset < bytes.size()) {
			const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 65536U));
			DWORD written = 0;
			Require(WriteFile(file, bytes.data() + offset, count, &written, nullptr) && written > 0U,
				"Cannot persist the complete fixture evidence.");
			offset += written;
		}
		Require(FlushFileBuffers(file) != FALSE, "Cannot flush fixture evidence.");
	}

	std::string Sha256(std::span<const std::uint8_t> bytes)
	{
		// Windows CNG belongs to the SDK already required to build this fixture.
		// Hash the exact bytes sent to the decoder, avoiding a second file-read race.
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "SHA256 provider unavailable.");
		struct AlgorithmOwner { BCRYPT_ALG_HANDLE value; ~AlgorithmOwner() { BCryptCloseAlgorithmProvider(value, 0); } } owner{algorithm};
		BCRYPT_HASH_HANDLE hash = nullptr;
		Require(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0, "SHA256 context unavailable.");
		struct HashOwner { BCRYPT_HASH_HANDLE value; ~HashOwner() { BCryptDestroyHash(value); } } hashOwner{hash};
		while (!bytes.empty()) {
			const auto count = static_cast<ULONG>(std::min<std::size_t>(bytes.size(), 65536U));
			Require(BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), count, 0) >= 0, "SHA256 update failed.");
			bytes = bytes.subspan(count);
		}
		std::array<unsigned char, 32> digest{};
		Require(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0, "SHA256 finish failed.");
		constexpr char hex[] = "0123456789abcdef";
		std::string result;
		for (const auto byte : digest) { result.push_back(hex[byte >> 4]); result.push_back(hex[byte & 15]); }
		return result;
	}

	Json Decode(const std::filesystem::path& input)
	{
		std::ifstream stream(input, std::ios::binary | std::ios::ate);
		Require(stream.is_open(), "Cannot open the actual PNG input.");
		const auto length = stream.tellg();
		Require(length > 0 && static_cast<std::uint64_t>(length) <= 0x7fffffffU, "PNG input exceeds independent decoder representation.");
		std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
		stream.seekg(0);
		stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		Require(stream.good(), "PNG input was not read completely.");
		const auto decoded = TerraEngine::TestSupport::DecodePng(bytes);
		return {{"status", "Decoded"}, {"imageSha256", Sha256(bytes)}, {"width", decoded.width}, {"height", decoded.height},
			{"checksumsVerified", true}, {"executablePath", ExecutablePath()}, {"processId", GetCurrentProcessId()}, {"exitCode", 0}};
	}

	struct SessionLog
	{
		std::mutex mutex;
		Json entries = Json::array();
		Json callbacks = Json::array();
		Json errors = Json::array();
		void Append(const LogEntry& entry)
		{
			std::lock_guard lock(mutex);
			Json row{{"sequence", entries.size() + 1U}, {"level", static_cast<int>(entry.level)}, {"message", entry.message}};
			entries.push_back(row);
			if (entry.message.find("[Vulkan]") != std::string::npos) {
				callbacks.push_back(row);
				if (entry.level >= LogLevel::Error) errors.push_back(row);
			}
		}
		void Copy(Json& output)
		{
			std::lock_guard lock(mutex);
			output["allSessionLog"] = entries;
			output["validationCallbackLog"] = callbacks;
			output["validation"]["errors"] = errors;
			output["validation"]["errorCount"] = errors.size();
		}
		bool HasErrors()
		{
			// Callback delivery can occur on native worker threads. Read the same
			// locked, cumulative log used by the final receipt; never reset errors
			// merely because a later operation starts with a nonzero baseline.
			std::lock_guard lock(mutex);
			return !errors.empty();
		}
	};

	// An armed timeline semaphore belongs to the engine. Signal before entering any
	// idle wait or shutdown; a watchdog kill is a failure, never cleanup evidence.
	struct ContextOwner
	{
		VulkanContext context;
		bool gateArmed = false;
		bool released = false;
		bool closed = false;
		void Release()
		{
			if (!gateArmed || released) return;
			Require(context.ReleaseReadbackLifetimeGate(), "Actual host timeline-semaphore signal failed.");
			released = true;
		}
		void Close()
		{
			if (closed) return;
			Release();
			context.Shutdown();
			Require(context.IsShutdownComplete(), "Shutdown returned with native context ownership retained.");
			closed = true;
		}
		~ContextOwner()
		{
			// Keep the release attempt before Shutdown even on failed assertions.
			// The outer receipt preserves the failure instead of blessing this path.
			try { Close(); } catch (...) {}
		}
	};

	Json OwnerCounts(const std::array<std::uint32_t, 4>& counts)
	{
		return {{"stagingBuffer", counts[0]}, {"allocation", counts[1]}, {"commandBuffer", counts[2]}, {"fence", counts[3]}};
	}
	Json EventLog(const Snapshot& snapshot)
	{
		Json events = Json::array();
		for (std::uint32_t index = 0; index < snapshot.eventCount; ++index) {
			const auto& event = snapshot.events.at(index);
			events.push_back({{"sequence", event.sequence}, {"nativeKind", static_cast<unsigned>(event.kind)}, {"result", event.nativeResult}});
		}
		return events;
	}
	Json Facts(const Snapshot& value)
	{
		// Preserve the richer native observations as well as the six protocol
		// milestones. No event is invented to fill a missing production transition.
		return {{"operationId", value.operationId}, {"sessionGeneration", value.renderSessionGeneration},
			{"clientState", static_cast<unsigned>(value.clientState)}, {"nativeState", static_cast<unsigned>(value.nativeState)},
			{"gateRecorded", value.gateRecorded}, {"submitAccepted", value.submitAccepted}, {"hostReleased", value.hostReleased},
			{"sourceImageBlocked", value.sourceImageBlocked}, {"ownerCreated", OwnerCounts(value.ownerCreated)},
			{"ownerDestroyed", OwnerCounts(value.ownerDestroyed)}, {"terminalDeliveries", value.terminalDeliveries},
			{"commandPoolAliveAtRetirement", value.commandPoolAliveAtRetirement},
			{"commandPoolDestroyedBeforeRetirement", value.commandPoolDestroyedBeforeRetirement},
			{"sourceImageDestroyedBeforeRetirement", value.sourceImageDestroyedBeforeRetirement},
			{"sourceImageDestroyObserved", value.sourceImageDestroyObserved}, {"sourceBlockerReleases", value.sourceBlockerReleases},
			{"commandPoolDependencyReleases", value.commandPoolDependencyReleases},
			{"gateCreated", value.gateCreated}, {"gateSignaled", value.gateSignaled}, {"gateDestroyed", value.gateDestroyed},
			{"timelineSemaphoreEnabled", value.timelineSemaphoreEnabled}, {"gateTimelineSemaphore", value.gateTimelineSemaphore},
			{"gateInitialValue", value.gateInitialValue}, {"gateWaitValue", value.gateWaitValue},
			{"gateSignalValue", value.gateSignalValue}, {"gateWaitStageMask", value.gateWaitStageMask},
			{"idleObserved", value.idleObserved}, {"idleResult", value.idleResult}, {"lastFenceResult", value.lastFenceResult},
			{"validationCallbackInstalled", value.validationCallbackInstalled},
			{"validationErrorsBefore", value.validationErrorsBefore}, {"validationErrorsAfter", value.validationErrorsAfter},
			{"payloadAllocationFaultArmed", value.payloadAllocationFaultArmed},
			{"payloadAllocationFaultTriggered", value.payloadAllocationFaultTriggered},
			{"eventLogOverflow", value.eventLogOverflow}, {"nativeEvents", EventLog(value)}};
	}

	const ReadbackValidationEvent& FindEvent(const Snapshot& snapshot, EventKind kind,
		std::int32_t result = VK_SUCCESS, bool matchResult = false)
	{
		Require(!snapshot.eventLogOverflow && snapshot.eventCount <= snapshot.events.size(), "Native event log overflowed.");
		for (std::uint32_t index = 0; index < snapshot.eventCount; ++index) {
			const auto& event = snapshot.events[index];
			if (event.kind == kind && (!matchResult || event.nativeResult == result)) return event;
		}
		throw std::runtime_error("Required actual native event was not observed.");
	}
	Json Milestone(const Snapshot& value, EventKind kind, std::string_view label,
		std::string_view call, std::int32_t result = VK_SUCCESS, bool matchResult = false)
	{
		const auto& event = FindEvent(value, kind, result, matchResult);
		return {{"kind", label}, {"sequence", event.sequence}, {"call", call},
			{"result", matchResult ? Json(event.nativeResult) : Json(nullptr)}};
	}

	std::shared_ptr<const RenderFramePublication> Publication(VulkanContext& context, std::uint64_t frame,
		glm::uvec2 extent, glm::uvec2 windowExtent)
	{
		const auto session = context.GetLatestRendererRecoverySnapshot().renderSessionGeneration;
		auto persistent = std::make_shared<RenderPersistentTopologyRoot>();
		auto dynamic = std::make_shared<RenderDynamicStateRoot>();
		persistent->generation = dynamic->generation = frame;
		persistent->rendererSessionGeneration = dynamic->rendererSessionGeneration = session;
		auto publication = std::make_shared<RenderFramePublication>();
		publication->frameId = publication->generation = frame;
		publication->rendererSessionGeneration = session;
		publication->backendResourceGeneration = context.GetBackendResourceGeneration();
		publication->sceneGeneration = 1U;
		publication->persistent = std::move(persistent);
		publication->dynamic = std::move(dynamic);
		auto backend = std::make_shared<RenderBackendWorkPublication>();
		backend->frameId = backend->animationWork.frameId = frame;
		backend->valid = backend->animationWork.valid = true;
		publication->backendWork = std::move(backend);
		// Render the genuinely smaller dedicated target first. A later runtime
		// view must not reuse AO/depth copy bounds from this earlier output.
		const auto identity = ResolveDedicatedCaptureViewIdentity(RenderViewType::Game);
		RenderViewSelection selection;
		selection.generation = frame;
		selection.outputId = identity.outputId;
		selection.outputKind = RenderOutputKind::OffscreenTexture;
		selection.viewType = identity.viewType;
		publication->views.push_back(selection);
		RenderViewPresentationInput input;
		input.output.kind = selection.outputKind;
		input.output.outputId = selection.outputId;
		input.output.diagnosticsView = selection.viewType;
		input.output.targetSize = extent;
		input.referenceFrame.frameId = frame;
		input.referenceFrame.viewId = input.referenceFrame.outputId = identity.outputId;
		input.referenceFrame.originGeneration = input.referenceFrame.spatialGeneration = input.referenceFrame.residencyGeneration = 1U;
		input.sceneViewGridVisible = false;
		publication->referenceFrames.push_back(input.referenceFrame);
		publication->presentationInputs.push_back(std::move(input));
		// BeginFrame acquired a real swapchain image. Render its ordinary runtime
		// clear second, including the PRESENT transition needed by EndFrame.
		RenderViewSelection runtime;
		runtime.generation = frame;
		runtime.outputId = 2U;
		runtime.outputKind = RenderOutputKind::MainRuntimeView;
		runtime.viewType = RenderViewType::Game;
		publication->views.push_back(runtime);
		RenderViewPresentationInput runtimeInput;
		runtimeInput.output.kind = runtime.outputKind;
		runtimeInput.output.outputId = runtime.outputId;
		runtimeInput.output.diagnosticsView = runtime.viewType;
		runtimeInput.output.targetSize = windowExtent;
		runtimeInput.referenceFrame.frameId = frame;
		runtimeInput.referenceFrame.viewId = runtimeInput.referenceFrame.outputId = runtime.outputId;
		runtimeInput.referenceFrame.originGeneration = runtimeInput.referenceFrame.spatialGeneration = runtimeInput.referenceFrame.residencyGeneration = 1U;
		runtimeInput.sceneViewGridVisible = false;
		publication->referenceFrames.push_back(runtimeInput.referenceFrame);
		publication->presentationInputs.push_back(std::move(runtimeInput));
		const auto sealed = SealRenderFramePublication(std::move(publication), session);
		Require(sealed.Receipt().IsSuccess() && sealed.Publication(), "Native offscreen publication failed production sealing.");
		return sealed.Publication();
	}

	void RenderTarget(ContextOwner& owner, std::uint64_t frame, glm::uvec2 extent)
	{
		// Dedicated offscreen rendering still follows normal publication/queue
		// submission. No scene object or managed callback is needed to clear it.
		owner.Release();
		auto* window = static_cast<GLFWwindow*>(WindowSystem::Instance().GetNativeHandle());
		Require(window != nullptr, "The runtime output requires its live native window.");
		int width = 0, height = 0;
		glfwGetFramebufferSize(window, &width, &height);
		Require(width > 0 && height > 0, "The runtime framebuffer has no positive extent.");
		owner.context.BeginFrame();
		const auto publication = Publication(owner.context, frame, extent,
			{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)});
		Require(owner.context.ConsumeRenderFramePublication(publication), "Backend rejected the sealed offscreen publication.");
		// Publication order is intentional: small capture, then full-size runtime.
		owner.context.RenderFrame(publication, 0U);
		owner.context.RenderFrame(publication, 1U);
		owner.context.EndFrame();
		owner.context.WaitIdle();
	}
	RenderCaptureRequest Request(std::string id, std::uint64_t frame, glm::uvec2 size, std::uint32_t timeout)
	{
		RenderCaptureRequest request;
		request.requestId = std::move(id);
		request.sourceKind = RenderCaptureSourceKind::CustomCamera;
		request.sourceName = "Installed readback lifetime fixture";
		request.targetSize = size;
		request.submittedFrameId = frame;
		request.dedicatedRenderTarget = true;
		request.allowBackendFallback = false;
		request.timeoutMs = timeout;
		return request;
	}
	std::vector<RenderCaptureResult> PollUntilRetired(VulkanContext& context)
	{
		std::vector<RenderCaptureResult> results;
		const auto deadline = std::chrono::steady_clock::now() + kWaitBudget;
		for (;;) {
			for (auto& result : context.PollCaptureReadbacks()) results.push_back(std::move(result));
			const auto snapshot = context.GetReadbackLifetimeValidationSnapshot();
			Require(!snapshot.eventLogOverflow, "Completion polling exceeded bounded native event storage.");
			if (snapshot.nativeState == ReadbackNativeState::Retired) return results;
			Require(std::chrono::steady_clock::now() < deadline, "Actual readback completion was not observed within five seconds.");
			// Space real queries to retain the complete bounded event log on slower
			// adapters. This wait never substitutes for the next actual fence query.
			std::this_thread::sleep_for(std::chrono::milliseconds(60));
		}
	}
	Json HealthyCapture(ContextOwner& owner, std::string id, std::uint64_t frame, glm::uvec2 extent)
	{
		RenderTarget(owner, frame, extent);
		const auto acknowledgement = owner.context.ReadbackCapture(Request(id, frame, extent, 10000U));
		Require(acknowledgement.ok && acknowledgement.pendingAsync, "Healthy follow-up did not submit real readback.");
		std::vector<RenderCaptureResult> completed;
		const auto deadline = std::chrono::steady_clock::now() + kWaitBudget;
		while (completed.empty()) {
			completed = owner.context.PollCaptureReadbacks();
			if (!completed.empty()) break;
			Require(std::chrono::steady_clock::now() < deadline, "Healthy readback failed to progress.");
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		Require(completed.size() == 1 && completed.front().requestId == id, "Healthy capture terminal identity/count mismatch.");
		const auto& result = completed.front();
		Require(result.ok && result.backendReadback && !result.deterministicFallback && result.backendTarget.has_value(),
			"Healthy follow-up did not return real-backend pixels with native target provenance.");
		const auto decoded = TerraEngine::TestSupport::DecodeCaptureBase64(result.base64Data);
		Require(decoded.width == extent.x && decoded.height == extent.y && result.size == extent
			&& result.backendTarget->sourceWidth == extent.x && result.backendTarget->sourceHeight == extent.y,
			"Decoded healthy pixels/native target disagree with the submitted extent.");
		Require(owner.context.PollCaptureReadbacks().empty(), "Healthy capture produced a second terminal result.");
		return {{"requestId", id}, {"backendReadback", result.backendReadback}, {"deterministicFallback", result.deterministicFallback},
			{"decodedWidth", decoded.width}, {"decodedHeight", decoded.height}, {"checksumsVerified", true},
			{"targetGeneration", result.backendTarget->targetGeneration}, {"rendererSessionGeneration", result.backendTarget->rendererSessionGeneration}};
	}

	void CheckRetirement(const Snapshot& snapshot)
	{
		Require(snapshot.nativeState == ReadbackNativeState::Retired && !snapshot.sourceImageBlocked
			&& snapshot.terminalDeliveries == 1U && snapshot.sourceBlockerReleases == 1U
			&& snapshot.commandPoolDependencyReleases == 1U && snapshot.commandPoolAliveAtRetirement
			&& !snapshot.commandPoolDestroyedBeforeRetirement && !snapshot.sourceImageDestroyedBeforeRetirement,
			"Native retirement, dependency lifetime or exactly-once delivery failed.");
		for (std::size_t index = 0; index < snapshot.ownerCreated.size(); ++index)
			Require(snapshot.ownerCreated[index] == 1U && snapshot.ownerDestroyed[index] == 1U, "A native owner did not retire exactly once.");
		Require(snapshot.gateCreated == 1U && snapshot.gateSignaled == 1U && snapshot.gateDestroyed == 1U,
			"Engine-owned timeline semaphore creation/signal/destruction did not balance exactly once.");
		Require(snapshot.timelineSemaphoreEnabled && snapshot.gateTimelineSemaphore
			&& snapshot.gateInitialValue == 0U && snapshot.gateWaitValue == 1U && snapshot.gateSignalValue == 1U
			&& snapshot.gateWaitStageMask == VK_PIPELINE_STAGE_TRANSFER_BIT,
			"Native completion did not use the enabled host-signalled timeline semaphore contract.");
		Require(!snapshot.eventLogOverflow && snapshot.validationErrorsAfter == snapshot.validationErrorsBefore,
			"Native observations overflowed or Vulkan validation reported an error.");
	}

	Json RunOperation(WindowSystem& window, Json& receipt, std::uint32_t iteration, std::string_view nonce,
		std::string_view scenario, bool injectAllocationFailure)
	{
		Json row;
		{
			ContextOwner owner;
			owner.context.SetValidationLayersRequested(true);
			owner.context.Initialize(window);
			const auto startup = owner.context.GetEmptyFrameValidationReceipt();
			Prerequisite(startup.initialized && startup.validationActuallyEnabled,
				"An actual supported Vulkan adapter and enabled validation layer are required.");
			const auto adapter = owner.context.GetLatestRendererRecoverySnapshot();
			Prerequisite(!adapter.hardwareName.empty() && !adapter.driverVersion.empty() && !adapter.apiVersion.empty(), "Actual adapter/driver identity is unavailable.");
			const Json observedAdapter{{"hardwareName", adapter.hardwareName}, {"driverVersion", adapter.driverVersion}, {"apiVersion", adapter.apiVersion}};
			if (receipt["adapter"].empty()) receipt["adapter"] = observedAdapter;
			Require(receipt["adapter"] == observedAdapter, "Native campaign changed its selected adapter.");
			const glm::uvec2 initialExtent(320U, 180U);
			RenderTarget(owner, 1U, initialExtent);
			const auto beforeTarget = owner.context.GetConventionalVisibilityIdentityTargetMetadata(RenderViewType::Game, true);
			Require(beforeTarget && beforeTarget->targetGeneration != 0U, "Rendered dedicated target has no actual generation.");
			const std::string id = std::string(nonce) + (injectAllocationFailure ? ":allocation-fault:" : ":operation:") + std::to_string(iteration);
			Prerequisite(owner.context.GetReadbackLifetimeValidationSnapshot().timelineSemaphoreEnabled,
				"The native adapter must enable timeline semaphores and host signalling for this validation fixture.");
			Require(owner.context.ArmReadbackLifetimeGate(id), "Native readback validation timeline gate could not be armed.");
			owner.gateArmed = true;
			if (injectAllocationFailure) Require(owner.context.ArmReadbackPayloadAllocationFailure(id), "Native payload allocation checkpoint could not be armed.");
			const auto acknowledgement = owner.context.ReadbackCapture(Request(id, 1U, initialExtent, injectAllocationFailure ? 10000U : 10U));
			Require(acknowledgement.ok && acknowledgement.pendingAsync && acknowledgement.backendReadback, "Actual readback queue submission was not accepted.");
			const auto submitted = owner.context.GetReadbackLifetimeValidationSnapshot();
			Require(submitted.operationId == id && submitted.submitAccepted && submitted.gateRecorded && !submitted.hostReleased
				&& submitted.timelineSemaphoreEnabled && submitted.gateTimelineSemaphore
				&& submitted.gateInitialValue == 0U && submitted.gateWaitValue == 1U && submitted.gateSignalValue == 0U
				&& submitted.gateWaitStageMask == VK_PIPELINE_STAGE_TRANSFER_BIT,
				"Submitted operation did not consume its correlated unsignalled transfer-stage timeline wait.");
			Require(submitted.validationCallbackInstalled, "The native Vulkan validation callback was not installed.");
			receipt["validation"]["actuallyEnabled"] = submitted.validationActuallyEnabled;
			receipt["validation"]["callbackInstalled"] = submitted.validationCallbackInstalled;
			receipt["validation"]["enabledLayers"] = Json::array({"VK_LAYER_KHRONOS_validation"});
			std::vector<RenderCaptureResult> terminal = owner.context.PollCaptureReadbacks();
			const auto incomplete = owner.context.GetReadbackLifetimeValidationSnapshot();
			FindEvent(incomplete, EventKind::FenceObserved, VK_NOT_READY, true);
			Snapshot retained;
			if (!injectAllocationFailure) {
				if (terminal.empty()) {
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
					terminal = owner.context.PollCaptureReadbacks();
				}
				Require(terminal.size() == 1U && terminal.front().requestId == id && !terminal.front().ok
					&& terminal.front().errorCategory == "timeout" && terminal.front().base64Data.empty(), "Expected exactly one actual deadline failure while the timeline wait remains blocked.");
				retained = owner.context.GetReadbackLifetimeValidationSnapshot();
				Require(retained.clientState == ReadbackClientState::TerminalPublished && retained.nativeState == ReadbackNativeState::Submitted
					&& retained.sourceImageBlocked && retained.terminalDeliveries == 1U
					&& owner.context.GetCaptureReadbackOwnership(id) == CaptureReadbackOwnership::Retained,
					"Client timeout released native ownership or its genuine source-image blocker.");
				for (std::size_t index = 0; index < retained.ownerCreated.size(); ++index)
					Require(retained.ownerCreated[index] == 1U && retained.ownerDestroyed[index] == 0U, "Timeout destroyed an incomplete native owner.");
			} else Require(terminal.empty(), "Allocation checkpoint must be reached before any deadline failure.");

			owner.Release();
			std::vector<RenderCaptureResult> later;
			if (scenario == "orderly-shutdown" && !injectAllocationFailure) {
				// Cleanup supplies the sequenced actual vkDeviceWaitIdle proof. Poll
				// after close drains client delivery without inventing a fence result.
				owner.Close();
				later = owner.context.PollCaptureReadbacks();
			} else later = PollUntilRetired(owner.context);
			const auto retired = owner.context.GetReadbackLifetimeValidationSnapshot();
			CheckRetirement(retired);
			Require(owner.context.GetCaptureReadbackOwnership(id) == CaptureReadbackOwnership::Released, "Retired operation still blocks its source image.");
			if (injectAllocationFailure) {
				Require(retired.payloadAllocationFaultTriggered && later.size() == 1U && later.front().requestId == id
					&& !later.front().ok && later.front().errorCategory == "backend_readback_unavailable"
					&& later.front().base64Data.empty(), "Injected native payload allocation did not yield one explicit empty-image failure.");
				FindEvent(retired, EventKind::PayloadAllocationFault);
				row = {{"status", "Passed"}, {"evidenceKind", "CppAllocationFaultInjection"}, {"operationId", id},
					{"simulatedDeviceLoss", false}, {"nativeFenceObservationsActual", true}, {"native", Facts(retired)},
					{"healthyFollowup", HealthyCapture(owner, id + ":healthy", 2U, initialExtent)}};
			} else {
				Require(later.empty() && owner.context.PollCaptureReadbacks().empty(), "Late completion published a second client result.");
				const auto completionKind = scenario == "orderly-shutdown" ? EventKind::IdleObserved : EventKind::FenceObserved;
				Json events = Json::array({
					Milestone(retired, EventKind::SubmitAccepted, "Submitted", "vkQueueSubmit", VK_SUCCESS, true),
					Milestone(retired, EventKind::FenceObserved, "NotReady", "vkGetFenceStatus", VK_NOT_READY, true),
					Milestone(retired, EventKind::ClientTerminal, "ClientTerminal", "controller-terminal-delivery"),
					Milestone(retired, EventKind::HostReleased, "HostRelease", "vkSignalSemaphore", VK_SUCCESS, true),
					Milestone(retired, completionKind, "Completed", scenario == "orderly-shutdown" ? "vkDeviceWaitIdle" : "vkGetFenceStatus", VK_SUCCESS, true),
					Milestone(retired, EventKind::NativeRetired, "Retired", "controller-retirement")});
				std::uint64_t previous = 0;
				for (const auto& event : events) { const auto sequence = event.at("sequence").get<std::uint64_t>(); Require(sequence > previous, "Actual native milestone order is invalid."); previous = sequence; }
				row = {{"iteration", iteration}, {"operationId", id}, {"sessionGeneration", retired.renderSessionGeneration},
					{"scenario", scenario}, {"simulated", false}, {"forcedFenceObservation", false},
					{"gate", {{"kind", "HostTimelineSemaphore"}, {"waitCall", "vkQueueSubmit"}, {"releaseCall", "vkSignalSemaphore"},
						{"initialValue", submitted.gateInitialValue}, {"waitValue", submitted.gateWaitValue},
						{"signalValue", retired.gateSignalValue}, {"waitStageMask", submitted.gateWaitStageMask},
						{"timelineSemaphoreEnabled", submitted.timelineSemaphoreEnabled}, {"timelineSemaphore", submitted.gateTimelineSemaphore},
						{"recordedBeforeCopy", submitted.gateRecorded},
						{"signaledBeforeSubmit", submitted.hostReleased}, {"releaseGuardBeforeIdle", owner.released}}},
					{"events", events}, {"terminalReason", terminal.front().errorCategory},
					{"terminalCountAfterDeadline", retained.terminalDeliveries}, {"terminalCountAfterCompletion", retired.terminalDeliveries},
					{"retainedAfterTerminal", OwnerCounts(retained.ownerCreated)}, {"destroyedAfterTerminal", OwnerCounts(retained.ownerDestroyed)},
					{"destroyedAfterCompletion", OwnerCounts(retired.ownerDestroyed)}, {"sourceBlockedAfterTerminal", retained.sourceImageBlocked},
					{"sourceBlockedAfterCompletion", retired.sourceImageBlocked}, {"commandPoolAliveThroughRetirement", retired.commandPoolAliveAtRetirement && !retired.commandPoolDestroyedBeforeRetirement},
					{"sourceImageDestroyedBeforeRetirement", retired.sourceImageDestroyedBeforeRetirement}, {"native", Facts(retired)},
					{"retainedNative", Facts(retained)}, {"resizeAfterRetirement", false}, {"resizeSequence", 0}};
				if (scenario == "resize") {
					const auto healthy = HealthyCapture(owner, id + ":resized", 2U, {384U, 216U});
					Require(healthy.at("targetGeneration").get<std::uint64_t>() > beforeTarget->targetGeneration,
						"Changed PNG extent did not advance the actual dedicated image generation.");
					const auto resized = owner.context.GetReadbackLifetimeValidationSnapshot();
					row["resizeAfterRetirement"] = true;
					row["resizeObservation"] = healthy;
					row["resizeNative"] = Facts(resized);
					row["sourceDestructionObservedBeforeClose"] = resized.sourceImageDestroyObserved;
					row["resizeQualification"] = "Actual target generation/extent changed after retirement; resizeSequence identifies observed old-source destruction, not target creation.";
				} else if (scenario == "timeout") row["healthyFollowup"] = HealthyCapture(owner, id + ":healthy", 2U, initialExtent);
			}
			owner.Close();
			const auto closed = owner.context.GetReadbackLifetimeValidationSnapshot();
			Require(!closed.sourceImageDestroyedBeforeRetirement && !closed.commandPoolDestroyedBeforeRetirement,
				"Native source/pool teardown preceded readback retirement.");
			if (scenario == "resize" && !injectAllocationFailure) {
				// Replacement may defer the old image's physical destruction until
				// owning-context cleanup. Use its real production event ordinal; never
				// manufacture an ordinal from the number of earlier observations.
				Require(closed.sourceImageDestroyObserved, "Tracked old source image destruction was not observed.");
				const auto& destroyed = FindEvent(closed, EventKind::SourceImageDestroyed);
				const auto& retirement = FindEvent(closed, EventKind::NativeRetired);
				Require(destroyed.sequence > retirement.sequence, "Old source image destruction preceded readback retirement.");
				row["resizeSequence"] = destroyed.sequence;
				row["resizeSequenceEvidence"] = {{"nativeKind", "SourceImageDestroyed"}, {"sequence", destroyed.sequence},
					{"snapshot", "closedNative.nativeEvents"},
					{"observedAt", row.at("sourceDestructionObservedBeforeClose").get<bool>() ? "after-resize-capture" : "owning-context-shutdown"}};
			}
			row["closedNative"] = Facts(closed);
			row["contextShutdownCompleted"] = owner.closed;
			receipt["createdGateSemaphores"] = receipt.at("createdGateSemaphores").get<std::uint64_t>() + closed.gateCreated;
			receipt["destroyedGateSemaphores"] = receipt.at("destroyedGateSemaphores").get<std::uint64_t>() + closed.gateDestroyed;
		}
		// This line follows both native shutdown and the real C++ context destructor.
		row["contextDestructionCompleted"] = true;
		return row;
	}

	int Native(const std::filesystem::path& root, std::string_view nonce, std::uint32_t iterations, Json& output)
	{
		output = {{"schemaVersion", "readback-lifetime-native-observations-v1"}, {"protocolFixture", false}, {"status", "Incomplete"},
			{"evidenceKind", "RealVulkan"}, {"runNonce", nonce}, {"processId", GetCurrentProcessId()}, {"executablePath", ExecutablePath()},
			{"iterations", iterations}, {"iterationsPerScenario", iterations / 3U}, {"observations", Json::array()}, {"adapter", Json::object()},
			{"createdGateSemaphores", 0U}, {"destroyedGateSemaphores", 0U}, {"contextDestroyedAfterOperations", false},
			{"validation", {{"actuallyEnabled", false}, {"callbackInstalled", false}, {"capturedThroughShutdown", false},
				{"enabledLayers", Json::array()}, {"errors", Json::array()}, {"errorCount", 0U}}}};
		SessionLog logs;
		const auto sink = Logger::Instance().AddSink([&](const LogEntry& entry) { logs.Append(entry); });
		struct SinkOwner { Logger::SinkId id; ~SinkOwner() { Logger::Instance().RemoveSink(id); } } sinkOwner{sink};
		int exitCode = 2;
		{
			auto& pool = ThreadPool::instance();
			struct PoolOwner { ThreadPool& pool; ~PoolOwner() { pool.shutdown(); } } poolOwner{pool};
			auto& window = WindowSystem::Instance();
			struct WindowOwner { WindowSystem& window; ~WindowOwner() { window.Shutdown(); } } windowOwner{window};
			try {
				Prerequisite(std::filesystem::is_directory(root), "Installed runtime root is unavailable.");
				constexpr std::array shaders{"Shaders/Default.task.tspf", "Shaders/Default.mesh.tspf", "Shaders/Default.frag.tspf",
					"Shaders/Default.ConventionalVisibilityIdentity.frag.spv", "Shaders/Terrain/TerrainVisibility.task.tspf",
					"Shaders/Terrain/TerrainVisibility.mesh.tspf", "Shaders/Terrain/TerrainVisibility.frag.tspf", "Shaders/Terrain/TerrainSurface.comp.tspf"};
				for (const auto* shader : shaders) Prerequisite(std::filesystem::is_regular_file(root / "Editor" / shader), "Installed shader closure is incomplete.");
				ResourceManager::Instance().SetEngineRoot(root / "Editor");
				pool.initialize(2U);
				Prerequisite(pool.getThreadCount() == 2U, "Native fixture worker startup is unavailable.");
				window.Configure(std::make_unique<GlfwWindowBackend>(), {640, 360, "Readback lifetime native validation", false}, [] {});
				window.Initialize();
				auto* nativeWindow = static_cast<GLFWwindow*>(window.GetNativeHandle());
				Prerequisite(nativeWindow != nullptr, "A usable native desktop/window is required.");
				glfwHideWindow(nativeWindow);
				Prerequisite(glfwGetWindowAttrib(nativeWindow, GLFW_VISIBLE) == GLFW_FALSE, "Validation window could not be hidden.");
				constexpr std::array<std::string_view, 3> scenarios{"timeout", "resize", "orderly-shutdown"};
				for (std::uint32_t iteration = 1; iteration <= iterations; ++iteration) {
					output["observations"].push_back(RunOperation(window, output, iteration, nonce, scenarios[(iteration - 1U) % 3U], false));
					// RunOperation has closed and destroyed this context before its row
					// is appended. Retain that row, then stop after any known error from
					// initialization through cleanup instead of running more GPU work.
					Require(!logs.HasErrors()
						&& output["observations"].back().at("closedNative").at("validationErrorsAfter").get<std::uint64_t>() == 0U,
						"Actual Vulkan validation error observed; stopping the native campaign.");
				}
				// This is deliberately outside the 300 core GPU rows. Only the C++
				// allocation checkpoint is injected; submission/fence/cleanup stay real.
				output["faultQualification"] = RunOperation(window, output, 1U, nonce, "allocation-failure", true);
				Require(!logs.HasErrors()
					&& output.at("faultQualification").at("closedNative").at("validationErrorsAfter").get<std::uint64_t>() == 0U,
					"Actual Vulkan validation error observed during allocation-fault qualification.");
				output["contextDestroyedAfterOperations"] = true;
				output["status"] = "Passed";
				exitCode = 0;
			} catch (const MissingPrerequisite& error) {
				output["status"] = "Incomplete"; output["reason"] = error.what();
			} catch (const std::exception& error) {
				output["status"] = "Failed"; output["reason"] = error.what(); exitCode = 1;
			}
		}
		output["validation"]["capturedThroughShutdown"] = output.at("contextDestroyedAfterOperations");
		logs.Copy(output);
		if (output["validation"]["errorCount"].get<std::size_t>() != 0U) {
			output["status"] = "Failed"; output["reason"] = "Actual Vulkan validation callbacks reported initialization/execution/teardown errors."; exitCode = 1;
		}
		output["threadPoolWorkersAfterCleanup"] = ThreadPool::instance().getThreadCount();
		output["windowHandleCleared"] = WindowSystem::Instance().GetNativeHandle() == nullptr;
		if (exitCode == 0 && (output["threadPoolWorkersAfterCleanup"] != 0 || !output["windowHandleCleared"].get<bool>())) {
			output["status"] = "Failed"; output["reason"] = "Host window/worker cleanup was incomplete."; exitCode = 1;
		}
		return exitCode;
	}

	struct Options
	{
		std::string mode, nonce;
		std::filesystem::path root, input, output;
		std::uint32_t iterations = 0;
	};
	Options Parse(int argc, char** argv)
	{
		Options result;
		std::vector<std::string_view> seen;
		for (int index = 1; index < argc; index += 2) {
			Require(index + 1 < argc, "Every fixture option requires a value.");
			const std::string_view option = argv[index], value = argv[index + 1];
			Require(std::find(seen.begin(), seen.end(), option) == seen.end(), "Duplicate fixture option.");
			seen.push_back(option);
			if (option == "--mode") result.mode = value;
			else if (option == "--install-root") result.root = Path(value);
			else if (option == "--input") result.input = Path(value);
			else if (option == "--output") result.output = Path(value);
			else if (option == "--run-nonce") result.nonce = value;
			else if (option == "--iterations") {
				const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result.iterations);
				Require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(), "Iterations must be a decimal integer.");
			} else throw std::runtime_error("Unknown fixture option.");
		}
		Require(result.output.is_absolute() && !std::filesystem::exists(result.output)
			&& std::filesystem::is_directory(result.output.parent_path()), "Output must be an absolute fresh file with an existing parent.");
		if (result.mode == "native") {
			Require(result.root.is_absolute() && !result.nonce.empty()
				&& result.input.empty() && result.iterations >= 300U && result.iterations <= 300000U && result.iterations % 3U == 0U,
				"Native mode requires an install root, run nonce and total iterations divisible by three, at least 300.");
		} else Require(result.mode == "decode" && result.input.is_absolute() && std::filesystem::is_regular_file(result.input)
			&& result.root.empty() && result.nonce.empty() && result.iterations == 0U, "Decode mode requires only an actual PNG input and fresh output.");
		return result;
	}
}

int main(int argc, char** argv)
{
	try {
		const auto options = Parse(argc, argv);
		Json output;
		int exitCode = 0;
		if (options.mode == "native") exitCode = Native(options.root, options.nonce, options.iterations, output);
		else {
			try { output = Decode(options.input); }
			catch (const std::exception& error) {
				output = {{"status", "Rejected"}, {"reason", error.what()}, {"checksumsVerified", false},
					{"executablePath", ExecutablePath()}, {"processId", GetCurrentProcessId()}, {"exitCode", 1}};
				exitCode = 1;
			}
		}
		WriteFresh(options.output, output);
		std::cout << "Readback lifetime " << options.mode << ": " << output.at("status").get<std::string>() << '\n';
		return exitCode;
	} catch (const std::exception& error) {
		std::cerr << "Readback fixture rejected: " << error.what() << '\n';
		return 1;
	}
}
