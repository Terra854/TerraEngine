// NavigationInspectorSupport.h - UI-independent navigation authoring validation.

#pragma once

#include "Components/NavigationAgentComponent.h"
#include "Navigation/NavigationSettings.h"

#include <string>

namespace TerraEngine::Editor::Navigation {
	struct NavigationAuthoringValidation {
		bool accepted = false;
		std::string explanation;
	};

	struct NavigationTerrainAuthoringState {
		bool terrainExistsInScene = false;
		bool hasCurrentTerrainCollision = false;
	};

	struct NavigationTerrainControlDescription {
		bool sourceKindVisible = true;
		bool noTerrainSceneAccepted = true;
		bool terrainSourceRequired = false;
	};

	[[nodiscard]] NavigationAuthoringValidation ValidateNavigationProfileDraft(
		const TerraEngine::Navigation::NavigationProfile& profile);
	// Copies only traversal values from a project-owned navigation profile into a
	// detached agent candidate. Agent-only arrival and replanning values remain
	// authored per object, so selecting a preset never overwrites them silently.
	[[nodiscard]] NavigationAuthoringValidation ApplyNavigationAgentProjectProfile(
		NavigationAgentComponent& candidate,
		const TerraEngine::Navigation::NavigationProfile& projectProfile);
	// Validates the full detached component value used by inspector multi-edit
	// staging, before a transaction can describe any selected live object.
	[[nodiscard]] NavigationAuthoringValidation ValidateNavigationAgentDraft(
		const NavigationAgentComponent& candidate);
	[[nodiscard]] NavigationTerrainControlDescription DescribeNavigationTerrainControls(
		const NavigationTerrainAuthoringState& state) noexcept;
	[[nodiscard]] NavigationAuthoringValidation ValidateNavigationTerrainAuthoring(
		const NavigationTerrainAuthoringState& state);
}
