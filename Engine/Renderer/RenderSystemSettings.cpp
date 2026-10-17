// RenderSystem settings facade, preserving existing protected ownership and publication boundaries.
#include "Frame/RenderFrameState.h"
#include "Frame/RenderVirtualGeometrySelection.h"
#include "Renderer/RHI/Vulkan/VulkanContext.h"
#include "Resource/ResourceManager.h"
#include "Core/Logger/Logger.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureControls.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureCameras.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureDepthControls.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureShadowBatchControls.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>

// Existing sanitizer helper below is retained; do not add a second definition.
// Render settings normalization shared by protected capture and settings facades.
#include "Frame/RenderFrameState.h"

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
	if (preset == QualityPreset::Custom)
	{
		std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
		currentPreset = QualityPreset::Custom;
		return;
	}

	std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
	const std::uint32_t priorTerrainAnisotropy = currentSettings.anisotropicFiltering;
	currentPreset = preset;
	// Dispatch by explicit engine state so each case documents one supported behavior.
	switch (preset)
	{
	case QualityPreset::Low:
		currentSettings.shadowResolution = 512u;
		currentSettings.shadowFilteringSize = 1u;
		currentSettings.maxUploadedLights = 64u;
		currentSettings.maxDynamicLights = 32u;
		currentSettings.maxShadowLights = 1u;
		currentSettings.maxAreaLights = 0u;
		currentSettings.enableLTC = 0u;
		currentSettings.enableIBL = 0u;
		currentSettings.indirectStrength = 0.35f;
		currentSettings.ambientOcclusionStrength = 0.5f;
		currentSettings.ambientOcclusionQuality = 0u;
		currentSettings.anisotropicFiltering = 1u;
		currentSettings.variableRateShading = 0u;
		break;
	case QualityPreset::Medium:
		currentSettings.shadowResolution = 1024u;
		currentSettings.shadowFilteringSize = 3u;
		currentSettings.maxUploadedLights = 128u;
		currentSettings.maxDynamicLights = 64u;
		currentSettings.maxShadowLights = 2u;
		currentSettings.maxAreaLights = 2u;
		currentSettings.enableLTC = 1u;
		currentSettings.enableIBL = 1u;
		currentSettings.indirectStrength = 0.75f;
		currentSettings.ambientOcclusionStrength = 0.75f;
		currentSettings.ambientOcclusionQuality = 1u;
		currentSettings.anisotropicFiltering = 1u;
		currentSettings.variableRateShading = 0u;
		break;
	case QualityPreset::High:
		currentSettings.shadowResolution = 2048u;
		currentSettings.shadowFilteringSize = 5u;
		currentSettings.maxUploadedLights = 256u;
		currentSettings.maxDynamicLights = 128u;
		currentSettings.maxShadowLights = 3u;
		currentSettings.maxAreaLights = 4u;
		currentSettings.enableLTC = 1u;
		currentSettings.enableIBL = 1u;
		currentSettings.indirectStrength = 1.0f;
		currentSettings.ambientOcclusionStrength = 1.0f;
		currentSettings.ambientOcclusionQuality = 2u;
		currentSettings.anisotropicFiltering = 4u;
		currentSettings.variableRateShading = 0u;
		break;
	case QualityPreset::Ultra:
		currentSettings.shadowResolution = 4096u;
		currentSettings.shadowFilteringSize = 7u;
		currentSettings.maxUploadedLights = 512u;
		currentSettings.maxDynamicLights = 256u;
		currentSettings.maxShadowLights = 8u;
		currentSettings.maxAreaLights = 8u;
		currentSettings.enableLTC = 1u;
		currentSettings.enableIBL = 1u;
		currentSettings.indirectStrength = 1.25f;
		currentSettings.ambientOcclusionStrength = 1.25f;
		currentSettings.ambientOcclusionQuality = 3u;
		currentSettings.anisotropicFiltering = 16u;
		currentSettings.variableRateShading = 0u;
		break;
	}

	currentSettings = SanitizeRenderSettings(currentSettings);
	if(priorTerrainAnisotropy != currentSettings.anisotropicFiltering)
	{
		for(const TerraEngine::UUID& materialId :
			TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeRegistry::Instance().RepublishSamplerQuality(
				currentSettings.anisotropicFiltering))
		{
			InvalidateGeneratedMaterial(materialId);
		}
	}
	if (persistProjectSettings)
	{
		SaveSettings();
	}
}

void RenderSystem::SetRenderSettings(const RenderSettings& settings)
{
	const std::uint32_t priorTerrainAnisotropy = currentSettings.anisotropicFiltering;
	{
		std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
		currentSettings = SanitizeRenderSettings(settings);
		currentPreset = QualityPreset::Custom;
	}
	if(priorTerrainAnisotropy != currentSettings.anisotropicFiltering)
	{
		for(const TerraEngine::UUID& materialId :
			TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeRegistry::Instance().RepublishSamplerQuality(
				currentSettings.anisotropicFiltering))
		{
			InvalidateGeneratedMaterial(materialId);
		}
	}
	ResourceManager::Instance().SetRenderDebugEnabled(ResolveRenderDebugDiagnosticsEnabled());
	ApplyRenderDiagnosticsRequest();
	if (auto* vulkanContext = dynamic_cast<VulkanContext*>(rhi.get()))
	{
		vulkanContext->SetVariableRateShadingEnabled(currentSettings.variableRateShading > 0u);
	}
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
	const std::uint32_t priorTerrainAnisotropy = currentSettings.anisotropicFiltering;

	if (request->usePreset)
	{
		ApplyQualityPreset(request->preset, false);
	}
	else
	{
		std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
		currentSettings = SanitizeRenderSettings(request->settings);
		currentPreset = QualityPreset::Custom;
	}
	if(priorTerrainAnisotropy != currentSettings.anisotropicFiltering)
	{
		for(const TerraEngine::UUID& materialId :
			TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeRegistry::Instance().RepublishSamplerQuality(
				currentSettings.anisotropicFiltering))
		{
			InvalidateGeneratedMaterial(materialId);
		}
	}

	// Backend-affecting toggles are now applied by RenderSystem itself rather
	// than by the worker that submitted the managed request.
	ResourceManager::Instance().SetRenderDebugEnabled(ResolveRenderDebugDiagnosticsEnabled());
	ApplyRenderDiagnosticsRequest();
	if (auto* vulkanContext = dynamic_cast<VulkanContext*>(rhi.get()))
	{
		vulkanContext->SetVariableRateShadingEnabled(currentSettings.variableRateShading > 0U);
	}
	{
		std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
		++runtimeRenderSettingsGeneration;
	}
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

void RenderSystem::LoadSettings()
{
	namespace fs = std::filesystem;
	fs::path settingsPath = ResolveRuntimeSettingsDirectory() / "RenderSettings.json";


	if (!fs::exists(settingsPath))
	{
		const fs::path legacySettingsPath = ResolveLegacyAssetSettingsPath();

		if (!legacySettingsPath.empty() && fs::exists(legacySettingsPath))
		{
			settingsPath = legacySettingsPath;
		}
		else
		{
			SetQualityPreset(QualityPreset::High);
			return;
		}
	}

	try
	{
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
		SetQualityPreset(QualityPreset::High);
	}
}

void RenderSystem::SaveSettings()
{
	namespace fs = std::filesystem;
	const fs::path settingsDir = ResolveRuntimeSettingsDirectory();
	const fs::path settingsPath = settingsDir / "RenderSettings.json";


	if (!fs::exists(settingsDir))
	{
		fs::create_directories(settingsDir);
	}

	try
	{
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
