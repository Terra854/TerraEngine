// RenderSystem settings facade, preserving existing protected ownership and publication boundaries.
#include "Frame/RenderFrameState.h"
#include "Frame/RenderVirtualGeometrySelection.h"
#include "Renderer/RHI/Vulkan/VulkanContext.h"
#include "Renderer/GraphicsSettings/GraphicsSettingsPersistence.h"
#include "Resource/ResourceManager.h"
#include "Scene/SceneManager.h"
#include "Core/Logger/Logger.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureControls.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureCameras.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureDepthControls.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureShadowBatchControls.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include "Tests/Support/ReviewRemediation/RenderSettingsFailureHooks.h"
#include <mutex>
#include <nlohmann/json.hpp>
#include "ThreadPool/ThreadPool.h"
#include <chrono>

// Existing sanitizer helper below is retained; do not add a second definition.
// Render settings normalization shared by protected capture and settings facades.
#include "Frame/RenderFrameState.h"

namespace TerraEngine::Tests::Review {
	// Owner-thread-only validation state. It never crosses a worker lifetime or
	// changes a scheduler declaration, publication boundary, or ordinary IO path.
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
	static thread_local RenderSettingsFailureHook* settingsFailureHook = nullptr;
#endif
	bool SetRenderSettingsFailureHook(RenderSettingsFailureHook* hook) noexcept
	{
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
		if (hook != nullptr && settingsFailureHook != nullptr) return false;
		settingsFailureHook = hook; return true;
#else
		return hook == nullptr;
#endif
	}
	static void SettingsFilesystemCheckpoint(SettingsFilesystemOperation operation, const std::filesystem::path& path)
	{
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
		if (settingsFailureHook != nullptr && settingsFailureHook->operation == operation && settingsFailureHook->remaining != 0)
		{
			--settingsFailureHook->remaining; ++settingsFailureHook->reached;
			throw std::filesystem::filesystem_error("Controlled render-settings filesystem failure", path, std::make_error_code(std::errc::io_error));
		}
#else
		(void)operation; (void)path;
#endif
	}
	static bool SettingsPathExists(const std::filesystem::path& path)
	{
		SettingsFilesystemCheckpoint(SettingsFilesystemOperation::Exists, path);
		return std::filesystem::exists(path);
	}
	static void SettingsCreateDirectories(const std::filesystem::path& path)
	{
		SettingsFilesystemCheckpoint(SettingsFilesystemOperation::CreateDirectories, path);
		std::filesystem::create_directories(path);
	}
}

namespace TerraEngine::Renderer::Frame {
	RenderSettings SanitizeRenderSettings(const RenderSettings& input)
	{
		RenderSettings sanitized = input;
		sanitized.maxUploadedLights = std::max(sanitized.maxUploadedLights, 1u);
		sanitized.maxDynamicLights = std::min(sanitized.maxDynamicLights, sanitized.maxUploadedLights);
		sanitized.maxAreaLights = std::min(sanitized.maxAreaLights, sanitized.maxDynamicLights);
		sanitized.maxShadowLights = std::min(sanitized.maxShadowLights, sanitized.maxDynamicLights);
		// Shadow maps are allocated lazily by the Vulkan backend, so authoring
		// budgets may exceed four lights without paying for unused slots.
		sanitized.maxShadowLights = std::min(sanitized.maxShadowLights, kMaxShadowResourceSlots);
		sanitized.indirectStrength = std::clamp(sanitized.indirectStrength, 0.0f, 8.0f);
		sanitized.ambientOcclusionStrength = std::clamp(sanitized.ambientOcclusionStrength, 0.0f, 4.0f);
		sanitized.ambientOcclusionQuality = std::min(sanitized.ambientOcclusionQuality, 3u);
		sanitized.enableIBL = sanitized.enableIBL > 0u ? 1u : 0u;
		sanitized.variableRateShading = sanitized.variableRateShading > 0u ? 1u : 0u;
		sanitized.validationEnabled = sanitized.validationEnabled > 0u ? 1u : 0u;
		sanitized.debugEnabled = sanitized.debugEnabled > 0u ? 1u : 0u;
		sanitized.virtualGeometry = SanitizeVirtualGeometryQualityProfile(sanitized.virtualGeometry);
		return sanitized;
	}
}

namespace
{
std::filesystem::path ResolveRuntimeSettingsDirectory()
	{
		auto& resourceManager = ResourceManager::Instance();
		// Exported builds keep writable settings beside the packaged runtime,
		// while editor/project sessions must write next to Assets so authored
		// ProjectSettings stay in <project>/ProjectSettings.
		if (resourceManager.IsExportRuntime())
		{
			return resourceManager.GetAssetRoot() / "Settings";
		}

		return resourceManager.GetProjectRoot() / "ProjectSettings";
	}

std::filesystem::path ResolveLegacyAssetSettingsPath()
	{
		auto& resourceManager = ResourceManager::Instance();

		if (resourceManager.IsExportRuntime())
		{
			return {};
		}

		// Older builds incorrectly wrote render settings inside Assets. Loading
		// that file as a fallback preserves a user's settings once, and the next
		// save writes them back to the corrected project-level directory.
		return resourceManager.GetAssetRoot() / "ProjectSettings" / "RenderSettings.json";
	}
}

void RenderSystem::SetRenderGraphDiagnosticsRequested(bool requested)
{
	renderGraphDiagnosticsRequestedByUi = requested;
	ApplyRenderDiagnosticsRequest();
}

bool RenderSystem::ResolveRenderDebugDiagnosticsEnabled() const
{
	// Editor Preferences are per-user state, so they override project-authored
	// debug diagnostics while the editor is running without changing the saved
	// RenderSettings.json value.
	return transientRenderDebugDiagnosticsOverride.value_or(currentSettings.debugEnabled > 0u);
}

void RenderSystem::ApplyRenderDiagnosticsRequest()
{
	// Diagnostics can come from a user preference override, project render
	// settings, or a temporary diagnostics window. Resolve the saved/project
	// side first, then OR in windows that only need capture data while open.
	const bool requested = renderGraphDiagnosticsRequestedByUi || ResolveRenderDebugDiagnosticsEnabled();
	TerraEngine::Renderer::SetRenderDiagnosticsRequested(requested);
	if (!requested)
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestRenderGraphDiagnostics = {};
	}
}

SkyRuntimeState RenderSystem::GetLatestSkyRuntimeState() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestSkyRuntimeState;
}

void RenderSystem::MarkSkyStateDirty()
{
	skyStateDirty = true;
	lastLoggedSkyIssueKey.clear();
}

void RenderSystem::SetQualityPreset(QualityPreset preset)
{
	ApplyQualityPreset(preset, true);
}

void RenderSystem::ApplyQualityPreset(QualityPreset preset, bool persistProjectSettings)
{
	auto settings = currentSettings;
	if(!TerraEngine::Renderer::GraphicsSettings::ApplyLegacyQualityPreset(settings, preset)) return;
	SetTransientNativeGraphicsSettings(settings, preset, taskShaderCullingEnabled);
	if(persistProjectSettings && preset != QualityPreset::Custom) SaveSettings();
}

void RenderSystem::SetTransientNativeGraphicsSettings(const RenderSettings& settings, QualityPreset preset, bool taskCulling)
{
	std::uint32_t priorTerrainAnisotropy = 0;
	{
		// This path is shared by authored, legacy, and typed protected publication.
		// Only the authored entry point below can write project settings.
		std::lock_guard lock(runtimeRenderSettingsMutex);
		priorTerrainAnisotropy = currentSettings.anisotropicFiltering;
		currentSettings = SanitizeRenderSettings(settings);
		currentPreset = preset;
		taskShaderCullingEnabled = taskCulling;
		++runtimeRenderSettingsGeneration;
	}
	if(priorTerrainAnisotropy != currentSettings.anisotropicFiltering)
	{
		for(const auto& materialId : TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeRegistry::Instance()
			.RepublishSamplerQuality(currentSettings.anisotropicFiltering))
			InvalidateGeneratedMaterial(materialId);
	}
	ResourceManager::Instance().SetRenderDebugEnabled(ResolveRenderDebugDiagnosticsEnabled());
	ApplyRenderDiagnosticsRequest();
}

