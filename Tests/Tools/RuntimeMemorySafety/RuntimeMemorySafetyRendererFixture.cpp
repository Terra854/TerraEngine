// Installed native renderer lifecycle qualification. The main fixture parses
// --mode renderer and passes an exact frozen lane plus fresh owned paths here.
#include "RuntimeMemorySafetyHostInstance.h"
#include "RendererBorrowerGate.h"
#include "Renderer/Graph/RenderFrameScratchValidation.h"
#include "Renderer/RenderSystem.h"
#include "Core/Scheduling/JobGraph.h"
#include <bit>
#include <algorithm>
#include <future>
#include <tuple>
#include <string>
#include <vector>

namespace RuntimeMemorySafetyDraft {

	using namespace TerraEngine::Renderer;
	using Inputs = RenderFrameScratchCapturedInputs;
	using Event = RenderFrameScratchValidationEvent;
	using Checkpoint = RenderFrameScratchValidationCheckpoint;
	using RangeOracle = std::function<std::uint64_t(const Inputs&, std::size_t, std::size_t)>;
	// Independent documented wire recipe over authored rows. Do not call the
	// renderer's helper: a matching bug there must remain detectable here.
	std::uint64_t AuthoredRangeDigest(const Inputs& expected, std::size_t begin, std::size_t end) {
		std::uint64_t hash = 14695981039346656037ULL;
		const auto byte = [&](std::uint8_t value) { hash ^= value; hash *= 1099511628211ULL; };
		const auto u32 = [&](std::uint32_t value) { for(unsigned shift = 0; shift < 32U; shift += 8U) byte(static_cast<std::uint8_t>(value >> shift)); };
		const auto u64 = [&](std::uint64_t value) { for(unsigned shift = 0; shift < 64U; shift += 8U) byte(static_cast<std::uint8_t>(value >> shift)); };
		const auto scalar = [&](float value) { u32(std::bit_cast<std::uint32_t>(value)); };
		u64(begin); u64(end);
		for(std::size_t index = begin; index < end; ++index) {
			const auto& row = expected.lights.at(index); const auto& value = row.light;
			for(const auto part : row.objectId) byte(part);
			u32(static_cast<std::uint32_t>(value.type)); u32(static_cast<std::uint32_t>(value.unit)); u32(static_cast<std::uint32_t>(value.appearance));
			scalar(value.color.x); scalar(value.color.y); scalar(value.color.z); scalar(value.temperature);
			scalar(value.intensity); scalar(value.range); scalar(value.innerConeAngle); scalar(value.outerConeAngle);
			scalar(value.width); scalar(value.height); byte(value.castsShadows ? 1U : 0U);
			scalar(value.shadowStrength); scalar(value.shadowBias); scalar(value.indirectMultiplier);
			scalar(value.authoredPriority); scalar(value.stabilityPreference);
		}
		return hash;
	}

	// Compare every authored member by value bits, not component padding or a
	// renderer-generated hash. The expected objects predate scene insertion.
	std::uint32_t Bits(float value) { return std::bit_cast<std::uint32_t>(value); }
	auto CameraValues(const CameraComponent& value) {
		return std::tuple{Bits(value.verticalFovDegrees), Bits(value.aspectRatio), Bits(value.nearPlane),
			Bits(value.farPlane), Bits(value.up.x), Bits(value.up.y), Bits(value.up.z)};
	}
	auto LightValues(const LightComponent& value) {
		return std::tuple{value.type, value.unit, value.appearance,
			Bits(value.color.x), Bits(value.color.y), Bits(value.color.z), Bits(value.temperature),
			Bits(value.intensity), Bits(value.range), Bits(value.innerConeAngle), Bits(value.outerConeAngle),
			Bits(value.width), Bits(value.height), value.castsShadows, Bits(value.shadowStrength),
			Bits(value.shadowBias), Bits(value.indirectMultiplier), Bits(value.authoredPriority), Bits(value.stabilityPreference)};
	}
	void Check(bool value, const char* reason) { if(!value) throw std::runtime_error(reason); }
	auto ScratchValues(const RenderFrameScratchDiagnostics& value) {
		return std::tuple{value.lifetimeId, value.episodeId, value.frameId, value.capacityBytes, value.usedBytes, value.peakBytes,
			value.lifetimeHighWaterBytes, value.reuseAllocationCount, value.fallbackAllocationCount, value.resetCount, value.outcome};
	}
	void RequireExactInputs(const Inputs& expected, const Inputs& captured) {
		Check(expected.cameras.size() == captured.cameras.size(), "Camera capture cardinality differs from authored oracle");
		Check(expected.lights.size() == captured.lights.size(), "Light capture cardinality differs from authored oracle");
		for(std::size_t index = 0; index < expected.cameras.size(); ++index)
			Check(expected.cameras[index].objectId == captured.cameras[index].objectId
				&& CameraValues(expected.cameras[index].camera) == CameraValues(captured.cameras[index].camera),
				"Actual camera UUID order or component values differ");
		for(std::size_t index = 0; index < expected.lights.size(); ++index)
			Check(expected.lights[index].objectId == captured.lights[index].objectId
				&& LightValues(expected.lights[index].light) == LightValues(captured.lights[index].light),
				"Actual light UUID order or component values differ");
	}

