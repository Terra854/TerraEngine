// NavigationAuthoringTests.cpp - editor-facing ground-navigation contract tests.
//
// These tests describe UI-independent authoring behavior so the ImGui window can
// remain a thin presentation layer. They intentionally include the planned
// support headers before their implementation tasks land.

#include "TestHarness.h"

#include "Components/Navigation/NavigationConnectionComponent.h"
#include "Components/Navigation/NavigationSourceComponent.h"
#include "Navigation/NavigationSettings.h"
#include "Editor/TerraEngine-Editor/Navigation/NavigationInspectorSupport.h"
#include "Editor/TerraEngine-Editor/Windows/NavigationWindow.h"

#include <limits>

namespace TerraEngine::EditorTests
{
	void RegisterNavigationAuthoringTests(TestRegistry& registry)
	{
		registry.Add("Editor.NavigationAuthoring.InspectorValidatesSourceProfileAndConnectionDrafts", [](TestContext& context) {
			NavigationSourceComponent source;
			source.sourceKind = TerraEngine::Navigation::NavigationSourceKind::Mesh;
			source.deduplicationGroup = 1U;
			const auto validSource = TerraEngine::Editor::Navigation::ValidateNavigationSourceDraft(source);
			context.Expect(validSource.accepted && validSource.explanation.empty(),
				"A valid mesh source must be accepted with no stale inspector error.");

			source.deduplicationGroup = 0U;
			const auto invalidSource = TerraEngine::Editor::Navigation::ValidateNavigationSourceDraft(source);
			context.Expect(!invalidSource.accepted && !invalidSource.explanation.empty(),
				"An invalid source draft must remain in the editor with an actionable explanation.");

			TerraEngine::Navigation::NavigationProfile profile = TerraEngine::Navigation::NavigationProfile::Defaults();
			context.Expect(TerraEngine::Editor::Navigation::ValidateNavigationProfileDraft(profile).accepted,
				"A valid upright-capsule profile must be authorable through the inspector.");
			profile.radiusMetres = 0.0;
			const auto invalidProfile = TerraEngine::Editor::Navigation::ValidateNavigationProfileDraft(profile);
			context.Expect(!invalidProfile.accepted && !invalidProfile.explanation.empty(),
				"Invalid profile dimensions must identify the authoring problem before mutation is queued.");

			NavigationConnectionComponent connection;
			context.Expect(TerraEngine::Editor::Navigation::ValidateNavigationConnectionDraft(connection).accepted,
				"A default explicit ground connection must be representable by the inspector.");
			connection.traversalCost = std::numeric_limits<float>::quiet_NaN();
			const auto invalidConnection = TerraEngine::Editor::Navigation::ValidateNavigationConnectionDraft(connection);
			context.Expect(!invalidConnection.accepted && !invalidConnection.explanation.empty(),
				"Non-finite connection costs must be rejected with an actionable inspector explanation.");
		});

		registry.Add("Editor.NavigationAuthoring.TerrainControlsAreMandatoryOnlyWhenTerrainExists", [](TestContext& context) {
			TerraEngine::Editor::Navigation::NavigationTerrainAuthoringState terrainFree{};
			terrainFree.terrainExistsInScene = false;
			const auto terrainFreeControls = TerraEngine::Editor::Navigation::DescribeNavigationTerrainControls(terrainFree);
			context.Expect(terrainFreeControls.sourceKindVisible && terrainFreeControls.noTerrainSceneAccepted,
				"Terrain-free mesh and collider scenes must remain authorable without fabricating a terrain object.");

			TerraEngine::Editor::Navigation::NavigationTerrainAuthoringState terrainPresent{};
			terrainPresent.terrainExistsInScene = true;
			terrainPresent.hasEnabledEligibleTerrainSource = false;
			const auto missingTerrain = TerraEngine::Editor::Navigation::ValidateNavigationTerrainAuthoring(terrainPresent);
			context.Expect(!missingTerrain.accepted && !missingTerrain.explanation.empty(),
				"When terrain exists, controls must require an enabled eligible terrain source instead of silently omitting it.");

			terrainPresent.hasEnabledEligibleTerrainSource = true;
			context.Expect(TerraEngine::Editor::Navigation::ValidateNavigationTerrainAuthoring(terrainPresent).accepted,
				"An enabled eligible terrain source must satisfy the mandatory terrain authoring control.");
		});

		registry.Add("Editor.NavigationAuthoring.PreviewExplainsSuccessAndFailure", [](TestContext& context) {
			TerraEngine::Editor::Navigation::NavigationPreviewObservation complete{};
			complete.outcome = TerraEngine::Navigation::NavigationOutcome::Complete;
			const auto completePreview = TerraEngine::Editor::Navigation::BuildNavigationPreview(complete);
			context.Expect(completePreview.routeVisible && completePreview.explanation.empty(),
				"A completed preview must expose a route without presenting a false failure explanation.");

			TerraEngine::Editor::Navigation::NavigationPreviewObservation blocked{};
			blocked.outcome = TerraEngine::Navigation::NavigationOutcome::Unreachable;
			blocked.reason = TerraEngine::Navigation::NavigationReason::InsufficientClearance;
			const auto blockedPreview = TerraEngine::Editor::Navigation::BuildNavigationPreview(blocked);
			context.Expect(!blockedPreview.routeVisible && !blockedPreview.explanation.empty()
				&& blockedPreview.reason == TerraEngine::Navigation::NavigationReason::InsufficientClearance,
				"A failed preview must retain the concrete navigation reason rather than reporting generic failure.");

			TerraEngine::Editor::Navigation::NavigationPreviewObservation preparing{};
			preparing.outcome = TerraEngine::Navigation::NavigationOutcome::PendingPreparation;
			preparing.reason = TerraEngine::Navigation::NavigationReason::DirtyCoverage;
			const auto preparingPreview = TerraEngine::Editor::Navigation::BuildNavigationPreview(preparing);
			context.Expect(!preparingPreview.routeVisible && preparingPreview.isPreparing
				&& !preparingPreview.explanation.empty(),
				"Preview must distinguish a preparing area from a definitive unreachable route.");
		});
	}
}