void RenderSystem::SetRenderSettings(const RenderSettings& settings)
{
	SetTransientNativeGraphicsSettings(settings, QualityPreset::Custom, taskShaderCullingEnabled);
	if(auto* vulkan = dynamic_cast<VulkanContext*>(rhi.get()))
		vulkan->SetVariableRateShadingEnabled(currentSettings.variableRateShading != 0);
	SaveSettings();
}

bool RenderSystem::QueueRuntimeQualityPreset(QualityPreset preset)
{
	if (preset > QualityPreset::Ultra)
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
	pendingRuntimeRenderSettings = RuntimeRenderSettingsRequest{ {}, preset, true };
	return true;
}

bool RenderSystem::QueueRuntimeRenderSettings(const RenderSettings& settings)
{
	std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
	pendingRuntimeRenderSettings = RuntimeRenderSettingsRequest{
		SanitizeRenderSettings(settings), QualityPreset::Custom, false };
	return true;
}

RenderSettings RenderSystem::GetRuntimeRenderSettingsSnapshot() const
{
	std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
	return currentSettings;
}

QualityPreset RenderSystem::GetRuntimeQualityPresetSnapshot() const
{
	std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
	return currentPreset;
}

std::uint64_t RenderSystem::GetRuntimeRenderSettingsGeneration() const
{
	std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
	return runtimeRenderSettingsGeneration;
}

void RenderSystem::ConfigureGraphicsSettingsCatalog(
	std::vector<TerraEngine::Renderer::GraphicsSettings::GraphicsSettingDescriptor> descriptors)
{
	auto catalog = std::make_shared<const std::vector<TerraEngine::Renderer::GraphicsSettings::GraphicsSettingDescriptor>>(std::move(descriptors));
	std::lock_guard lock(graphicsSettingsState->mutex);
	// Catalog replacement is a native registration operation. An in-flight
	// candidate must finish before its descriptor contract can be replaced.
	if(graphicsSettingsState->controller.HasPendingWork())
		throw std::logic_error("Graphics settings catalog has pending requests.");
	graphicsSettingsState->catalog = std::move(catalog);
	graphicsSettingsState->configured = true;
	++graphicsSettingsState->epoch;
}

void RenderSystem::ConfigureGraphicsSettingsViewTargets(
	std::vector<TerraEngine::Renderer::GraphicsSettings::GraphicsTarget> targets)
{
	std::lock_guard lock(graphicsSettingsState->mutex);
	graphicsSettingsState->requestedTargets = std::move(targets);
}

void RenderSystem::SetGraphicsSettingsPlaySessionGeneration(std::uint64_t generation)
{
	std::lock_guard lock(graphicsSettingsState->mutex);
	if(generation != 0 && graphicsSettingsState->requestedPlaySessionGeneration == 0)
	{
		std::lock_guard nativeLock(runtimeRenderSettingsMutex);
		graphicsSettingsState->settingsBeforePlay = currentSettings;
		graphicsSettingsState->presetBeforePlay = currentPreset;
		graphicsSettingsState->taskCullingBeforePlay = taskShaderCullingEnabled;
	}
	graphicsSettingsState->requestedPlaySessionGeneration = generation;
}

std::vector<TerraEngine::Renderer::GraphicsSettings::GraphicsSettingDescriptor> RenderSystem::GetGraphicsSettingsCatalogSnapshot(
	const TerraEngine::Renderer::GraphicsSettings::GraphicsTarget& target) const
{
	using namespace TerraEngine::Renderer::GraphicsSettings;
	std::lock_guard lock(graphicsSettingsState->mutex);
	const auto& owner = *graphicsSettingsState;
	auto descriptors = *owner.catalog;
	auto modes = owner.publishedGameSpatialAAModeMask;
	if(target.scope == GraphicsScope::Camera)
	{
		modes.reset();
		bool exactViewAvailable = !target.view;
		// A camera write applies to all admitted outputs of the same actor.
		// A selected view must exist, but cannot relax that shared capability.
		for(const auto& view : owner.publishedCameraViews)
		{
			if(view.target.camera != target.camera) continue;
			exactViewAvailable |= view.target.view == target.view;
			modes = modes ? (*modes & view.spatialAAModeMask) : view.spatialAAModeMask;
		}
		if(!modes || !exactViewAvailable) modes = 1u;
	}
	if(modes)
		for(auto& descriptor : descriptors)
			if(descriptor.id == GraphicsSettingId::AntiAliasingMode)
				std::erase_if(descriptor.options, [&](const auto& option) {
					const auto value = std::get_if<std::uint32_t>(&option);
					return !value || *value >= 32u || (*modes & (1u << *value)) == 0u;
				});
	return descriptors;
}

std::optional<TerraEngine::Renderer::GraphicsSettings::GraphicsTarget> RenderSystem::ResolveGraphicsSettingsCameraTarget(
	std::uint32_t entity) const
{
	// Managed callbacks can run on workers. Only use the last protected native
	// registration, which includes the full object and component lifetime.
	std::lock_guard lock(graphicsSettingsState->mutex);
	const auto& targets = graphicsSettingsState->boundTargets;
	const auto found = std::find_if(targets.begin(), targets.end(), [entity](const auto& row) { return row.first == entity; });
	return found == targets.end() ? std::nullopt : std::optional(found->second);
}

std::shared_ptr<const TerraEngine::Renderer::GraphicsSettings::GraphicsSettingsSnapshot>
RenderSystem::GetRequestedGraphicsSettingsSnapshot(const TerraEngine::Renderer::GraphicsSettings::GraphicsTarget& target) const
{
	using namespace TerraEngine::Renderer::GraphicsSettings;
	std::lock_guard lock(graphicsSettingsState->mutex);
	const auto& pending = graphicsSettingsState->pendingRebuild;
	if(!pending || pending->snapshot.target.scope != target.scope || pending->snapshot.target.camera != target.camera)
	{
		if(target != GraphicsTarget{} || !graphicsSettingsState->startup || !graphicsSettingsState->persistence) return {};
		const auto startup = graphicsSettingsState->persistence->StartupSnapshot();
		if(!startup || !startup->pending) return {};
		auto proposed = std::make_shared<GraphicsSettingsSnapshot>(*graphicsSettingsState->controller.Snapshot());
		proposed->publishedFrameId = 0;
		for(const auto& patch : startup->pending->transaction.request.patches)
		{
			const auto value = std::find_if(proposed->values.begin(), proposed->values.end(), [&](const auto& entry) { return entry.id == patch.id; });
			if(value == proposed->values.end()) proposed->values.push_back(patch);
			else *value = patch;
		}
		return proposed;
	}
	auto snapshot = std::make_shared<TerraEngine::Renderer::GraphicsSettings::GraphicsSettingsSnapshot>(pending->snapshot);
	if(target.view)
	{
		if(std::find(graphicsSettingsState->targets.begin(), graphicsSettingsState->targets.end(), target)
			== graphicsSettingsState->targets.end()) return {};
		snapshot->target = target;
	}
	return snapshot;
}

TerraEngine::Renderer::GraphicsSettings::GraphicsLifecycleStamp RenderSystem::GetGraphicsSettingsLifecycleSnapshot() const
{
	std::lock_guard lock(graphicsSettingsState->mutex);
	auto lifecycle = graphicsSettingsState->lifecycle;
	lifecycle.playSessionGeneration = graphicsSettingsState->requestedPlaySessionGeneration;
	return lifecycle;
}

