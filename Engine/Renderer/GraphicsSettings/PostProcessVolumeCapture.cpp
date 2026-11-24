#include "PostProcessVolumeCapture.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/WorkerSafety.h"
#include "Resource/ResourceManager.h"
#include "Scene/PostProcessVolumeComponent.h"
#include "Scene/SceneManager.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace TerraEngine::Renderer::GraphicsSettings
{
	bool TryTransformPostProcessVolumeBounds(const Scene::WorldPose& pose,
		const Scene::WorldPosition& referenceOrigin, const std::array<double, 3>& localMinimum,
		const std::array<double, 3>& localMaximum, double maximumRelativeAxisMetres,
		PostProcessVolumeBounds& destination) noexcept
	{
		if (!Scene::IsWorldPoseFinite(pose) || !std::isfinite(maximumRelativeAxisMetres)
			|| maximumRelativeAxisMetres < 0.0) return false;
		for (std::size_t axis = 0; axis < 3; ++axis)
			if (!std::isfinite(localMinimum[axis]) || !std::isfinite(localMaximum[axis])
				|| localMinimum[axis] > localMaximum[axis]) return false;
		const auto localized = Scene::LocalizeWorldPosition(pose.position, referenceOrigin, maximumRelativeAxisMetres);
		if (!localized.Succeeded()) return false;
		const glm::dmat3 rotation = glm::mat3_cast(glm::dquat(pose.rotation));
		PostProcessVolumeBounds result;
		result.minimum.fill(std::numeric_limits<double>::infinity());
		result.maximum.fill(-std::numeric_limits<double>::infinity());
		for (std::size_t corner = 0; corner < 8; ++corner)
		{
			glm::dvec3 local;
			for (std::size_t axis = 0; axis < 3; ++axis)
				local[axis] = (corner & (std::size_t{1} << axis)) ? localMaximum[axis] : localMinimum[axis];
			// This follows the renderer's resolved TRS convention, without turning
			// an absolute cell address into a lossy floating-point translation.
			const glm::dvec3 transformed = localized.offsetMetres + rotation * (glm::dvec3(pose.scale) * local);
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				if (!std::isfinite(transformed[axis]) || std::abs(transformed[axis]) > maximumRelativeAxisMetres) return false;
				result.corners[corner][axis] = transformed[axis];
				result.minimum[axis] = std::min(result.minimum[axis], transformed[axis]);
				result.maximum[axis] = std::max(result.maximum[axis], transformed[axis]);
			}
		}
		destination = result;
		return true;
	}

	bool PostProcessVolumeCapture::IsReady() const noexcept
	{
		return std::all_of(volumes.begin(), volumes.end(), [](const auto& volume) {
			return volume.readiness == PostProcessVolumeReadiness::Ready;
		});
	}

	bool IsPostProcessVolumeCaptureCurrent(const PostProcessVolumeCapture& capture,
		const ObjectManager& objects, const SceneManager& scenes, const GraphicsLifecycleStamp& lifecycle) noexcept
	{
		if (Scheduling::IsWorkerSafetyScopeActive()) return false;
		const auto* document = scenes.GetActiveScene();
		if (!(capture.context.lifecycle == lifecycle && capture.context.sceneId == scenes.GetActiveSceneId()
			&& capture.context.sceneGeneration == objects.GetSceneGeneration()
			&& capture.sceneDocumentIdentity == (document ? document->saveIdentity : nullptr)
			&& capture.spatialRevision == objects.GetSpatialRevision()
			&& capture.hierarchyRevision == objects.GetHierarchyRevision()
			&& capture.componentMutationRevision == objects.GetComponentMutationRevision())) return false;
		// Managed binding reload deliberately does not dirty scene authoring, so
		// its per-object lifetime must be checked separately from scene revisions.
		for (const auto& volume : capture.volumes)
		{
			const Object object = objects.makeObjectHandle(static_cast<entt::entity>(volume.nativeEntityValue));
			if (!objects.isObjectValid(object) || objects.getUUID(object) != volume.input.stableId
				|| objects.getObjectLifetimeGeneration(object) != volume.objectLifetimeGeneration
				|| objects.getObjectComponentGeneration(object) != volume.componentGeneration
				|| objects.getObjectBindingGeneration(object) != volume.bindingGeneration
				|| objects.GetObjectTransformRevision(object) != volume.transformRevision) return false;
		}
		return true;
	}

	PostProcessVolumeCaptureResult CapturePostProcessVolumes(ObjectManager& objects,
		const ResourceManager& resources, const SceneManager& scenes, const PostProcessVolumeCaptureContext& context)
	{
		PostProcessVolumeCaptureResult result;
		auto reject = [&](GraphicsRequestReason reason, const char* detail) {
			result.reason = reason; result.detail = detail; result.capture.reset(); return result;
		};
		if (Scheduling::IsWorkerSafetyScopeActive())
			return reject(GraphicsRequestReason::InvalidScope, "Live volume capture requires the protected scene collection boundary.");
		if (!context.sceneGeneration || context.sceneGeneration != objects.GetSceneGeneration()
			|| context.sceneId != scenes.GetActiveSceneId())
			return reject(GraphicsRequestReason::StaleLifetime, "The requested volume scene lifetime is no longer active.");
		const auto origin = Scene::NormalizeWorldPosition(context.referenceOrigin);
		if (!origin.Succeeded() || !std::isfinite(context.maximumRelativeAxisMetres) || context.maximumRelativeAxisMetres < 0.0)
			return reject(GraphicsRequestReason::InvalidValue, "Volume capture requires a valid bounded double reference frame.");
		try
		{
			auto capture = std::make_shared<PostProcessVolumeCapture>();
			capture->context = context;
			capture->context.referenceOrigin = origin.position;
			if (const auto* document = scenes.GetActiveScene()) capture->sceneDocumentIdentity = document->saveIdentity;
			capture->spatialRevision = objects.GetSpatialRevision();
			capture->hierarchyRevision = objects.GetHierarchyRevision();
			capture->componentMutationRevision = objects.GetComponentMutationRevision();
			// Const registry access ensures collection cannot accidentally emplace
			// components or update runtime active-membership caches.
			const auto& registry = objects.getRegistry();
			for (const auto entity : registry.view<const PostProcessVolumeComponent>())
			{
				const auto& authored = registry.get<PostProcessVolumeComponent>(entity);
				const Object object = objects.makeObjectHandle(entity);
				if (!IsRuntimeComponentActive(authored) || !objects.isObjectActiveInHierarchy(object)) continue;
				const auto* transform = registry.try_get<Transform>(entity);
				if (transform && !IsRuntimeComponentActive(*transform)) continue;
				if (!authored.ValidateAuthoredValues())
					return reject(GraphicsRequestReason::InvalidValue, "An active post-process volume has invalid authored values.");
				const auto pose = objects.GetResolvedWorldPose(object);
				if (!pose || !Scene::IsWorldPoseFinite(*pose))
					return reject(GraphicsRequestReason::InvalidValue, "An active post-process volume has no valid resolved world pose.");
				CapturedPostProcessVolume volume;
				volume.input.stableId = objects.getUUID(object);
				if (volume.input.stableId == EMPTY_UUID)
					return reject(GraphicsRequestReason::InvalidValue, "An active post-process volume has no stable object UUID.");
				volume.profileAssetId = authored.profileAssetId;
				volume.nativeEntityValue = object.entityValue;
				volume.objectLifetimeGeneration = objects.getObjectLifetimeGeneration(object);
				volume.componentGeneration = objects.getObjectComponentGeneration(object);
				volume.bindingGeneration = objects.getObjectBindingGeneration(object);
				volume.pose = *pose;
				volume.transformRevision = objects.GetObjectTransformRevision(object);
				volume.coordinateFrameRevision = pose->coordinateFrameRevision;
				volume.input.global = authored.global;
				volume.input.layerMask = authored.layerMask;
				volume.input.priority = authored.priority;
				volume.input.blendDistance = authored.blendDistance;
				volume.input.weight = authored.weight;
				const bool localized = TryTransformPostProcessVolumeBounds(*pose, capture->context.referenceOrigin,
					authored.boundsMinimum, authored.boundsMaximum, context.maximumRelativeAxisMetres, volume.bounds);
				const auto profile = resources.TryAcquirePostProcessProfileSnapshot(authored.profileAssetId);
				if (profile) volume.input.profile = *profile;
				volume.readiness = !localized ? PostProcessVolumeReadiness::OutsideReferenceRange
					: !profile ? PostProcessVolumeReadiness::ProfileCacheBusy
					: !*profile ? PostProcessVolumeReadiness::ProfileMissing : PostProcessVolumeReadiness::Ready;
				if (localized)
				{
					volume.input.boundsMinimum = volume.bounds.minimum;
					volume.input.boundsMaximum = volume.bounds.maximum;
				}
				capture->volumes.push_back(std::move(volume));
			}
			// Stable ordering makes evidence and later worker preparation independent
			// of EnTT's storage order; resolver still owns priority precedence.
			std::sort(capture->volumes.begin(), capture->volumes.end(), [](const auto& left, const auto& right) {
				return left.input.stableId < right.input.stableId;
			});
			if (!IsPostProcessVolumeCaptureCurrent(*capture, objects, scenes, context.lifecycle))
				return reject(GraphicsRequestReason::StaleLifetime, "Scene authoring changed while volume inputs were captured.");
			result.capture = std::move(capture);
			return result;
		}
		catch (...)
		{
			// A partially copied set is never published as a valid capture. Avoid
			// allocation while reporting an allocation/preparation exception.
			result.capture.reset(); result.reason = GraphicsRequestReason::PreparationFailed;
			result.detail.clear(); return result;
		}
	}
}
