// NavigationWindow.cpp - thin presentation state over engine navigation APIs.

#include "NavigationWindow.h"

#include "Navigation/Agents/NavigationAgentSystem.h"
#include "Navigation/NavigationSystem.h"

#include <utility>

namespace TerraEngine::Editor::Navigation {
	NavigationWindow::NavigationWindow()
		: EditorWindow("Navigation", false, "Navigation") {}

	void NavigationWindow::SetDiagnosticsWindowOpen(bool open) noexcept { SetVisible(open); }
	bool NavigationWindow::IsDiagnosticsWindowOpen() const noexcept { return IsVisible(); }
	void NavigationWindow::SetOverlayInput(NavigationSceneOverlayInput input) { overlayInput_ = std::move(input); }
	void NavigationWindow::SetSelectedAgentObject(UUID objectId) noexcept {
		selectedAgentObject_ = objectId == EMPTY_UUID ? std::nullopt : std::optional<UUID>{objectId};
	}

	void NavigationWindow::Update() {
		// Closed windows perform no geometry extraction, preserving the editor's
		// idle cost while navigation diagnostics continue publishing independently.
		const bool diagnosticsWindowOpen = IsDiagnosticsWindowOpen();
		if(!diagnosticsWindowOpen) return;
		NavigationSceneOverlayInput input = overlayInput_;
		// World and agent roots are independently immutable publications. Copy only
		// the current values; no editor path reads ObjectManager or agent storage.
		if(const auto world = TerraEngine::Navigation::NavigationSystem::Instance().PublishedWorld())
			input.coverage = world->coverage;

		const auto diagnostics = TerraEngine::Navigation::NavigationAgentSystem::Instance().PublishedDiagnostics();
		if(diagnostics) {
			input.agentDiagnostics = {diagnostics->activeAgents, diagnostics->requestingAgents,
				diagnostics->followingAgents, diagnostics->arrivedAgents, diagnostics->stoppedAgents,
				diagnostics->failedAgents, diagnostics->pendingCommands, diagnostics->commandCapacity,
				diagnostics->retainedReceipts, diagnostics->currentOwnedMemoryBytes, diagnostics->retryCount,
				diagnostics->rejectedCommandCount, diagnostics->coalescedCommandCount,
				diagnostics->lastProtectedPublishMicroseconds};
			input.selectedAgent.reset();
			if(selectedAgentObject_) {
				const auto identity = TerraEngine::Navigation::NavigationAgentSystem::Instance()
					.ResolvePublishedAgent(*selectedAgentObject_);
				const auto selected = identity ? TerraEngine::Navigation::NavigationAgentSystem::Instance()
					.ReadPublishedSelectedDiagnostic(*identity, diagnostics->revision) : std::nullopt;
				if(selected) {
					NavigationAgentOverlayRecord agent{};
					agent.guidance = selected->agent.guidance;
					agent.routeCurrent = selected->routeCurrent;
					agent.routePoints.reserve(selected->routePoints.size());
					for(const auto& point : selected->routePoints) agent.routePoints.push_back(point.position);
					input.selectedAgent = std::move(agent);
				}
			}
		}
		overlay_ = ExtractNavigationOverlay(input);
	}

	const NavigationSceneOverlaySnapshot& NavigationWindow::Overlay() const noexcept { return overlay_; }

	void NavigationWindow::OnGui() {
		// The same open-state gate is used by Scene View. Rendering this small
		// summary therefore never requests data while the diagnostics window is off.
		Update();
		const NavigationSceneOverlaySnapshot& overlay = Overlay();
		ImGui::TextUnformatted("Ground navigation diagnostics");
		ImGui::Separator();
		ImGui::Text("Coverage: %zu", overlay.coverage.size());
		ImGui::Text("Connections: %zu", overlay.connectionEndpoints.size() / 2U);
		ImGui::Text("Route points: %zu", overlay.routePoints.size());
		ImGui::Text("Dirty coverage: %zu", overlay.dirtyCoverage.size());
		const NavigationAgentOverlayPresentation selected = BuildNavigationAgentOverlayPresentation(overlay.selectedAgent);
		ImGui::Separator();
		ImGui::Text("Agents: %zu active, %zu following, %zu requesting, %zu failed",
			overlay.agentDiagnostics.activeAgents, overlay.agentDiagnostics.followingAgents,
			overlay.agentDiagnostics.requestingAgents, overlay.agentDiagnostics.failedAgents);
		ImGui::Text("Commands: %zu / %zu pending, %llu rejected, %llu coalesced, %llu retries",
			overlay.agentDiagnostics.pendingCommands, overlay.agentDiagnostics.commandCapacity,
			static_cast<unsigned long long>(overlay.agentDiagnostics.rejectedCommandCount),
			static_cast<unsigned long long>(overlay.agentDiagnostics.coalescedCommandCount),
			static_cast<unsigned long long>(overlay.agentDiagnostics.retryCount));
		ImGui::Text("Current agent-owned memory: %zu bytes", overlay.agentDiagnostics.currentOwnedMemoryBytes);
		ImGui::Text("Last protected agent publish: %llu us",
			static_cast<unsigned long long>(overlay.agentDiagnostics.lastProtectedPublishMicroseconds));
		if(overlay.selectedAgent) {
			const auto& guidance = overlay.selectedAgent->guidance;
			const auto printLocation = [](const char* label, const TerraEngine::Navigation::NavigationLocation& location) {
				const auto& position = location.position;
				ImGui::Text("%s: world %llu cell (%lld, %lld, %lld), offset (%.3f, %.3f, %.3f)",
					label, static_cast<unsigned long long>(location.world.worldId.value),
					static_cast<long long>(position.cell.x), static_cast<long long>(position.cell.y),
					static_cast<long long>(position.cell.z), position.offsetMetres.x,
					position.offsetMetres.y, position.offsetMetres.z);
			};
			ImGui::Text("Selected agent: %s (%s)",
				std::string(TerraEngine::Navigation::ToString(selected.state)).c_str(),
				std::string(TerraEngine::Navigation::ToString(selected.reason)).c_str());
			ImGui::Text("Guidance: %s; route publication: %s",
				selected.usableGuidance ? "current and usable" : "unusable",
				overlay.selectedAgent->routeCurrent ? "current" : "invalidated or unavailable");
			if(overlay.selectedAgent->routeTruncated)
				ImGui::TextUnformatted("Selected route display is bounded to a prefix; its final drawn point is not the destination.");
			ImGui::Text("Destination: %s", std::string(TerraEngine::Navigation::ToString(guidance.destination.kind)).c_str());
			if(guidance.destination.kind == TerraEngine::Navigation::NavigationDestinationKind::Location)
				printLocation("Destination location", guidance.destination.location);
			else
				ImGui::Text("Target: %s (lifetime %llu)",
					TerraEngine::ToHexString(guidance.destination.object.objectId).c_str(),
					static_cast<unsigned long long>(guidance.destination.object.lifetimeGeneration));
			printLocation("Next waypoint", guidance.nextWaypoint);
			ImGui::Text("Remaining distance: %.3f m", guidance.remainingDistanceMetres);
			ImGui::Text("Config %llu, intent %llu, publication %llu, route %llu",
				static_cast<unsigned long long>(guidance.configurationRevision),
				static_cast<unsigned long long>(guidance.intentSequence),
				static_cast<unsigned long long>(guidance.publicationGeneration),
				static_cast<unsigned long long>(guidance.routeGeneration));
			ImGui::TextUnformatted(selected.explanation.c_str());
		}
		if(overlay.truncated)
			ImGui::TextUnformatted("Display is bounded; additional diagnostics were omitted.");
	}
}
