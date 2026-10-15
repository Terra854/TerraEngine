// Installed preflight, original empty-frame red probe, and actual Vulkan lanes.
// Usage: --mode vulkan --scenario <frozen lane> --iterations <positive count>
//   --install-root <absolute Build> --output <fresh absolute json>.
// Renderer lifecycle acceptance is a separate later fixture assignment.
#include "Core/Memory/ReusableArenaResource.h"
#include "Core/Logger/Logger.h"
#include "Renderer/RHI/Vulkan/VulkanContext.h"
#include "Renderer/Graph/RenderFramePublication.h"
#include "Resource/ResourceManager.h"
#include "ThreadPool/ThreadPool.h"
#include "Window/WindowSystem.h"
#include "Window/GLFW/GlfwWindowBackend.h"
#include "RuntimeMemorySafetyRendererFixture.h"
#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <mutex>
#include <atomic>
#include <array>
#include <charconv>
#include <chrono>
#include <set>
#include <algorithm>
#include <optional>

namespace
{
	void Require(bool condition, const char* reason)
	{
		if (!condition) throw std::runtime_error(reason);
	}
	std::filesystem::path Path(std::string_view text)
	{
		return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
	}

	using Json = nlohmann::json;
	using EmptyRequest = TerraEngine::Renderer::Vulkan::EmptyFrameValidationRequest;
	using EmptyReceipt = TerraEngine::Renderer::Vulkan::EmptyFrameValidationReceipt;
	using CandidateCheckpoint = TerraEngine::Renderer::Vulkan::EmptyFrameCandidateCheckpoint;
	constexpr std::array<std::string_view, 10> kGpuScenarios{
		"fresh-acquisition", "recreated-acquisition", "previously-presented", "acquire-wait-consumed",
		"record-failure", "fence-reset-failure", "submit-failure", "candidate-rollback", "maintenance-enabled", "maintenance-disabled"};

