// Narrow scene-to-worker adapter for the existing profile resolver.
#pragma once
#include "PostProcessVolumeCapture.h"

namespace TerraEngine::Renderer::GraphicsSettings
{
	enum class PostProcessCameraProfileReadiness : std::uint8_t { Empty, Ready, Missing, CacheBusy };
	struct PostProcessCameraProfileCapture
	{
		GraphicsTarget target;
		GraphicsLifecycleStamp lifecycle;
		UUID sceneId = EMPTY_UUID;
		std::shared_ptr<const std::uint8_t> sceneDocumentIdentity;
		std::uint32_t nativeEntityValue = 0;
		UUID profileAssetId = EMPTY_UUID;
		std::shared_ptr<const Resource::PostProcessProfile> profile;
		Scene::WorldPose pose;
		std::uint64_t cameraLayerMask = 0u;
		std::uint64_t spatialRevision = 0, hierarchyRevision = 0, componentMutationRevision = 0;
		PostProcessCameraProfileReadiness readiness = PostProcessCameraProfileReadiness::Empty;
	};
	struct PostProcessCameraProfileCaptureResult
	{
		GraphicsRequestReason reason = GraphicsRequestReason::None;
		std::string detail;
		std::shared_ptr<const PostProcessCameraProfileCapture> capture;
	};

	// Protected scene-collection call. The caller supplies the exact live view
	// identity from its protected view registry; this adapter verifies the actual
	// camera component/object lifetime, not the registry's ownership of that view.
	// Reads camera/Transform/scene identity and try-locks the profile cache. Writes
	// private copied ownership only, with no IO, load requests or live mutation.
	TERRAENGINE_API PostProcessCameraProfileCaptureResult CapturePostProcessCameraProfile(
		ObjectManager& objects, const ResourceManager& resources, const SceneManager& scenes,
		const GraphicsTarget& target, const GraphicsLifecycleStamp& lifecycle);

	struct PostProcessViewAuthoringInput
	{
		// The inherited Game root precedes authored profiles/volumes and late
		// camera/player patches. Feeding back a previous resolved root would blend
		// weighted volumes repeatedly and erase explicit precedence information.
		std::shared_ptr<const GraphicsSettingsSnapshot> inheritedBase;
		std::shared_ptr<const PostProcessCameraProfileCapture> camera;
		std::shared_ptr<const PostProcessVolumeCapture> volumes;
		// Reuse the existing typed layer contract. Supply target/lifecycle/catalog,
		// optional sceneProfile, cameraOverrides, runtimeCameraOverrides and trusted
		// allowed-player policy/overrides. The adapter fills baseRevision, inherited
		// engineDefaults, cameraProfile, volumes and cameraPosition itself. All other
		// inherited layers must already be represented by inheritedBase.
		PostProcessProfileResolutionInput layers;
	};
	struct PreparedPostProcessViewAuthoring
	{
		std::shared_ptr<const PostProcessViewAuthoringInput> source;
		PostProcessResolvedProfile resolved;
	};
	struct PostProcessViewAuthoringResult
	{
		GraphicsRequestReason reason = GraphicsRequestReason::None;
		std::string detail;
		std::optional<PreparedPostProcessViewAuthoring> candidate;
	};

	// Worker-safe adapter, delegating every value/custom-ID precedence rule to
	// ResolvePostProcessProfile. Missing contributing assets or unsafe bounds
	// return no candidate; never fabricate fallback values or resource proof.
	TERRAENGINE_API PostProcessViewAuthoringResult PreparePostProcessViewAuthoring(
		std::shared_ptr<const PostProcessViewAuthoringInput> input);

	// Protected publication guard. Pass the current ordinary Game root and the
	// exact target from the live view registry, not copies from this candidate.
	// Rechecks scene/camera/volume generations and accepted immutable asset owners
	// without waiting for cache locks. A valid demand still needs native resource
	// preparation and ordinary protected graphics publication before execution.
	TERRAENGINE_API bool IsPostProcessViewAuthoringCurrent(const PreparedPostProcessViewAuthoring& candidate,
		const GraphicsTarget& currentTarget, const GraphicsLifecycleStamp& lifecycle,
		const std::shared_ptr<const GraphicsSettingsSnapshot>& currentInheritedBase,
		const ObjectManager& objects, const ResourceManager& resources, const SceneManager& scenes);
}
