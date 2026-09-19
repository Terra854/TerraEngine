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

	NavigationAuthoringValidation ValidateNavigationAgentDraft(
		const NavigationAgentComponent& candidate) {
		if(candidate.ValidateAuthoredValues()) return {true, {}};
		return {false, "Navigation Agent settings require a finite valid traversal profile, positive arrival and target tolerances, and a bounded replan interval."};
	}

	NavigationAuthoringValidation ApplyNavigationAgentProjectProfile(
		NavigationAgentComponent& candidate,
		const TerraEngine::Navigation::NavigationProfile& projectProfile) {
		const NavigationAuthoringValidation profileValidation = ValidateNavigationProfileDraft(projectProfile);
		if(!profileValidation.accepted) return profileValidation;
		NavigationAgentComponent staged = candidate;
		staged.radiusMetres = projectProfile.radiusMetres;
		staged.heightMetres = projectProfile.heightMetres;
		staged.stepHeightMetres = projectProfile.stepHeightMetres;
		staged.maxSlopeDegrees = projectProfile.maxSlopeDegrees;
		staged.clearanceMarginMetres = projectProfile.clearanceMarginMetres;
		staged.horizontalResolutionMetres = projectProfile.horizontalResolutionMetres;
		staged.verticalResolutionMetres = projectProfile.verticalResolutionMetres;
		staged.excludedAreasMask = projectProfile.excludedAreasMask;
		staged.areaPenalties = projectProfile.areaPenalties;
		const NavigationAuthoringValidation candidateValidation = ValidateNavigationAgentDraft(staged);
		if(candidateValidation.accepted) candidate = staged;
		return candidateValidation;
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