	// A requested checkpoint rejects a host operation before its native call.
	// Null VkResults distinguish that rejection from a genuine driver return.
	Json NativeReceipt(const EmptyReceipt& value)
	{
		return {{"sequence", value.sequence}, {"correlationId", value.correlationId}, {"generation", value.generation},
			{"frameSlot", value.frameSlot}, {"imageIndex", value.imageIndex}, {"acquired", value.acquired},
			{"swapchainImageCount", value.swapchainImageCount}, {"frameSlotCount", value.frameSlotCount},
			{"forcedEmpty", value.forcedEmpty}, {"commandBufferCount", value.commandBufferCount},
			{"recordEnded", value.recordEnded}, {"acquireWaitConsumedBefore", value.acquireWaitConsumedBefore},
			{"acquireWaitConsumed", value.acquireWaitConsumedBefore}, {"fallbackCommandSubmitted", value.forcedEmpty && value.submitAccepted && value.commandBufferCount == 1U},
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
	bool Presented(const EmptyReceipt& value)
	{
		// VK_SUBOPTIMAL_KHR still queues a real presentation. Recovery generations
		// are sampled again before constructing each later immutable publication.
		return value.presentAttempted && (value.presentResult == 0 || value.presentResult == 1000001003);
	}
	void RequireFallback(const EmptyReceipt& value)
	{
		Require(value.acquired && value.forcedEmpty && value.commandBufferCount == 1U && value.recordEnded,
			"Empty fallback did not record exactly one real scene-independent command buffer.");
		Require(value.resetAttempted && value.fenceResetAccepted && value.resetResult == 0
			&& value.submitAttempted && value.submitAccepted && value.submitResult == 0 && Presented(value),
			"Empty fallback lacked actual successful reset, submit, or presentation.");
		Require(value.waitIdleAttempted && value.waitIdleCompletionObserved && value.waitIdleResult == 0,
			"Empty fallback completion was not proven by an actual successful native wait.");
		Require(!value.ordinarySubmitAccepted && !value.recoverySuspended, "Fallback cannot claim an ordinary scene receipt or suspended success.");
	}

	std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication> BuildHealthyPublication(
		VulkanContext& context, std::uint64_t frameId)
	{
		using namespace TerraEngine::Renderer;
		// Follow the normal native publication contract with zero scene objects.
		// This admits a real normal scene clear; it deliberately claims no mesh draw.
		const auto session = context.GetLatestRendererRecoverySnapshot().renderSessionGeneration;
		auto persistent = std::make_shared<RenderPersistentTopologyRoot>();
		auto dynamic = std::make_shared<RenderDynamicStateRoot>();
		persistent->generation = dynamic->generation = frameId;
		persistent->rendererSessionGeneration = dynamic->rendererSessionGeneration = session;
		auto publication = std::make_shared<RenderFramePublication>();
		publication->frameId = publication->generation = frameId;
		publication->rendererSessionGeneration = session;
		publication->backendResourceGeneration = context.GetBackendResourceGeneration();
		publication->sceneGeneration = 1U;
		publication->persistent = std::move(persistent); publication->dynamic = std::move(dynamic);
		auto backend = std::make_shared<RenderBackendWorkPublication>();
		backend->frameId = backend->animationWork.frameId = frameId;
		backend->valid = backend->animationWork.valid = true;
		publication->backendWork = std::move(backend);
		RenderViewSelection selection;
		selection.generation = frameId; selection.outputId = 2U;
		selection.outputKind = RenderOutputKind::MainRuntimeView; selection.viewType = RenderViewType::Game;
		publication->views.push_back(selection);
		RenderViewPresentationInput presentation;
		presentation.output.kind = selection.outputKind; presentation.output.outputId = selection.outputId;
		presentation.output.diagnosticsView = selection.viewType; presentation.output.targetSize = {640U, 360U};
		presentation.referenceFrame.frameId = frameId; presentation.referenceFrame.viewId = 2U; presentation.referenceFrame.outputId = 2U;
		presentation.referenceFrame.originGeneration = presentation.referenceFrame.spatialGeneration = presentation.referenceFrame.residencyGeneration = 1U;
		presentation.sceneViewGridVisible = false;
		publication->referenceFrames.push_back(presentation.referenceFrame);
		publication->presentationInputs.push_back(std::move(presentation));
		const auto sealed = SealRenderFramePublication(std::move(publication), session);
		Require(sealed.Receipt().IsSuccess() && sealed.Publication() != nullptr, "Normal follow-up publication was rejected by the production seal validator.");
		return sealed.Publication();
	}
	Json FinishHealthyFrame(VulkanContext& context, std::uint64_t frameId, const EmptyReceipt& before)
	{
		const auto acquired = context.GetEmptyFrameValidationReceipt();
		Require(acquired.acquired && acquired.sequence > before.sequence && !acquired.recoverySuspended,
			"Normal follow-up did not acquire a fresh safe frame after protected recovery.");
		const auto publication = BuildHealthyPublication(context, frameId);
		const bool admitted = context.ConsumeRenderFramePublication(publication);
		Require(admitted, "Normal follow-up immutable publication was not admitted by the real backend.");
		context.RenderFrame(publication, 0U); context.EndFrame(); context.WaitIdle();
		const auto actual = context.GetEmptyFrameValidationReceipt();
		Require(actual.ordinarySubmitAccepted && actual.ordinarySubmissionSerial > before.ordinarySubmissionSerial
			&& actual.commandBufferCount > 0U && !actual.forcedEmpty && actual.ordinaryCompletionObserved
			&& actual.waitIdleCompletionObserved && Presented(actual) && !actual.recoverySuspended,
			"Normal follow-up did not submit, present, and complete a real scene command buffer.");
		return {{"publicationFrameId", frameId}, {"publicationAdmitted", admitted}, {"sceneCommandBufferSubmitted", true},
			{"meshDrawClaim", false}, {"rendererSessionGeneration", publication->rendererSessionGeneration},
			{"backendResourceGeneration", publication->backendResourceGeneration}, {"native", NativeReceipt(actual)}};
	}
	Json HealthyFrame(VulkanContext& context, std::uint64_t frameId)
	{
		const auto before = context.GetEmptyFrameValidationReceipt();
		context.BeginFrame();
		return FinishHealthyFrame(context, frameId, before);
	}
	EmptyReceipt EmptyFrame(VulkanContext& context, EmptyRequest request)
	{
		const auto before = context.GetEmptyFrameValidationReceipt();
		Require(context.ArmEmptyFrameValidation(request), "Empty-frame hook could not be armed at a quiescent protected boundary.");
		context.BeginFrame();
		const auto acquired = context.GetEmptyFrameValidationReceipt();
		Require(acquired.acquired && acquired.sequence > before.sequence && acquired.correlationId == request.correlationId,
			"Empty-frame hook did not observe a genuine newly acquired correlated image.");
		context.EndFrame(); context.WaitIdle();
		return context.GetEmptyFrameValidationReceipt();
	}
	struct SessionLog {
		std::mutex mutex;
		std::atomic<unsigned> phase{0U};
		std::uint32_t iteration = 0U;
		std::string phaseName = "WindowInitialize";
		std::size_t sliceStart = 0U;
		Json entries = Json::array();
		Json callbacks = Json::array();
		std::size_t callbackErrors = 0U;
		void Append(const LogEntry& entry) {
			std::lock_guard lock(mutex);
			Json row{{"sequence", entries.size() + 1U}, {"iteration", iteration}, {"phase", phaseName}, {"nativeStage", phase.load()},
				{"eventKind", "logger"}, {"level", static_cast<int>(entry.level)}, {"message", entry.message}};
			entries.push_back(row);
			if(entry.message.find("[Vulkan]") != std::string::npos) {
				callbacks.push_back(row);
				if(entry.level >= LogLevel::Error) ++callbackErrors;
			}
		}
		void CopyTo(Json& output) {
			std::lock_guard lock(mutex);
			output["allSessionLog"] = entries; output["validationCallbackLog"] = callbacks;
			output["allSessionValidationErrors"] = callbackErrors;
		}
		void BoundaryLocked(std::string name, bool succeeded) {
			entries.push_back({{"sequence", entries.size()+1U}, {"iteration", iteration}, {"phase", std::move(name)},
				{"eventKind", "fixtureBoundary"}, {"level", nullptr}, {"message", "Native fixture lifecycle boundary"}, {"succeeded", succeeded}});
		}
		void WindowBoundary(std::string name, bool succeeded) {
			std::lock_guard lock(mutex); iteration = 0U;
			phaseName = name.starts_with("WindowShutdown") ? "WindowShutdown" : "WindowInitialize";
			BoundaryLocked(std::move(name), succeeded);
		}
		void BeginIteration(std::uint32_t index) {
			std::lock_guard lock(mutex); iteration = index; phaseName = "Iteration"; sliceStart = entries.size();
			phase.store(0U); BoundaryLocked("IterationBegin", true);
		}
		void EndIteration(Json& output) {
			std::lock_guard lock(mutex); BoundaryLocked("IterationEnd", output.at("status") == "Passed");
			Json rows = Json::array(), localCallbacks = Json::array(); std::size_t errors = 0U;
			for(std::size_t index = sliceStart; index < entries.size(); ++index) rows.push_back(entries[index]);
			for(const auto& row : callbacks) if(row["iteration"] == iteration) {
				localCallbacks.push_back(row); if(row["level"].get<int>() >= static_cast<int>(LogLevel::Error)) ++errors;
			}
			output["allSessionLog"] = std::move(rows); output["validationCallbackLog"] = std::move(localCallbacks); output["allSessionValidationErrors"] = errors;
			iteration = 0U; phaseName = "BetweenIterations";
		}
	};
	std::string PointerIdentity(const void* value) {
		// This is a sampled handle value, not a globally unique GPU lifetime ID.
		return std::to_string(reinterpret_cast<std::uintptr_t>(value));
	}
	// Ordinary RenderFrame preparation submits jobs to the process pool. Keep
	// those workers alive until all GPU/context and window owners have retired.
	struct ThreadPoolCleanup {
		ThreadPool& pool;
		bool attempted = false, completed = false;
		void Close() noexcept { if(attempted) return; attempted = true; try { pool.shutdown(); completed = true; } catch(...) {} }
		~ThreadPoolCleanup() { Close(); }
	};
	struct WindowCleanup {
		WindowSystem& window;
		bool attempted = false, completed = false;
		void Close() noexcept { if(attempted) return; attempted = true; try { window.Shutdown(); completed = true; } catch(...) {} }
		~WindowCleanup() { Close(); }
	};
	struct ContextCleanup {
		VulkanContext& context;
		bool attempted = false, completed = false;
		std::optional<bool> initializedAfterShutdown;
		void Close() noexcept
		{
			if(attempted) return;
			attempted = true;
			try {
				context.Shutdown();
				// Shutdown can return while native owners remain retained. Observe
				// the current native state before reporting completed cleanup.
				initializedAfterShutdown = context.GetEmptyFrameValidationReceipt().initialized;
				completed = !*initializedAfterShutdown;
			} catch(...) {}
		}
		~ContextCleanup() { Close(); }
	};
	void ValidateShaders(const std::filesystem::path& root)
	{
		constexpr std::array paths{"Shaders/Default.task.tspf", "Shaders/Default.mesh.tspf", "Shaders/Default.frag.tspf",
			"Shaders/Default.ConventionalVisibilityIdentity.frag.spv", "Shaders/Terrain/TerrainVisibility.task.tspf",
			"Shaders/Terrain/TerrainVisibility.mesh.tspf", "Shaders/Terrain/TerrainVisibility.frag.tspf", "Shaders/Terrain/TerrainSurface.comp.tspf"};
		for(const auto* relative : paths) Require(std::filesystem::is_regular_file(root / "Editor" / relative), "Installed Vulkan shader closure is incomplete.");
	}
	Json RunVulkanIteration(const std::filesystem::path& root, std::string_view scenario, std::uint32_t iteration,
		WindowSystem& window, GLFWwindow* sharedWindow, SessionLog& logs)
	{
		Json result{{"iteration", iteration}, {"scenarioId", scenario}, {"status", "Unverified"},
			{"graphicsInstantiated", false}, {"validationActuallyEnabled", false}, {"healthyFrames", Json::array()},
			{"preludeFrames", Json::array()}, {"candidateSubcases", Json::array()},
			{"contextShutdownAttempted", false}, {"contextInitializedAfterShutdown", nullptr}};
		result["contextLifetime"] = {{"fixtureLifetimeId", iteration}, {"constructed", false}, {"destructionCompleted", false},
			{"instanceHandle", nullptr}, {"deviceHandle", nullptr}};
		result["sharedWindow"] = {{"windowIdentity", PointerIdentity(sharedWindow)}, {"windowRetainedForSession", true},
			{"liveBeforeContextInitialization", false}, {"liveAfterContextCleanup", false},
			{"hiddenBeforeContextInitialization", false}, {"hiddenAfterContextCleanup", false}};
		auto& pool = ThreadPool::instance();
		ThreadPoolCleanup poolCleanup{pool};
		bool initialized = false, contextCleanupCompleted = false, contextConstructed = false;
		const auto started = std::chrono::steady_clock::now();
		try {
			ValidateShaders(root);
			pool.initialize();
			result["threadPoolWorkerCount"] = pool.getThreadCount();
			Require(pool.getThreadCount() > 0U, "Normal renderer preparation requires an actually started worker pool.");
			ResourceManager::Instance().SetEngineRoot(root / "Editor");
			// Only the window owner is shared. Every context below constructs its
			// own Vulkan instance/device/surface/swapchain and retires them normally.
			const bool windowLive = sharedWindow != nullptr && window.GetNativeHandle() == sharedWindow && glfwWindowShouldClose(sharedWindow) == GLFW_FALSE;
			Require(windowLive, "Shared window was not live before context initialization.");
			result["sharedWindow"]["liveBeforeContextInitialization"] = windowLive;
			result["windowHidden"] = glfwGetWindowAttrib(sharedWindow, GLFW_VISIBLE) == GLFW_FALSE;
			result["sharedWindow"]["hiddenBeforeContextInitialization"] = result["windowHidden"];
			Require(result["windowHidden"].get<bool>(), "Fixture-owned validation window could not be hidden.");
			VulkanContext context;
			contextConstructed = true; result["contextLifetime"]["constructed"] = true;
			ContextCleanup cleanup{context};
			try {
				Require(context.ConfigureEmptyFrameValidation(scenario == "maintenance-disabled"), "Native validation configuration is unavailable before initialization.");
				context.SetValidationLayersRequested(true); context.Initialize(window);
				const auto initial = context.GetEmptyFrameValidationReceipt();
				initialized = initial.initialized;
				if(initial.initialized) {
					// Existing read-only editor handles sample this live native context.
					// Driver handle reuse across destroyed contexts remains legal.
					VulkanContext::EditorHandles handles; context.GetEditorHandles(handles);
					result["contextLifetime"]["instanceHandle"] = PointerIdentity(handles.instance);
					result["contextLifetime"]["deviceHandle"] = PointerIdentity(handles.device);
					Require(handles.instance != nullptr && handles.device != nullptr, "Initialized context did not expose live instance/device handles.");
				}
				result["graphicsInstantiated"] = initial.initialized; result["validationActuallyEnabled"] = initial.validationActuallyEnabled;
				result["maintenanceEnabled"] = initial.maintenanceEnabled;
				const auto adapter = context.GetLatestRendererRecoverySnapshot();
				result["adapter"] = {{"hardwareName", adapter.hardwareName}, {"driverVersion", adapter.driverVersion},
					{"apiVersion", adapter.apiVersion}, {"renderSessionGeneration", adapter.renderSessionGeneration}};
				if(!initial.initialized || !initial.validationActuallyEnabled || adapter.hardwareName.empty() || adapter.driverVersion.empty())
					result["reason"] = "A native adapter/surface and actually enabled validation layer with adapter/driver identity are required.";
				else if(scenario == "maintenance-enabled" && !initial.maintenanceEnabled)
					result["reason"] = "The native adapter did not enable presentation maintenance fences.";
				else {
					Require(scenario != "maintenance-disabled" || !initial.maintenanceEnabled, "Requested disabled maintenance was actually enabled.");
					std::uint64_t nextFrame = 1U;
					logs.phase.store(1U);
					if(scenario == "recreated-acquisition") {
						Require(context.RecreateEmptyFrameValidation(false), "Protected recreation did not publish a complete replacement generation.");
						result["recreatedBeforeFirstAcquisition"] = true;
					}
					if(scenario == "candidate-rollback" || scenario == "maintenance-disabled") {
						const bool replaced = context.RecreateEmptyFrameValidation(true);
						const auto rejected = context.GetEmptyFrameValidationReceipt();
						result["candidateSubcases"].push_back({{"checkpoint", "BeforeSwapchain"}, {"replacementPublished", replaced}, {"native", NativeReceipt(rejected)}});
						Require(!replaced && rejected.candidateRejected && !rejected.candidateOldSwapchainConsumed,
							"Pre-handoff candidate rejection did not preserve a retryable previous bundle.");
						Require(context.RecreateEmptyFrameValidation(false), "Pre-handoff rollback could not publish its actual healthy retry.");
					}
					EmptyRequest request{true, static_cast<std::uint64_t>(iteration) * 100U + 1U};
					request.consumeAcquire = scenario == "acquire-wait-consumed";
					if(scenario == "record-failure") request.checkpoint = EmptyRequest::Checkpoint::BeforeRecording;
					if(scenario == "fence-reset-failure") request.checkpoint = EmptyRequest::Checkpoint::BeforeFenceReset;
					if(scenario == "submit-failure") request.checkpoint = EmptyRequest::Checkpoint::BeforeQueueSubmit;
					logs.phase.store(2U);
					EmptyReceipt operation;
					if(scenario == "previously-presented") {
						// Qualify actual reuse by observed native image identities. Never
						// assume a particular driver image count or acquisition order.
						std::set<std::pair<std::uint64_t, std::uint32_t>> presented;
						bool reused = false;
						for(std::uint32_t attempt = 0U; attempt < 64U; ++attempt) {
							request.correlationId += attempt;
							operation = EmptyFrame(context, request); RequireFallback(operation);
							const auto key = std::make_pair(operation.generation, operation.imageIndex);
							if(presented.contains(key)) { reused = true; break; }
							presented.insert(key); result["preludeFrames"].push_back(NativeReceipt(operation));
						}
						Require(reused, "No previously presented image was reacquired within the declared 64-operation qualification bound.");
						result["previousPresentationObservedForImage"] = true;
					} else operation = EmptyFrame(context, request);
					result["operation"] = NativeReceipt(operation);
					if(request.checkpoint != EmptyRequest::Checkpoint::None) {
						Require(operation.acquired && operation.forcedEmpty && operation.rejectedCheckpoint == request.checkpoint
							&& operation.recoverySuspended && !operation.submitAttempted && !operation.submitAccepted && !operation.presentAttempted,
							"Host failure checkpoint did not preserve real suspension without native submit/presentation.");
						Require(operation.acquisitionDrainSubmitted && operation.acquisitionDrainCompleted, "Rejected operation did not settle its acquisition-only drain.");
						Require(operation.resetAttempted == (request.checkpoint == EmptyRequest::Checkpoint::BeforeQueueSubmit),
							"Host rejection claimed the wrong native fence-reset attempt.");
					} else RequireFallback(operation);
					if(scenario == "maintenance-enabled") Require(operation.maintenanceEnabled,
						"The maintenance-enabled operation silently fell back to unextended presentation.");
					if(scenario == "maintenance-disabled") Require(!operation.maintenanceEnabled,
						"The disabled-maintenance operation unexpectedly enabled presentation maintenance.");
					if(request.consumeAcquire) Require(operation.acquireWaitConsumedBefore && operation.acquisitionDrainCompleted,
						"Consumed-wait scenario did not genuinely drain the acquisition before fallback submit.");
					logs.phase.store(3U);
					for(unsigned followup = 0U; followup < 2U; ++followup) result["healthyFrames"].push_back(HealthyFrame(context, nextFrame++));
					if(scenario == "maintenance-enabled") for(const auto& followup : result["healthyFrames"])
						Require(followup["native"]["maintenanceEnabled"].get<bool>(), "A healthy follow-up did not retain actual maintenance enablement.");
					if(scenario == "maintenance-disabled") for(const auto& followup : result["healthyFrames"])
						Require(!followup["native"]["maintenanceEnabled"].get<bool>(), "A healthy follow-up unexpectedly enabled presentation maintenance.");
					if(scenario == "candidate-rollback" || scenario == "maintenance-disabled") {
						const auto before = context.GetEmptyFrameValidationReceipt();
						const bool replaced = context.RecreateEmptyFrameValidationAt(CandidateCheckpoint::AfterFallbackPass);
						const auto partial = context.GetEmptyFrameValidationReceipt();
						if(scenario == "maintenance-disabled") Require(!partial.maintenanceEnabled,
							"Partial candidate unexpectedly enabled presentation maintenance.");
						Json subcase{{"checkpoint", "AfterFallbackPass"}, {"replacementPublished", replaced}, {"native", NativeReceipt(partial)}};
						Require(!replaced && partial.candidateRejected && partial.candidateFallbackPassCreated
							&& partial.candidatePartialCleanupCompleted && partial.candidateOldSwapchainConsumed && partial.recoverySuspended,
							"Partial fallback-pass candidate failure lacked actual cleanup and consumed-handoff suspension.");
						logs.phase.store(4U);
						// A retry can succeed only through production retirement proof.
						// No proof means terminal fail-closed shutdown, not fake recovery.
						bool recovered = false;
						if(partial.maintenanceEnabled) recovered = context.RecreateEmptyFrameValidation(false);
						subcase["healthyFrames"] = Json::array();
						if(recovered) {
							for(unsigned followup = 0U; followup < 2U; ++followup) subcase["healthyFrames"].push_back(HealthyFrame(context, nextFrame++));
							Require(subcase["healthyFrames"][0]["native"]["generation"].get<std::uint64_t>() > partial.generation,
								"Consumed-handoff recovery did not acquire from a newer complete generation.");
							subcase["outcome"] = "RecoveredAfterNativeRetirementProof";
						} else {
							context.BeginFrame();
							const auto blocked = context.GetEmptyFrameValidationReceipt();
							if(!blocked.recoverySuspended && blocked.sequence > before.sequence) {
								// Real presentation completion may become visible between
								// the explicit retry and BeginFrame's protected retry.
								Require(partial.maintenanceEnabled && blocked.generation > partial.generation,
									"A consumed unproven handoff escaped suspension without a newer generation.");
								subcase["healthyFrames"].push_back(FinishHealthyFrame(context, nextFrame++, before));
								subcase["healthyFrames"].push_back(HealthyFrame(context, nextFrame++));
								subcase["outcome"] = "RecoveredAfterNativeRetirementProof";
							} else {
								Require(blocked.recoverySuspended && blocked.sequence == before.sequence
									&& blocked.ordinarySubmissionSerial == before.ordinarySubmissionSerial,
									"Unproven consumed handoff allowed new image acquisition or ordinary submission.");
								subcase["blockedBeginFrame"] = NativeReceipt(blocked);
								subcase["outcome"] = "FailClosedTerminalShutdownRequired";
							}
						}
						result["candidateSubcases"].push_back(std::move(subcase));
					}
					result["status"] = "Passed";
				}
			} catch(const std::exception& error) {
				result["status"] = initialized ? "Failed" : "Unverified"; result["reason"] = error.what();
			}
			logs.phase.store(5U); cleanup.Close(); contextCleanupCompleted = cleanup.completed;
			result["contextShutdownAttempted"] = cleanup.attempted;
			// A missing observation stays null; partial initialization remains
			// Unverified through the existing initialization status handling.
			result["contextInitializedAfterShutdown"] = cleanup.initializedAfterShutdown.has_value()
				? Json(*cleanup.initializedAfterShutdown) : Json(nullptr);
		} catch(const std::exception& error) { result["status"] = initialized ? "Failed" : "Unverified"; result["reason"] = error.what(); }
		// This assignment occurs only after the context's lexical scope and its
		// actual C++ destructor above have both completed, including unwind paths.
		result["contextLifetime"]["destructionCompleted"] = contextConstructed;
		const bool windowLiveAfter = sharedWindow != nullptr && window.GetNativeHandle() == sharedWindow && glfwWindowShouldClose(sharedWindow) == GLFW_FALSE;
		result["sharedWindow"]["liveAfterContextCleanup"] = windowLiveAfter;
		result["sharedWindow"]["hiddenAfterContextCleanup"] = windowLiveAfter && glfwGetWindowAttrib(sharedWindow, GLFW_VISIBLE) == GLFW_FALSE;
		logs.phase.store(7U); poolCleanup.Close();
		result["contextCleanupCompleted"] = contextCleanupCompleted;
		result["threadPoolCleanupCompleted"] = poolCleanup.completed;
		if(!poolCleanup.completed || !windowLiveAfter || !result["sharedWindow"]["hiddenAfterContextCleanup"].get<bool>()
			|| (initialized && !contextCleanupCompleted)) {
			result["status"] = "Failed"; result["reason"] = "Validation callbacks or native lifecycle cleanup failed; full initialization-through-shutdown log is retained.";
		}
		result["elapsedMilliseconds"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		return result;
	}
	int RunVulkanScenario(const std::filesystem::path& root, std::string_view scenario, std::uint32_t iterations, Json& receipt)
	{
		receipt["scenario"] = scenario; receipt["requestedIterations"] = iterations;
		receipt["observations"] = Json::array(); receipt["attemptedIterations"] = 0U; receipt["completedIterations"] = 0U;
		receipt["vulkanEvidenceVersion"] = 2U;
		Json windowSession{{"windowInitializeCount", 0U}, {"windowShutdownCount", 0U}, {"windowCleanupCompleted", false},
			{"windowIdentity", nullptr}, {"windowHidden", false}, {"handleCleared", false}};
		SessionLog logs;
		const auto sink = Logger::Instance().AddSink([&](const LogEntry& entry) { logs.Append(entry); });
		struct SinkCleanup { Logger::SinkId sink; ~SinkCleanup() { Logger::Instance().RemoveSink(sink); } } sinkCleanup{sink};
		auto& window = WindowSystem::Instance(); WindowCleanup windowCleanup{window};
		std::string status = "Passed";
		logs.WindowBoundary("WindowInitializeBegin", true);
		try {
			// The OS window is one lane-owned host. Native context construction,
			// devices, surfaces and swapchains remain independent per iteration.
			window.Configure(std::make_unique<GlfwWindowBackend>(), {640, 360, "Runtime memory safety Vulkan validation", false}, [] {});
			windowSession["windowInitializeCount"] = 1U; window.Initialize();
			auto* sharedWindow = static_cast<GLFWwindow*>(window.GetNativeHandle());
			Require(sharedWindow != nullptr, "Shared window backend did not create a native handle.");
			windowSession["windowIdentity"] = PointerIdentity(sharedWindow);
			glfwHideWindow(sharedWindow); windowSession["windowHidden"] = glfwGetWindowAttrib(sharedWindow, GLFW_VISIBLE) == GLFW_FALSE;
			Require(windowSession["windowHidden"].get<bool>(), "Shared fixture window could not be hidden.");
			logs.WindowBoundary("WindowInitializeEnd", true);
			for(std::uint32_t iteration = 1U; iteration <= iterations; ++iteration) {
				logs.BeginIteration(iteration);
				auto result = RunVulkanIteration(root, scenario, iteration, window, sharedWindow, logs);
				logs.EndIteration(result);
				if(result["allSessionValidationErrors"].get<std::size_t>() != 0U) {
					result["status"] = "Failed"; result["reason"] = "Actual iteration Vulkan validation callbacks reported errors.";
				}
				receipt["attemptedIterations"] = iteration;
				if(result["graphicsInstantiated"].get<bool>()) receipt["graphicsInstantiated"] = true;
				const auto outcome = result["status"].get<std::string>();
				if(outcome == "Passed") receipt["completedIterations"] = receipt["completedIterations"].get<std::uint32_t>() + 1U;
				else if(outcome == "Failed" || status == "Passed") status = outcome;
				receipt["observations"].push_back(std::move(result));
				if(outcome != "Passed") break; // Preserve the first failure, without selective retry.
			}
		} catch(const std::exception& error) {
			status = receipt["attemptedIterations"] == 0U ? "Unverified" : "Failed"; receipt["reason"] = error.what();
		}
		// Every per-iteration VulkanContext and pool has already retired. Only now
		// destroy the lane's real window, retaining the logger sink through close.
		logs.WindowBoundary("WindowShutdownBegin", true);
		windowSession["windowShutdownCount"] = 1U; windowCleanup.Close();
		windowSession["windowCleanupCompleted"] = windowCleanup.completed;
		windowSession["handleCleared"] = window.GetNativeHandle() == nullptr;
		const bool windowClosed = windowCleanup.completed && windowSession["handleCleared"].get<bool>();
		logs.WindowBoundary("WindowShutdownEnd", windowClosed); logs.CopyTo(receipt);
		receipt["windowSession"] = std::move(windowSession);
		if(!windowClosed || receipt["allSessionValidationErrors"].get<std::size_t>() != 0U) {
			status = "Failed"; receipt["reason"] = "Actual shared-window cleanup or initialization-through-shutdown validation failed.";
		}
		receipt["status"] = status;
		receipt["nativeAcceptance"] = status == "Passed" && receipt["completedIterations"] == iterations && windowClosed;
		return status == "Passed" ? 0 : status == "Failed" ? 1 : 2;
	}

	int CaptureEmptyFirstFrame(const std::filesystem::path& root, nlohmann::json& receipt)
	{
		// Real window/device operations are reserved for the explicit Vulkan mode.
		// No scenes or rendering results are fabricated by this red-stage probe.
		std::mutex logMutex;
		nlohmann::json validationLog = nlohmann::json::array();
		nlohmann::json firstImageLayoutMessages = nlohmann::json::array();
		std::atomic<bool> firstImageOperationActive{false};
		bool contextCleanupCompleted = false;
		const auto sink = Logger::Instance().AddSink([&](const LogEntry& entry) {
			if (entry.message.find("[Vulkan]") == std::string::npos) return;
			std::lock_guard lock(logMutex);
			validationLog.push_back({{"level", static_cast<int>(entry.level)}, {"message", entry.message}});
			// Only a present-layout error during the actual first-image operation
			// proves this defect. Initialization warnings/errors cannot stand in for it.
			const bool presentDiagnostic = entry.message.find("vkQueuePresentKHR") != std::string::npos
				|| entry.message.find("VkPresentInfoKHR") != std::string::npos;
			const bool layoutDiagnostic = entry.message.find("layout") != std::string::npos
				&& entry.message.find("PRESENT_SRC_KHR") != std::string::npos;
			if (firstImageOperationActive.load() && entry.level == LogLevel::Error && presentDiagnostic && layoutDiagnostic)
				firstImageLayoutMessages.push_back(entry.message);
		});
		struct SinkLifetime { Logger::SinkId id; ~SinkLifetime() { Logger::Instance().RemoveSink(id); } } sinkLifetime{sink};
		auto& window = WindowSystem::Instance();
		struct WindowLifetime { WindowSystem& window; ~WindowLifetime() { window.Shutdown(); } } windowLifetime{window};
		try {
			const auto engineAssets = root / "Editor";
			// Match the installed editor's runtime layout. Shader resolution appends
			// Shaders/... to this root; Build itself is not a shader asset root.
			constexpr std::array requiredShaders{
				"Shaders/Default.task.tspf", "Shaders/Default.mesh.tspf", "Shaders/Default.frag.tspf",
				"Shaders/Default.ConventionalVisibilityIdentity.frag.spv",
				"Shaders/Terrain/TerrainVisibility.task.tspf", "Shaders/Terrain/TerrainVisibility.mesh.tspf",
				"Shaders/Terrain/TerrainVisibility.frag.tspf", "Shaders/Terrain/TerrainSurface.comp.tspf"};
			for (const auto shader : requiredShaders)
				Require(std::filesystem::is_regular_file(engineAssets / shader), "Installed Vulkan shader closure is incomplete.");
			receipt["engineAssetRoot"] = engineAssets.generic_string();
			ResourceManager::Instance().SetEngineRoot(engineAssets);
			window.Configure(std::make_unique<GlfwWindowBackend>(), {640, 360, "Runtime memory safety Vulkan validation", false}, [] {});
			window.Initialize();
			VulkanContext context;
			// VulkanContext's default destructor does not call Shutdown. This guard
			// also runs if Initialize throws after creating only part of the device.
			struct ContextLifetime {
				VulkanContext& context;
				bool& completed;
				~ContextLifetime() { try { context.Shutdown(); completed = true; } catch (...) { completed = false; } }
			} contextLifetime{context, contextCleanupCompleted};
			context.SetValidationLayersRequested(true);
			context.Initialize(window);
			auto actual = context.GetEmptyFrameValidationReceipt();
			receipt["graphicsInstantiated"] = actual.initialized;
			receipt["validationActuallyEnabled"] = actual.validationActuallyEnabled;
			if (!actual.initialized || !actual.validationActuallyEnabled) {
				receipt["status"] = "Unverified";
				receipt["reason"] = "A usable Vulkan surface/device and actually enabled validation layer are required.";
			} else {
				Require(context.ArmEmptyFrameValidation({true, 1}), "Native empty-frame hook is unavailable or not quiescent.");
				firstImageOperationActive.store(true);
				context.BeginFrame();
				context.EndFrame();
				context.WaitIdle();
				actual = context.GetEmptyFrameValidationReceipt();
				firstImageOperationActive.store(false);
				receipt["operation"] = {{"sequence", actual.sequence}, {"generation", actual.generation},
					{"frameSlot", actual.frameSlot}, {"imageIndex", actual.imageIndex}, {"acquired", actual.acquired},
					{"forcedEmpty", actual.forcedEmpty}, {"commandBufferCount", actual.commandBufferCount},
					{"recordEnded", actual.recordEnded}, {"fenceResetAccepted", actual.fenceResetAccepted},
					{"submitAccepted", actual.submitAccepted}, {"presentAttempted", actual.presentAttempted},
					{"resetResult", actual.resetResult}, {"submitResult", actual.submitResult}, {"presentResult", actual.presentResult},
					{"validationErrorsBefore", actual.validationErrorsBefore}, {"validationErrorsAfter", actual.validationErrorsAfter}};
				// This stage captures the actual old empty submission. Full acceptance
				// needs the fallback fix, recreation matrix and normal rendered follow-ups.
				const bool oldEmptySubmitted = actual.acquired && actual.forcedEmpty && actual.commandBufferCount == 0
					&& actual.submitAccepted && actual.presentAttempted;
				{
					std::lock_guard lock(logMutex);
					receipt["status"] = oldEmptySubmitted && !firstImageLayoutMessages.empty() ? "ValidationFailureObserved" : "Unverified";
				}
				receipt["reason"] = "First-image operation probe only; no normal scene follow-up acceptance.";
			}
		} catch (const std::exception& error) {
			receipt["status"] = "Unverified";
			receipt["reason"] = error.what();
		}
		{
			std::lock_guard lock(logMutex);
			receipt["validationCallbackLog"] = validationLog;
			receipt["firstImageLayoutValidationMessages"] = firstImageLayoutMessages;
			std::size_t sessionErrors = 0;
			for (const auto& row : validationLog)
				if (row.at("level").get<int>() == static_cast<int>(LogLevel::Error)) ++sessionErrors;
			receipt["allSessionValidationErrors"] = sessionErrors;
		}
		receipt["contextCleanupCompleted"] = contextCleanupCompleted;
		return receipt["status"] == "ValidationFailureObserved" ? 3 : 2;
	}
}

int main(int argc, char** argv)
{
	try
	{
		std::string mode, scenario;
		std::uint32_t iterations = 1U;
		std::filesystem::path root, output, project;
		bool hasMode = false, hasRoot = false, hasOutput = false, hasScenario = false, hasIterations = false, hasProject = false;
		for (int index = 1; index < argc; index += 2)
		{
			Require(index + 1 < argc, "Every option requires a value.");
			const std::string_view option = argv[index], value = argv[index + 1];
			if (option == "--mode" && !hasMode) { mode = value; hasMode = true; }
			else if (option == "--install-root" && !hasRoot) { root = Path(value); hasRoot = true; }
			else if (option == "--output" && !hasOutput) { output = Path(value); hasOutput = true; }
			else if (option == "--project" && !hasProject) { project = Path(value); hasProject = true; }
			else if (option == "--scenario" && !hasScenario) { scenario = value; hasScenario = true; }
			else if (option == "--iterations" && !hasIterations) {
				const auto parsed = std::from_chars(value.data(), value.data() + value.size(), iterations);
				Require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && iterations > 0U && iterations <= 100000U,
					"Iterations must be a positive decimal integer no greater than 100000.");
				hasIterations = true;
			}
			else throw std::runtime_error("Unknown or duplicate option.");
		}
		Require(hasMode && hasRoot && hasOutput && !mode.empty() && root.is_absolute() && output.is_absolute(),
			"Require --mode, absolute --install-root and absolute --output.");
		Require(std::filesystem::is_directory(root), "Install root is unavailable.");
		Require(!std::filesystem::exists(output), "Receipt output must be fresh.");
		Require(std::filesystem::is_directory(output.parent_path()), "Receipt parent directory must already exist.");
		if(mode == "vulkan") Require(hasScenario && std::find(kGpuScenarios.begin(), kGpuScenarios.end(), scenario) != kGpuScenarios.end(),
			"Vulkan scenario must exactly match one frozen gpuFailureLanes value.");
		else if(mode == "renderer") {
			constexpr std::array<std::string_view, 6> rendererLanes{"completed", "failed", "cancelled", "scene-clear", "shutdown", "restart"};
			Require(hasScenario && std::find(rendererLanes.begin(), rendererLanes.end(), scenario) != rendererLanes.end(),
				"Renderer scenario must exactly match one frozen rendererLifecycleLanes value.");
		} else Require(!hasScenario && !hasIterations, "Scenario/iterations arguments belong only to vulkan or renderer mode.");
		Require(!hasProject || mode == "renderer", "Project argument belongs only to renderer mode.");
		if(mode == "renderer") {
			if(!hasProject) project = output.parent_path() / (output.stem().string() + "-projects");
			Require(project.is_absolute() && !std::filesystem::exists(project), "Renderer project root must be absolute and fresh.");
			// The renderer helper owns its full receipt and preserves all lifecycle
			// logs, so return before the legacy Vulkan/preflight writer below.
			return RuntimeMemorySafetyDraft::RunRendererMode(root, output, project, scenario, iterations);
		}
		nlohmann::json receipt{{"schemaVersion", 1}, {"mode", mode}, {"nativeAcceptance", false},
			{"graphicsInstantiated", false}, {"installRoot", root.generic_string()}};
		int exitCode = 2;
		if (mode == "preflight")
		{
			// Calling exported construction/reset/destruction proves the actual
			// engine DLL closure. It neither creates graphics nor validates stories.
			TerraEngine::Memory::ReusableArenaResource arena(256);
			void* allocation = arena.allocate(16, alignof(std::max_align_t));
			Require(allocation != nullptr, "Reusable arena allocation unavailable.");
			arena.deallocate(allocation, 16, alignof(std::max_align_t));
			arena.Reset();
			receipt["status"] = "PreflightPassed";
			receipt["reusableArenaApiAvailable"] = true;
			receipt["arenaResetCount"] = arena.Statistics().resetCount;
			exitCode = 0;
		}
		else if (mode == "vulkan-empty-first")
		{
			exitCode = CaptureEmptyFirstFrame(root, receipt);
		}
		else if(mode == "vulkan") exitCode = RunVulkanScenario(root, scenario, iterations, receipt);
		else
		{
			receipt["status"] = "Unverified";
			receipt["reason"] = "Requested real-operation mode is not implemented in foundation preflight.";
		}
		std::ofstream stream(output, std::ios::binary);
		Require(stream.is_open(), "Cannot open receipt output.");
		stream << receipt.dump(2) << '\n';
		stream.close();
		Require(stream.good(), "Cannot persist receipt output.");
		std::cout << receipt.dump() << '\n';
		return exitCode;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Runtime memory safety fixture rejected: " << error.what() << '\n';
		return 1;
	}
}