TerraEngine::Renderer::GraphicsSettings::GraphicsRequestReceipt RenderSystem::QueueGraphicsSettingsRequest(
	TerraEngine::Renderer::GraphicsSettings::GraphicsSettingsRequest request)
{
	using namespace TerraEngine::Renderer::GraphicsSettings;
	auto& state = *graphicsSettingsState;
	std::lock_guard lock(state.mutex);
	auto admissionLifecycle = state.lifecycle;
	admissionLifecycle.playSessionGeneration = state.requestedPlaySessionGeneration;
	if(request.lifecycle != admissionLifecycle)
		return state.controller.Reject(std::move(request), GraphicsRequestReason::StaleLifetime);
	if(request.operation == GraphicsRequestOperation::StageNextLaunch)
	{
		if(!state.persistence || !state.startup)
			return state.controller.Reject(std::move(request), GraphicsRequestReason::InvalidScope);
		GraphicsStartupTransaction transaction;
		transaction.request = std::move(request);
		// Start from the committed launch profile. A transient disjoint write
		// in the running game is not an input to another process's startup.
		const auto baseline = CaptureNativeGraphicsSettings(state.startup->priorSettings,
			state.startup->priorTaskCulling, state.profile, state.sceneSky);
		for(const auto& patch : transaction.request.patches)
		{
			const auto& values = patch.id >= GraphicsSettingId::AntiAliasingMode ? state.startup->priorColorValues : baseline;
			const auto value = std::find_if(values.begin(), values.end(), [&](const auto& entry) { return entry.id == patch.id; });
			if(value != values.end()) transaction.fieldPreconditions.push_back(*value);
			else if(const auto* descriptor = FindGraphicsSetting(*state.catalog, patch.id))
				transaction.fieldPreconditions.push_back({patch.id, descriptor->defaultValue});
		}
		return state.persistence->Stage(std::move(transaction), *state.catalog);
	}
	if(request.target.scope == GraphicsScope::Game && state.persistence && state.startup)
	{
		auto ids = request.resetSettings;
		for(const auto& value : request.patches) ids.push_back(value.id);
		if(request.preset)
			for(const auto& preset : *state.presets)
				if(preset.preset == request.preset)
					for(const auto& value : preset.values) ids.push_back(value.id);
		if(state.persistence->Conflicts(ids))
			return state.controller.Reject(std::move(request), GraphicsRequestReason::ConflictingRequest);
	}
	return state.controller.Enqueue(std::move(request));
}

TerraEngine::Renderer::GraphicsSettings::GraphicsRequestReceipt RenderSystem::QueueGraphicsPreferencesSave(
	std::string name, TerraEngine::Renderer::GraphicsSettings::GraphicsTarget target, std::uint64_t expectedStoreRevision)
{
	using namespace TerraEngine::Renderer::GraphicsSettings;
	auto& state = *graphicsSettingsState;
	std::lock_guard lock(state.mutex);
	const auto effective = state.controller.Snapshot(target);
	if(!state.persistence || !effective || target.scope != GraphicsScope::Game)
		return state.controller.Reject({}, GraphicsRequestReason::InvalidScope);
	std::vector<GraphicsSettingValue> values;
	for(const auto& value : effective->values)
	{
		const auto* descriptor = FindGraphicsSetting(*state.catalog, value.id);
		// Ordinary saved preferences contain player-adjustable settings. Current
		// startup flags and authored metadata stay with their original owners.
		if(descriptor && descriptor->gameScope && descriptor->available
			&& (descriptor->activation == GraphicsActivation::Live || descriptor->activation == GraphicsActivation::Rebuild))
			values.push_back(value);
	}
	return state.persistence->Save(std::move(name), std::move(values), expectedStoreRevision);
}

TerraEngine::Renderer::GraphicsSettings::GraphicsRequestReceipt RenderSystem::QueueGraphicsPreferencesRestore(
	std::string name, TerraEngine::Renderer::GraphicsSettings::GraphicsTarget target)
{
	using namespace TerraEngine::Renderer::GraphicsSettings;
	auto& state = *graphicsSettingsState;
	std::lock_guard lock(state.mutex);
	if(!state.persistence || target.scope != GraphicsScope::Game || target.camera || target.view)
		return state.controller.Reject({}, GraphicsRequestReason::InvalidScope);
	GraphicsSettingsRequest request;
	request.target = std::move(target);
	request.lifecycle = state.lifecycle;
	return state.persistence->Restore(std::move(name), std::move(request));
}

TerraEngine::Renderer::GraphicsSettings::GraphicsRequestReceipt RenderSystem::CancelStagedGraphicsSettingsRequest(std::uint64_t id)
{
	using namespace TerraEngine::Renderer::GraphicsSettings;
	auto& state = *graphicsSettingsState;
	std::lock_guard lock(state.mutex);
	if(!state.persistence || !state.startup) return state.controller.Reject({}, GraphicsRequestReason::InvalidScope);
	return state.persistence->Cancel(id);
}

std::optional<TerraEngine::Renderer::GraphicsSettings::GraphicsRequestReceipt> RenderSystem::GetGraphicsSettingsReceipt(
	TerraEngine::Renderer::GraphicsSettings::GraphicsRequestId requestId) const
{
	std::lock_guard lock(graphicsSettingsState->mutex);
	if(graphicsSettingsState->persistence)
		if(auto receipt = graphicsSettingsState->persistence->GetReceipt(requestId)) return receipt;
	return graphicsSettingsState->controller.GetReceipt(requestId);
}

std::shared_ptr<const TerraEngine::Renderer::GraphicsSettings::GraphicsSettingsSnapshot> RenderSystem::GetGraphicsSettingsSnapshot(
	const TerraEngine::Renderer::GraphicsSettings::GraphicsTarget& target) const
{
	auto& owner = *graphicsSettingsState;
	std::lock_guard lock(owner.mutex);
	const auto base = owner.controller.Snapshot(target);
	if(target.scope != TerraEngine::Renderer::GraphicsSettings::GraphicsScope::Camera || !base) return base;
	// Resolve any camera-only selector through the ordinary target controller
	// before reading the settings for the exact accepted output lifetime.
	const auto accepted = std::find_if(owner.acceptedAuthoringViews.begin(), owner.acceptedAuthoringViews.end(),
		[&](const auto& view) { return view.source->lifecycle == owner.lifecycle && view.source->target == base->target; });
	return accepted == owner.acceptedAuthoringViews.end() ? base : accepted->settings;
}

void RenderSystem::CompleteGraphicsSettingsResourcePreparation(
	TerraEngine::Renderer::GraphicsSettings::GraphicsRequestId requestId,
	TerraEngine::Renderer::GraphicsSettings::GraphicsLifecycleStamp lifecycle,
	bool succeeded, bool postProcessPrepared,
	TerraEngine::Renderer::GraphicsSettings::GraphicsRequestReason failureReason)
{
	std::lock_guard lock(graphicsSettingsState->mutex);
	graphicsSettingsState->resourceCompletions.push_back({requestId, lifecycle, succeeded, postProcessPrepared, failureReason});
}

std::optional<TerraEngine::Renderer::GraphicsSettings::GraphicsPreparedCandidate> RenderSystem::GetPendingGraphicsSettingsResourcePreparation() const
{
	std::lock_guard lock(graphicsSettingsState->mutex);
	return graphicsSettingsState->pendingRebuild;
}

TerraEngine::Renderer::GraphicsSettings::GraphicsRequestReason RenderSystem::GetGraphicsSettingsPreparationFailure() const
{
	std::lock_guard lock(graphicsSettingsState->mutex);
	return graphicsSettingsState->lastPreparationFailure;
}

