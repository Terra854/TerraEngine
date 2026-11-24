// Worker-safe profile preparation. The protected owner alone publishes results.
#pragma once
#include "Resource/PostProcessProfile.h"
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace TerraEngine::Renderer::GraphicsSettings
{
	// Native scene preparation copies bounds into one consistent double-precision
	// coordinate frame. No live transform/component reads occur in this resolver.
	struct PostProcessVolumeInput
	{
		TerraEngine::UUID stableId = TerraEngine::EMPTY_UUID;
		std::shared_ptr<const Resource::PostProcessProfile> profile;
		std::array<double, 3> boundsMinimum{};
		std::array<double, 3> boundsMaximum{};
		bool global = false;
		std::uint64_t layerMask = ~std::uint64_t{0};
		std::int32_t priority = 0;
		float blendDistance = 0.0f;
		float weight = 1.0f;
	};

	struct PostProcessProfileResolutionInput
	{
		GraphicsTarget target{};
		GraphicsLifecycleStamp lifecycle{};
		GraphicsRevision baseRevision = 0u;
		std::vector<GraphicsSettingDescriptor> descriptors;
		std::vector<GraphicsSettingValue> engineDefaults;
		std::vector<GraphicsSettingValue> projectDefaults;
		std::vector<GraphicsSettingValue> qualityPreset;
		std::shared_ptr<const Resource::PostProcessProfile> sceneProfile;
		std::vector<PostProcessVolumeInput> volumes;
		std::array<double, 3> cameraPosition{};
		std::uint64_t cameraLayerMask = ~std::uint64_t{0};
		std::shared_ptr<const Resource::PostProcessProfile> cameraProfile;
		std::vector<GraphicsSettingValue> cameraOverrides;
		std::vector<GraphicsSettingValue> runtimeCameraOverrides;
		// These policy IDs come from the trusted game owner, not the player patch.
		std::vector<GraphicsSettingId> playerAllowedSettings;
		std::vector<GraphicsSettingValue> playerOverrides;
		// Only Boolean false or a catalog-declared Unsigned32 Off(0) is allowed.
		std::vector<GraphicsSettingId> playerDisabledSettings;
	};

	struct PostProcessResolvedProfile
	{
		GraphicsRevision baseRevision = 0u;
		GraphicsSettingsSnapshot settings{};
		std::vector<TerraEngine::UUID> customEffects;
	};

	struct PostProcessProfileResolution
	{
		GraphicsRequestReason reason = GraphicsRequestReason::None;
		std::string detail;
		// Empty on ANY failure; an earlier effective root stays owned by caller.
		std::optional<PostProcessResolvedProfile> candidate;
	};

	// Order: defaults, project, quality, scene, volumes, authored camera, runtime
	// camera, player. Volume order is ascending priority then UUID bytes; higher
	// priority/larger UUID wins ties. Floats lerp; discrete values switch at>=0.5.
	// Preparation reads copied inputs only and is safe to run in parallel jobs.
	TERRAENGINE_API PostProcessProfileResolution ResolvePostProcessProfile(const PostProcessProfileResolutionInput& input);

	// Recheck at the declared protected publication boundary. Passing this guard
	// does not certify GPU resources; normal controller/backend validation follows.
	TERRAENGINE_API bool IsPostProcessResolutionCurrent(const PostProcessResolvedProfile& candidate,
		const GraphicsTarget& target, const GraphicsLifecycleStamp& lifecycle,
		GraphicsRevision currentRevision) noexcept;
}
