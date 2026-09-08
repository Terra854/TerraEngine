// NavigationInspectorSupport.cpp - actionable editor validation explanations.

#include "NavigationInspectorSupport.h"

namespace TerraEngine::Editor::Navigation {
	NavigationAuthoringValidation ValidateNavigationSourceDraft(const NavigationSourceComponent& source) {
		const TerraEngine::Navigation::NavigationSourceValidationResult validation = source.Validate();
		return {validation.accepted, validation.accepted ? std::string{} : validation.message};
	}

	NavigationAuthoringValidation ValidateNavigationProfileDraft(
		const TerraEngine::Navigation::NavigationProfile& profile) {
		const auto validation = profile.Validate();
		return {validation.accepted, validation.accepted
			? std::string{}
			: std::string("The ground profile contains an invalid capsule, slope, step, resolution, or area value: ")
				+ std::string(TerraEngine::Navigation::ToString(validation.code))};
	}

	NavigationAuthoringValidation ValidateNavigationConnectionDraft(
		const NavigationConnectionComponent& connection) {
		const auto validation = connection.Validate();
		return {validation.accepted, validation.accepted ? std::string{} : validation.message};
	}

	NavigationTerrainControlDescription DescribeNavigationTerrainControls(
		const NavigationTerrainAuthoringState& state) noexcept {
		return {true, !state.terrainExistsInScene, state.terrainExistsInScene};
	}

	NavigationAuthoringValidation ValidateNavigationTerrainAuthoring(
		const NavigationTerrainAuthoringState& state) {
		if(state.terrainExistsInScene && !state.hasEnabledEligibleTerrainSource)
			return {false, "Terrain is present, so enable one eligible Terrain navigation source."};
		return {true, {}};
	}
}