void RenderSystem::CaptureGraphicsSettingsAtProtectedBoundary()
{
	using namespace TerraEngine::Renderer::GraphicsSettings;
	const auto state = graphicsSettingsState;
	{
		std::lock_guard lock(state->mutex);
		// Legacy projects register no typed catalog. Their normal disabled route
		// pays only this ownership check, with no path copies or backend queries.
		if(!state->configured && !state->controller.HasPendingWork()) return;
	}
	try
	{
		const auto projectRoot = ResourceManager::Instance().GetProjectRoot();
		const auto rendererSession = rhi ? rhi->GetLatestRendererRecoverySnapshot().renderSessionGeneration : 0;
		const auto backendGeneration = rhi ? rhi->GetBackendResourceGeneration() : 0;
		std::lock_guard lock(state->mutex);
		const auto previousLifecycle = state->lifecycle;
		if(state->projectRoot != projectRoot)
		{
			state->projectRoot = projectRoot;
			++state->lifecycle.projectGeneration;
			state->settingsBeforePlay.reset();
			state->restoredPlayerSettingIds.clear();
			if(state->nativeCatalog && editorModeEnabled)
			{
				// A project opened through the editor hub owns a new authored
				// baseline and preference namespace, even on the same Vulkan device.
				state->authoredDefaults = currentSettings;
				const auto input = ResourceManager::Instance().GetGraphicsStartupInputs();
				state->profile = {input.textureBudgetMegabytes, input.profileResolutionScale};
				GraphicsCatalogCapabilities capabilities;
				if(const auto* native = dynamic_cast<VulkanContext*>(rhi.get()))
					capabilities = native->ReadGraphicsCatalogCapabilities(false);
				capabilities.developmentDiagnostics = true;
				auto descriptors = BuildGraphicsSettingsCatalog(*state->authoredDefaults,
					taskShaderCullingEnabled, state->profile, state->sceneSky, capabilities);
				for(const auto& descriptor : *state->catalog)
					if(descriptor.id >= GraphicsSettingId::AntiAliasingMode && !FindGraphicsSetting(descriptors, descriptor.id))
						descriptors.push_back(descriptor);
				state->catalog = std::make_shared<const std::vector<GraphicsSettingDescriptor>>(std::move(descriptors));
				state->pendingPreferenceRestores.clear();
				state->persistence.reset();
				if(!projectRoot.empty() && !state->editorPreferenceDataRoot.empty())
				{
					state->persistence = std::make_shared<GraphicsSettingsPersistence>(
						state->editorPreferenceDataRoot, "editor:" + projectRoot.generic_string(), false);
				}
			}
		}
		state->lifecycle.playSessionGeneration = state->requestedPlaySessionGeneration;
		state->lifecycle.rendererSessionGeneration = rendererSession;
		state->lifecycle.backendResourceGeneration = backendGeneration;
		if(previousLifecycle.playSessionGeneration != state->lifecycle.playSessionGeneration
			&& state->lifecycle.playSessionGeneration == 0 && state->settingsBeforePlay)
		{
			SetTransientNativeGraphicsSettings(*state->settingsBeforePlay, state->presetBeforePlay, state->taskCullingBeforePlay);
			state->settingsBeforePlay.reset();
			state->restoredPlayerSettingIds.clear();
		}
		if(previousLifecycle.projectGeneration != state->lifecycle.projectGeneration
			|| previousLifecycle.playSessionGeneration != state->lifecycle.playSessionGeneration
			|| previousLifecycle.rendererSessionGeneration != rendererSession)
		{
			state->publishedCameraViews.clear();
			state->boundTargets.clear();
			state->publishedGameSpatialAAModeMask.reset();
		}

		// Only this protected section can resolve camera identity. The worker receives
		// plain validated stamps and cannot traverse mutable object/component storage.
		std::vector<GraphicsTarget> targets;
		targets.reserve(state->requestedTargets.size() + state->publishedCameraViews.size());
		const auto resolveOwner = [&](const GraphicsCameraTarget& owner) -> std::optional<std::uint32_t> {
			const auto object = objManager.getObject(owner.cameraId);
			if(!object || !objManager.HasComponent<CameraComponent>(*object)
				|| owner.sceneGeneration != objManager.GetSceneGeneration()
				|| owner.objectLifetimeGeneration != objManager.getObjectLifetimeGeneration(*object)
				|| owner.componentGeneration != objManager.getObjectComponentGeneration(*object)
				|| owner.bindingGeneration != objManager.getObjectBindingGeneration(*object)) return {};
			return object->entityValue;
		};

		// A skipped primary, closed viewport or dedicated capture can remove a
		// published view while its authored camera component is still alive. Keep
		// the protected camera binding independent of per-frame native view admission.
		std::erase_if(state->boundTargets, [&](const auto& binding) {
			return !binding.second.camera || resolveOwner(*binding.second.camera) != binding.first;
		});
		const auto validateTarget = [&](const GraphicsTarget& target) {
			if(target.scope != GraphicsScope::Camera || !target.camera
				|| (target.view && target.view->lifetimeGeneration == 0)) return;
			const auto entity = resolveOwner(*target.camera);
			if(!entity) return;
			if(std::find(targets.begin(), targets.end(), target) != targets.end()) return;
			targets.push_back(target);
			if(std::none_of(state->boundTargets.begin(), state->boundTargets.end(), [&](const auto& binding) {
				return binding.first == *entity;
			})) state->boundTargets.emplace_back(*entity, GraphicsTarget{GraphicsScope::Camera, target.camera, {}});
		};
		for(const auto& target : state->requestedTargets) validateTarget(target);
		for(const auto& view : state->publishedCameraViews) validateTarget(view.target);
		if(previousLifecycle != state->lifecycle || targets != state->targets)
			++state->epoch;
		state->targets = std::move(targets);
		const GraphicsPreparationContext context{state->lifecycle, *state->catalog, state->targets, nextRenderFrameId, *state->presets};
		std::vector<GraphicsCameraTarget> liveCameraOwners;
		liveCameraOwners.reserve(state->boundTargets.size());
		for(const auto& binding : state->boundTargets) liveCameraOwners.push_back(*binding.second.camera);
		state->controller.Invalidate(context, liveCameraOwners);
		if(state->nativeCatalog)
		{
			const auto& scene = SceneManager::Instance().GetActiveSceneRenderSettings();
			const auto nativeGeneration = GetRuntimeRenderSettingsGeneration();
			if(nativeGeneration != state->observedNativeGeneration || previousLifecycle != state->lifecycle
				|| scene.indirectStrength != state->sceneSky.sceneIndirectStrength
				|| scene.ambientOcclusionStrength != state->sceneSky.sceneAmbientOcclusionStrength
				|| scene.ambientOcclusionQuality != state->sceneSky.sceneAmbientOcclusionQuality)
			{
				state->sceneSky.sceneIndirectStrength = scene.indirectStrength;
				state->sceneSky.sceneAmbientOcclusionStrength = scene.ambientOcclusionStrength;
				state->sceneSky.sceneAmbientOcclusionQuality = scene.ambientOcclusionQuality;
				auto actual = currentSettings;
				if(auto* native = dynamic_cast<VulkanContext*>(rhi.get()))
				{
					const auto [validation, vrs] = native->ReadGraphicsStartupFlags();
					actual.validationEnabled = validation ? 1u : 0u;
					actual.variableRateShading = vrs ? 1u : 0u;
				}
				state->controller.RefreshNativeValues(context,
					CaptureNativeGraphicsSettings(actual, taskShaderCullingEnabled, state->profile, state->sceneSky), currentPreset);
				state->observedNativeGeneration = nativeGeneration;
			}
		}
		if(state->persistence)
		{
			// The background file reader only produces copied patches. Resolve
			// their receipt and publish them through the same native controller.
			for(auto& restore : state->persistence->DrainRestores())
			{
				auto& request = restore.request;
				std::vector<GraphicsSettingId> ids;
				for(const auto& value : request.patches) ids.push_back(value.id);
				auto receipt = request.lifecycle != state->lifecycle
					? state->controller.Reject(std::move(request), GraphicsRequestReason::StaleLifetime)
					: state->startup && state->persistence->Conflicts(ids)
						? state->controller.Reject(std::move(request), GraphicsRequestReason::ConflictingRequest)
						: state->controller.Enqueue(std::move(request));
				state->persistence->UpdateRestoreReceipt(restore.requestId, receipt);
				if(receipt.state == GraphicsRequestState::Accepted)
					state->pendingPreferenceRestores.emplace_back(restore.requestId, receipt.requestId);
			}
		}
		if(state->pendingRebuild)
		{
			const auto receipt = state->controller.GetReceipt(state->pendingRebuild->requestId);
			if(!receipt || receipt->state != GraphicsRequestState::PendingRebuild)
			{
				state->pendingRebuild.reset();
				state->pendingPostProcessLookups.clear();
				if(auto* native = dynamic_cast<VulkanContext*>(rhi.get())) native->DiscardPostProcessCandidates();
			}
		}

		const auto publish = [&](GraphicsPreparedCandidate candidate, bool ready, std::uint64_t generation,
			GraphicsRequestReason failureReason = GraphicsRequestReason::PreparationFailed) {
			auto settings = currentSettings;
			auto culling = taskShaderCullingEnabled;
			auto profile = state->profile;
			bool applyNative = false;
			const auto preset = candidate.snapshot.preset.value_or(QualityPreset::Custom);
			if(ready && state->nativeCatalog && candidate.snapshot.target.scope == GraphicsScope::Game)
			{
				std::vector<GraphicsSettingValue> patches;
				const auto receipt = state->controller.GetReceipt(candidate.requestId);
				if(receipt)
					for(const auto& value : candidate.snapshot.values)
						if(value.id >= GraphicsSettingId::ShadowResolution && value.id <= GraphicsSettingId::ProfileResolutionScale
							&& std::find(receipt->affectedSettings.begin(), receipt->affectedSettings.end(), value.id)
								!= receipt->affectedSettings.end())
							patches.push_back(value);
				std::string detail;
				applyNative = !patches.empty();
				ready = !applyNative || ApplyNativeGraphicsSettings(patches, *state->catalog, settings, culling, profile, detail);
			}
			const auto activation = candidate.activation;
			const auto receipt = state->controller.Publish(std::move(candidate), context, ready, generation, failureReason);
			if(activation == GraphicsActivation::Rebuild && receipt.state != GraphicsRequestState::Applied)
				if(auto* native = dynamic_cast<VulkanContext*>(rhi.get())) native->DiscardPostProcessCandidates();
			if(receipt.state == GraphicsRequestState::Applied && applyNative)
			{
				SetTransientNativeGraphicsSettings(settings, preset, culling);
				state->profile = profile;
				state->observedNativeGeneration = GetRuntimeRenderSettingsGeneration();
			}
		};
		// Polling a promise-backed ThreadPool future never waits on the main loop.
		// Applying only here keeps every later view on one immutable revision.
		if(state->preparation.valid() && state->preparation.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			auto candidate = state->preparation.get();
			const auto receipt = candidate ? state->controller.GetReceipt(candidate->requestId) : std::nullopt;
			if(candidate && receipt && (receipt->state == GraphicsRequestState::Accepted
				|| receipt->state == GraphicsRequestState::PendingRebuild))
			{
				if(candidate->activation == GraphicsActivation::Rebuild)
					state->pendingRebuild = std::move(*candidate);
				else
					publish(std::move(*candidate), true, 0);
			}
		}
		for(const auto& completion : state->resourceCompletions)
		{
			if(!state->pendingRebuild || completion.requestId != state->pendingRebuild->requestId
				|| completion.lifecycle != state->lifecycle) continue;
			const auto qualifiedGeneration = completion.postProcessPrepared
				? completion.lifecycle.backendResourceGeneration : 0;
			auto candidate = std::move(*state->pendingRebuild);
			state->pendingRebuild.reset();
			state->pendingPostProcessLookups.clear();
			publish(std::move(candidate), completion.succeeded, qualifiedGeneration, completion.failureReason);
		}
		state->resourceCompletions.clear();
		state->controller.UpdateViewExtent({}, state->publishedGameExtent.x, state->publishedGameExtent.y);
		for(const auto& view : state->publishedCameraViews)
			state->controller.UpdateViewExtent(view.target, view.width, view.height);
		if(state->persistence)
			std::erase_if(state->pendingPreferenceRestores, [&](const auto& ids) {
				const auto receipt = state->controller.GetReceipt(ids.second);
				if(!receipt) return true;
				state->persistence->UpdateRestoreReceipt(ids.first, *receipt);
				if(receipt->state == GraphicsRequestState::Applied)
					state->restoredPlayerSettingIds = receipt->affectedSettings;
				return receipt->state != GraphicsRequestState::Accepted && receipt->state != GraphicsRequestState::PendingRebuild;
			});
		if(state->preparation.valid() || state->pendingRebuild || state->scene.HasPreparation()
			|| !state->controller.HasPendingWork()) return;

		const auto catalog = state->catalog;
		const auto presets = state->presets;
		const auto capturedTargets = state->targets;
		const auto lifecycle = state->lifecycle;
		const auto epoch = state->epoch;
		const auto frameId = nextRenderFrameId;
		auto task = ThreadPool::instance().createTask([state, catalog, presets, capturedTargets, lifecycle, epoch, frameId]() -> std::optional<GraphicsPreparedCandidate> {
			std::lock_guard workerLock(state->mutex);
			if(epoch != state->epoch) return std::nullopt;
			const GraphicsPreparationContext captured{lifecycle, *catalog, capturedTargets, frameId, *presets};
			return state->controller.Prepare(captured);
		});
		auto future = task->getFuture();
		ThreadPool::instance().enqueue(task);
		state->preparation = std::move(future);
		state->lastPreparationFailure = GraphicsRequestReason::None;
	}
	catch(...)
	{
		// Task allocation, submission, and future-result failure leave the last
		// published roots available. Native diagnostics expose this typed result
		// without throwing an infrastructure failure through render capture.
		std::lock_guard lock(state->mutex);
		state->lastPreparationFailure = GraphicsRequestReason::PreparationFailed;
	}
}

