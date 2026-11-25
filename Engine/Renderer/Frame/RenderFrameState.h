// Stable renderer lifetime and coroutine ownership shared by compiled stages.
#pragma once
#include "RenderFrameCaches.h"
#include "RenderFrameAlgorithms.h"
#include "Renderer/Graph/RenderFrameScratch.h"
#include "Renderer/RHI/Vulkan/VulkanRendererWorkCache.h"
#include "Renderer/GraphicsSettings/GraphicsSettingsController.h"
#include "Renderer/GraphicsSettings/GraphicsSettingsCatalog.h"
#include "Renderer/GraphicsSettings/GraphicsStartupStore.h"
#include "Renderer/GraphicsSettings/PostProcessAuthoringController.h"
#include "Renderer/GraphicsSettings/PostProcessAuthoringFrame.h"
#include <coroutine>
#include <atomic>
#include <stdexcept>
#include <exception>
#include <mutex>
#include <utility>
#include <future>
#include <filesystem>
#include <map>

namespace TerraEngine::Renderer::GraphicsSettings { class GraphicsSettingsPersistence; }

// A settings worker owns copied catalog and target data. This ingress lock
// orders preparation against context replacement, so an old job cannot consume
// requests admitted under a newer project, camera, or renderer lifetime.
struct RenderGraphicsSettingsState
{
	using Preset = TerraEngine::Renderer::GraphicsSettings::GraphicsPresetDefinition;
	using Descriptor = TerraEngine::Renderer::GraphicsSettings::GraphicsSettingDescriptor;
	using Target = TerraEngine::Renderer::GraphicsSettings::GraphicsTarget;
	using Lifecycle = TerraEngine::Renderer::GraphicsSettings::GraphicsLifecycleStamp;
	using Candidate = TerraEngine::Renderer::GraphicsSettings::GraphicsPreparedCandidate;
	struct ResourceCompletion
	{
		TerraEngine::Renderer::GraphicsSettings::GraphicsRequestId requestId = 0;
		Lifecycle lifecycle{};
		bool succeeded = false;
		bool postProcessPrepared = false;
		TerraEngine::Renderer::GraphicsSettings::GraphicsRequestReason failureReason =
			TerraEngine::Renderer::GraphicsSettings::GraphicsRequestReason::PreparationFailed;
	};
	std::mutex mutex;
	TerraEngine::Renderer::GraphicsSettings::GraphicsSettingsController controller;
	TerraEngine::Renderer::GraphicsSettings::PostProcessAuthoringController postProcessAuthoring;
	struct PendingAuthoringSceneWrite
	{
		TerraEngine::Renderer::GraphicsSettings::PostProcessSceneProfileIntent intent;
		std::string transactionId;
	};
	std::vector<PendingAuthoringSceneWrite> pendingAuthoringSceneWrites;
	std::vector<Target> postProcessAuthoringTargets;
	std::vector<std::shared_ptr<const TerraEngine::Renderer::GraphicsSettings::PostProcessAuthoringFrame>> preparedAuthoringViews;
	struct AcceptedAuthoringView
	{
		std::shared_ptr<const TerraEngine::Renderer::GraphicsSettings::PostProcessAuthoringFrame> source;
		std::shared_ptr<const TerraEngine::Renderer::GraphicsSettings::GraphicsSettingsSnapshot> settings;
	};
	// Keep prepared settings private until a matching native view records them.
	std::vector<AcceptedAuthoringView> acceptedAuthoringViews;
	std::shared_ptr<const std::vector<Descriptor>> catalog = std::make_shared<const std::vector<Descriptor>>();
	std::shared_ptr<const std::vector<Preset>> presets = std::make_shared<const std::vector<Preset>>();
	std::vector<Target> targets;
	std::vector<Target> requestedTargets;
	std::vector<std::pair<std::uint32_t, Target>> boundTargets;
	std::vector<TerraEngine::Renderer::PublishedGraphicsCameraView> publishedCameraViews;
	std::uint64_t nextCameraViewLifetime = 1;
	glm::uvec2 publishedGameExtent{};
	std::optional<std::uint32_t> publishedGameSpatialAAModeMask;
	Lifecycle lifecycle{1, 0, 0, 0};
	std::filesystem::path projectRoot;
	std::uint64_t epoch = 1;
	std::uint64_t requestedPlaySessionGeneration = 0;
	bool configured = false;
	// Automatic runtime registration is separate from the native fixture catalog.
	bool nativeCatalog = false;
	bool startupFinalized = false;
	bool taskCullingBeforePlay = true;
	std::optional<RenderSettings> authoredDefaults;
	std::optional<RenderSettings> settingsBeforePlay;
	QualityPreset presetBeforePlay = QualityPreset::Custom;
	TerraEngine::Renderer::GraphicsSettings::GraphicsProfileMetadata profile;
	TerraEngine::Renderer::GraphicsSettings::GraphicsSceneSkyValues sceneSky;
	std::uint64_t observedNativeGeneration = 0;
	struct Startup
	{
		std::filesystem::path dataRoot;
		std::string identity;
		std::unique_ptr<TerraEngine::Renderer::GraphicsSettings::GraphicsStartupStore> store;
		TerraEngine::Renderer::GraphicsSettings::GraphicsStartupStoreSnapshot state;
		RenderSettings priorSettings;
		bool priorTaskCulling = true;
		// Only successfully read/settled snapshots can seed durable receipt state.
		bool snapshotValid = false;
		bool recoveryAttempted = false;
		bool authoredTaskCulling = true;
		// The replay lease survives deferred zero-extent initialization. Failure
		// overlays are launch-local and never masquerade as disk settlements.
		std::unique_ptr<TerraEngine::Renderer::GraphicsSettings::GraphicsStartupReplayLease> replayLease;
		std::optional<TerraEngine::Renderer::GraphicsSettings::GraphicsStartupStoreResult> launchFailure;
		std::optional<TerraEngine::Renderer::GraphicsSettings::GraphicsRequestReceipt> launchFailureReceipt;
		bool candidate = false;
		std::vector<TerraEngine::Renderer::GraphicsSettings::GraphicsSettingValue> candidateProfile;
		std::vector<TerraEngine::Renderer::GraphicsSettings::GraphicsSettingValue> priorColorValues;
		std::vector<TerraEngine::Renderer::GraphicsSettings::GraphicsSettingValue> candidateColorValues;
		std::uint64_t colorPreparedGeneration = 0;
	};
	std::optional<Startup> startup;
	std::shared_ptr<TerraEngine::Renderer::GraphicsSettings::GraphicsSettingsPersistence> persistence;
	std::filesystem::path editorPreferenceDataRoot;
	std::vector<std::pair<std::uint64_t, std::uint64_t>> pendingPreferenceRestores;
	// Only a settled named player restore can add IDs to this precedence layer.
	std::vector<TerraEngine::Renderer::GraphicsSettings::GraphicsSettingId> restoredPlayerSettingIds;
	TerraEngine::Renderer::GraphicsSettings::GraphicsRequestReason lastPreparationFailure = TerraEngine::Renderer::GraphicsSettings::GraphicsRequestReason::None;
	std::future<std::optional<Candidate>> preparation;
	std::optional<Candidate> pendingRebuild;
	std::vector<std::pair<TerraEngine::UUID, std::shared_future<TerraEngine::UUID>>> pendingPostProcessLookups;
	std::vector<ResourceCompletion> resourceCompletions;
	// Scene View is an editor-owned transaction domain. Its private Game root
	// reuses typed validation without introducing a public managed Scene scope.
	struct SceneGraphics
	{
		TerraEngine::Renderer::GraphicsSettings::GraphicsSettingsController controller;
		std::shared_ptr<const std::vector<Descriptor>> catalog = std::make_shared<const std::vector<Descriptor>>();
		std::shared_ptr<const std::vector<Descriptor>> sourceCatalog;
		std::uint32_t catalogModeMask = 0;
		Lifecycle lifecycle{};
		TerraEngine::Renderer::GraphicsSettings::GraphicsViewIdentity view{};
		glm::uvec2 extent{};
		std::uint32_t spatialModeMask = 1u;
		std::uint64_t epoch = 1, nextViewLifetime = 1, nextMergedRevision = 1;
		bool registered = false;
		std::future<std::optional<Candidate>> preparation;
		std::optional<Candidate> pendingRebuild;
		std::shared_ptr<const TerraEngine::Renderer::GraphicsSettings::GraphicsSettingsSnapshot> preparationGame;
		std::vector<ResourceCompletion> completions;
		std::vector<TerraEngine::Renderer::GraphicsSettings::GraphicsSettingId> colorOverrides;
		std::vector<std::pair<std::uint64_t, std::vector<TerraEngine::Renderer::GraphicsSettings::GraphicsSettingId>>> requestFields;
		// A small value-keyed cache gives preflight and the later matching render
		// the same revision. Revisions never combine unrelated counters with XOR.
		struct Projection
		{
			std::uint64_t gameRevision = 0, sceneRevision = 0;
			std::shared_ptr<const TerraEngine::Renderer::GraphicsSettings::GraphicsSettingsSnapshot> snapshot;
		};
		std::vector<Projection> projections;
		bool HasPreparation() const noexcept { return preparation.valid() || pendingRebuild.has_value(); }
	} scene;
};

