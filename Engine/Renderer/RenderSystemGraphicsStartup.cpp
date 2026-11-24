// Launch-time profile replay runs before handing any views to the backend.
#include "Frame/RenderFrameState.h"
#include "GraphicsSettings/GraphicsSettingsPersistence.h"
#include "GraphicsSettings/GraphicsStartupStore.h"
#include "GraphicsSettings/PostProcessColorSettings.h"
#include "GraphicsSettings/PostProcessEffectSettings.h"
#include "Renderer/Graph/RenderFramePublication.h"
#include "ThreadPool/ThreadPool.h"
#include "Renderer/RHI/Vulkan/VulkanContext.h"
#include "Resource/ResourceManager.h"
#include "Scene/SceneManager.h"
#include "Utilities/PlatformUtils.h"
#include "Core/Logger/Logger.h"
#include "Window/WindowSystem.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <set>
#include <stdexcept>

using namespace TerraEngine::Renderer::GraphicsSettings;
using TerraEngine::Renderer::Frame::SanitizeRenderSettings;

namespace
{
	std::filesystem::path ResolveGraphicsDataRoot(const GraphicsStartupInputs& input)
	{
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
		// Installed fixtures use a unique absolute save directory per test lane.
		if(const char* path = std::getenv("TERRAENGINE_GRAPHICS_FIXTURE_DATA_ROOT"))
			if(auto isolated = std::filesystem::u8path(path); isolated.is_absolute()) return isolated;
#endif
		return TerraEngine::PlatformUtils::GetPersistentDataPath(
			input.productName.empty() ? "TerraEngine" : input.productName);
	}

	bool StartupFixtureRejectsPreparation()
	{
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
		const char* enabled = std::getenv("TERRAENGINE_GRAPHICS_FIXTURE_FAIL_STARTUP");
		return enabled && std::string_view(enabled) == "1";
#else
		return false;
#endif
	}

