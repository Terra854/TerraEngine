#include "PostProcessViewAuthoring.h"
#include "Components/CameraComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/WorkerSafety.h"
#include "Resource/ResourceManager.h"
#include "Scene/SceneManager.h"
#include <algorithm>
#include <cmath>

namespace TerraEngine::Renderer::GraphicsSettings
{
	namespace
	{
		std::uint64_t CameraLayerMask(const ObjectManager& objects, const Object& object)
		{
			// Volume bits follow the existing project layer order. A layer beyond
			// the mask's capacity has no matching volume contribution.
			const auto& layers = objects.getAvailableLayers();
			const auto found = std::find(layers.begin(), layers.end(), objects.getObjectLayer(object));
			const auto index = std::distance(layers.begin(), found);
			return found != layers.end() && index < 64 ? std::uint64_t{1} << index : 0u;
		}

		bool CameraMatches(const PostProcessCameraProfileCapture& capture, const ObjectManager& objects)
		{
			if (!capture.target.camera || !capture.target.view) return false;
			const Object object = objects.makeObjectHandle(static_cast<entt::entity>(capture.nativeEntityValue));
			const auto& target = *capture.target.camera;
			return objects.isObjectValid(object) && objects.getUUID(object) == target.cameraId
				&& objects.GetSceneGeneration() == target.sceneGeneration
				&& objects.getObjectLifetimeGeneration(object) == target.objectLifetimeGeneration
				&& objects.getObjectComponentGeneration(object) == target.componentGeneration
				&& objects.getObjectBindingGeneration(object) == target.bindingGeneration
				&& CameraLayerMask(objects, object) == capture.cameraLayerMask
				&& objects.GetSpatialRevision() == capture.spatialRevision
				&& objects.GetHierarchyRevision() == capture.hierarchyRevision
				&& objects.GetComponentMutationRevision() == capture.componentMutationRevision;
		}
		bool ProfileCurrent(const ResourceManager& resources, const UUID& id,
			const std::shared_ptr<const Resource::PostProcessProfile>& owner)
		{
			if (id == EMPTY_UUID) return !owner;
			const auto current = resources.TryAcquirePostProcessProfileSnapshot(id);
			return current && *current && *current == owner;
		}
		bool Contributes(const PostProcessVolumeInput& volume, const std::array<double, 3>& camera,
			std::uint64_t cameraMask)
		{
			if (!(volume.layerMask & cameraMask) || volume.weight <= 0.0f) return false;
			if (volume.global) return true;
			double distanceSquared = 0.0;
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				const double distance = std::max({volume.boundsMinimum[axis] - camera[axis], 0.0,
					camera[axis] - volume.boundsMaximum[axis]});
				distanceSquared += distance * distance;
			}
			return distanceSquared == 0.0 || (volume.blendDistance > 0.0f
				&& distanceSquared < double(volume.blendDistance) * volume.blendDistance);
		}
	}

	PostProcessCameraProfileCaptureResult CapturePostProcessCameraProfile(ObjectManager& objects,
		const ResourceManager& resources, const SceneManager& scenes, const GraphicsTarget& target,
		const GraphicsLifecycleStamp& lifecycle)
	{
		PostProcessCameraProfileCaptureResult result;
		auto reject = [&](GraphicsRequestReason reason, const char* detail) {
			result.reason = reason; result.detail = detail; return result;
		};
		if (Scheduling::IsWorkerSafetyScopeActive())
			return reject(GraphicsRequestReason::InvalidScope, "Camera profile capture requires protected scene collection.");
		if (target.scope != GraphicsScope::Camera || !target.camera || !target.view || !target.view->lifetimeGeneration)
			return reject(GraphicsRequestReason::InvalidScope, "Authoring requires an exact authored camera and registered view.");
		try
		{
			const auto object = objects.getObject(target.camera->cameraId);
			if (!object) return reject(GraphicsRequestReason::StaleLifetime, "The authored camera no longer exists.");
			const auto& registry = objects.getRegistry();
			const auto* camera = registry.try_get<CameraComponent>(object->EntityId());
			const auto* transform = registry.try_get<Transform>(object->EntityId());
			const auto pose = objects.GetResolvedWorldPose(*object);
			if (!camera || !camera->active || !transform || !transform->active
				|| !objects.isObjectActiveInHierarchy(*object) || !pose || !Scene::IsWorldPoseFinite(*pose))
				return reject(GraphicsRequestReason::TargetUnavailable, "The authored camera has no active resolved component/pose.");
			auto capture = std::make_shared<PostProcessCameraProfileCapture>();
			capture->target = target; capture->lifecycle = lifecycle;
			capture->sceneId = scenes.GetActiveSceneId();
			if (const auto* document = scenes.GetActiveScene()) capture->sceneDocumentIdentity = document->saveIdentity;
			capture->nativeEntityValue = object->entityValue;
			capture->profileAssetId = camera->PostProcessProfileId; capture->pose = *pose;
			capture->cameraLayerMask = CameraLayerMask(objects, *object);
			capture->spatialRevision = objects.GetSpatialRevision(); capture->hierarchyRevision = objects.GetHierarchyRevision();
			capture->componentMutationRevision = objects.GetComponentMutationRevision();
			if (!CameraMatches(*capture, objects)) return reject(GraphicsRequestReason::StaleLifetime, "The camera lifetime changed before profile capture.");
			if (capture->profileAssetId != EMPTY_UUID)
			{
				const auto owner = resources.TryAcquirePostProcessProfileSnapshot(capture->profileAssetId);
				capture->readiness = !owner ? PostProcessCameraProfileReadiness::CacheBusy
					: !*owner ? PostProcessCameraProfileReadiness::Missing : PostProcessCameraProfileReadiness::Ready;
				if (owner) capture->profile = *owner;
			}
			result.capture = std::move(capture); return result;
		}
		catch (...)
		{
			result.reason = GraphicsRequestReason::PreparationFailed; result.detail.clear(); return result;
		}
	}

	PostProcessViewAuthoringResult PreparePostProcessViewAuthoring(std::shared_ptr<const PostProcessViewAuthoringInput> input)
	{
		PostProcessViewAuthoringResult result;
		auto reject = [&](GraphicsRequestReason reason, const char* detail) {
			result.reason = reason; result.detail = detail; return result;
		};
		try
		{
			if (!input || !input->inheritedBase || !input->camera || !input->volumes)
				return reject(GraphicsRequestReason::TargetUnavailable, "View authoring requires owned base, camera and volume captures.");
			const auto& base = *input->inheritedBase; const auto& camera = *input->camera; const auto& volumes = *input->volumes;
			if (base.target.scope != GraphicsScope::Game || base.target.camera || !base.revision)
				return reject(GraphicsRequestReason::InvalidScope, "Inherited authoring input must precede camera/profile/player resolution.");
			if (input->layers.baseRevision && input->layers.baseRevision != base.revision)
				return reject(GraphicsRequestReason::StaleRevision, "The requested inherited base revision is no longer the supplied root.");
			if (input->layers.target != camera.target || input->layers.lifecycle != camera.lifecycle || base.lifecycle != camera.lifecycle
				|| volumes.context.lifecycle != camera.lifecycle || !camera.target.camera
				|| volumes.context.sceneId != camera.sceneId || volumes.context.sceneGeneration != camera.target.camera->sceneGeneration
				|| volumes.sceneDocumentIdentity != camera.sceneDocumentIdentity || volumes.spatialRevision != camera.spatialRevision
				|| volumes.hierarchyRevision != camera.hierarchyRevision || volumes.componentMutationRevision != camera.componentMutationRevision)
				return reject(GraphicsRequestReason::StaleLifetime, "The copied view, camera, volume and base lifetimes differ.");
			if (!input->layers.engineDefaults.empty() || !input->layers.projectDefaults.empty() || !input->layers.qualityPreset.empty()
				|| input->layers.cameraProfile || !input->layers.volumes.empty())
				return reject(GraphicsRequestReason::InvalidValue, "The adapter's captured layers cannot be supplied a second time.");
			if (camera.readiness != PostProcessCameraProfileReadiness::Empty && camera.readiness != PostProcessCameraProfileReadiness::Ready)
				return reject(GraphicsRequestReason::TargetUnavailable, "The authored camera profile is missing or its cache is busy.");
			if ((camera.profileAssetId == EMPTY_UUID) != !camera.profile
				|| (camera.profile && camera.profile->assetId != camera.profileAssetId))
				return reject(GraphicsRequestReason::InvalidValue, "The copied camera profile does not own its requested identity.");
			const auto position = Scene::LocalizeWorldPosition(camera.pose.position, volumes.context.referenceOrigin, volumes.context.maximumRelativeAxisMetres);
			if (!position.Succeeded()) return reject(GraphicsRequestReason::TargetUnavailable, "The camera is outside the captured volume reference frame.");
			auto layers = input->layers;
			layers.baseRevision = base.revision; layers.engineDefaults = base.values; layers.cameraProfile = camera.profile;
			layers.cameraPosition = {position.offsetMetres.x, position.offsetMetres.y, position.offsetMetres.z};
			for (const auto& volume : volumes.volumes)
			{
				if (!(volume.input.layerMask & layers.cameraLayerMask) || volume.input.weight <= 0.0f) continue;
				if (!volume.input.global && volume.readiness == PostProcessVolumeReadiness::OutsideReferenceRange)
					return reject(GraphicsRequestReason::TargetUnavailable, "A local volume cannot be represented in the copied reference frame.");
				if (!Contributes(volume.input, layers.cameraPosition, layers.cameraLayerMask)) continue;
				if (!volume.input.profile || volume.input.profile->assetId != volume.profileAssetId)
					return reject(GraphicsRequestReason::TargetUnavailable, "A contributing volume profile is missing or busy.");
				layers.volumes.push_back(volume.input);
			}
			auto resolved = ResolvePostProcessProfile(layers);
			if (!resolved.candidate) { result.reason = resolved.reason; result.detail = std::move(resolved.detail); return result; }
			result.candidate = PreparedPostProcessViewAuthoring{std::move(input), std::move(*resolved.candidate)};
			return result;
		}
		catch (...) { result.reason = GraphicsRequestReason::PreparationFailed; result.detail.clear(); return result; }
	}

	bool IsPostProcessViewAuthoringCurrent(const PreparedPostProcessViewAuthoring& candidate,
		const GraphicsTarget& currentTarget, const GraphicsLifecycleStamp& lifecycle,
		const std::shared_ptr<const GraphicsSettingsSnapshot>& currentInheritedBase,
		const ObjectManager& objects, const ResourceManager& resources, const SceneManager& scenes)
	{
		if (Scheduling::IsWorkerSafetyScopeActive() || !candidate.source || !candidate.source->camera
			|| !candidate.source->volumes || !currentInheritedBase || candidate.source->inheritedBase != currentInheritedBase) return false;
		const auto& source = *candidate.source; const auto& camera = *source.camera;
		const auto* document = scenes.GetActiveScene();
		if (camera.target != currentTarget || camera.lifecycle != lifecycle || currentInheritedBase->lifecycle != lifecycle
			|| camera.sceneId != scenes.GetActiveSceneId() || camera.sceneDocumentIdentity != (document ? document->saveIdentity : nullptr)
			|| !CameraMatches(camera, objects) || !IsPostProcessVolumeCaptureCurrent(*source.volumes, objects, scenes, lifecycle)
			|| !IsPostProcessResolutionCurrent(candidate.resolved, currentTarget, lifecycle, currentInheritedBase->revision)
			|| !ProfileCurrent(resources, camera.profileAssetId, camera.profile)) return false;
		if (source.layers.sceneProfile && !ProfileCurrent(resources, source.layers.sceneProfile->assetId, source.layers.sceneProfile)) return false;
		const auto position = Scene::LocalizeWorldPosition(camera.pose.position, source.volumes->context.referenceOrigin,
			source.volumes->context.maximumRelativeAxisMetres);
		if (!position.Succeeded()) return false;
		const std::array<double, 3> localized{position.offsetMetres.x, position.offsetMetres.y, position.offsetMetres.z};
		for (const auto& volume : source.volumes->volumes)
			if (Contributes(volume.input, localized, source.layers.cameraLayerMask)
				&& !ProfileCurrent(resources, volume.profileAssetId, volume.input.profile)) return false;
		return true;
	}
}