void RenderSystem::ApplyQueuedRuntimeRenderSettings()
{
	// The existing capture boundary is the sole consumer of transient options.
	// Recovery changes token ownership before any prior-session ingress can apply.
	const std::uint64_t backendSession = rhi
		? rhi->GetLatestRendererRecoverySnapshot().renderSessionGeneration : 0U;
	std::optional<RuntimeRenderSettingsRequest> request;
	std::vector<RendererResourcePublicationInvalidation> resourceInvalidations;
	std::uint64_t currentOptionsEpoch = 0;
	bool resetEfficiencyEvidence = false;
	{
		std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
		if (efficiencyObservedBackendSession != 0U && backendSession != efficiencyObservedBackendSession)
		{
			rendererEfficiencyOptionsQueue.InvalidateSession();
			// Requests copied before device recovery belong to the retired renderer.
			pendingRuntimeRenderSettings.reset();
			resetEfficiencyEvidence = true;
		}
		efficiencyObservedBackendSession = backendSession;
		rendererEfficiencyOptionsQueue.BindRendererSession(backendSession);
		rendererEfficiencyOptionsQueue.CaptureAtProtectedBoundary();
		const auto options = rendererEfficiencyOptionsQueue.Snapshot();
		currentOptionsEpoch = options.optionsSessionGeneration;
		resourceInvalidations.swap(pendingRendererResourceInvalidations);
		if (pendingRendererEfficiencyFixtureIntent)
		{
			const auto& correlation = pendingRendererEfficiencyFixtureIntent->correlation;
			if (correlation.settingsGeneration == options.settingsGeneration
				&& correlation.rendererSessionGeneration == options.rendererSessionGeneration)
				capturedRendererEfficiencyFixtureIntent = std::move(*pendingRendererEfficiencyFixtureIntent);
			pendingRendererEfficiencyFixtureIntent.reset();
		}
		if (capturedRendererEfficiencyFixtureIntent.correlation.settingsGeneration != options.settingsGeneration
			|| capturedRendererEfficiencyFixtureIntent.correlation.rendererSessionGeneration != options.rendererSessionGeneration)
			capturedRendererEfficiencyFixtureIntent = {};
		request = std::move(pendingRuntimeRenderSettings);
		pendingRuntimeRenderSettings.reset();
	}
	if (resetEfficiencyEvidence)
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestRendererEfficiencyEvidence.clear(); latestRendererEfficiencyFixtureTargets.clear();
		latestRenderFrameSnapshot.efficiencyFixtureIntent = {};
		TerraEngine::Renderer::ResetRendererEfficiencyFixtureEvidenceBatch(latestRendererEfficiencyEvidenceBatch);
	}
	// Actual asynchronous loader publication happens before ingress. Only this
	// existing protected boundary invalidates GPU payloads; it neither reloads CPU
	// resources nor introduces a scene mutation flush or a new GPU wait.
	const auto publicationGeneration = ResourceManager::Instance().GetRendererResourcePublicationGeneration();
	for (const auto& invalidation : resourceInvalidations)
	{
		if (!rhi || invalidation.optionsSessionGeneration != currentOptionsEpoch || !invalidation.publicationGeneration
			|| invalidation.publicationGeneration > publicationGeneration) continue;
		switch (invalidation.type)
		{
		case AssetType::Mesh: rhi->InvalidateMesh(invalidation.assetId); break;
		case AssetType::Material: rhi->InvalidateMaterial(invalidation.assetId); break;
		case AssetType::Texture: rhi->InvalidateTexture(invalidation.assetId); break;
		default: break;
		}
	}
	if (!request)
	{
		return;
	}
	if(request->usePreset)
	{
		ApplyQualityPreset(request->preset, false);
	}
	else
	{
		// V1 packets predate these three budgets. Preserve the current native
		// values even when another request changed them after this packet queued.
		auto settings = request->settings;
		settings.virtualGeometry.deformationCurrentBudgetBytes = currentSettings.virtualGeometry.deformationCurrentBudgetBytes;
		settings.virtualGeometry.deformationHistoryBudgetBytes = currentSettings.virtualGeometry.deformationHistoryBudgetBytes;
		settings.virtualGeometry.deformationBoundsBudgetBytes = currentSettings.virtualGeometry.deformationBoundsBudgetBytes;
		SetTransientNativeGraphicsSettings(settings, QualityPreset::Custom, taskShaderCullingEnabled);
	}
	if(auto* vulkan = dynamic_cast<VulkanContext*>(rhi.get()))
		vulkan->SetVariableRateShadingEnabled(currentSettings.variableRateShading != 0);
}