	struct ChunkObservation {
		std::size_t begin, end;
		std::uint64_t beforeDigest, afterDigest, computedSignatureDigest;
		bool ended;
	};
	class CaptureObserver final {
	public:
		CaptureObserver(std::shared_ptr<const Inputs> expected, RangeOracle oracle, bool failInline)
			: expected_(std::move(expected)), oracle_(std::move(oracle)), gate_(failInline) {}
		void Observe(const Event& event) {
			if(event.checkpoint == Checkpoint::Captured) {
				Check(event.capturedInputs != nullptr, "Protected capture did not retain copied input proof");
				RequireExactInputs(*expected_, *event.capturedInputs);
				std::lock_guard lock(mutex_); captured_ = true; episode_ = event.episode;
				return;
			}
			Check(event.start < event.end && event.end <= expected_->lights.size(), "Native light range is invalid");
			Check(event.workerLimit > 1U, "Real renderer light preparation has no parallel worker capacity");
			{
				std::lock_guard lock(mutex_);
				Check(captured_ && event.episode.lifetimeId == episode_.lifetimeId
					&& event.episode.episodeId == episode_.episodeId, "Worker read changed scratch identity");
			}
			Check(event.rangeDigest == oracle_(*expected_, event.start, event.end), "Actual source range differs from independent authored oracle");
			if(event.checkpoint == Checkpoint::LightReadBegin) {
				{
					std::lock_guard lock(mutex_);
					chunks_.push_back({event.start, event.end, event.rangeDigest, 0U, 0U, false});
				}
				gate_.Begin(event.start, event.end);
			} else if(event.checkpoint == Checkpoint::LightReadEnd) {
				{
					std::lock_guard lock(mutex_);
					auto found = std::find_if(chunks_.begin(), chunks_.end(), [&](const auto& row) {
						return row.begin == event.start && row.end == event.end;
					});
					Check(found != chunks_.end() && !found->ended, "ReadEnd lacked exactly one matching real ReadBegin");
					found->afterDigest = event.rangeDigest; found->computedSignatureDigest = event.computedSignatureDigest; found->ended = true;
				}
				gate_.End(event.start, event.end, event.rangeDigest);
			}
		}
		RenderFrameScratchDiagnostics CapturedEpisode() {
			std::lock_guard lock(mutex_); Check(captured_, "No protected actual capture was observed"); return episode_;
		}
		std::vector<ChunkObservation> CopyChunks() { std::lock_guard lock(mutex_); return chunks_; }
		std::size_t ExpectedLightCount() const { return expected_->lights.size(); }
		RendererBorrowerGate& Gate() { return gate_; }
	private:
		std::shared_ptr<const Inputs> expected_;
		RangeOracle oracle_;
		RendererBorrowerGate gate_;
		std::mutex mutex_;
		bool captured_ = false;
		RenderFrameScratchDiagnostics episode_;
		std::vector<ChunkObservation> chunks_;
	};

	// The installed host calls this only after real normal startup and authored
	// protected scene creation. No callback performs live ECS writes.
	struct PreparationProbe {
		RenderFrameScratchDiagnostics captured;
		RendererBorrowerGate::Facts held;
		std::vector<ChunkObservation> chunks;
		bool activeWhileHeld = false, phaseUnsettledWhileHeld = false;
		bool protectedClearRejectedWhileHeld = false;
		std::string failure;
	};
	PreparationProbe ProbeRealPreparation(RenderSystem& renderer, std::shared_ptr<const Inputs> expected,
		RangeOracle oracle, bool failInline) {
		auto observer = std::make_shared<CaptureObserver>(std::move(expected), std::move(oracle), failInline);
		auto callbacks = std::make_shared<RenderFrameScratchValidationCallbacks>();
		callbacks->onEvent = [observer](const Event& event) { observer->Observe(event); };
		renderer.SetFrameScratchValidationCallbacks(callbacks);
		auto phases = renderer.DeclareFrameWorkloads(0.0f);
		Check(phases.size() == 5U && phases[0].workloadName == "RenderPublicationCapture"
			&& phases[1].workloadName == "GeometryRoutePrepare", "Normal renderer phase contract changed");
		TerraEngine::Scheduling::JobGraphContext context;
		phases[0].work(context);
		PreparationProbe result; result.captured = observer->CapturedEpisode();
		// Light hashing lies between suspension1 and2: GeometryRoutePrepare.
		// The outer async owner leaves all engine pool workers available to children.
		auto phaseFuture = std::async(std::launch::async, [&] { phases[1].work(context); });
		struct ReleaseBeforeJoin {
			RendererBorrowerGate& gate;
			~ReleaseBeforeJoin() { gate.Release(); }
		} release{observer->Gate()};
		Check(observer->Gate().WaitForHeldChild(), "Backend did not schedule a real parallel light child");
		if(failInline) Check(observer->Gate().WaitForInlineFailure(), "Inline exception was not reached after child arrival");
		result.activeWhileHeld = renderer.GetFrameScratchValidationState().active;
		result.phaseUnsettledWhileHeld = phaseFuture.wait_for(std::chrono::milliseconds(25)) != std::future_status::ready;
		Check(result.activeWhileHeld && result.phaseUnsettledWhileHeld, "Held borrower lost its active owner or failed phase settled early");
		const auto beforeRejectedClear = renderer.GetFrameScratchValidationState();
		const auto backendBeforeRejectedClear = renderer.GetContext()->GetBackendResourceGeneration();
		try { renderer.Clear(); } catch(const std::logic_error&) { result.protectedClearRejectedWhileHeld = true; }
		const auto afterRejectedClear = renderer.GetFrameScratchValidationState();
		const auto reclamationValues = [](const auto& value) { return std::tuple{value.lifetimeId, value.totalFallbackAllocationCount,
			value.totalFallbackDeallocationCount, value.outstandingFallbackAllocationCount, value.outstandingFallbackBytes, value.resetCount}; };
		Check(result.protectedClearRejectedWhileHeld && afterRejectedClear.active
			&& ScratchValues(beforeRejectedClear.latest) == ScratchValues(afterRejectedClear.latest)
			&& ScratchValues(beforeRejectedClear.previous) == ScratchValues(afterRejectedClear.previous)
			&& reclamationValues(beforeRejectedClear.reclamation) == reclamationValues(afterRejectedClear.reclamation)
			&& renderer.GetContext()->GetBackendResourceGeneration() == backendBeforeRejectedClear,
			"Overlapping protected Clear was not rejected before lifetime/backend changes");
		observer->Gate().Release();
		Check(phaseFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready, "Released native preparation did not settle");
		try { phaseFuture.get(); } catch(const std::exception& error) { result.failure = error.what(); }
		if(failInline) Check(result.failure == "RMS renderer inline failure after held child arrived", "Original renderer inline failure was lost");
		else Check(result.failure.empty(), "Healthy renderer preparation failed");
		result.held = observer->Gate().CopyFacts(); result.chunks = observer->CopyChunks();
		Check(result.held.childReadCompleted, "Held child never completed its actual source read before retirement");
		if(!failInline) {
			std::sort(result.chunks.begin(), result.chunks.end(), [](const auto& first, const auto& second) { return first.begin < second.begin; });
			std::size_t next = 0U;
			for(const auto& chunk : result.chunks) {
				Check(chunk.ended && chunk.begin == next && chunk.beforeDigest == chunk.afterDigest,
					"Actual completed light ranges have a gap, overlap, or altered source");
				next = chunk.end;
			}
			Check(next == observer->ExpectedLightCount(), "Actual light ranges did not cover every authored light");
			for(std::size_t index = 2U; index < phases.size(); ++index) phases[index].work(context);
		}
		return result;
	}
}