namespace TerraEngine::Renderer::Frame {
	// Every resume, synchronous capture and lifecycle retirement admits through
	// the same stable RenderSystem-owned atomic gate before touching borrowers.
	class RenderExecutionBoundary final {
	public:
		explicit RenderExecutionBoundary(std::atomic_flag& flag) : flag_(flag) {
			if(flag_.test_and_set(std::memory_order_acquire))
				throw std::logic_error("Render frame execution is already active");
		}
		~RenderExecutionBoundary() { flag_.clear(std::memory_order_release); }
		RenderExecutionBoundary(const RenderExecutionBoundary&) = delete;
		RenderExecutionBoundary& operator=(const RenderExecutionBoundary&) = delete;
	private:
		std::atomic_flag& flag_;
	};
}

using namespace TerraEngine::Renderer::Frame;

struct RenderFrameExecution
{
	struct promise_type
	{
		std::exception_ptr failure;

		RenderFrameExecution get_return_object()
		{
			return RenderFrameExecution{
				std::coroutine_handle<promise_type>::from_promise(*this)};
		}
		std::suspend_always initial_suspend() const noexcept { return {}; }
		std::suspend_always final_suspend() const noexcept { return {}; }
		void return_void() const noexcept {}
		void unhandled_exception() noexcept { failure = std::current_exception(); }
	};