TerraEngine::Renderer::RendererEfficiencyOptionsReceipt RenderSystem::QueueRendererEfficiencyOptions(
	const TerraEngine::Renderer::RendererEfficiencyOptions& options)
{
	std::lock_guard lock(runtimeRenderSettingsMutex);
	return rendererEfficiencyOptionsQueue.Queue(options);
}

TerraEngine::Renderer::RendererEfficiencyOptionsReceipt RenderSystem::QueueRendererEfficiencyRestore(
	const TerraEngine::Renderer::RendererEfficiencyOptionsToken& token)
{
	std::lock_guard lock(runtimeRenderSettingsMutex);
	return rendererEfficiencyOptionsQueue.Restore(token);
}

TerraEngine::Renderer::RendererEfficiencyOptionsSnapshot RenderSystem::GetRendererEfficiencyOptionsSnapshot() const
{
	std::lock_guard lock(runtimeRenderSettingsMutex);
	return rendererEfficiencyOptionsQueue.Snapshot();
}

TerraEngine::Renderer::RendererEfficiencyEvidence RenderSystem::GetLatestRendererEfficiencyEvidence(
	RenderViewType viewType, std::uint32_t viewId) const
{
	// Do not nest the ingress and diagnostic locks: requests may arrive while
	// protected publication replaces the copied per-view evidence collection.
	const auto options = GetRendererEfficiencyOptionsSnapshot();
	TerraEngine::Renderer::RendererEfficiencyFixtureIntent activeIntent;
	{ std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex); activeIntent = capturedRendererEfficiencyFixtureIntent; }
	std::lock_guard lock(statsMutex);
	for (const auto& evidence : latestRendererEfficiencyEvidence)
		if (evidence.viewType == viewType && evidence.viewId == viewId
			&& TerraEngine::Renderer::IsCurrentRendererEfficiencyFixtureEvidence(evidence, options, activeIntent)) return evidence;
	TerraEngine::Renderer::RendererEfficiencyEvidence unavailable;
	unavailable.viewType = viewType;
	unavailable.viewId = viewId;
	unavailable.effectiveOptions = options;
	unavailable.staticCacheBudgetRequestedBytes = options.options.staticShadowCacheBudgetBytes;
	unavailable.staticCacheBudgetEffectiveBytes = unavailable.staticCacheBudgetRequestedBytes;
	unavailable.unavailableReason = "No completed renderer efficiency publication exists for this view.";
	return unavailable;
}

void RenderSystem::ClearRendererEfficiencyFixtureIntent(std::string_view operationId)
{
	if (operationId.empty()) return;
	bool cleared = false;
	{
	std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
	if (pendingRendererEfficiencyFixtureIntent && pendingRendererEfficiencyFixtureIntent->correlation.operationId == operationId)
	{ pendingRendererEfficiencyFixtureIntent.reset(); cleared = true; }
	if (capturedRendererEfficiencyFixtureIntent.correlation.operationId == operationId)
	{ capturedRendererEfficiencyFixtureIntent = {}; cleared = true; }
	}
	std::lock_guard<std::mutex> lock(statsMutex);
	if (latestRenderFrameSnapshot.efficiencyFixtureIntent.correlation.operationId == operationId)
	{ latestRenderFrameSnapshot.efficiencyFixtureIntent = {}; cleared = true; }
	if (cleared)
	{
		latestRendererEfficiencyFixtureTargets.clear(); latestRendererEfficiencyEvidence.clear();
		TerraEngine::Renderer::ResetRendererEfficiencyFixtureEvidenceBatch(latestRendererEfficiencyEvidenceBatch);
	}
}

TerraEngine::Renderer::RendererEfficiencyFixtureIntent RenderSystem::GetRendererEfficiencyFixtureControlSnapshot() const
{
	TerraEngine::Renderer::RendererEfficiencyFixtureIntent activeIntent;
	{ std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex); activeIntent = capturedRendererEfficiencyFixtureIntent; }
	std::lock_guard<std::mutex> lock(statsMutex);
	auto result = latestRenderFrameSnapshot.efficiencyFixtureIntent;
	if (result.correlation.operationId != activeIntent.correlation.operationId || result.controlGeneration != activeIntent.controlGeneration) return {};
	if (result.controlsApplied && !TerraEngine::Renderer::HasRendererEfficiencyFixtureControlAcknowledgment(latestRendererEfficiencyEvidenceBatch.rows, result))
	{
		result.controlsApplied = false;
		result.controlReason = "Awaiting an actual recorded-command completion receipt for these fixture controls.";
	}
	return result;
}

std::optional<TerraEngine::Renderer::RendererEfficiencyFixtureTargetProof> RenderSystem::GetRendererEfficiencyFixtureTarget(const TerraEngine::UUID& objectId) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	const auto found = std::find_if(latestRendererEfficiencyFixtureTargets.begin(), latestRendererEfficiencyFixtureTargets.end(), [&](const auto& row) { return row.objectId == objectId; });
	return found == latestRendererEfficiencyFixtureTargets.end() ? std::nullopt : std::optional(*found);
}

bool RenderSystem::QueueRendererResourcePublicationInvalidation(const TerraEngine::UUID& assetId, AssetType type,
	std::uint64_t generation, std::string* reason)
{
	if (reason) reason->clear();
	const auto current = ResourceManager::Instance().GetRendererResourcePublicationGeneration();
	if (assetId == TerraEngine::EMPTY_UUID || !generation || generation > current
		|| (type != AssetType::Mesh && type != AssetType::Material && type != AssetType::Texture))
	{ if (reason) *reason = "Actual renderer resource publication identity is unavailable."; return false; }
	std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
	const auto epoch = rendererEfficiencyOptionsQueue.Snapshot().optionsSessionGeneration;
	const auto found = std::find_if(pendingRendererResourceInvalidations.begin(), pendingRendererResourceInvalidations.end(), [&](const auto& row) { return row.assetId == assetId && row.type == type; });
	if (found == pendingRendererResourceInvalidations.end()) pendingRendererResourceInvalidations.push_back({assetId, type, generation, epoch});
	else *found = {assetId, type, generation, epoch};
	return true;
}

TerraEngine::Renderer::RendererEfficiencyEvidenceBatch RenderSystem::GetRendererEfficiencyEvidenceBatch(std::uint64_t afterBatchSequence) const
{
	const auto options = GetRendererEfficiencyOptionsSnapshot();
	TerraEngine::Renderer::RendererEfficiencyFixtureIntent activeIntent;
	{ std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex); activeIntent = capturedRendererEfficiencyFixtureIntent; }
	std::lock_guard<std::mutex> lock(statsMutex);
	if (latestRendererEfficiencyEvidenceBatch.batchSequence <= afterBatchSequence) return {};
	auto result = latestRendererEfficiencyEvidenceBatch;
	std::erase_if(result.rows, [&](const auto& row) { return !TerraEngine::Renderer::IsCurrentRendererEfficiencyFixtureEvidence(row, options, activeIntent); });
	return result;
}

