// Protected scene mutations for the managed post-process authoring API.
#include "Frame/RenderFrameState.h"
#include "Components/CameraComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/SceneManager.h"
#include "Scene/Serializer.h"
#include "Scene/PostProcessVolumeComponent.h"
#include "Resource/ResourceManager.h"
#include "Renderer/GraphicsSettings/PostProcessViewAuthoring.h"
#include "Renderer/RHI/Vulkan/VulkanContext.h"
#include "Resource/PostProcessEffectAsset.h"
#include "ThreadPool/ThreadPool.h"
#include <chrono>

using namespace TerraEngine::Renderer::GraphicsSettings;

namespace
{
	enum class AuthoringAssetLoadState { Pending, Ready, Failed };

	// A loader publishes its immutable owner before completing its receipt.
	// Poll once at the boundary and leave unfinished disk work on its own job.
	AuthoringAssetLoadState ReadAuthoringAssetLoad(std::shared_future<TerraEngine::UUID> load,
		const TerraEngine::UUID& expected)
	{
		if(!load.valid()) return AuthoringAssetLoadState::Failed;
		if(load.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return AuthoringAssetLoadState::Pending;
		try { return load.get() == expected ? AuthoringAssetLoadState::Ready : AuthoringAssetLoadState::Failed; }
		catch(const std::exception&) { return AuthoringAssetLoadState::Failed; }
	}

	AuthoringAssetLoadState PreflightAuthoredProfile(ResourceManager& resources, const TerraEngine::UUID& id)
	{
		if(id == TerraEngine::EMPTY_UUID) return AuthoringAssetLoadState::Ready;
		const auto profile = resources.AcquirePostProcessProfileSnapshot(id);
		if(!profile)
		{
			const auto state = ReadAuthoringAssetLoad(resources.LoadPostProcessProfileAsync(id), id);
			return state == AuthoringAssetLoadState::Ready ? AuthoringAssetLoadState::Pending : state;
		}
		for(const auto& effectId : profile->customEffects)
		{
			const auto effect = resources.AcquirePostProcessEffectSnapshot(effectId);
			if(!effect)
			{
				const auto state = ReadAuthoringAssetLoad(resources.LoadPostProcessEffectAsync(effectId), effectId);
				return state == AuthoringAssetLoadState::Ready ? AuthoringAssetLoadState::Pending : state;
			}
			const auto shaderId = effect->descriptor.shaderAssetId;
			const auto shader = resources.AcquirePostProcessShaderStageSnapshot(shaderId);
			if(!shader)
			{
				const auto state = ReadAuthoringAssetLoad(resources.LoadPostProcessShaderStageAsync(shaderId), shaderId);
				return state == AuthoringAssetLoadState::Ready ? AuthoringAssetLoadState::Pending : state;
			}
			std::vector<TerraEngine::Renderer::CustomPostProcessParameterValue> defaults;
			for(const auto& parameter : effect->descriptor.parameters)
				defaults.push_back({parameter.layout.name, parameter.defaultValue});
			TerraEngine::Renderer::PostProcessCustomPassRegistration registration;
			std::string detail;
			if(!TerraEngine::Resource::PreparePostProcessEffectRegistration(*effect, shaderId, shader, false,
				defaults, registration, detail)) return AuthoringAssetLoadState::Failed;
		}
		return AuthoringAssetLoadState::Ready;
	}
}

bool PostProcessAuthoringFrame::HasEnabledEffects() const noexcept
{
	return effects && std::any_of(effects->passes.begin(), effects->passes.end(),
		[](const auto& pass) { return pass.enabled; });
}

PostProcessAuthoringController& RenderSystem::GetPostProcessAuthoringController()
{
	return graphicsSettingsState->postProcessAuthoring;
}

PostProcessAuthoringAdmission RenderSystem::SubmitPostProcessAuthoring(
	const TerraEngine::Scripting::PostProcessAuthoringWire::Request& request)
{
	auto& owner = *graphicsSettingsState;
	std::lock_guard lock(owner.mutex);
	// Reserve the retained intent slot before admission. Allocation failure
	// must not leave an accepted request with no protected scene producer.
	owner.pendingAuthoringSceneWrites.reserve(owner.pendingAuthoringSceneWrites.size() + 1u);
	auto admission = owner.postProcessAuthoring.Submit(request);
	if(admission.sceneIntent)
		owner.pendingAuthoringSceneWrites.push_back({*admission.sceneIntent, {}});
	return admission;
}

void RenderSystem::SettlePostProcessAuthoringSceneWrites(std::uint64_t frameId)
{
	auto& owner = *graphicsSettingsState;
	std::lock_guard lock(owner.mutex);
	if(owner.pendingAuthoringSceneWrites.empty()) return;
	auto& objects = ObjectManager::instance();
	auto& resources = ResourceManager::Instance();
	auto& scenes = SceneManager::Instance();
	auto& queue = TerraEngine::Scene::SceneMutationQueue::Instance();
	std::erase_if(owner.pendingAuthoringSceneWrites, [&](auto& pending) {
		const auto& intent = pending.intent;
		const auto current = std::find_if(owner.publishedCameraViews.begin(), owner.publishedCameraViews.end(),
			[&](const auto& view) { return view.target == intent.target; });
		if(current == owner.publishedCameraViews.end() || intent.lifecycle != owner.lifecycle)
		{
			if(!pending.transactionId.empty()) queue.CancelTransaction(pending.transactionId);
			owner.postProcessAuthoring.RejectRequest(intent.requestId, GraphicsRequestReason::StaleLifetime,
				"The authored camera changed before the protected scene write completed.");
			return true;
		}
		if(!pending.transactionId.empty())
		{
			const auto receipt = queue.TakeCompletedAutomationTransactionReceipt(pending.transactionId);
			if(!receipt) return false;
			const bool accepted = receipt->status == TerraEngine::Scene::TransactionStatus::Committed
				&& receipt->rejectedWriteCount == 0;
			owner.postProcessAuthoring.ReportSceneTransaction(intent.requestId, intent.target,
				intent.lifecycle, accepted, accepted ? frameId : 0u);
			return true;
		}
		// This is the same protected camera capture used for profile resolution.
		// Managed callers never inspect or mutate live component storage.
		const auto camera = CapturePostProcessCameraProfile(objects, resources, scenes, intent.target, intent.lifecycle);
		if(!camera.capture)
		{
			owner.postProcessAuthoring.RejectRequest(intent.requestId, GraphicsRequestReason::StaleLifetime,
				"The exact protected camera component is no longer available.");
			return true;
		}
		// Check the candidate package dependencies before mutating the scene so
		// a broken reference can preserve the previous authored assignment.
		const auto ready = PreflightAuthoredProfile(resources, intent.profileAssetId);
		if(ready == AuthoringAssetLoadState::Pending) return false;
		if(ready == AuthoringAssetLoadState::Failed)
		{
			owner.postProcessAuthoring.RejectRequest(intent.requestId, GraphicsRequestReason::PreparationFailed,
				"The profile, effect descriptor, or compiled shader dependency could not be validated.");
			return true;
		}
		try
		{
			const auto transactionId = "post-process-profile-" + std::to_string(intent.requestId);
			auto transaction = TerraEngine::Scene::TransactionalMutationBuilder(transactionId,
				TerraEngine::Scene::TransactionOrigin::Automation, "Managed post-process profile")
				.ObserveScene(objects).ObserveObject(objects, intent.target.camera->cameraId)
				.UpdateComponentProperty(intent.target.camera->cameraId, std::string(CameraComponent::kComponentName),
					"PostProcessProfileId", TerraEngine::Reflection::Variant{intent.profileAssetId}).Build();
			pending.transactionId = transactionId;
			queue.EnqueueTransaction(std::move(transaction));
		}
		catch(const std::exception&)
		{
			owner.postProcessAuthoring.RejectRequest(intent.requestId, GraphicsRequestReason::PreparationFailed,
				"The typed authored camera profile transaction could not be queued.");
			return true;
		}
		return false;
	});
}

void RenderSystem::PreparePostProcessAuthoringViews(
	const TerraEngine::Renderer::RenderFramePublication& publication, VulkanContext& native)
{
	using namespace TerraEngine::Renderer;
	namespace Wire = TerraEngine::Scripting::PostProcessAuthoringWire;
	auto& owner = *graphicsSettingsState;
	std::lock_guard lock(owner.mutex);
	if(publication.rendererSessionGeneration != owner.lifecycle.rendererSessionGeneration
		|| publication.backendResourceGeneration != owner.lifecycle.backendResourceGeneration) return;
	auto& objects = ObjectManager::instance();
	auto& resources = ResourceManager::Instance();
	auto& scenes = SceneManager::Instance();
	auto& controller = owner.postProcessAuthoring;
	const auto inherited = owner.controller.Snapshot();
	if(!inherited) return;
	for(const auto& previous : owner.postProcessAuthoringTargets)
		if(std::none_of(owner.publishedCameraViews.begin(), owner.publishedCameraViews.end(),
			[&](const auto& view) { return view.target == previous; }))
			if(const auto retired = controller.Read(previous); retired.snapshot)
				controller.RetireView(previous, retired.snapshot->lifecycle);
	std::erase_if(owner.preparedAuthoringViews, [&](const auto& previous) {
		return previous->lifecycle != owner.lifecycle
			|| std::none_of(owner.publishedCameraViews.begin(), owner.publishedCameraViews.end(),
				[&](const auto& view) { return view.target == previous->target; });
	});
	std::erase_if(owner.acceptedAuthoringViews, [&](const auto& previous) {
		return previous.source->lifecycle != owner.lifecycle
			|| std::none_of(owner.publishedCameraViews.begin(), owner.publishedCameraViews.end(),
				[&](const auto& view) { return view.target == previous.source->target; });
	});
	std::shared_ptr<const TerraEngine::Resource::PostProcessProfile> sceneProfile;
	if(const auto* document = scenes.GetActiveScene())
	{
		const auto settings = document->parsedJson.find("sceneSettings");
		if(settings != document->parsedJson.end())
		{
			const auto id = Serializer::DeserializeScenePostProcessProfileId(*settings);
			if(!id) return;
			if(*id != TerraEngine::EMPTY_UUID)
			{
				sceneProfile = resources.AcquirePostProcessProfileSnapshot(*id);
				if(!sceneProfile) { resources.LoadPostProcessProfileAsync(*id); return; }
			}
		}
	}
	// Empty volume storage is the common case. It avoids a scene-wide capture
	// for each ordinary camera while keeping volume-only camera views eligible.
	const bool hasVolumeComponents = !std::as_const(objects.getRegistry()).view<const PostProcessVolumeComponent>().empty();
	owner.postProcessAuthoringTargets.clear();
	for(const auto& live : owner.publishedCameraViews)
	{
		if(!live.target.camera || !live.target.view) continue;
		// A transient capture borrows the authored camera configuration but
		// does not register a second externally selectable editing target.
		if(live.target.view->outputKind == RenderOutputKind::OffscreenTexture) continue;
		owner.postProcessAuthoringTargets.push_back(live.target);
		if(live.publicationIndex >= publication.presentationInputs.size()) continue;
		const auto& actual = publication.presentationInputs[live.publicationIndex];
		if(actual.graphicsAuthoring && actual.graphicsAuthoring->target == live.target && actual.graphicsSettings)
		{
			const auto accepted = std::find_if(owner.acceptedAuthoringViews.begin(), owner.acceptedAuthoringViews.end(),
				[&](const auto& view) { return view.source->target == live.target; });
			if(accepted == owner.acceptedAuthoringViews.end() || accepted->source != actual.graphicsAuthoring)
			{
				const auto revision = actual.graphicsAuthoring->authoringRevision;
				const bool recorded = controller.ReportRecordedView(live.target, owner.lifecycle, revision, publication.frameId);
				const auto authority = controller.ReadAcceptedView(live.target);
				// The same authoring revision may pick up a new inherited settings
				// revision. In either case only this native frame certifies its pixels.
				if(recorded || (authority.snapshot && authority.snapshot->revision == revision))
				{
					auto settings = std::make_shared<GraphicsSettingsSnapshot>(*actual.graphicsSettings);
					settings->publishedFrameId = publication.frameId;
					RenderGraphicsSettingsState::AcceptedAuthoringView next{actual.graphicsAuthoring, std::move(settings)};
					if(accepted == owner.acceptedAuthoringViews.end()) owner.acceptedAuthoringViews.push_back(std::move(next));
					else *accepted = std::move(next);
				}
			}
		}
		const auto captured = CapturePostProcessCameraProfile(objects, resources, scenes, live.target, owner.lifecycle);
		if(!captured.capture) continue;
		const auto& camera = *captured.capture;
		if(camera.profileAssetId != TerraEngine::EMPTY_UUID && !camera.profile)
		{
			if(camera.readiness == PostProcessCameraProfileReadiness::Missing)
				resources.LoadPostProcessProfileAsync(camera.profileAssetId);
			continue;
		}
		auto resolvedSettings = owner.controller.Snapshot(live.target);
		if(!resolvedSettings) resolvedSettings = inherited;
		std::vector<TerraEngine::UUID> effectAssets;
		bool hasAuthoredLayers = camera.profile || sceneProfile;
		if(hasAuthoredLayers || hasVolumeComponents)
		{
			const auto capturedVolumes = CapturePostProcessVolumes(objects, resources, scenes,
				{camera.sceneId, live.target.camera->sceneGeneration, owner.lifecycle, actual.referenceFrame.canonicalOrigin});
			if(!capturedVolumes.capture) continue;
			hasAuthoredLayers = hasAuthoredLayers || !capturedVolumes.capture->volumes.empty();
			for(const auto& volume : capturedVolumes.capture->volumes)
				if(volume.readiness == PostProcessVolumeReadiness::ProfileMissing && volume.input.weight > 0.0f)
					resources.LoadPostProcessProfileAsync(volume.profileAssetId);
			if(hasAuthoredLayers)
			{
				auto copied = std::make_shared<PostProcessViewAuthoringInput>();
				copied->inheritedBase = inherited;
				copied->camera = captured.capture;
				copied->volumes = capturedVolumes.capture;
				copied->layers.target = live.target;
				copied->layers.lifecycle = owner.lifecycle;
				copied->layers.cameraLayerMask = camera.cameraLayerMask;
				copied->layers.sceneProfile = sceneProfile;
				copied->layers.descriptors = *owner.catalog;
				for(const auto& setting : resolvedSettings->values)
					if(owner.controller.CameraOverrides(*live.target.camera, setting.id))
						copied->layers.runtimeCameraOverrides.push_back(setting);
				// Saved player controls were admitted in Game scope. Only fields
				// that the catalog also permits on this camera may override a profile.
				for(const auto& setting : inherited->values)
				{
					const auto* descriptor = FindGraphicsSetting(*owner.catalog, setting.id);
					if(!descriptor || !descriptor->gameScope || !descriptor->cameraScope || !descriptor->available
						|| std::find(owner.restoredPlayerSettingIds.begin(), owner.restoredPlayerSettingIds.end(), setting.id)
							== owner.restoredPlayerSettingIds.end()) continue;
					copied->layers.playerAllowedSettings.push_back(setting.id);
					copied->layers.playerOverrides.push_back(setting);
				}
				// Resolve only retained DTOs off the protected scene owner. The
				// coordinator rechecks every generation before preparing Vulkan.
				auto work = ThreadPool::instance().createTask([copied] { return PreparePostProcessViewAuthoring(copied); });
				auto finished = work->getFuture();
				ThreadPool::instance().enqueue(work, copied->volumes->volumes.size());
				auto projected = finished.get();
				if(!projected.candidate || !IsPostProcessViewAuthoringCurrent(*projected.candidate, live.target, owner.lifecycle,
					inherited, objects, resources, scenes)) continue;
				effectAssets = std::move(projected.candidate->resolved.customEffects);
				auto settings = std::make_shared<GraphicsSettingsSnapshot>(std::move(projected.candidate->resolved.settings));
				settings->revision = resolvedSettings->revision;
				settings->internalWidth = resolvedSettings->internalWidth;
				settings->internalHeight = resolvedSettings->internalHeight;
				settings->outputWidth = live.width; settings->outputHeight = live.height;
				settings->sampleCount = resolvedSettings->sampleCount;
				settings->postProcessResourceGeneration = resolvedSettings->postProcessResourceGeneration;
				resolvedSettings = std::move(settings);
			}
		}

		Wire::Snapshot baseline;
		baseline.target = live.target; baseline.lifecycle = owner.lifecycle; baseline.revision = 1u;
		baseline.authoredProfileAssetId = camera.profileAssetId;
		baseline.sceneVisibleFrameId = publication.frameId;
		if(!hasAuthoredLayers && !actual.graphicsAuthoring)
			baseline.effectiveFrameId = publication.frameId;
		bool waitingForAssets = false;
		for(const auto& id : effectAssets)
		{
			auto effect = resources.AcquirePostProcessEffectSnapshot(id);
			if(!effect)
			{
				const auto load = ReadAuthoringAssetLoad(resources.LoadPostProcessEffectAsync(id), id);
				if(load == AuthoringAssetLoadState::Failed)
					if(const auto demand = controller.Read(live.target); demand.snapshot)
						controller.RejectRequestedView(live.target, owner.lifecycle, demand.snapshot->revision,
							GraphicsRequestReason::PreparationFailed, "A profile effect dependency could not be loaded.");
				waitingForAssets = true;
				break;
			}
			const auto& description = effect->descriptor;
			const bool colorOnly = std::all_of(description.inputs.begin(), description.inputs.end(), [](const auto& input) {
				return input.kind == CustomPostProcessInputKind::SourceColor;
			});
			Wire::DescriptorState descriptor{effect, colorOnly,
				colorOnly ? GraphicsRequestReason::None : GraphicsRequestReason::Unsupported,
				colorOnly ? "" : "This native view currently supports SourceColor inputs for custom effects."};
			baseline.descriptors.push_back(std::move(descriptor));
			Wire::EffectState defaults{id, colorOnly, {}};
			for(const auto& parameter : description.parameters)
				defaults.parameters.push_back({parameter.layout.name, parameter.defaultValue});
			baseline.requestedEffects.push_back(std::move(defaults));
		}
		if(waitingForAssets) continue;
		Wire::Diagnostic diagnostic;
		controller.PublishView(baseline, diagnostic);
		const auto demand = controller.Read(live.target);
		if(!demand.snapshot || demand.snapshot->lifecycle != owner.lifecycle) continue;
		const auto reject = [&](std::string detail) {
			controller.RejectRequestedView(live.target, owner.lifecycle, demand.snapshot->revision,
				GraphicsRequestReason::PreparationFailed, std::move(detail));
		};
		const auto previous = std::find_if(owner.preparedAuthoringViews.begin(), owner.preparedAuthoringViews.end(),
			[&](const auto& view) { return view->target == live.target; });
		if(previous != owner.preparedAuthoringViews.end()
			&& (*previous)->authoringRevision == demand.snapshot->revision && (*previous)->inheritedRevision == inherited->revision
			&& (*previous)->sourceSettingsRevision == resolvedSettings->revision
			&& (*previous)->authoredResourceRevision == (camera.profile ? camera.profile->revision : 0u)
			&& std::ranges::equal((*previous)->settings->values, resolvedSettings->values,
				[](const auto& a, const auto& b) { return a.id == b.id && a.value == b.value; })) continue;
		if(!hasAuthoredLayers && demand.snapshot->requestedEffects.empty()
			&& previous == owner.preparedAuthoringViews.end() && demand.snapshot->revision == 1u) continue;
		std::vector<PostProcessCustomPassRegistration> registrations;
		std::string reason;
		for(const auto& effect : demand.snapshot->requestedEffects)
		{
			const auto descriptor = std::find_if(demand.snapshot->descriptors.begin(), demand.snapshot->descriptors.end(),
				[&](const auto& item) { return item.effect && item.effect->descriptor.assetId == effect.assetId; });
			if(descriptor == demand.snapshot->descriptors.end() || !descriptor->available) continue;
			const auto shaderId = descriptor->effect->descriptor.shaderAssetId;
			auto shader = resources.AcquirePostProcessShaderStageSnapshot(shaderId);
			if(!shader)
			{
				if(ReadAuthoringAssetLoad(resources.LoadPostProcessShaderStageAsync(shaderId), shaderId) == AuthoringAssetLoadState::Failed)
				{
					reason = "The custom stage has no valid compiled shader publication.";
					break;
				}
				waitingForAssets = true;
				break;
			}
			PostProcessCustomPassRegistration registration;
			if(!TerraEngine::Resource::PreparePostProcessEffectRegistration(*descriptor->effect, shaderId, std::move(shader),
				effect.enabled, effect.parameters, registration, reason)) break;
			registrations.push_back(std::move(registration));
		}
		if(!reason.empty()) { reject(std::move(reason)); continue; }
		if(waitingForAssets) continue;
		auto ready = std::make_shared<PostProcessAuthoringFrame>();
		ready->target = live.target; ready->lifecycle = owner.lifecycle;
		ready->authoringRevision = demand.snapshot->revision; ready->inheritedRevision = inherited->revision;
		ready->sourceSettingsRevision = resolvedSettings->revision;
		ready->authoredResourceRevision = camera.profile ? camera.profile->revision : 0u;
		ready->preparedFromFrameId = publication.frameId;
		CustomPostProcessAvailability availability;
		availability.inputMasks.fill(1u << static_cast<std::uint32_t>(CustomPostProcessInputKind::SourceColor));
		for(const auto& registration : registrations) availability.availableAssets.push_back(registration.descriptor.shaderAssetId);
		CustomPostProcessDiagnostic nativeDiagnostic;
		if(!TryPreparePostProcessCustomPasses(registrations, availability, ready->effects, nativeDiagnostic))
		{ reject(nativeDiagnostic.detail.data()); continue; }
		auto settings = std::make_shared<GraphicsSettingsSnapshot>(*resolvedSettings);
		settings->target = live.target;
		settings->outputWidth = live.width;
		settings->outputHeight = live.height;
		GraphicsAAConfiguration aa;
		if(ResolveGraphicsAAConfiguration(settings->values, aa) != GraphicsRequestReason::None)
		{ reject("The authored camera settings have an incompatible AA configuration."); continue; }
		settings->sampleCount = aa.sampleCount;
		settings->internalWidth = ResolveGraphicsAAInternalDimension(live.width, aa);
		settings->internalHeight = ResolveGraphicsAAInternalDimension(live.height, aa);
		settings->customPostProcessStages = 0u;
		for(const auto& pass : ready->effects->passes)
			if(pass.enabled) settings->customPostProcessStages |= 1u << static_cast<std::uint32_t>(pass.descriptor.stage);
		auto candidate = actual;
		candidate.graphicsView = live.target.view;
		candidate.graphicsAuthoring = ready;
		candidate.graphicsSettings = settings;
		if(!native.PrepareFixedExposureView(candidate, *settings, reason))
		{ reject(std::move(reason)); continue; }
		settings->postProcessResourceGeneration = owner.lifecycle.backendResourceGeneration;
		ready->settings = std::move(settings);
		if(previous == owner.preparedAuthoringViews.end()) owner.preparedAuthoringViews.push_back(std::move(ready));
		else *previous = std::move(ready);
	}
}