	using Handle = std::coroutine_handle<promise_type>;
	Handle handle{};

	RenderFrameExecution() = default;
	explicit RenderFrameExecution(Handle value) : handle(value) {}
	RenderFrameExecution(const RenderFrameExecution&) = delete;
	RenderFrameExecution& operator=(const RenderFrameExecution&) = delete;
	RenderFrameExecution(RenderFrameExecution&& other) noexcept
		: handle(std::exchange(other.handle, {})) {}
	RenderFrameExecution& operator=(RenderFrameExecution&& other) noexcept
	{
		if(this == &other) return *this;
		if(handle) handle.destroy();
		handle = std::exchange(other.handle, {});
		return *this;
	}
	~RenderFrameExecution()
	{
		if(handle) handle.destroy();
	}

	// Returns true when another scheduler stage remains. Exceptions are rethrown
	// at the owning callback so the scheduler reports the correct failed phase.
	bool Resume()
	{
		if(!handle || handle.done()) return false;
		handle.resume();
		if(handle.promise().failure) std::rethrow_exception(handle.promise().failure);
		return !handle.done();
	}
};

// One implementation-owned aggregate replaces every former function-static
// cache. Project switches and renderer shutdown can now release all retained
// roots and scratch capacity together without leaking state across sessions.
struct RenderSystemLifetimeState
{
	struct FramePhaseState
	{
		float deltaTime = 0.0F;
		std::uint64_t captureSequence = 0U;
		std::unique_ptr<RenderFrameExecution> execution;
		bool captureReady = false;
		bool routeReady = false;
		bool virtualGeometryReady = false;
		bool backendComplete = false;
		bool publicationComplete = false;
	};
	// Scheduler callbacks may cross worker and protected executors. This mutex
	// guards the small immutable-root handoff only; no live ECS or GPU work runs
	// while it is held.
	std::mutex framePhaseMutex;
	// Declared before the continuation: reverse destruction retires its vectors
	// and strong lease before this lifetime can release the stable scratch owner.
	std::shared_ptr<TerraEngine::Renderer::RenderFrameScratch> frameScratch;
	FramePhaseState framePhase;
	// Protected renderer-owned state. Workers only own native resource requests;
	// they cannot change which scene or viewport the renderer reveals.
	struct InitialSceneState
	{
		static constexpr std::uint8_t SceneView = 1u;
		static constexpr std::uint8_t GameView = 2u;
		std::uint64_t sceneGeneration = 0u;
		TerraEngine::UUID sceneId = TerraEngine::EMPTY_UUID;
		bool warming = false;
		std::map<std::pair<AssetType, TerraEngine::UUID>, std::shared_future<TerraEngine::UUID>> requests;
	} initialScene;
	RetainedRenderTopologyState retainedRenderTopology;
	GroupedSubmissionMembershipCacheState groupedSubmissionMembershipCache;
	TerraEngine::Renderer::Vulkan::VulkanRendererPreparationCache rendererPreparationCache;
	VisibleCommandCacheState visibleCommandCache;
	ShadowDrawListCacheState shadowDrawListCache;
	SpatialPreparationCacheState spatialPreparationCache;
	RenderableDrawWorkingSet renderableDrawWorkingSet;
	DrawCommandCacheState drawCommandCache;
	RenderableFrameIdentityCacheState renderableFrameIdentityCache;
	LightCandidateCacheState lightCandidateCache;
	// View commands are fully lowered into immutable selection and presentation
	// records before the next view starts. Retaining this temporary vector avoids
	// allocating crowd-sized command storage again for every view and frame.
	std::vector<RenderItemRecord> viewCommandScratch;
};
// Clear retires the continuation before replacing the caches declared above.