void RenderSystem::ApplyTransientRenderDiagnostics(bool validationEnabled, bool debugEnabled)
{
	const bool validationChanged = !transientRenderValidationOverride.has_value()
		|| transientRenderValidationOverride.value() != validationEnabled;
	const bool debugChanged = !transientRenderDebugDiagnosticsOverride.has_value()
		|| transientRenderDebugDiagnosticsOverride.value() != debugEnabled;
	if (!validationChanged && !debugChanged)
	{
		return;
	}

	// Preferences are user/editor state, so this path deliberately updates only
	// live runtime flags. It does not call SetRenderSettings or SaveSettings,
	// because those APIs own project-authored RenderSettings.json data.
	transientRenderValidationOverride = validationEnabled;
	transientRenderDebugDiagnosticsOverride = debugEnabled;
	ResourceManager::Instance().SetRenderValidationEnabled(validationEnabled);
	ResourceManager::Instance().SetRenderDebugEnabled(debugEnabled);
	ApplyRenderDiagnosticsRequest();

	if (auto* vulkanContext = dynamic_cast<VulkanContext*>(rhi.get()))
	{
		// Existing Vulkan instances cannot gain layers after creation, but
		// retaining the requested value keeps diagnostics accurate without
		// modifying project settings. A restart applies the new layer request.
		vulkanContext->SetValidationLayersRequested(validationEnabled);
	}
}

void RenderSystem::ClearTransientRenderDiagnosticsOverride()
{
	if (!transientRenderValidationOverride.has_value() && !transientRenderDebugDiagnosticsOverride.has_value())
	{
		return;
	}

	transientRenderValidationOverride.reset();
	transientRenderDebugDiagnosticsOverride.reset();
	ResourceManager::Instance().SetRenderValidationEnabled(currentSettings.validationEnabled > 0u);
	ResourceManager::Instance().SetRenderDebugEnabled(currentSettings.debugEnabled > 0u);
	ApplyRenderDiagnosticsRequest();

	if (auto* vulkanContext = dynamic_cast<VulkanContext*>(rhi.get()))
	{
		vulkanContext->SetValidationLayersRequested(currentSettings.validationEnabled > 0u);
	}
}

void RenderSystem::SetVirtualGeometryQualityProfile(const VirtualGeometryQualityProfile& profile)
{
	currentSettings.virtualGeometry = SanitizeVirtualGeometryQualityProfile(profile);
	currentPreset = QualityPreset::Custom;
	SaveSettings();
}

VirtualGeometryQualityProfile RenderSystem::GetVirtualGeometryQualityProfile() const
{
	return SanitizeVirtualGeometryQualityProfile(currentSettings.virtualGeometry);
}

void RenderSystem::SetVirtualGeometryDebugVisualizationMode(RenderViewType viewType, std::uint32_t outputId, VirtualGeometryDebugVisualizationMode mode)
{
	// View-local toolbar state is runtime-only. Normalize here so editor UI,
	// automation, or stale project values cannot publish removed debug modes
	// into the visible-command cache or shader push constants.
	const VirtualGeometryDebugVisualizationMode normalized = NormalizeVirtualGeometryDebugVisualizationMode(mode);
	std::lock_guard<std::mutex> lock(statsMutex);
	virtualGeometryDebugModes[BuildVirtualGeometryDebugModeKey(viewType, outputId)] = normalized;
}

VirtualGeometryDebugVisualizationMode RenderSystem::GetVirtualGeometryDebugVisualizationMode(RenderViewType viewType, std::uint32_t outputId) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	const auto mode = virtualGeometryDebugModes.find(BuildVirtualGeometryDebugModeKey(viewType, outputId));
	if (mode != virtualGeometryDebugModes.end())
	{
		return NormalizeVirtualGeometryDebugVisualizationMode(mode->second);
	}
	return NormalizeVirtualGeometryDebugVisualizationMode(currentSettings.virtualGeometry.debugVisualizationMode);
}

bool RenderSystem::SubmitSceneViewPhysicsDebugPackets(
	const std::vector<std::shared_ptr<const TerraEngine::Physics::PhysicsDebugPacket>>& packets,
	const TerraEngine::Physics::PhysicsDebugDisplaySelection& selection)
{
	// Renderer recovery and headless/editor shutdown paths must not force an RHI
	// allocation merely because physics published a safe diagnostic packet.
	if (rhi == nullptr)
	{
		return false;
	}

	SceneViewPhysicsDebugSubmissionSummary summary;
	summary.rendererAvailable = true;
	summary.packetCount = packets.size();
	for (const std::shared_ptr<const TerraEngine::Physics::PhysicsDebugPacket>& packet : packets)
	{
		if (!packet)
		{
			continue;
		}
		if (selection.showLines && packet->lines) { summary.primitiveCount += packet->lines->size(); }
		if (selection.showTriangles && packet->triangles) { summary.primitiveCount += packet->triangles->size(); }
		if (selection.showContacts && packet->contacts) { summary.primitiveCount += packet->contacts->size(); }
		if (selection.showConstraints && packet->constraints) { summary.primitiveCount += packet->constraints->size(); }
		if (selection.showCharacterSupport && packet->characters) { summary.primitiveCount += packet->characters->size(); }
		if (selection.showRecentQueries && packet->queries) { summary.primitiveCount += packet->queries->size(); }
	}

	std::lock_guard<std::mutex> lock(statsMutex);
	latestSceneViewPhysicsDebugSubmission = summary;
	return true;
}

RenderSystem::SceneViewPhysicsDebugSubmissionSummary RenderSystem::GetLatestSceneViewPhysicsDebugSubmission() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestSceneViewPhysicsDebugSubmission;
}

void RenderSystem::ClearVirtualGeometryDebugVisualizationModes()
{
	std::lock_guard<std::mutex> lock(statsMutex);
	virtualGeometryDebugModes.clear();
}

VirtualGeometryInactiveReason RenderSystem::GetVirtualGeometryInactiveReason() const
{
	const bool meshShaderRouteSupported = rhi != nullptr && rhi->SupportsVirtualGeometryMeshShaderRoute();
	return ResolveVirtualGeometryInactiveReason(currentSettings.virtualGeometry, meshShaderRouteSupported);
}

