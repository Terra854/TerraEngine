// Protected scene capture for worker-safe post-process volume resolution.
#pragma once

#include "PostProcessProfileResolver.h"
#include "Scene/WorldCoordinate.h"

class ObjectManager;
class ResourceManager;
class SceneManager;

namespace TerraEngine::Renderer::GraphicsSettings
{
	struct PostProcessVolumeBounds
	{
		std::array<std::array<double, 3>, 8> corners{};
		std::array<double, 3> minimum{};
		std::array<double, 3> maximum{};
	};

	// Pure copied-pose preparation, usable in parallel jobs. Localization subtracts
	// exact cells before conversion; rotation, scale and all eight corners use
	// double arithmetic. A rejected transform leaves destination unchanged.
	TERRAENGINE_API bool TryTransformPostProcessVolumeBounds(const Scene::WorldPose& pose,
		const Scene::WorldPosition& referenceOrigin, const std::array<double, 3>& localMinimum,
		const std::array<double, 3>& localMaximum, double maximumRelativeAxisMetres,
		PostProcessVolumeBounds& destination) noexcept;

	enum class PostProcessVolumeReadiness : std::uint8_t
	{
		Ready,
		ProfileMissing,
		ProfileCacheBusy,
		OutsideReferenceRange
	};

	struct CapturedPostProcessVolume
	{
		PostProcessVolumeInput input;
		UUID profileAssetId = EMPTY_UUID;
		// A copied registry slot is only a lookup hint; publication also checks
		// its stable UUID and every retained lifetime token before accepting it.
		std::uint32_t nativeEntityValue = 0;
		std::uint64_t objectLifetimeGeneration = 0;
		std::uint64_t componentGeneration = 0;
		std::uint64_t bindingGeneration = 0;
		std::uint64_t transformRevision = 0;
		std::uint64_t coordinateFrameRevision = 0;
		// Retain the canonical copied pose even when localization cannot represent
		// this distant volume in the explicitly bounded camera reference frame.
		Scene::WorldPose pose{};
		PostProcessVolumeBounds bounds;
		PostProcessVolumeReadiness readiness = PostProcessVolumeReadiness::ProfileMissing;
	};

	struct PostProcessVolumeCaptureContext
	{
		UUID sceneId = EMPTY_UUID;
		std::uint64_t sceneGeneration = 0;
		GraphicsLifecycleStamp lifecycle{};
		Scene::WorldPosition referenceOrigin{};
		double maximumRelativeAxisMetres = Scene::kTransformSpaceMaximumRelativeRadiusMetres;
	};

	struct PostProcessVolumeCapture
	{
		PostProcessVolumeCaptureContext context;
		// A reload of the same document UUID has a different retained identity.
		std::shared_ptr<const std::uint8_t> sceneDocumentIdentity;
		std::uint64_t spatialRevision = 0;
		std::uint64_t hierarchyRevision = 0;
		std::uint64_t componentMutationRevision = 0;
		std::vector<CapturedPostProcessVolume> volumes;
		// Do not feed unresolved bounds/profiles to the resolver. Busy/missing
		// owners remain explicit; a retry must perform a new protected capture.
		[[nodiscard]] bool IsReady() const noexcept;
	};

	struct PostProcessVolumeCaptureResult
	{
		GraphicsRequestReason reason = GraphicsRequestReason::None;
		std::string detail;
		std::shared_ptr<const PostProcessVolumeCapture> capture;
	};

	// Call only during the existing protected scene collection boundary, after
	// mutations flush and while scene/component/hierarchy writers are excluded.
	// Reads: scene identity, active hierarchy, volume/Transform, profile cache.
	// Writes: private copied output only. No renderer publication, live mutation,
	// resource loading, futures, or IO. The cache lookup uses try-lock admission.
	// The bounded ECS collection is sequential because registry lifetime belongs
	// to that protected owner; the copied bounds helper and resolver are worker
	// safe and can be scheduled independently for different camera frames.
	TERRAENGINE_API PostProcessVolumeCaptureResult CapturePostProcessVolumes(ObjectManager& objects,
		const ResourceManager& resources, const SceneManager& scenes,
		const PostProcessVolumeCaptureContext& context);

	// Protected publication guard. A false result requires recapture; this does
	// not certify GPU resources or independently prove the renderer lifecycle.
	TERRAENGINE_API bool IsPostProcessVolumeCaptureCurrent(const PostProcessVolumeCapture& capture,
		const ObjectManager& objects, const SceneManager& scenes,
		const GraphicsLifecycleStamp& lifecycle) noexcept;
}
