// NavigationInspectorSupport.h - UI-independent navigation authoring validation.

#pragma once

#include "Components/Navigation/NavigationConnectionComponent.h"
#include "Components/Navigation/NavigationSourceComponent.h"
#include "Navigation/NavigationSettings.h"

#include <string>

namespace TerraEngine::Editor::Navigation {
	struct NavigationAuthoringValidation {
		bool accepted = false;
		std::string explanation;
	};

	struct NavigationTerrainAuthoringState {
		bool terrainExistsInScene = false;
		bool hasEnabledEligibleTerrainSource = false;
	};

	struct NavigationTerrainControlDescription {
		bool sourceKindVisible = true;
		bool noTerrainSceneAccepted = true;
		bool terrainSourceRequired = false;
	};

	[[nodiscard]] NavigationAuthoringValidation ValidateNavigationSourceDraft(const NavigationSourceComponent& source);
	[[nodiscard]] NavigationAuthoringValidation ValidateNavigationProfileDraft(
		const TerraEngine::Navigation::NavigationProfile& profile);
	[[nodiscard]] NavigationAuthoringValidation ValidateNavigationConnectionDraft(
		const NavigationConnectionComponent& connection);
	[[nodiscard]] NavigationTerrainControlDescription DescribeNavigationTerrainControls(
		const NavigationTerrainAuthoringState& state) noexcept;
	[[nodiscard]] NavigationAuthoringValidation ValidateNavigationTerrainAuthoring(
		const NavigationTerrainAuthoringState& state);
}
