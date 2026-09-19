// NavigationWindow.h - UI-independent state for the ground navigation window.

#pragma once

#include "../Framework/EditorWindow.h"
#include "../Navigation/NavigationSceneOverlay.h"
#include "Navigation/NavigationTypes.h"
#include "Utilities/UUID.h"

#include <optional>
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

	// A route is drawn only when the guidance and copied route share a current
	// publication. Requesting and failed agents remain visible as diagnostics,
	// but never masquerade as movement-ready controller input.
	struct NavigationAgentOverlayPresentation {
		bool routeVisible = false;
		bool usableGuidance = false;
		TerraEngine::Navigation::NavigationAgentState state = TerraEngine::Navigation::NavigationAgentState::Idle;
		TerraEngine::Navigation::NavigationReason reason = TerraEngine::Navigation::NavigationReason::None;
		std::string explanation;
	};

	[[nodiscard]] inline NavigationAgentOverlayPresentation BuildNavigationAgentOverlayPresentation(
		const std::optional<NavigationAgentOverlayRecord>& selectedAgent) {
		NavigationAgentOverlayPresentation result{};
		if(!selectedAgent) {
			result.explanation = "No navigation agent is selected.";
			return result;
		}
		result.state = selectedAgent->guidance.state;
		result.reason = selectedAgent->guidance.reason;
		result.usableGuidance = selectedAgent->routeCurrent && selectedAgent->guidance.IsUsable();
		result.routeVisible = result.usableGuidance && !selectedAgent->routePoints.empty();
		if(result.routeVisible && selectedAgent->routeTruncated)
			result.explanation = "Current advisory route (display prefix is bounded).";
		else if(result.routeVisible)
			result.explanation = "Current advisory route.";
		else if(result.state == TerraEngine::Navigation::NavigationAgentState::Requesting)
			result.explanation = "Navigation is requesting or retrying coverage.";
		else
			result.explanation = std::string("No usable guidance: ")
				+ std::string(TerraEngine::Navigation::ToString(result.reason));
		return result;
	}

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
		// Selection is an object UUID instead of an ECS entity. The next refresh
		// resolves it through the agent system's immutable binding publication.
		void SetSelectedAgentObject(UUID objectId) noexcept;
		void Update();
		[[nodiscard]] const NavigationSceneOverlaySnapshot& Overlay() const noexcept;

	protected:
		void OnGui() override;

	private:
		NavigationSceneOverlayInput overlayInput_{};
		NavigationSceneOverlaySnapshot overlay_{};
		std::optional<UUID> selectedAgentObject_;
	};
}