// Normal Engine startup and protected native authoring qualify the renderer's
// real collection and lifecycle paths; this host never claims mesh draws.
// CLI: --mode renderer --scenario completed|failed|cancelled|scene-clear|shutdown|restart
// --iterations N --install-root <Build> --output <fresh.json> [--project <fresh root>]
#include "RuntimeMemorySafetyRendererFixture.h"
#include "Core/MainEngine.h"
#include "Core/Logger/Logger.h"
#include "Components/Transform.h"
#include "Renderer/RHI/Vulkan/VulkanContext.h"
#include "Resource/ResourceManager.h"
#include "Scene/SceneManager.h"
#include "Animation/AnimationSystem.h"
#include "Audio/AudioSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Scripting/ScriptingSystem.h"
#include "Window/WindowSystem.h"
#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <charconv>
#include <map>
#include <set>
#include <iostream>

namespace RuntimeMemorySafetyDraft {
	using Json = nlohmann::json;
	constexpr std::size_t kPressureLights = 131072U;
	// Missing native prerequisites cannot establish acceptance or a behavioral
	// failure. Keep this distinct from actual initialized-path invariant errors.
	class RendererPrerequisiteUnavailable final : public std::runtime_error {
	public:
		using std::runtime_error::runtime_error;
	};
	Json Episode(const RenderFrameScratchDiagnostics& value) {
		return {{"lifetimeId", value.lifetimeId}, {"episodeId", value.episodeId}, {"frameId", value.frameId},
			{"capacityBytes", value.capacityBytes}, {"usedBytes", value.usedBytes}, {"peakBytes", value.peakBytes},
			{"lifetimeHighWaterBytes", value.lifetimeHighWaterBytes}, {"reuseAllocationCount", value.reuseAllocationCount},
			{"fallbackAllocationCount", value.fallbackAllocationCount}, {"resetCount", value.resetCount}, {"outcome", static_cast<unsigned>(value.outcome)}};
	}
	Json Reclamation(const RenderFrameScratchReclamationDiagnostics& value) {
		return {{"lifetimeId", value.lifetimeId}, {"totalFallbackAllocationCount", value.totalFallbackAllocationCount},
			{"totalFallbackDeallocationCount", value.totalFallbackDeallocationCount},
			{"outstandingFallbackAllocationCount", value.outstandingFallbackAllocationCount},
			{"outstandingFallbackBytes", value.outstandingFallbackBytes}, {"resetCount", value.resetCount}};
	}
	template<class State> Json StateJson(const State& state) {
		return {{"active", state.active}, {"latest", Episode(state.latest)}, {"previous", Episode(state.previous)},
			{"reclamation", Reclamation(state.reclamation)}};
	}
	template<class State> void RequireOutcome(const State& state, const RenderFrameScratchDiagnostics& captured, RenderFrameScratchOutcome outcome) {
		const auto matches = [&](const auto& row) { return row.lifetimeId == captured.lifetimeId && row.episodeId == captured.episodeId && row.outcome == outcome; };
		Check(matches(state.latest) || matches(state.previous), "Actual terminal episode outcome/identity was not retained");
	}
	void RequireBalanced(const RenderFrameScratchReclamationDiagnostics& value) {
		Check(value.totalFallbackAllocationCount > 0U && value.totalFallbackAllocationCount == value.totalFallbackDeallocationCount
			&& value.outstandingFallbackAllocationCount == 0U && value.outstandingFallbackBytes == 0U,
			"Actual pressure fallback allocations were not fully reclaimed");
	}
	TerraEngine::UUID Id(bool camera, std::size_t ordinal) {
		TerraEngine::UUID id{}; id[0] = 0x06U; id[1] = camera ? 0x6cU : 0x6dU;
		for(unsigned index = 0U; index < 8U; ++index) id[15U-index] = static_cast<std::uint8_t>(ordinal >> (index * 8U));
		return id;
	}
	std::shared_ptr<const Inputs> AuthorProtectedInputs() {
		auto& objects = ObjectManager::instance();
		objects.clear();
		std::map<TerraEngine::UUID, CameraComponent> authoredCameras;
		std::map<TerraEngine::UUID, LightComponent> authoredLights;
		// Reverse insertion avoids accidentally testing only insertion order. The
		// independent preexisting protected query below supplies actual oracle order.
		for(std::size_t ordinal = 4U; ordinal > 0U; --ordinal) {
			CameraComponent value; value.verticalFovDegrees = 40.0f + static_cast<float>(ordinal);
			value.aspectRatio = 16.0f/9.0f; value.nearPlane = 0.125f; value.farPlane = 128.0f;
			const auto object = objects.createObject("RMS camera"); const auto id = Id(true, ordinal);
			objects.UpdateObjectUUID(object, id); *objects.AddComponent<CameraComponent>(object) = value;
			authoredCameras.emplace(id, value);
		}
		for(std::size_t ordinal = kPressureLights; ordinal > 0U; --ordinal) {
			LightComponent value; value.type = LightType::Point; value.castsShadows = false;
			value.color = {0.25f + static_cast<float>(ordinal % 5U)*0.125f, 0.5f, 0.75f};
			value.intensity = 1.0f + static_cast<float>(ordinal % 7U)*0.25f;
			value.range = 2.0f + static_cast<float>(ordinal % 3U); value.authoredPriority = static_cast<float>(ordinal % 11U)*0.125f;
			value.stabilityPreference = 0.25f;
			const auto object = objects.createObject("RMS pressure point"); const auto id = Id(false, ordinal);
			objects.UpdateObjectUUID(object, id); *objects.AddComponent<LightComponent>(object) = value;
			authoredLights.emplace(id, value);
		}
		auto expected = std::make_shared<Inputs>();
		objects.forEachObjectWithComponents<Transform, CameraComponent>([&](const Object& object, Transform&, CameraComponent&) {
			const auto id = objects.getUUID(object); expected->cameras.push_back({id, authoredCameras.at(id)});
		});
		objects.forEachObjectWithComponents<Transform, LightComponent>([&](const Object& object, Transform&, LightComponent&) {
			const auto id = objects.getUUID(object); expected->lights.push_back({id, authoredLights.at(id)});
		});
		Check(expected->cameras.size() == 4U && expected->lights.size() == kPressureLights, "Protected oracle omitted authored qualification objects");
		return expected;
	}
	void EnterPausedPlay() {
		auto& physics = TerraEngine::Physics::PhysicsSystem::Instance();
		Check(physics.QueueSettingsUpdate(TerraEngine::Physics::PhysicsSettings::Defaults()) && physics.StartWorld(), "Fixture physics startup failed");
		auto& animation = TerraEngine::Animation::AnimationSystem::Instance();
		(void)animation.NotifyLifecycleInvalidation(TerraEngine::Animation::AnimationLifecycleBoundary::PlayMode);
		animation.SetAuthoredComponentPlaybackEnabled(true); animation.SetAuthoredComponentPlaybackAdvancing(true);
		auto& audio = TerraEngine::Audio::AudioSystem::Instance(); audio.SetRuntimePlaybackEnabled(true);
		ScriptingSystem::Instance().SetRunning(true); Check(physics.SetPaused(true), "Fixture pause failed");
		ScriptingSystem::Instance().SetPaused(true); audio.SetRuntimePlaybackPaused(true);
	}
	void RequirePaused() {
		const auto time = ScriptingSystem::Instance().GetRuntimeTimeSnapshot();
		Check(time.playSessionActive && time.playSessionPaused && !time.advancesSimulation
			&& TerraEngine::Physics::PhysicsSystem::Instance().IsPaused(), "Renderer fixture advanced simulation");
	}
	// No held worker exists in this path: capture returns at the real first
	// suspension and lifecycle retirement occurs on this protected coordinator.
	RenderFrameScratchDiagnostics CaptureOnly(RenderSystem& renderer, std::shared_ptr<const Inputs> expected) {
		auto observer = std::make_shared<CaptureObserver>(expected, AuthoredRangeDigest, false);
		auto callbacks = std::make_shared<RenderFrameScratchValidationCallbacks>();
		callbacks->onEvent = [observer](const Event& event) { observer->Observe(event); };
		renderer.SetFrameScratchValidationCallbacks(callbacks);
		auto phases = renderer.DeclareFrameWorkloads(0.0f); TerraEngine::Scheduling::JobGraphContext context;
		phases.at(0).work(context);
		const auto captured = observer->CapturedEpisode();
		Check(renderer.GetFrameScratchValidationState().active, "Capture did not retain a suspended strong episode lease");
		return captured;
	}
	void RequirePressure(const RenderFrameScratchDiagnostics& value) {
		Check(value.capacityBytes == 16U*1024U*1024U && value.fallbackAllocationCount > 0U,
			"Actual authored light capture did not exceed unchanged16MiB scratch backing");
	}
	Json PausedFacts() {
		const auto time = ScriptingSystem::Instance().GetRuntimeTimeSnapshot();
		return {{"playSessionActive", time.playSessionActive}, {"playSessionPaused", time.playSessionPaused},
			{"advancesSimulation", time.advancesSimulation}, {"physicsPaused", TerraEngine::Physics::PhysicsSystem::Instance().IsPaused()}};
	}
	// These are observed context lifetimes, not vkCreateInstance instrumentation.
	// Startup predates the optional host; later Initialize calls run under it.
	class HostContextObservations final {
	public:
		explicit HostContextObservations(RuntimeMemorySafetySupport::HostInstanceOwner& host) : host_(host) {}
		std::size_t Initialized(RenderSystem& renderer, std::string_view role, std::uint32_t iteration, bool hostLiveBefore) {
			auto* context = dynamic_cast<VulkanContext*>(renderer.GetContext());
			Check(context != nullptr && host_.Ready(), "Host context observation requires actual Vulkan and a live host");
			const auto native = context->GetEmptyFrameValidationReceipt(); VulkanContext::EditorHandles handles; context->GetEditorHandles(handles);
			const bool distinct = host_.Distinct(reinterpret_cast<VkInstance>(handles.instance));
			Check(native.initialized && native.validationActuallyEnabled && handles.instance && handles.device && distinct,
				"Observed renderer context is not independently initialized and validated");
			contexts_.push_back({{"iteration", iteration}, {"role", role},
				{"instanceHandle", std::to_string(reinterpret_cast<std::uintptr_t>(handles.instance))},
				{"deviceHandle", std::to_string(reinterpret_cast<std::uintptr_t>(handles.device))},
				{"initialized", native.initialized}, {"validationActuallyEnabled", native.validationActuallyEnabled},
				{"hostLiveBefore", hostLiveBefore}, {"hostLiveAfter", false}, {"distinctFromHost", distinct},
				{"shutdownAttempted", false}, {"contextReleased", false},
				{"observationCheckpoint", role == "Startup" ? "ExistingStartupContextAfterHostCreation" : "ContextInitializedUnderHost"}});
			++initialized_; return contexts_.size()-1U;
		}
		void Shutdown(RenderSystem& renderer, std::optional<std::size_t>& index) {
			if(index) contexts_.at(*index)["shutdownAttempted"] = true;
			renderer.Shutdown();
			if(index) {
				auto& observation = contexts_.at(*index);
				const bool released = renderer.GetContext() == nullptr;
				observation["contextReleased"] = released; observation["hostLiveAfter"] = host_.Ready();
				if(released) { ++retired_; index.reset(); }
				Check(released && host_.Ready(), "Renderer context was not retired while the fixture host remained live");
			}
		}
		Json ForIteration(std::uint32_t iteration) const {
			Json result = Json::array(); for(const auto& row : contexts_) if(row["iteration"] == iteration) result.push_back(row); return result;
		}
		Json Snapshot(bool finalShutdownAttempted, bool contextReleased, bool windowLive) const {
			return {{"observedInitializationCount", initialized_}, {"observedRetirementCount", retired_}, {"contexts", contexts_},
				{"finalRendererShutdownAttempted", finalShutdownAttempted}, {"finalContextReleased", contextReleased}, {"windowLiveBeforeHostClose", windowLive}};
		}
	private:
		RuntimeMemorySafetySupport::HostInstanceOwner& host_;
		Json contexts_ = Json::array(); std::size_t initialized_ = 0U, retired_ = 0U;
	};
	void StopPausedPlayAtProtectedBoundary() {
		// Mirror editor stop ordering without shutting down the managed host.
		auto& animation = TerraEngine::Animation::AnimationSystem::Instance();
		(void)animation.NotifyLifecycleInvalidation(TerraEngine::Animation::AnimationLifecycleBoundary::PlayMode);
		Check(TerraEngine::Physics::PhysicsSystem::Instance().StopWorld(), "Fixture physics stop failed");
		TerraEngine::Audio::AudioSystem::Instance().SetRuntimePlaybackEnabled(false);
		ScriptingSystem::Instance().SetRunning(false); ScriptingSystem::Instance().SetPaused(false);
		animation.SetAuthoredComponentPlaybackEnabled(false); animation.SetAuthoredComponentPlaybackAdvancing(false);
	}
	Json RunRendererIteration(Engine& engine, const std::filesystem::path& project, std::string_view scenario, std::uint32_t iteration,
		RuntimeMemorySafetySupport::HostInstanceOwner* hostInstance = nullptr, HostContextObservations* contextObservations = nullptr) {
		Json output{{"status", "Unverified"}, {"scenarioId", scenario}, {"authoredLightCount", kPressureLights}, {"meshDrawClaim", false}};
		auto& renderer = RenderSystem::Instance();
		bool initialized = false, enteredPlay = false;
		std::optional<std::size_t> observedContext;
		if(hostInstance) output["hostInstance"] = {{"instanceHandle", hostInstance->Identity()},
			{"liveBeforeContextInitialization", hostInstance->Ready()}, {"liveAfterContextCleanup", false}, {"contextLifecycles", Json::array()}};
		try {
			// The prior iteration already retired its renderer before clearing scene
			// authority. Keep the process's engine/providers/pool alive throughout.
			Check(renderer.GetContext() == nullptr, "Previous iteration retained a renderer context");
			output["threadPoolWorkerCount"] = ThreadPool::instance().getThreadCount();
			Check(output["threadPoolWorkerCount"].get<std::size_t>() > 0U, "Renderer iteration lost its engine-owned workers");
			if(auto* native = static_cast<GLFWwindow*>(WindowSystem::Instance().GetNativeHandle())) {
				glfwHideWindow(native); output["windowHidden"] = glfwGetWindowAttrib(native, GLFW_VISIBLE) == GLFW_FALSE;
			}
			Check(output.value("windowHidden", false), "Renderer fixture window was not actually hidden");
			auto& scenes = SceneManager::Instance(); std::string error;
			const auto scenePath = project / "Assets/Scenes" / ("Qualification-"+std::to_string(iteration)+".tescene");
			Check(scenes.CreateSceneFile(scenePath, "Runtime memory safety qualification", &error), "Cannot create isolated qualification scene");
			Check(scenes.SetActiveScene(scenes.LoadScene(scenePath)), "Cannot activate isolated qualification scene");
			const bool hostLiveBeforePrimary = !hostInstance || hostInstance->Ready();
			renderer.Initialize(); initialized = true;
			if(contextObservations) observedContext = contextObservations->Initialized(renderer, "Primary", iteration, hostLiveBeforePrimary);
			// SetRunning/SetPaused select state; the normal frame publishes the
			// copied time snapshot and immutable producer inputs. This is a paused
			// empty-scene warm frame, with no explicit simulation step.
			enteredPlay = true; EnterPausedPlay(); Check(engine.Update(0.0f), "Paused empty-scene warm frame failed"); RequirePaused();
			output["pausedBefore"] = PausedFacts();
			auto expected = AuthorProtectedInputs();
			auto* native = dynamic_cast<VulkanContext*>(renderer.GetContext());
			if(native == nullptr) throw RendererPrerequisiteUnavailable("Renderer host did not instantiate native Vulkan");
			const auto initial = native->GetEmptyFrameValidationReceipt();
			output["graphicsInstantiated"] = initial.initialized; output["validationActuallyEnabled"] = initial.validationActuallyEnabled;
			if(!initial.initialized || !initial.validationActuallyEnabled)
				throw RendererPrerequisiteUnavailable("Renderer graphics or actually enabled validation layer unavailable");
			const auto adapter = native->GetLatestRendererRecoverySnapshot();
			output["rendererStartup"] = {{"initialized", initial.initialized}, {"validationActuallyEnabled", initial.validationActuallyEnabled},
				{"renderSessionGeneration", adapter.renderSessionGeneration}};
			output["adapter"] = {{"hardwareName", adapter.hardwareName}, {"driverVersion", adapter.driverVersion}, {"apiVersion", adapter.apiVersion}};
			if(adapter.hardwareName.empty() || adapter.driverVersion.empty())
				throw RendererPrerequisiteUnavailable("Renderer actual adapter identity unavailable");
			RenderFrameScratchDiagnostics captured;
			if(scenario == "completed" || scenario == "failed") {
				const auto probe = ProbeRealPreparation(renderer, expected, AuthoredRangeDigest, scenario == "failed");
				captured = probe.captured; RequirePressure(captured);
				output["heldRead"] = {{"arrived", probe.held.childArrived}, {"completed", probe.held.childReadCompleted},
					{"begin", probe.held.heldBegin}, {"end", probe.held.heldEnd}, {"digest", probe.held.childDigest},
					{"activeWhileHeld", probe.activeWhileHeld}, {"phaseUnsettledWhileHeld", probe.phaseUnsettledWhileHeld}, {"originalFailure", probe.failure}};
				output["heldRead"]["protectedClearRejectedWhileHeld"] = probe.protectedClearRejectedWhileHeld;
				output["chunks"] = Json::array();
				for(const auto& chunk : probe.chunks) output["chunks"].push_back({{"begin", chunk.begin}, {"end", chunk.end},
					{"beforeDigest", chunk.beforeDigest}, {"afterDigest", chunk.afterDigest}, {"computedSignatureDigest", chunk.computedSignatureDigest}, {"ended", chunk.ended}});
				// Failed worker destruction marks the lease only. Its terminal copy is
				// published by this protected lifecycle call after every child settles.
				if(scenario == "failed") renderer.Clear();
				const auto terminal = renderer.GetFrameScratchValidationState();
				RequireOutcome(terminal, captured, scenario == "failed" ? RenderFrameScratchOutcome::Failed : RenderFrameScratchOutcome::Completed);
				Check(!terminal.active, "Settled completion/failure retained active lease"); output["terminal"] = StateJson(terminal);
			} else {
				captured = CaptureOnly(renderer, expected); RequirePressure(captured);
				if(scenario == "cancelled") {
					const auto next = CaptureOnly(renderer, expected);
					const auto state = renderer.GetFrameScratchValidationState(); RequireOutcome(state, captured, RenderFrameScratchOutcome::Cancelled);
					Check(next.lifetimeId == captured.lifetimeId && next.episodeId > captured.episodeId, "Cancelled next capture lost lifetime/episode ordering");
					// State.latest/previous remain terminal copies; record the actual
					// new capture event separately rather than relabeling those copies.
					output["nextCaptured"] = Episode(next);
					output["nextCapture"] = StateJson(state);
				} else if(scenario == "scene-clear") {
					renderer.Clear(); ObjectManager::instance().clear();
				} else { if(contextObservations) contextObservations->Shutdown(renderer, observedContext); else renderer.Shutdown(); }
				const auto retired = renderer.GetFrameScratchValidationState(); RequireOutcome(retired, captured, RenderFrameScratchOutcome::Cancelled);
				output["retired"] = StateJson(retired);
				if(scenario != "cancelled") { Check(!retired.active, "Protected lifecycle left a live scratch lease"); RequireBalanced(retired.reclamation); }
				if(scenario == "restart") {
					const bool hostLiveBeforeRestart = !hostInstance || hostInstance->Ready();
					renderer.Initialize();
					if(contextObservations) observedContext = contextObservations->Initialized(renderer, "Restart", iteration, hostLiveBeforeRestart);
				}
				if(scenario == "scene-clear" || scenario == "restart") expected = AuthorProtectedInputs();
			}
			output["captured"] = Episode(captured);
			// Every surviving host finishes with a healthy real normal renderer
			// frame, then protected Clear proves actual pressure reclamation.
			if(scenario != "shutdown") {
				renderer.SetFrameScratchValidationCallbacks(nullptr);
				native = dynamic_cast<VulkanContext*>(renderer.GetContext());
				const auto beforeHealthy = native->GetEmptyFrameValidationReceipt();
				const auto healthy = ProbeRealPreparation(renderer, expected, AuthoredRangeDigest, false);
				RequirePressure(healthy.captured);
				if(scenario == "restart" || scenario == "scene-clear") Check(healthy.captured.lifetimeId != captured.lifetimeId, "Lifecycle reused retired scratch identity");
				native = dynamic_cast<VulkanContext*>(renderer.GetContext()); native->WaitIdle();
				const auto receipt = native->GetEmptyFrameValidationReceipt();
				Check(receipt.acquired && receipt.sequence > beforeHealthy.sequence
					&& receipt.ordinarySubmissionSerial > beforeHealthy.ordinarySubmissionSerial
					&& receipt.ordinarySubmitAccepted && receipt.ordinaryCompletionObserved && receipt.waitIdleCompletionObserved
					&& receipt.commandBufferCount > 0U && !receipt.forcedEmpty && receipt.presentAttempted
					&& (receipt.presentResult == 0 || receipt.presentResult == 1000001003) && !receipt.recoverySuspended,
					"Healthy lifecycle follow-up lacks actual ordinary submission/completion");
				output["healthyFollowup"] = {{"captured", Episode(healthy.captured)}, {"ordinarySubmissionSerial", receipt.ordinarySubmissionSerial},
					{"previousOrdinarySubmissionSerial", beforeHealthy.ordinarySubmissionSerial}, {"previousAcquisitionSequence", beforeHealthy.sequence},
					{"sequence", receipt.sequence}, {"acquired", receipt.acquired}, {"generation", receipt.generation},
					{"commandBufferCount", receipt.commandBufferCount}, {"forcedEmpty", receipt.forcedEmpty},
					{"ordinarySubmitAccepted", receipt.ordinarySubmitAccepted}, {"ordinaryCompletionObserved", receipt.ordinaryCompletionObserved},
					{"waitIdleAttempted", receipt.waitIdleAttempted}, {"waitIdleResult", receipt.waitIdleAttempted ? Json(receipt.waitIdleResult) : Json(nullptr)},
					{"waitIdleCompletionObserved", receipt.waitIdleCompletionObserved}, {"presentAttempted", receipt.presentAttempted},
					{"presentResult", receipt.presentAttempted ? Json(receipt.presentResult) : Json(nullptr)}, {"recoverySuspended", receipt.recoverySuspended}};
				renderer.Clear(); const auto cleared = renderer.GetFrameScratchValidationState(); RequireBalanced(cleared.reclamation); output["balancedAfterClear"] = StateJson(cleared);
			}
			RequirePaused(); output["pausedAfter"] = PausedFacts(); output["authoredCapturedParity"] = true; output["status"] = "Passed";
		} catch(const RendererPrerequisiteUnavailable& error) { output["status"] = "Unverified"; output["reason"] = error.what(); }
		catch(const std::exception& error) { output["status"] = initialized ? "Failed" : "Unverified"; output["reason"] = error.what(); }
		// Retire the continuation before callback owners or scene caches. A failed
		// attempt remains a failed observation, but still releases all actual owners.
		Json rendererCleanup{{"attempted", true}, {"completed", false}, {"contextReleased", false},
			{"activeAfterRetirement", true}, {"retiredState", nullptr}, {"workerCountAfterRetirement", ThreadPool::instance().getThreadCount()}};
		try {
			if(enteredPlay) StopPausedPlayAtProtectedBoundary();
		} catch(const std::exception& error) { output["status"] = "Failed"; output["reason"] = error.what(); }
		try {
			if(renderer.GetContext() != nullptr) {
				if(contextObservations) contextObservations->Shutdown(renderer, observedContext); else renderer.Shutdown();
			}
			renderer.SetFrameScratchValidationCallbacks(nullptr);
			const auto retired = renderer.GetFrameScratchValidationState();
			rendererCleanup["contextReleased"] = renderer.GetContext() == nullptr;
			rendererCleanup["activeAfterRetirement"] = retired.active;
			rendererCleanup["retiredState"] = StateJson(retired);
			rendererCleanup["workerCountAfterRetirement"] = ThreadPool::instance().getThreadCount();
			Check(!retired.active && renderer.GetContext() == nullptr && ThreadPool::instance().getThreadCount() > 0U,
				"Renderer retirement lost context/lease cleanup or engine worker ownership");
			if(output["status"] == "Passed") RequireBalanced(retired.reclamation);
			rendererCleanup["completed"] = true;
			SceneManager::Instance().UnloadAll(); ObjectManager::instance().clear();
			output["loadedScenesAfterCleanup"] = SceneManager::Instance().GetLoadedSceneIds().size();
			output["sceneObjectsAfterCleanup"] = ObjectManager::instance().GetRuntimeActiveMembershipSnapshot().size();
			Check(output["loadedScenesAfterCleanup"] == 0U && output["sceneObjectsAfterCleanup"] == 0U, "Iteration retained old scene documents or objects");
		} catch(const std::exception& error) { output["status"] = "Failed"; output["reason"] = error.what(); }
		output["rendererCleanup"] = std::move(rendererCleanup);
		if(hostInstance) {
			output["hostInstance"]["liveAfterContextCleanup"] = hostInstance->Ready();
			output["hostInstance"]["contextLifecycles"] = contextObservations->ForIteration(iteration);
		}
		return output;
	}