	bool IsStartupPostProcess(GraphicsSettingId id)
	{
		return IsGraphicsAASetting(id) || IsPostProcessEffectSetting(id) || id == GraphicsSettingId::FixedExposureCompensation
			|| (id >= GraphicsSettingId::ExposureMode && id <= GraphicsSettingId::BloomSoftKnee);
	}
	std::vector<GraphicsSettingValue> StartupPostProcessDefaults(std::span<const GraphicsSettingDescriptor> catalog)
	{
		std::vector<GraphicsSettingValue> values;
		for(const auto& field : catalog) if(IsStartupPostProcess(field.id)) values.push_back({field.id,field.defaultValue});
		return values;
	}
	bool ValidStartupField(const GraphicsSettingDescriptor& field, const GraphicsValue& value)
	{
		return value.index() == static_cast<std::size_t>(field.valueKind)
			&& (!std::holds_alternative<float>(value) || std::isfinite(std::get<float>(value)))
			&& (!field.minimum || (field.minimum->index() == value.index() && value >= *field.minimum))
			&& (!field.maximum || (field.maximum->index() == value.index() && value <= *field.maximum))
			&& (field.options.empty() || std::find(field.options.begin(),field.options.end(),value) != field.options.end());
	}
	bool ApplyMixedStartupValues(std::span<const GraphicsSettingValue> patches, std::span<const GraphicsSettingDescriptor> catalog,
		RenderSettings& native, bool& culling, GraphicsProfileMetadata& profile,
		std::vector<GraphicsSettingValue>& colors, std::string& detail, GraphicsRequestReason* rejectionReason = nullptr)
	{
		if(rejectionReason) *rejectionReason = GraphicsRequestReason::InvalidValue;
		// Both adapters operate on private copies. A late color conflict cannot
		// retain an earlier native startup switch or a partial color profile.
		auto proposedNative=native; auto proposedProfile=profile; bool proposedCulling=culling;
		auto proposedColors=colors;
		std::vector<GraphicsSettingValue> nativePatches;
		std::set<GraphicsSettingId> visited;
		for(const auto& patch : patches)
		{
			const auto* field=FindGraphicsSetting(catalog,patch.id);
			// Retained off/default color values remain a viable disabled profile on
			// a device without color support. Newly staged fields still pass the
			// stricter actual transaction validator before this merge is used.
			const bool supported=field && (field->available || (IsStartupPostProcess(patch.id) && patch.value == field->defaultValue));
			if(field && IsGraphicsAASetting(patch.id) && (field->available || patch.value != field->defaultValue))
			{
				const auto aaReason = ValidateGraphicsAASettingValue(*field, patch.value);
				if(aaReason != GraphicsRequestReason::None)
				{ if(rejectionReason) *rejectionReason = aaReason; detail = "Startup AA/raster field is invalid or unavailable."; return false; }
			}
			if(!visited.insert(patch.id).second || !field || !supported || !field->gameScope
				|| field->authoringScope != GraphicsAuthoringScope::None || !ValidStartupField(*field,patch.value))
			{ detail="Startup profile contains an unavailable, duplicate, scoped or invalid typed field."; return false; }
			if(IsStartupPostProcess(patch.id))
			{
				const auto found=std::find_if(proposedColors.begin(),proposedColors.end(),[&](const auto& value){return value.id == patch.id;});
				if(found == proposedColors.end()) proposedColors.push_back(patch); else *found=patch;
			}
			else nativePatches.push_back(patch);
		}
		if(!ApplyNativeGraphicsSettings(nativePatches,catalog,proposedNative,proposedCulling,proposedProfile,detail)) return false;
		// Native startup switches and image settings form one candidate. Use the
		// proposed VRS state, not an unrelated current/default renderer, when
		// validating coverage/reconstruction/indexed compatibility before launch.
		auto tupleValues = proposedColors;
		tupleValues.push_back({GraphicsSettingId::VariableRateShading, proposedNative.variableRateShading != 0u});
		GraphicsAAConfiguration configuration;
		const auto spatialReason = ResolveGraphicsAAConfiguration(tupleValues, configuration);
		if(spatialReason != GraphicsRequestReason::None)
		{ if(rejectionReason) *rejectionReason = spatialReason; detail="The startup AA/upscaling/raster tuple is incompatible with its native VRS state."; return false; }
		const auto color=ResolvePostProcessColorSettings(proposedColors);
		if(color.minEV > color.maxEV) { detail="Startup automatic exposure minimum exceeds its maximum."; return false; }
		if(const auto effects = ValidatePostProcessEffectValues(proposedColors); effects != GraphicsRequestReason::None)
		{ if(rejectionReason) *rejectionReason = effects; detail = "Startup camera or screen-space settings are invalid."; return false; }
		native=proposedNative; culling=proposedCulling; profile=proposedProfile; colors=std::move(proposedColors);
		if(rejectionReason) *rejectionReason = GraphicsRequestReason::None;
		detail.clear(); return true;
	}
	enum class StartupColorPreparation { Ready, Waiting, Failed };
	StartupColorPreparation PrepareStartupColor(RenderGraphicsSettingsState& owner, VulkanContext& native,
		std::span<const GraphicsSettingValue> values, std::string& detail, GraphicsRequestReason* failureReason = nullptr)
	{
		try
		{
			owner.startup->colorPreparedGeneration=0;
			if(failureReason) *failureReason = GraphicsRequestReason::PreparationFailed;
			const auto color=ResolvePostProcessColorSettings(values);
			auto tupleValues = std::vector<GraphicsSettingValue>(values.begin(), values.end());
			tupleValues.push_back({GraphicsSettingId::VariableRateShading, native.ReadGraphicsStartupFlags().second});
			GraphicsAAConfiguration configuration;
			const auto tupleReason = ResolveGraphicsAAConfiguration(tupleValues, configuration);
			if(tupleReason != GraphicsRequestReason::None)
			{ if(failureReason) *failureReason = tupleReason; detail = "Startup image/raster tuple does not match the actual native renderer."; return StartupColorPreparation::Failed; }
			if(configuration.raster == GraphicsGeometryRasterMode::Indexed)
			{
				// The live indexed preparer requires an actual sealed publication.
				// Startup has none yet, and color's inactive success is not indexed
				// qualification. Keep the full prior profile until a genuine native
				// prepublication raster factory can certify this configuration.
				if(failureReason) *failureReason = GraphicsRequestReason::Unsupported;
				detail = "Indexed startup graphics require native prepublication raster qualification, which is not available.";
				return StartupColorPreparation::Failed;
			}
			const bool rasterActive = configuration.antiAliasing != GraphicsAntiAliasingMode::Off
				|| configuration.upscaling != GraphicsUpscalingMode::Off;
			PostProcessEffectSettings effects;
			auto effectReason = ResolvePostProcessEffectSettings(values, effects);
			if(effectReason == GraphicsRequestReason::None && effects.RequestedCapabilities() != PostProcessEffectCapability::None)
			{
				const auto mask = native.ReadGraphicsCatalogCapabilities(true).postProcessEffectMask;
				effectReason = ValidatePostProcessEffectCapabilities(effects, static_cast<PostProcessEffectCapability>(mask));
			}
			if(effectReason != GraphicsRequestReason::None)
			{ if(failureReason) *failureReason = effectReason; detail = "The native renderer cannot prepare these startup effects."; return StartupColorPreparation::Failed; }
			if(!color.Active() && !rasterActive && !effects.Active()) return StartupColorPreparation::Ready;
			auto& resources=ResourceManager::Instance();
			bool waiting=false;
			if(color.grading)
				for(const auto& id : {color.curve,color.lut})
				{
					if(id == TerraEngine::EMPTY_UUID || resources.AcquireTextureSnapshot(id)) continue;
					auto lookup=std::find_if(owner.pendingPostProcessLookups.begin(),owner.pendingPostProcessLookups.end(),
						[&](const auto& work){return work.first == id;});
					if(lookup == owner.pendingPostProcessLookups.end())
					{
						const auto path=resources.GetRegisteredAssetPath(id,AssetType::Texture);
						if(path.empty()) { detail="A startup color grading asset is unregistered."; return StartupColorPreparation::Failed; }
						// Initialization may precede the host's normal resource producers in a
						// native fixture. Bootstrap existing workers/admission, never reset the
						// mounted manifest or block a frame waiting for texture decoding.
						ThreadPool::instance().initialize();
						if(!resources.GetRequestDiagnostics().admitting) resources.BeginRequestSession();
						owner.pendingPostProcessLookups.emplace_back(id,resources.LoadTextureAsync(path));
						lookup=std::prev(owner.pendingPostProcessLookups.end());
					}
					if(!lookup->second.valid()) { detail="A startup grading asset has no valid load receipt."; return StartupColorPreparation::Failed; }
					if(lookup->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { waiting=true; continue; }
					if(lookup->second.get() != id || !resources.AcquireTextureSnapshot(id))
					{ detail="A startup grading asset failed to load."; return StartupColorPreparation::Failed; }
				}
			if(waiting) return StartupColorPreparation::Waiting;
			TerraEngine::Renderer::RenderViewPresentationInput view;
			view.output.kind=RenderOutputKind::MainRuntimeView; view.output.diagnosticsView=RenderViewType::Game; view.output.outputId=2;
			// Extent0 explicitly requests the actual initialized native swapchain.
			// This prepares resources before first view, without inventing a camera or
			// marking any output recorded/presented for diagnostics or C# discovery.
			GraphicsSettingsSnapshot candidate;
			candidate.lifecycle=owner.lifecycle;
			candidate.lifecycle.rendererSessionGeneration=native.GetLatestRendererRecoverySnapshot().renderSessionGeneration;
			candidate.lifecycle.backendResourceGeneration=native.GetBackendResourceGeneration();
			candidate.values = std::move(tupleValues);
			candidate.sampleCount = configuration.sampleCount;
			// Extent0 stays a real native-output placeholder. The backend derives
			// internal dimensions using the same scale rule once its output is known.
			candidate.internalWidth = ResolveGraphicsAAInternalDimension(candidate.outputWidth, configuration);
			candidate.internalHeight = ResolveGraphicsAAInternalDimension(candidate.outputHeight, configuration);
			if(!native.PreparePostProcessCandidate(view,candidate,detail)) return StartupColorPreparation::Failed;
			owner.startup->colorPreparedGeneration=candidate.lifecycle.backendResourceGeneration;
			return StartupColorPreparation::Ready;
		}
		catch(const std::exception& error)
		{
			detail=std::string("Startup color preparation failed: ") + error.what();
			return StartupColorPreparation::Failed;
		}
	}

	GraphicsRequestReceipt StartupReceipt(const GraphicsStartupRecord& record,
		GraphicsRequestState state, GraphicsRequestReason reason, std::string detail = {})
	{
		GraphicsRequestReceipt receipt;
		receipt.requestId = record.transaction.requestId;
		receipt.sequence = receipt.requestId & ~(GraphicsRequestId{1} << 63);
		receipt.requestedRevision = record.transaction.request.baseRevision;
		receipt.state = state;
		receipt.activation = GraphicsActivation::NextLaunch;
		receipt.reason = reason;
		receipt.detail = std::move(detail);
		for(const auto& value : record.transaction.request.patches) receipt.affectedSettings.push_back(value.id);
		return receipt;
	}

	// A launch failure is not a durable terminal outcome. Retain the last real
	// disk snapshot and expose the failure separately through the runtime facade.
	void NoteStartupFailure(RenderGraphicsSettingsState& owner, GraphicsStartupStoreResult failure,
		std::optional<GraphicsRequestReceipt> receipt = {})
	{
		owner.lastPreparationFailure = failure.reason;
		owner.startup->launchFailure = std::move(failure);
		owner.startup->launchFailureReceipt = std::move(receipt);
		TERRAENGINE_LOG(LogLevel::Warning, "Startup graphics persistence failed. " << owner.startup->launchFailure->detail);
	}

	void RefreshStartupSnapshot(RenderGraphicsSettingsState::Startup& startup)
	{
		// Read publishes only on success. A failed reload must never turn an old
		// local cache into the authoritative seed for future checked writes.
		startup.snapshotValid = startup.store->Read(startup.state).succeeded;
	}
}

void RenderSystem::PrepareGraphicsStartupAtProtectedBoundary()
{
	auto& owner = *graphicsSettingsState;
	const auto input = ResourceManager::Instance().GetGraphicsStartupInputs();
	owner.startupFinalized = false;
	owner.startup.reset();
	owner.profile = {input.textureBudgetMegabytes, input.profileResolutionScale};
	if(input.authoredSettings)
	{
		// Preserve the exported manifest and project defaults. Initialization and
		// player replay use transient renderer state without the authoring setter.
		SetTransientNativeGraphicsSettings(*input.authoredSettings, QualityPreset::Custom, taskShaderCullingEnabled);
	}
	else
	{
		LoadSettings(!input.exportRuntime);
	}
	currentSettings.validationEnabled = ResourceManager::Instance().IsRenderValidationEnabled() ? 1u : 0u;
	owner.authoredDefaults = currentSettings;
	if(!input.exportRuntime || input.productIdentity.empty()) return;

	auto& startup = owner.startup.emplace();
	startup.identity = input.productIdentity;
	startup.priorSettings = currentSettings;
	startup.priorTaskCulling = taskShaderCullingEnabled;
	startup.authoredTaskCulling = taskShaderCullingEnabled;
	try
	{
		startup.dataRoot = ResolveGraphicsDataRoot(input);
		startup.store = OpenGraphicsStartupStore(startup.dataRoot, startup.identity);
		if(!startup.store) return;
		GraphicsStartupStoreResult leaseResult;
		startup.replayLease = TryAcquireGraphicsStartupReplayLease(startup.dataRoot, startup.identity, leaseResult);
		if(!startup.replayLease)
		{
			NoteStartupFailure(owner, std::move(leaseResult));
			return;
		}
		const auto loaded = startup.store->Read(startup.state);
		if(!loaded.succeeded)
		{
			NoteStartupFailure(owner, loaded);
			startup.replayLease.reset();
			return;
		}
		startup.snapshotValid = true;
		// Native initialization is the only place that can prove support for a
		// startup switch. This first pass checks the serialized values; the ready
		// candidate below verifies the selected device and active instance state.
		GraphicsCatalogCapabilities provisional;
		provisional.vrsSupported = true;
		provisional.validationSupported = true;
		provisional.postProcessColorSupported = true;
		provisional.spatialAAModeMask = 0xFu;
		// Provisional flags validate wire syntax only before a device exists.
		// Actual discovery and exact native preparation govern all promotion.
		provisional.indexedRasterSupported = provisional.msaa4Supported = provisional.taaSupported = true;
		provisional.temporalUpscalingSupported = provisional.temporalSharpeningSupported = true;
		// Reserve the registered camera effect bits for serialized value checks.
		provisional.postProcessEffectMask = (static_cast<std::uint32_t>(PostProcessEffectCapability::OutputDithering) << 1u) - 1u;
		const auto catalog = BuildGraphicsSettingsCatalog(*owner.authoredDefaults, taskShaderCullingEnabled,
			owner.profile, owner.sceneSky, provisional);
		std::string detail;
		auto profile = owner.profile;
		startup.priorColorValues=StartupPostProcessDefaults(catalog);
		const bool validPrior = startup.state.committedProfile.empty()
			|| ApplyMixedStartupValues(startup.state.committedProfile,catalog,
				startup.priorSettings,startup.priorTaskCulling,profile,startup.priorColorValues,detail);
		if(!validPrior)
			TERRAENGINE_LOG(LogLevel::Warning, "Using authored startup graphics settings. " << detail);
		SetTransientNativeGraphicsSettings(startup.priorSettings, QualityPreset::Custom, startup.priorTaskCulling);
		if(!startup.state.pending)
		{
			startup.replayLease.reset();
			return;
		}

		const auto pending = *startup.state.pending;
		const auto reject = [&](GraphicsRequestReason reason, std::string message) {
			const auto receipt = StartupReceipt(pending, GraphicsRequestState::Rejected, reason, std::move(message));
			const auto result = startup.store->Reject(receipt.requestId, startup.state.storeRevision, receipt);
			if(result.succeeded)
			{
				startup.state.storeRevision = result.storeRevision;
				startup.state.pending.reset();
				startup.state.terminalReceipts.push_back(receipt);
			}
			else
			{
				auto failed = receipt;
				failed.reason = result.reason;
				failed.detail = result.detail;
				NoteStartupFailure(owner, result, std::move(failed));
				RefreshStartupSnapshot(startup);
			}
			startup.replayLease.reset();
		};
		if(pending.claimState == GraphicsStartupClaimState::Claimed)
		{
			reject(GraphicsRequestReason::InterruptedStartup, "A previous launch stopped after claiming this request.");
			return;
		}
		// Store revision serializes claim ownership across processes. A process
		// losing this checked update cannot prepare or promote the pending profile.
		const auto claimGeneration = startup.state.storeRevision + 1u;
		const auto claim = startup.store->Claim(pending.transaction.requestId, claimGeneration, startup.state.storeRevision);
		if(!claim.succeeded)
		{
			NoteStartupFailure(owner, claim, StartupReceipt(pending, GraphicsRequestState::Rejected, claim.reason, claim.detail));
			RefreshStartupSnapshot(startup);
			startup.replayLease.reset();
			return;
		}
		startup.state.storeRevision = claim.storeRevision;
		startup.state.pending->claimState = GraphicsStartupClaimState::Claimed;
		startup.state.pending->claimGeneration = claimGeneration;
		auto values = CaptureNativeGraphicsSettings(startup.priorSettings, startup.priorTaskCulling, owner.profile, owner.sceneSky);
		values.insert(values.end(),startup.priorColorValues.begin(),startup.priorColorValues.end());
		for(const auto& condition : pending.transaction.fieldPreconditions)
		{
			const auto found = std::find(values.begin(), values.end(), condition);
			if(found != values.end()) continue;
			reject(GraphicsRequestReason::StaleRevision, "Authoring or the committed startup profile changed a staged field.");
			return;
		}
		auto reason = ValidateGraphicsStartupTransaction(pending.transaction, catalog);
		auto candidateSettings = startup.priorSettings;
		bool candidateTaskCulling = startup.priorTaskCulling;
		startup.candidateColorValues=startup.priorColorValues;
		GraphicsRequestReason mergeReason = GraphicsRequestReason::InvalidValue;
		if(reason != GraphicsRequestReason::None || !ApplyMixedStartupValues(pending.transaction.request.patches,
			catalog,candidateSettings,candidateTaskCulling,profile,startup.candidateColorValues,detail,&mergeReason))
		{
			reject(reason == GraphicsRequestReason::None ? mergeReason : reason, std::move(detail));
			return;
		}
		// An invalid retained profile was never applied. Do not silently carry its
		// rejected values into a newly committed, otherwise valid transaction.
		startup.candidateProfile = validPrior ? startup.state.committedProfile : std::vector<GraphicsSettingValue>{};
		for(const auto& patch : pending.transaction.request.patches)
		{
			std::erase_if(startup.candidateProfile, [&](const auto& entry) { return entry.id == patch.id; });
			startup.candidateProfile.push_back(patch);
		}
		startup.candidate = true;
		SetTransientNativeGraphicsSettings(candidateSettings, QualityPreset::Custom, candidateTaskCulling);
	}
	catch(const std::exception& error)
	{
		// Bad identities, unavailable save directories and store construction are
		// ordinary persistence failures, never a reason to lose authored rendering.
		NoteStartupFailure(owner, {false, 0, GraphicsRequestReason::StorageFailed, error.what()});
		startup.snapshotValid = false;
		startup.candidate = false;
		startup.replayLease.reset();
		SetTransientNativeGraphicsSettings(startup.priorSettings, QualityPreset::Custom, startup.priorTaskCulling);
	}
}

void RenderSystem::InitializeConfiguredVulkan()
{
	// Deferred initialization reuses its existing owner. Never overwrite native
	// allocations until TryReleaseCaptureBackend has proven retirement.
	if(!rhi)
	{
		auto native = std::make_unique<VulkanContext>();
		native->SetValidationLayersRequested(currentSettings.validationEnabled != 0);
		native->SetVariableRateShadingEnabled(currentSettings.variableRateShading != 0);
		rhi = std::move(native);
	}
	try
	{
		rhi->Initialize(WindowSystem::Instance());
	}
	catch(...)
	{
		// Candidate failure may still leave native allocations. The fallback
		// finalizer requires the normal shutdown proof before replacing them.
		auto& startup = graphicsSettingsState->startup;
		if(!startup) throw;
		if(!startup->candidate)
		{
			if(startup->recoveryAttempted) throw;
			startup->recoveryAttempted = true;
			if(!TryReleaseCaptureBackend()) throw std::runtime_error("Startup recovery still owns Vulkan resources.");
			// One bounded supported recovery attempt is independent of persisted
			// switches. This never rewrites the player's last committed profile.
			auto recovery = *graphicsSettingsState->authoredDefaults;
			recovery.validationEnabled = 0u;
			recovery.variableRateShading = 0u;
			SetTransientNativeGraphicsSettings(recovery, QualityPreset::Custom, startup->authoredTaskCulling);
			startup->priorSettings = currentSettings;
			startup->priorTaskCulling = taskShaderCullingEnabled;
			startup->priorColorValues.clear();
			startup->colorPreparedGeneration=0;
			graphicsSettingsState->lastPreparationFailure = GraphicsRequestReason::PreparationFailed;
			TERRAENGINE_LOG(LogLevel::Warning, "Retained graphics startup profile failed; trying authored recovery with optional startup features disabled.");
			InitializeConfiguredVulkan();
			return;
		}
	}
	initialized = static_cast<VulkanContext*>(rhi.get())->IsReady();
}

void RenderSystem::FinalizeGraphicsStartupAtProtectedBoundary()
{
	auto& owner = *graphicsSettingsState;
	if(owner.startupFinalized || !rhi) return;
	auto* native = static_cast<VulkanContext*>(rhi.get());
	if(!initialized && native->IsInitializationDeferred()) return;
	const auto capabilities = [&] {
		auto result = static_cast<VulkanContext*>(rhi.get())->ReadGraphicsCatalogCapabilities(
			!editorModeEnabled);
		result.developmentDiagnostics = editorModeEnabled;
		return result;
	};
	auto& startup = owner.startup;
	if(startup && startup->candidate)
	{
		const auto pending = *startup->state.pending;
		auto reason = GraphicsRequestReason::PreparationFailed;
		if(initialized && !StartupFixtureRejectsPreparation())
		{
			const auto actualCatalog = BuildGraphicsSettingsCatalog(*owner.authoredDefaults, startup->priorTaskCulling,
				owner.profile, owner.sceneSky, capabilities());
			reason = ValidateGraphicsStartupTransaction(pending.transaction, actualCatalog);
			const auto [validation, vrs] = native->ReadGraphicsStartupFlags();
			const auto stages = [&](GraphicsSettingId id) {
				return std::any_of(pending.transaction.request.patches.begin(), pending.transaction.request.patches.end(),
					[id](const auto& patch) { return patch.id == id; });
			};
			if(reason == GraphicsRequestReason::None
				&& ((stages(GraphicsSettingId::ValidationEnabled) && validation != (currentSettings.validationEnabled != 0))
					|| (stages(GraphicsSettingId::VariableRateShading) && vrs != (currentSettings.variableRateShading != 0))))
				reason = GraphicsRequestReason::Unsupported;
			if(reason == GraphicsRequestReason::None)
			{
				auto checkedNative=startup->priorSettings; bool checkedCulling=startup->priorTaskCulling;
				auto checkedProfile=owner.profile; auto checkedColors=startup->priorColorValues;
				std::string detail;
				GraphicsRequestReason mergeReason = GraphicsRequestReason::InvalidValue;
				if(!ApplyMixedStartupValues(startup->candidateProfile,actualCatalog,checkedNative,checkedCulling,checkedProfile,checkedColors,detail,&mergeReason))
					reason=mergeReason;
				else
				{
					GraphicsRequestReason preparationReason = GraphicsRequestReason::PreparationFailed;
					const auto prepared=PrepareStartupColor(owner,*native,startup->candidateColorValues,detail,&preparationReason);
					if(prepared == StartupColorPreparation::Waiting) return;
					if(prepared == StartupColorPreparation::Failed) reason=preparationReason;
				}
				if(!detail.empty()) TERRAENGINE_LOG(LogLevel::Warning,"Startup color preparation failed. " << detail);
			}
		}
		auto receipt = StartupReceipt(pending, GraphicsRequestState::Rejected, reason, "Startup graphics preparation failed.");
		if(reason == GraphicsRequestReason::None)
		{
			receipt.state = GraphicsRequestState::Applied;
			receipt.detail.clear();
			receipt.effectiveRevision = 1u;
			receipt.appliedFrameId = nextRenderFrameId;
			const auto promoted = startup->store->Promote(receipt.requestId, pending.claimGeneration,
				startup->state.storeRevision, startup->candidateProfile, receipt);
			if(promoted.succeeded)
			{
				startup->state.storeRevision = promoted.storeRevision;
				startup->state.committedProfile = std::move(startup->candidateProfile);
				// Future staging preconditions inherit this successful native profile,
				// rather than the values that preceded this launch's transaction.
				startup->priorSettings = currentSettings;
				const auto [validation, vrs] = native->ReadGraphicsStartupFlags();
				startup->priorSettings.validationEnabled = validation ? 1u : 0u;
				startup->priorSettings.variableRateShading = vrs ? 1u : 0u;
				startup->priorTaskCulling = taskShaderCullingEnabled;
				startup->priorColorValues=startup->candidateColorValues;
				startup->state.pending.reset();
				startup->state.terminalReceipts.push_back(receipt);
			}
			else
			{
				reason = promoted.reason == GraphicsRequestReason::None ? GraphicsRequestReason::StorageFailed : promoted.reason;
				receipt = StartupReceipt(pending, GraphicsRequestState::Rejected, reason, promoted.detail);
			}
		}
		if(reason != GraphicsRequestReason::None)
		{
			const auto rejected = startup->store->Reject(receipt.requestId, startup->state.storeRevision, receipt);
			if(rejected.succeeded)
			{
				startup->state.storeRevision = rejected.storeRevision;
				startup->state.pending.reset();
				startup->state.terminalReceipts.push_back(receipt);
			}
			else
			{
				// Failed IO leaves the real claim pending. Report this launch's
				// failure separately and reserve those fields until recovery.
				receipt.reason = rejected.reason;
				receipt.detail = rejected.detail;
				NoteStartupFailure(owner, rejected, receipt);
				RefreshStartupSnapshot(*startup);
			}
			native->DiscardPostProcessCandidates();
			startup->colorPreparedGeneration=0;
			if(!TryReleaseCaptureBackend()) throw std::runtime_error("Failed startup candidate still owns Vulkan resources.");
			SetTransientNativeGraphicsSettings(startup->priorSettings, QualityPreset::Custom, startup->priorTaskCulling);
			startup->candidate = false;
			InitializeConfiguredVulkan();
		}
		startup->candidate = false;
		startup->replayLease.reset();
	}
	if(!initialized) return;
	native=static_cast<VulkanContext*>(rhi.get());
	if(startup)
	{
		const auto catalog=BuildGraphicsSettingsCatalog(*owner.authoredDefaults,startup->authoredTaskCulling,
			owner.profile,owner.sceneSky,capabilities());
		if(startup->priorColorValues.empty()) startup->priorColorValues=StartupPostProcessDefaults(catalog);
		std::string detail;
		// A retained profile is also prepared on ordinary subsequent launches.
		// Its old certificate never survives replacement of the Vulkan backend.
		if(startup->colorPreparedGeneration != native->GetBackendResourceGeneration())
		{
			bool supported=true;
			for(const auto& field : startup->priorColorValues)
			{
				const auto* descriptor=FindGraphicsSetting(catalog,field.id);
				if(!descriptor || !ValidStartupField(*descriptor,field.value)
					|| (!descriptor->available && field.value != descriptor->defaultValue)) { supported=false; break; }
			}
			const auto prepared=supported ? PrepareStartupColor(owner,*native,startup->priorColorValues,detail) : StartupColorPreparation::Failed;
			if(prepared == StartupColorPreparation::Waiting) return;
			if(prepared == StartupColorPreparation::Failed)
			{
				// The previous full profile is not viable on this device. Recover
				// native switches and color together; never rewrite its durable record.
				if(startup->recoveryAttempted) throw std::runtime_error("Authored startup color recovery failed: " + detail);
				startup->recoveryAttempted=true;
				native->DiscardPostProcessCandidates();
				if(!TryReleaseCaptureBackend()) throw std::runtime_error("Startup color recovery still owns Vulkan resources.");
				auto recovery=*owner.authoredDefaults; recovery.validationEnabled=0; recovery.variableRateShading=0;
				SetTransientNativeGraphicsSettings(recovery,QualityPreset::Custom,startup->authoredTaskCulling);
				startup->priorSettings=currentSettings; startup->priorTaskCulling=taskShaderCullingEnabled;
				startup->priorColorValues=StartupPostProcessDefaults(catalog); startup->colorPreparedGeneration=0;
				owner.lastPreparationFailure=GraphicsRequestReason::PreparationFailed;
				TERRAENGINE_LOG(LogLevel::Warning,"Retained startup color profile failed; restoring authored native/color recovery. " << detail);
				InitializeConfiguredVulkan();
				if(!initialized) return;
				native=static_cast<VulkanContext*>(rhi.get());
			}
		}
	}

	const auto& scene = SceneManager::Instance().GetActiveSceneRenderSettings();
	owner.sceneSky = {scene.indirectStrength, scene.ambientOcclusionStrength, scene.ambientOcclusionQuality};
	auto descriptors = BuildGraphicsSettingsCatalog(*owner.authoredDefaults,
		taskShaderCullingEnabled, owner.profile, owner.sceneSky, capabilities());

	const auto projectRoot = ResourceManager::Instance().GetProjectRoot();
	std::lock_guard lock(owner.mutex);
	if(owner.projectRoot != projectRoot)
	{
		owner.projectRoot = projectRoot;
		++owner.lifecycle.projectGeneration;
	}
	owner.lifecycle.playSessionGeneration = owner.requestedPlaySessionGeneration;
	owner.lifecycle.rendererSessionGeneration = rhi->GetLatestRendererRecoverySnapshot().renderSessionGeneration;
	owner.lifecycle.backendResourceGeneration = rhi->GetBackendResourceGeneration();
	owner.catalog = std::make_shared<const std::vector<GraphicsSettingDescriptor>>(std::move(descriptors));
	owner.presets = std::make_shared<const std::vector<GraphicsPresetDefinition>>(BuildGraphicsPresetDefinitions());
	owner.nativeCatalog = true;
	owner.configured = true;
	const GraphicsPreparationContext context{owner.lifecycle, *owner.catalog, owner.targets, nextRenderFrameId, *owner.presets};
	owner.controller.Invalidate(context);
	auto effective = currentSettings;
	const auto [validation, vrs] = static_cast<VulkanContext*>(rhi.get())->ReadGraphicsStartupFlags();
	effective.validationEnabled = validation ? 1u : 0u;
	effective.variableRateShading = vrs ? 1u : 0u;
	if(startup)
	{
		// Field preconditions for the next Stage use actual native values, including
		// supported recovery or device-disabled optional features on this launch.
		startup->priorSettings = effective;
		startup->priorTaskCulling = taskShaderCullingEnabled;
	}
	auto seedValues=CaptureNativeGraphicsSettings(effective,taskShaderCullingEnabled,owner.profile,owner.sceneSky);
	const auto colorValues=startup ? startup->priorColorValues : StartupPostProcessDefaults(*owner.catalog);
	seedValues.insert(seedValues.end(),colorValues.begin(),colorValues.end());
	owner.controller.RefreshNativeValues(context,seedValues,currentPreset,startup ? startup->colorPreparedGeneration : 0);
	owner.observedNativeGeneration = GetRuntimeRenderSettingsGeneration();
	if(startup && startup->store)
	{
		try
		{
			owner.persistence = std::make_shared<GraphicsSettingsPersistence>(startup->dataRoot, startup->identity, true,
				startup->snapshotValid ? std::optional(startup->state) : std::nullopt);
			if(startup->launchFailure)
				owner.persistence->RecordStartupFailure(*startup->launchFailure, startup->launchFailureReceipt);
		}
		catch(const std::exception& error)
		{
			owner.persistence.reset();
			NoteStartupFailure(owner, {false, 0, GraphicsRequestReason::StorageFailed, error.what()});
		}
	}
	else if(editorModeEnabled)
	{
		// Editor preferences remain isolated from any exported product transaction.
		const auto input = ResourceManager::Instance().GetGraphicsStartupInputs();
		try
		{
			owner.editorPreferenceDataRoot = ResolveGraphicsDataRoot(input);
			owner.persistence = std::make_shared<GraphicsSettingsPersistence>(
				owner.editorPreferenceDataRoot, "editor:" + projectRoot.generic_string(), false);
		}
		catch(const std::exception& error)
		{
			// Missing writable editor preferences cannot prevent rendering or cause
			// an authored-settings write as an accidental recovery side effect.
			owner.persistence.reset();
			owner.lastPreparationFailure = GraphicsRequestReason::StorageFailed;
			TERRAENGINE_LOG(LogLevel::Warning, "Editor graphics preferences unavailable. " << error.what());
		}
	}
	owner.startupFinalized = true;
	++owner.epoch;
}
