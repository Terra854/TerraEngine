// NavigationWindow.h - UI-independent state for the ground navigation window.

#pragma once

#include "../Framework/EditorWindow.h"
#include "../Navigation/NavigationSceneOverlay.h"
#include "Navigation/NavigationTypes.h"

#include <string>

namespace TerraEngine::Editor::Navigation {
	struct NavigationPreviewObservation {
		TerraEngine::Navigation::NavigationOutcome outcome = TerraEngine::Navigation::NavigationOutcome::Queued;
		TerraEngine::Navigation::NavigationReason reason = TerraEngine::Navigation::NavigationReason::None;
	};

	struct NavigationPreviewPresentation {
		bool routeVisible = false;
		bool isPreparing = false;
		TerraEngine::Navigation::NavigationReason reason = TerraEngine::Navigation::NavigationReason::None;
		std::string explanation;
	};

	[[nodiscard]] inline NavigationPreviewPresentation BuildNavigationPreview(
		const NavigationPreviewObservation& observation) {
		NavigationPreviewPresentation result{};
		result.reason = observation.reason;
		result.routeVisible = observation.outcome == TerraEngine::Navigation::NavigationOutcome::Complete;
		result.isPreparing = observation.outcome == TerraEngine::Navigation::NavigationOutcome::Queued
			|| observation.outcome == TerraEngine::Navigation::NavigationOutcome::PendingPreparation
			|| observation.outcome == TerraEngine::Navigation::NavigationOutcome::Searching;
		if(!result.routeVisible) {
			result.explanation = result.isPreparing
				? "Navigation coverage is still preparing."
				: std::string("Route preview failed: ") + std::string(TerraEngine::Navigation::ToString(observation.reason));
		}
		return result;
	}

	// The window owns only presentation data. Authoring validation and protected
	// transactions stay in their dedicated services so opening diagnostics cannot
	// directly mutate a live navigation world.
	class NavigationWindow final : public EditorWindow {
	public:
		NavigationWindow();

		void SetDiagnosticsWindowOpen(bool open) noexcept;
		[[nodiscard]] bool IsDiagnosticsWindowOpen() const noexcept;
		void SetOverlayInput(NavigationSceneOverlayInput input);
		void Update();
		[[nodiscard]] const NavigationSceneOverlaySnapshot& Overlay() const noexcept;

	protected:
		void OnGui() override;

	private:
		NavigationSceneOverlayInput overlayInput_{};
		NavigationSceneOverlaySnapshot overlay_{};
	};
}