	class RendererSessionLog final {
	public:
		void Append(const LogEntry& entry) {
			std::lock_guard lock(mutex_);
			Json row{{"sequence", entries_.size()+1U}, {"iteration", iteration_}, {"phase", phase_},
				{"eventKind", "logger"}, {"level", static_cast<int>(entry.level)}, {"message", entry.message}};
			entries_.push_back(row);
			if(entry.message.find("[Vulkan]") != std::string::npos) {
				callbacks_.push_back(row); if(entry.level >= LogLevel::Error) ++errors_;
			}
		}
		void HostBoundary(std::string phase, bool succeeded) {
			std::lock_guard lock(mutex_); iteration_ = 0U;
			phase_ = phase.starts_with("HostInstanceShutdown") ? "HostInstanceShutdown"
				: phase.starts_with("HostInstanceInitialize") ? "HostInstanceInitialize"
				: phase.starts_with("StartupRendererShutdown") ? "StartupRendererShutdown"
				: phase.starts_with("HostShutdown") ? "HostShutdown" : "HostInitialize";
			BoundaryLocked(std::move(phase), succeeded);
		}
		void BeginIteration(std::uint32_t iteration) {
			std::lock_guard lock(mutex_); iteration_ = iteration; phase_ = "Iteration"; sliceStart_ = entries_.size();
			BoundaryLocked("IterationBegin", true);
		}
		void EndIteration(Json& output) {
			std::lock_guard lock(mutex_); BoundaryLocked("IterationEnd", output.at("status") == "Passed");
			Json rows = Json::array(), callbacks = Json::array(); std::size_t errors = 0U;
			for(std::size_t index = sliceStart_; index < entries_.size(); ++index) rows.push_back(entries_[index]);
			for(const auto& row : callbacks_) if(row["iteration"] == iteration_) {
				callbacks.push_back(row); if(row["level"].get<int>() >= static_cast<int>(LogLevel::Error)) ++errors;
			}
			output["allSessionLog"] = std::move(rows); output["validationCallbackLog"] = std::move(callbacks);
			output["allSessionValidationErrors"] = errors;
			// Close the slice under the same lock as logger admission. Late async
			// messages belong to the root span and can never silently escape it.
			iteration_ = 0U; phase_ = "BetweenIterations";
		}
		void CopyAll(Json& output) {
			std::lock_guard lock(mutex_); output["allSessionLog"] = entries_; output["validationCallbackLog"] = callbacks_;
			output["allSessionValidationErrors"] = errors_;
		}
	private:
		void BoundaryLocked(std::string phase, bool succeeded) {
			entries_.push_back({{"sequence", entries_.size()+1U}, {"iteration", iteration_}, {"phase", std::move(phase)},
				{"eventKind", "fixtureBoundary"}, {"level", nullptr}, {"message", "Native fixture lifecycle boundary"}, {"succeeded", succeeded}});
		}
		std::mutex mutex_;
		Json entries_ = Json::array(), callbacks_ = Json::array();
		std::size_t errors_ = 0U, sliceStart_ = 0U;
		std::uint32_t iteration_ = 0U;
		std::string phase_ = "HostInitialize";
	};

