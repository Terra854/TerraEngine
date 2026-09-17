// NavigationInspectorSupport.cpp - actionable editor validation explanations.

#include "NavigationInspectorSupport.h"

namespace TerraEngine::Editor::Navigation {
	NavigationAuthoringValidation ValidateNavigationProfileDraft(
		const TerraEngine::Navigation::NavigationProfile& profile) {
		const auto validation = profile.Validate();
		return {validation.accepted, validation.accepted
			? std::string{}
			: std::string("The ground profile contains an invalid capsule, slope, step, resolution, or area value: ")
				+ std::string(TerraEngine::Navigation::ToString(validation.code))};
	}

	NavigationTerrainControlDescription DescribeNavigationTerrainControls(
		const NavigationTerrainAuthoringState& state) noexcept {
		return {false, !state.terrainExistsInScene, false};
	}

	NavigationAuthoringValidation ValidateNavigationTerrainAuthoring(
		const NavigationTerrainAuthoringState& state) {
		if(state.terrainExistsInScene && !state.hasCurrentTerrainCollision)
			return {false, "Terrain is present, but current published terrain collision is unavailable."};
		return {true, {}};
	}
}