void RenderSystem::LoadSettings(bool persistMissingDefaults)
{
	namespace fs = std::filesystem;

	try
	{
		// Path resolution and filesystem probes belong to the same reporting
		// boundary as parsing/writing; permission and IO errors must not escape.
		fs::path settingsPath = ResolveRuntimeSettingsDirectory() / "RenderSettings.json";


		if (!TerraEngine::Tests::Review::SettingsPathExists(settingsPath))
		{
			const fs::path legacySettingsPath = ResolveLegacyAssetSettingsPath();

			if (!legacySettingsPath.empty() && TerraEngine::Tests::Review::SettingsPathExists(legacySettingsPath))
			{
				settingsPath = legacySettingsPath;
			}
			else
			{
				ApplyQualityPreset(QualityPreset::High, persistMissingDefaults);
				return;
			}
		}

		std::ifstream file(settingsPath);
		nlohmann::json j;
		file >> j;

		currentPreset = static_cast<QualityPreset>(j.value("preset", 2u));

		auto s = j["settings"];
		currentSettings.shadowResolution = s.value("shadowResolution", 2048u);
		currentSettings.shadowFilteringSize = s.value("shadowFilteringSize", 5u);
		currentSettings.maxUploadedLights = s.value("maxUploadedLights", 256u);
		currentSettings.maxDynamicLights = s.value("maxDynamicLights", 128u);
		currentSettings.maxShadowLights = s.value("maxShadowLights", 8u);
		currentSettings.maxAreaLights = s.value("maxAreaLights", 4u);
		currentSettings.enableLTC = s.value("enableLTC", 1u);
		currentSettings.enableIBL = s.value("enableIBL", 1u);
		currentSettings.indirectStrength = s.value("indirectStrength", 1.0f);
		currentSettings.ambientOcclusionStrength = s.value("ambientOcclusionStrength", 1.0f);
		currentSettings.ambientOcclusionQuality = s.value("ambientOcclusionQuality", 2u);
		currentSettings.anisotropicFiltering = s.value("anisotropicFiltering", 4u);
		currentSettings.variableRateShading = s.value("variableRateShading", 0u);
		// Vulkan validation and render-debug diagnostics are startup-owned
		// profile flags. Keep them beside the render settings file so the editor
		// can expose them without requiring manual JSON edits.
		currentSettings.validationEnabled = j.value("validationEnabled", s.value("validationEnabled", false)) ? 1u : 0u;
		currentSettings.debugEnabled = j.value("debugEnabled", s.value("debugEnabled", false)) ? 1u : 0u;
		if (s.contains("virtualGeometry") && s["virtualGeometry"].is_object())
		{
			const auto& virtualGeometry = s["virtualGeometry"];
			currentSettings.virtualGeometry.lodErrorScale = virtualGeometry.value("lodErrorScale", 1.0f);
			currentSettings.virtualGeometry.maxLodDrop = virtualGeometry.value("maxLodDrop", 0u);
			// Older settings files may omit this field entirely. Missing means
			// "use the guarded engine default"; an explicit false still remains
			// an opt-out for debugging risky scenes.
			currentSettings.virtualGeometry.hiZOcclusionEnabled = virtualGeometry.value("hiZOcclusionEnabled", MakeDefaultVirtualGeometryQualityProfile().hiZOcclusionEnabled);
			currentSettings.virtualGeometry.hiZSelfOcclusionGuardEnabled = virtualGeometry.value("hiZSelfOcclusionGuardEnabled", true);
			currentSettings.virtualGeometry.hiZNearPlaneGuardEnabled = virtualGeometry.value("hiZNearPlaneGuardEnabled", true);
			currentSettings.virtualGeometry.hiZThinGeometryGuardEnabled = virtualGeometry.value("hiZThinGeometryGuardEnabled", true);
			currentSettings.virtualGeometry.debugVisualizationMode = static_cast<VirtualGeometryDebugVisualizationMode>(
				virtualGeometry.value("debugVisualizationMode", static_cast<std::uint32_t>(VirtualGeometryDebugVisualizationMode::None)));
			currentSettings.virtualGeometry.geometryBudgetBytes = virtualGeometry.value("geometryBudgetBytes", static_cast<std::uint64_t>(0u));
			currentSettings.virtualGeometry.deformationCurrentBudgetBytes =
				virtualGeometry.value(
					"deformationCurrentBudgetBytes",
					static_cast<std::uint64_t>(0u));
			currentSettings.virtualGeometry.deformationHistoryBudgetBytes =
				virtualGeometry.value(
					"deformationHistoryBudgetBytes",
					static_cast<std::uint64_t>(0u));
			currentSettings.virtualGeometry.deformationBoundsBudgetBytes =
				virtualGeometry.value(
					"deformationBoundsBudgetBytes",
					static_cast<std::uint64_t>(0u));
			currentSettings.virtualGeometry.preloadBias = virtualGeometry.value("preloadBias", 1.0f);
			currentSettings.virtualGeometry.lodTransitionHysteresisPixels = virtualGeometry.value("lodTransitionHysteresisPixels", 0.35f);
			currentSettings.virtualGeometry.lodTransitionStableFrames = virtualGeometry.value("lodTransitionStableFrames", 3u);
		}
		currentSettings = SanitizeRenderSettings(currentSettings);
		{
			std::lock_guard lock(runtimeRenderSettingsMutex);
			++runtimeRenderSettingsGeneration;
		}
		if (!transientRenderValidationOverride.has_value())
		{
			ResourceManager::Instance().SetRenderValidationEnabled(currentSettings.validationEnabled > 0u);
		}
		ResourceManager::Instance().SetRenderDebugEnabled(ResolveRenderDebugDiagnosticsEnabled());
		ApplyRenderDiagnosticsRequest();
	}
	catch (const std::exception& e)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to load RenderSettings: " << e.what());
		// A transient read failure must never persist fallback values over authored settings.
		ApplyQualityPreset(QualityPreset::High, false);
	}
}

void RenderSystem::SaveSettings()
{
	namespace fs = std::filesystem;

	try
	{
		// Path resolution and filesystem probes belong to the same reporting
		// boundary as parsing/writing; permission and IO errors must not escape.
		const fs::path settingsDir = ResolveRuntimeSettingsDirectory();
		const fs::path settingsPath = settingsDir / "RenderSettings.json";


		if (!TerraEngine::Tests::Review::SettingsPathExists(settingsDir))
		{
			TerraEngine::Tests::Review::SettingsCreateDirectories(settingsDir);
		}

		nlohmann::json j;
		j["preset"] = static_cast<uint32_t>(currentPreset);
		j["validationEnabled"] = currentSettings.validationEnabled > 0u;
		j["debugEnabled"] = currentSettings.debugEnabled > 0u;

		nlohmann::json s;
		s["shadowResolution"] = currentSettings.shadowResolution;
		s["shadowFilteringSize"] = currentSettings.shadowFilteringSize;
		s["maxUploadedLights"] = currentSettings.maxUploadedLights;
		s["maxDynamicLights"] = currentSettings.maxDynamicLights;
		s["maxShadowLights"] = currentSettings.maxShadowLights;
		s["maxAreaLights"] = currentSettings.maxAreaLights;
		s["enableLTC"] = currentSettings.enableLTC;
		s["enableIBL"] = currentSettings.enableIBL;
		s["indirectStrength"] = currentSettings.indirectStrength;
		s["ambientOcclusionStrength"] = currentSettings.ambientOcclusionStrength;
		s["ambientOcclusionQuality"] = currentSettings.ambientOcclusionQuality;
		s["anisotropicFiltering"] = currentSettings.anisotropicFiltering;
		s["variableRateShading"] = currentSettings.variableRateShading;
		nlohmann::json virtualGeometry;
		const VirtualGeometryQualityProfile virtualGeometryProfile = SanitizeVirtualGeometryQualityProfile(currentSettings.virtualGeometry);
		virtualGeometry["lodErrorScale"] = virtualGeometryProfile.lodErrorScale;
		virtualGeometry["maxLodDrop"] = virtualGeometryProfile.maxLodDrop;
		virtualGeometry["hiZOcclusionEnabled"] = virtualGeometryProfile.hiZOcclusionEnabled;
		virtualGeometry["hiZSelfOcclusionGuardEnabled"] = virtualGeometryProfile.hiZSelfOcclusionGuardEnabled;
		virtualGeometry["hiZNearPlaneGuardEnabled"] = virtualGeometryProfile.hiZNearPlaneGuardEnabled;
		virtualGeometry["hiZThinGeometryGuardEnabled"] = virtualGeometryProfile.hiZThinGeometryGuardEnabled;
		virtualGeometry["debugVisualizationMode"] = static_cast<std::uint32_t>(virtualGeometryProfile.debugVisualizationMode);
		virtualGeometry["geometryBudgetBytes"] = virtualGeometryProfile.geometryBudgetBytes;
		virtualGeometry["deformationCurrentBudgetBytes"] =
			virtualGeometryProfile.deformationCurrentBudgetBytes;
		virtualGeometry["deformationHistoryBudgetBytes"] =
			virtualGeometryProfile.deformationHistoryBudgetBytes;
		virtualGeometry["deformationBoundsBudgetBytes"] =
			virtualGeometryProfile.deformationBoundsBudgetBytes;
		virtualGeometry["preloadBias"] = virtualGeometryProfile.preloadBias;
		virtualGeometry["lodTransitionHysteresisPixels"] = virtualGeometryProfile.lodTransitionHysteresisPixels;
		virtualGeometry["lodTransitionStableFrames"] = virtualGeometryProfile.lodTransitionStableFrames;
		s["virtualGeometry"] = virtualGeometry;

		j["settings"] = s;

		std::ofstream file(settingsPath);
		file << j.dump(4);
	}
	catch (const std::exception& e)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to save RenderSettings: " << e.what());
	}
}