	// Parent CLI owns parsing. Recheck the narrow public contract before any
	// startup or filesystem ownership is acquired by this fixture helper.
	int RunRendererMode(const std::filesystem::path& install, const std::filesystem::path& outputPath,
		const std::filesystem::path& projectRoot, std::string_view scenario, std::uint32_t iterations, bool hostInstanceRequested) {
		const std::set<std::string_view> lanes{"completed", "failed", "cancelled", "scene-clear", "shutdown", "restart"};
		Check(lanes.contains(scenario) && iterations > 0U && iterations <= 100000U, "Invalid renderer lifecycle lane or iteration count");
		Check(!hostInstanceRequested || iterations >= 100U, "Host-instance qualification requires at least100 renderer lifetimes");
		Check(install.is_absolute() && outputPath.is_absolute() && projectRoot.is_absolute()
			&& std::filesystem::is_directory(install), "Renderer paths must be absolute with an existing install root");
		// ScriptingSystem's normal startup searches current/Editor/Managed. Pin
		// that search to this installed closure; never rely on a global .NET host.
		// Presence qualifies startup closure only; this fixture makes no managed
		// execution claim without an independently observed assembly-load proof.
		for(const auto* relative : {"TerraEngine.CSharp.dll", "TerraEngine.CSharp.runtimeconfig.json", "hostfxr.dll"})
			Check(std::filesystem::is_regular_file(install / "Editor/Managed" / relative), "Installed bundled Managed/runtime closure is incomplete");
		struct WorkingDirectoryRestore {
			std::filesystem::path original = std::filesystem::current_path();
			~WorkingDirectoryRestore() { std::error_code error; std::filesystem::current_path(original, error); }
		} restoreWorkingDirectory;
		std::filesystem::current_path(install);
		Check(!std::filesystem::exists(outputPath) && std::filesystem::is_directory(outputPath.parent_path()), "Renderer receipt must be fresh with an existing parent");
		Check(!std::filesystem::exists(projectRoot), "Renderer owned project parent must be fresh");
		std::filesystem::create_directories(projectRoot);
		std::filesystem::create_directories(projectRoot / "Build/Assets"); std::filesystem::create_directories(projectRoot / "Assets/Scenes");
		Json receipt{{"schemaVersion", 1}, {"rendererEvidenceVersion", hostInstanceRequested ? 3 : 2}, {"mode", "renderer"}, {"scenario", scenario}, {"requestedIterations", iterations},
			{"attemptedIterations", 0U}, {"completedIterations", 0U}, {"status", "Passed"}, {"nativeAcceptance", false},
			{"capturedLifetimeIds", Json::array()}, {"observations", Json::array()}};
		Json host{{"engineInitializeCount", 0U}, {"engineShutdownCount", 0U}, {"engineCleanupCompleted", false},
			{"threadPoolWorkerCount", 0U}, {"threadPoolWorkersAfterCleanup", nullptr}, {"frameFailure", nullptr}};
		RendererSessionLog logs;
		const auto sink = Logger::Instance().AddSink([&](const LogEntry& entry) { logs.Append(entry); });
		struct SinkRemoval { Logger::SinkId id; ~SinkRemoval() { Logger::Instance().RemoveSink(id); } } remove{sink};
		Engine engine;
		RuntimeMemorySafetySupport::HostInstanceOwner hostInstance{hostInstanceRequested,
			[](void* state, std::string_view name, bool succeeded) { static_cast<RendererSessionLog*>(state)->HostBoundary(std::string(name), succeeded); }, &logs};
		HostContextObservations contextObservations{hostInstance};
		// Unexpected unwind must retire the renderer first, then the independent
		// host, before Engine can terminate GLFW. Failed retained contexts never pass.
		struct EngineCleanup {
			Engine& engine; RuntimeMemorySafetySupport::HostInstanceOwner& host; bool enabled, closed = false;
			~EngineCleanup() noexcept {
				if(closed) return;
				if(enabled) { try { RenderSystem::Instance().Shutdown(); } catch(...) {} (void)host.Close(); }
				try { engine.Shutdown(); } catch(...) {}
			}
		} cleanup{engine, hostInstance, hostInstanceRequested};
		std::set<std::uint64_t> capturedLifetimes;
		std::uint64_t previousLifetime = 0U;
		logs.HostBoundary("HostInitializeBegin", true);
		try {
			// CoreCLR is process-owned. Configure and initialize it exactly once;
			// repetitions replace only actual renderer and scene lifetimes.
			auto& resources = ResourceManager::Instance(); resources.SetEngineRoot(install / "Editor"); resources.SetAssetRoot(projectRoot / "Build/Assets");
			resources.ConfigureEditorStartupProfile(RenderSettings{}, true, false, projectRoot, "RuntimeMemorySafetyFixture renderer lifecycle");
			host["engineInitializeCount"] = 1U; engine.Initialize();
			host["threadPoolWorkerCount"] = ThreadPool::instance().getThreadCount();
			Check(ThreadPool::instance().getThreadCount() > 0U, "Single engine host did not start actual workers");
			logs.HostBoundary("HostInitializeEnd", true);
			if(hostInstanceRequested) {
				logs.HostBoundary("HostInstanceInitializeBegin", true); hostInstance.Initialize();
				logs.HostBoundary("HostInstanceInitializeEnd", hostInstance.Ready());
			}
			// Engine startup creates its renderer too. Retire that unused lifetime
			// before the first protected scene reset, just like later iterations.
			auto& renderer = RenderSystem::Instance();
			std::optional<std::size_t> startupObservation;
			if(hostInstanceRequested) {
				// Engine created this context before the host existed; do not claim
				// its initialization happened under the later fixture-owned instance.
				startupObservation = contextObservations.Initialized(renderer, "Startup", 0U, false);
				logs.HostBoundary("StartupRendererShutdownBegin", true);
				contextObservations.Shutdown(renderer, startupObservation);
				logs.HostBoundary("StartupRendererShutdownEnd", renderer.GetContext() == nullptr && hostInstance.Ready());
			} else renderer.Shutdown();
			renderer.SetFrameScratchValidationCallbacks(nullptr);
			SceneManager::Instance().UnloadAll(); ObjectManager::instance().clear();
			for(std::uint32_t iteration = 1U; iteration <= iterations; ++iteration) {
				logs.BeginIteration(iteration); Json observation;
				try { observation = RunRendererIteration(engine, projectRoot, scenario, iteration,
					hostInstanceRequested ? &hostInstance : nullptr, hostInstanceRequested ? &contextObservations : nullptr); }
				catch(const std::exception& error) { observation = {{"status", "Failed"}, {"reason", error.what()}}; }
				observation["iteration"] = iteration; receipt["attemptedIterations"] = iteration;
				if(observation["status"] == "Passed") {
					const auto lifetime = observation.at("captured").at("lifetimeId").get<std::uint64_t>();
					if(lifetime == 0U || lifetime <= previousLifetime || !capturedLifetimes.insert(lifetime).second) {
						observation["status"] = "Failed"; observation["reason"] = "Renderer repetitions reused or reversed a captured scratch lifetime";
					} else { previousLifetime = lifetime; receipt["capturedLifetimeIds"].push_back(lifetime); }
				}
				logs.EndIteration(observation);
				if(observation["allSessionValidationErrors"].get<std::size_t>() != 0U) {
					observation["status"] = "Failed"; observation["reason"] = "Actual iteration Vulkan validation errors occurred";
				}
				const auto status = observation.at("status").get<std::string>();
				if(status == "Passed") receipt["completedIterations"] = receipt["completedIterations"].get<std::uint32_t>()+1U;
				else receipt["status"] = status;
				receipt["observations"].push_back(std::move(observation));
				if(status != "Passed") break;
			}
		} catch(const std::exception& error) {
			receipt["status"] = receipt["attemptedIterations"] == 0U ? "Unverified" : "Failed"; receipt["reason"] = error.what();
		}
		bool finalRendererShutdownAttempted = false, finalContextReleased = true, windowLiveBeforeHostClose = true, bootstrapClosed = true;
		if(hostInstanceRequested) {
			finalRendererShutdownAttempted = true;
			try { RenderSystem::Instance().Shutdown(); } catch(const std::exception& error) { receipt["status"] = "Failed"; receipt["reason"] = error.what(); }
			finalContextReleased = RenderSystem::Instance().GetContext() == nullptr;
			windowLiveBeforeHostClose = WindowSystem::Instance().GetNativeHandle() != nullptr;
			// A retained Engine context still owns its own instance; the separate
			// host has no borrowed Engine children. Its independent close is legal
			// on failure too, but missing context retirement cannot qualify a pass.
			bootstrapClosed = hostInstance.Close();
			receipt["hostInstance"] = hostInstance.Snapshot();
			receipt["rendererContexts"] = contextObservations.Snapshot(finalRendererShutdownAttempted, finalContextReleased, windowLiveBeforeHostClose);
		}
		logs.HostBoundary("HostShutdownBegin", true);
		host["engineShutdownCount"] = 1U; engine.Shutdown(); cleanup.closed = true;
		host["threadPoolWorkersAfterCleanup"] = ThreadPool::instance().getThreadCount();
		if(const auto& failure = engine.GetFrameFailure()) host["frameFailure"] = {{"frame", failure->frame}, {"workload", failure->workload}, {"phase", failure->phase}, {"reason", failure->reason}};
		const bool hostClean = ThreadPool::instance().getThreadCount() == 0U && RenderSystem::Instance().GetContext() == nullptr && !engine.GetFrameFailure();
		host["engineCleanupCompleted"] = hostClean; receipt["hostSession"] = std::move(host);
		// Logger sinks run synchronously when producers log. Engine.Shutdown
		// retires those producers and joins console logging before this snapshot;
		// queued console messages do not create newly admitted sink callbacks.
		logs.HostBoundary("HostShutdownEnd", hostClean); logs.CopyAll(receipt);
		if(!hostClean || !finalContextReleased || !windowLiveBeforeHostClose || !bootstrapClosed || receipt["allSessionValidationErrors"].get<std::size_t>() != 0U) {
			receipt["status"] = "Failed"; receipt["reason"] = "Single engine host cleanup or initialization-through-shutdown validation failed";
		}
		receipt["nativeAcceptance"] = receipt["status"] == "Passed" && receipt["completedIterations"] == iterations && capturedLifetimes.size() == iterations;
		std::ofstream output(outputPath, std::ios::binary); output << receipt.dump(2) << '\n'; output.close(); Check(output.good(), "Cannot save renderer receipt");
		return receipt["status"] == "Passed" ? 0 : receipt["status"] == "Failed" ? 1 : 2;
	}
}
