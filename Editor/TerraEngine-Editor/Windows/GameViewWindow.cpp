// GameViewWindow.cpp — Game View Window Implementation
//
// Implements the editor window that renders the live game output, allowing developers to test gameplay within the editor.
// It manages the transition between editor and game camera modes and provides runtime performance statistics.

#include "GameViewWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Renderer/RendererRecovery.h"
#include "Engine/Components/CameraComponent.h"
#include "Engine/Components/Transform.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/UI/UISystem.h"
#include "Engine/Audio/AudioSystem.h"
#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <string>

namespace
{
	void DrawGameViewPlaceholder(const ImVec2& availableSize, const char* message)
	{
		// A content-free Game View emits one background rectangle and one label.
		// It deliberately avoids render-target, texture, stats, and UI-batch reads.
		const ImVec2 cursorPosition = ImGui::GetCursorScreenPos();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(
			cursorPosition,
			ImVec2(cursorPosition.x + availableSize.x, cursorPosition.y + availableSize.y),
			ImGui::GetColorU32(ImGuiCol_WindowBg));

		const ImVec2 textSize = ImGui::CalcTextSize(message);
		ImGui::SetCursorScreenPos(ImVec2(
			cursorPosition.x + (availableSize.x - textSize.x) * 0.5f,
			cursorPosition.y + (availableSize.y - textSize.y) * 0.5f));
		ImGui::TextUnformatted(message);
	}
}

GameViewWindow::GameViewWindow(MainEditorSubsystem* owner)
	: EditorWindow("Game View", true, "Rendering"), m_Owner(owner) {
	m_AspectRatios.push_back({ "Free Aspect", 0.0f, 0.0f, false });
	m_AspectRatios.push_back({ "16:9 Aspect", 16.0f, 9.0f, false });
	m_AspectRatios.push_back({ "16:10 Aspect", 16.0f, 10.0f, false });
	m_AspectRatios.push_back({ "Full HD (1920x1080)", 1920.0f, 1080.0f, true });
	m_AspectRatios.push_back({ "4K UHD (3840x2160)", 3840.0f, 2160.0f, true });
	m_AspectRatios.push_back({ "Stand. (4:3)", 4.0f, 3.0f, false });
}

void GameViewWindow::OnBeforeBegin()
{
	// Begin can report a docked tab as collapsed, in which case OnGui is not
	// called. Clear this frame's request first and let an expanded active window
	// explicitly opt back in below. This keeps hidden/collapsed GPU work bounded.
	RenderSystem::Instance().SetGameViewSuppressed(true);
	RenderSystem::Instance().SetGameViewTargetSize(glm::uvec2(0u));
	UISystem::Instance().SetEditorGameViewPointer(glm::vec2(-1.0f, -1.0f), false, false);
}

// GameViewWindow::OnGui is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void GameViewWindow::OnGui() {
	// --- Toolbar ---
	ImGui::SetNextItemWidth(150.0f);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginCombo("##aspect", m_AspectRatios[m_SelectedAspectRatioIndex].Name.c_str())) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (int i = 0; i < static_cast<int>(m_AspectRatios.size()); i++) {
			bool isSelected = (m_SelectedAspectRatioIndex == i);
			if (ImGui::Selectable(m_AspectRatios[i].Name.c_str(), isSelected)) {
				m_SelectedAspectRatioIndex = i;
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (isSelected) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}

	ImGui::SameLine();
	ImGui::SetNextItemWidth(100.0f);
	ImGui::SliderFloat("Scale", &m_Scale, 1.0f, 5.0f, "%.1fx");

	ImGui::SameLine();
	ImGui::Checkbox("Maximize On Play", &m_MaximizeOnPlay);
	ImGui::SameLine();
	// Reflect external mixer/managed changes every frame, then publish checkbox
	// edits through the same master-mix state without conflating mute with pause.
	m_MuteAudio = TerraEngine::Audio::AudioSystem::Instance().LatestMixSnapshot().master.muted;
	if(ImGui::Checkbox("Mute Audio", &m_MuteAudio)) {
		const auto currentMix = TerraEngine::Audio::AudioSystem::Instance().LatestMixSnapshot();
		std::string diagnostic;
		(void)TerraEngine::Audio::AudioSystem::Instance().SetMasterMix(currentMix.master.volume, m_MuteAudio, diagnostic);
	}
	ImGui::SameLine();
	// Unity-style toolbar text toggle behavior for stats.
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, m_StatsToggleRounding);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, m_StatsTogglePadding);
	ImGui::PushStyleColor(ImGuiCol_Button, m_Stats ? m_StatsToggleActiveBg : m_StatsToggleInactiveBg);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, m_StatsToggleHoverBg);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, m_StatsToggleActiveBg);
	ImGui::PushStyleColor(ImGuiCol_Text, m_StatsToggleText);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("Stats")) {
		m_Stats = !m_Stats;
	}
	ImGui::PopStyleColor(4);
	ImGui::PopStyleVar(2);
	ImGui::SameLine();
	ImGui::Checkbox("Gizmos", &m_Gizmos);
	ImGui::SameLine();
	constexpr std::uint32_t kGameViewVirtualGeometryOutputId = 2u;
	if (!m_VirtualGeometryDebugModeInitialized)
	{
		m_VirtualGeometryDebugMode = RenderSystem::Instance().GetVirtualGeometryDebugVisualizationMode(RenderViewType::Game, kGameViewVirtualGeometryOutputId);
		m_VirtualGeometryDebugModeInitialized = true;
	}
	const VirtualGeometryDebugVisualizationMode normalizedDebugMode = NormalizeVirtualGeometryDebugVisualizationMode(m_VirtualGeometryDebugMode);
	if (normalizedDebugMode != m_VirtualGeometryDebugMode)
	{
		m_VirtualGeometryDebugMode = normalizedDebugMode;
		RenderSystem::Instance().SetVirtualGeometryDebugVisualizationMode(RenderViewType::Game, kGameViewVirtualGeometryOutputId, m_VirtualGeometryDebugMode);
	}
	int virtualGeometryDebugMode = static_cast<int>(m_VirtualGeometryDebugMode);
	ImGui::SetNextItemWidth(115.0f);
	const VirtualGeometryDebugVisualizationMode currentVirtualGeometryDebugMode =
		static_cast<VirtualGeometryDebugVisualizationMode>(virtualGeometryDebugMode);
	const bool virtualGeometryComboOpen = ImGui::BeginCombo("VG", ToString(currentVirtualGeometryDebugMode));
	if (ImGui::IsItemHovered())
	{
		// Keep the toolbar compact, but reveal the color legend on hover so
		// developers can understand a mode without opening external docs.
		ImGui::SetTooltip(
			"Virtual Geometry Visualizer\n%s",
			GetVirtualGeometryDebugVisualizationLegend(currentVirtualGeometryDebugMode));
	}
	if (virtualGeometryComboOpen)
	{
		for (const VirtualGeometryDebugVisualizationMode mode : GetVirtualGeometryDebugVisualizationModes())
		{
			const int modeIndex = static_cast<int>(mode);
			const bool selected = virtualGeometryDebugMode == modeIndex;
			if (ImGui::Selectable(ToString(mode), selected))
			{
				virtualGeometryDebugMode = modeIndex;
				m_VirtualGeometryDebugMode = mode;
				RenderSystem::Instance().SetVirtualGeometryDebugVisualizationMode(RenderViewType::Game, kGameViewVirtualGeometryOutputId, m_VirtualGeometryDebugMode);
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%s\n%s", ToString(mode), GetVirtualGeometryDebugVisualizationLegend(mode));
			}
			if (selected)
			{
				ImGui::SetItemDefaultFocus();
			}
		}
		ImGui::EndCombo();
	}

	// --- Calculate Render Area ---
	ImVec2 availableSize = ImGui::GetContentRegionAvail();
	if (availableSize.x < 1.0f) availableSize.x = 1.0f;
	if (availableSize.y < 1.0f) availableSize.y = 1.0f;

	ImVec2 imageSize = availableSize;
	AspectRatio& option = m_AspectRatios[m_SelectedAspectRatioIndex];
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (option.Width > 0 && option.Height > 0) {
		float targetRatio = option.Width / option.Height;
		float currentRatio = availableSize.x / availableSize.y;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (currentRatio > targetRatio) {
			imageSize.x = availableSize.y * targetRatio;
			imageSize.y = availableSize.y;
		} else {
			imageSize.x = availableSize.x;
			imageSize.y = availableSize.x / targetRatio;
		}
	}

	imageSize.x *= m_Scale;
	imageSize.y *= m_Scale;
	// --- Update Cameras ---
	float aspect = (imageSize.y > 0.0001f) ? (imageSize.x / imageSize.y) : 1.0f;
	if (aspect <= 0.0001f) aspect = 1.0f;

	const bool isAssetPrefabEditing = m_Owner != nullptr && m_Owner->IsAssetPrefabEditingMode();
	const bool hasPrefabModeSceneCameras = !isAssetPrefabEditing && (m_Owner != nullptr) && m_Owner->TryApplyPrefabModeGameViewCameras(aspect);
	bool hasCameras = hasPrefabModeSceneCameras;
	std::vector<Object> cameras;
	if (!hasPrefabModeSceneCameras)
	{
		auto& objectManager = ObjectManager::instance();
		const std::uint64_t membershipRevision = objectManager.GetRuntimeActiveMembershipRevision();
		if (m_CameraActivityCache.NeedsRefresh(membershipRevision))
		{
			cameras = objectManager.getObjectsWithComponents<Transform, CameraComponent>();
			m_CameraActivityCache.Store(membershipRevision, !cameras.empty());
		}
		hasCameras = m_CameraActivityCache.hasActiveCamera;
		if (hasCameras && cameras.empty())
		{
			// Active cameras still need current transforms every frame. Only the
			// repeated empty query is retained by the generation cache.
			cameras = objectManager.getObjectsWithComponents<Transform, CameraComponent>();
			hasCameras = !cameras.empty();
			if (!hasCameras)
			{
				// Component enablement can change between the membership-generation
				// read and the component query. Preserve the observed empty result so
				// the remainder of this frame cannot present stale camera content.
				m_CameraActivityCache.Store(membershipRevision, false);
			}
		}

		const TerraEngine::Editor::GameViewUpdateDecision contentDecision =
			TerraEngine::Editor::BuildGameViewUpdateDecision(true, hasCameras, false, m_Stats);
		if (contentDecision.path == TerraEngine::Editor::GameViewUpdatePath::ContentFree)
		{
			RenderSystem::Instance().SetGameViewSuppressed(true);
			const char* message = isAssetPrefabEditing
				? "Game View paused while editing prefab asset"
				: "No objects with cameras in the hierarchy";
			DrawGameViewPlaceholder(availableSize, message);
			return;
		}

		const std::size_t cameraCount = isAssetPrefabEditing ? std::min<std::size_t>(cameras.size(), 1u) : cameras.size();
		// Iterate over the active camera snapshot; asset-prefab preview uses the
		// first active prefab camera only so one camera owns the Game View output.
		for (std::size_t cameraIndex = 0; cameraIndex < cameraCount; ++cameraIndex) {
			Object obj = cameras[cameraIndex];
			Transform* transform = objectManager.GetComponent<Transform>(obj);
			CameraComponent* camera = objectManager.GetComponent<CameraComponent>(obj);
			if (transform == nullptr || camera == nullptr)
			{
				continue;
			}

			glm::vec3 pos = transform->position;
			glm::quat rot = transform->rotation;

			const glm::vec3 defaultForward(0.0f, 0.0f, -1.0f);
			const glm::vec3 forward = rot * defaultForward;
			glm::vec3 up = rot * camera->up;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (glm::length(up) < 0.0001f) up = glm::vec3(0.0f, 1.0f, 0.0f);

			glm::mat4 view = glm::lookAt(pos, pos + forward, glm::normalize(up));
			// The Vulkan renderer and Hi-Z culling both expect 0..1 clip-space
			// depth, so use the explicit GLM helper instead of relying on
			// translation-unit include order for GLM_FORCE_DEPTH_ZERO_TO_ONE.
			glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(camera->verticalFovDegrees), aspect, camera->nearPlane, camera->farPlane);
			proj[1][1] *= -1;
			RenderSystem::Instance().SetGameViewCamera(view, proj, pos);
		}
	}

	const TerraEngine::Renderer::RendererRecoverySnapshot recoverySnapshot =
		RenderSystem::Instance().GetLatestRendererRecoverySnapshot();
	const bool recoveryActive = TerraEngine::Renderer::IsRendererRecoveryActive(recoverySnapshot.state);
	const TerraEngine::Editor::GameViewUpdateDecision updateDecision =
		TerraEngine::Editor::BuildGameViewUpdateDecision(true, hasCameras, recoveryActive, m_Stats);
	if (updateDecision.path == TerraEngine::Editor::GameViewUpdatePath::RendererRecovery)
	{
		const std::string status = std::string("Renderer unavailable: ")
			+ TerraEngine::Renderer::ToString(recoverySnapshot.state)
			+ (recoverySnapshot.message.empty() ? std::string{} : " - " + recoverySnapshot.message);
		DrawGameViewPlaceholder(availableSize, status.c_str());
		return;
	}

	// Only an active, renderable Game View publishes target and viewport work.
	// Hidden, collapsed, recovery, and camera-free paths leave the request empty.
	RenderSystem::Instance().SetGameViewSuppressed(false);
	const glm::uvec2 gameTargetSize(
		static_cast<std::uint32_t>(std::max(1.0f, std::ceil(imageSize.x))),
		static_cast<std::uint32_t>(std::max(1.0f, std::ceil(imageSize.y))));
	RenderSystem::Instance().SetGameViewTargetSize(gameTargetSize);
	UISystem::Instance().SetGameViewportSize(glm::vec2(imageSize.x, imageSize.y));

	// --- Draw Render ---
	void* texID = (m_Owner != nullptr && updateDecision.queryRenderTexture) ? m_Owner->GetGameTextureID() : nullptr;
	ImVec2 cursorPos = ImGui::GetCursorScreenPos();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availableSize.x, cursorPos.y + availableSize.y), ImGui::GetColorU32(ImGuiCol_WindowBg));

	ImVec2 imageMin = cursorPos;
	ImVec2 imageMax = ImVec2(cursorPos.x + availableSize.x, cursorPos.y + availableSize.y);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (texID && hasCameras && updateDecision.publishRuntimePointer) {
		float xOffset = (availableSize.x - imageSize.x) * 0.5f;
		float yOffset = (availableSize.y - imageSize.y) * 0.5f;
		imageMin = ImVec2(cursorPos.x + xOffset, cursorPos.y + yOffset);
		imageMax = ImVec2(imageMin.x + imageSize.x, imageMin.y + imageSize.y);

		ImGui::PushClipRect(cursorPos, ImVec2(cursorPos.x + availableSize.x, cursorPos.y + availableSize.y), true);
		ImGui::SetCursorScreenPos(imageMin);
		ImGui::Image((ImTextureID)texID, imageSize);
		ImGui::PopClipRect();

		const ImVec2 mousePosition = ImGui::GetIO().MousePos;
		const bool pointerInsideImage = mousePosition.x >= imageMin.x
			&& mousePosition.x <= imageMax.x
			&& mousePosition.y >= imageMin.y
			&& mousePosition.y <= imageMax.y;
		glm::vec2 localPointerPosition(-1.0f, -1.0f);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (pointerInsideImage)
		{
			const glm::vec2 rawLocalPointerPosition(mousePosition.x - imageMin.x, mousePosition.y - imageMin.y);
			const UIRenderBatch currentUiBatch = UISystem::Instance().GetCombinedFrameBatch();
			const glm::vec2 logicalViewportSize = (currentUiBatch.viewportSize.x > 0.0f && currentUiBatch.viewportSize.y > 0.0f)
				? currentUiBatch.viewportSize
				: glm::vec2(imageSize.x, imageSize.y);

			// Letterboxed and scaled editor Game Views must translate the mouse
			// into the logical UI batch space, otherwise hover/clicks react as if
			// the UI were stretched across the full host window.
			localPointerPosition = glm::vec2(
				(rawLocalPointerPosition.x / std::max(imageSize.x, 1.0f)) * logicalViewportSize.x,
				(rawLocalPointerPosition.y / std::max(imageSize.y, 1.0f)) * logicalViewportSize.y);
		}

		// The editor translates ImGui's screen-space mouse into Game View-local
		// coordinates so UISystem can use the exact same hit-testing code as the
		// exported runtime without guessing about docking offsets.
		UISystem::Instance().SetEditorGameViewPointer(
			localPointerPosition,
			pointerInsideImage,
			ImGui::IsMouseDown(ImGuiMouseButton_Left));
	} else {
		UISystem::Instance().SetEditorGameViewPointer(glm::vec2(-1.0f, -1.0f), false, false);
		const char* text = !hasCameras ? "No objects with cameras in the hierarchy" : "No Camera Rendering";
		ImVec2 textSize = ImGui::CalcTextSize(text);
		ImGui::SetCursorScreenPos(ImVec2(cursorPos.x + (availableSize.x - textSize.x) * 0.5f, cursorPos.y + (availableSize.y - textSize.y) * 0.5f));
		ImGui::Text("%s", text);
	}

	// The VG toolbar combo still controls visualization modes, but the retired
	// top-left diagnostic panel is intentionally absent. It was sourced from
	// renderer summary snapshots that can lag behind the live Game View path,
	// while the right-side Stats panel reads the completed draw counters.

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (updateDecision.collectRenderStats) {
		RenderStatsSnapshot stats{};
		const bool statsReady = texID != nullptr
			&& hasCameras
			&& (m_Owner != nullptr)
			&& m_Owner->TryGetGameViewRenderStats(stats);

		std::array<std::string, 5> lines;
		if (statsReady) {
			char buffer[96];
			std::snprintf(buffer, sizeof(buffer), "FPS: %.1f", stats.fps);
			lines[0] = buffer;
			std::snprintf(buffer, sizeof(buffer), "Frame: %.2f ms", stats.frameTimeMs);
			lines[1] = buffer;
			std::snprintf(buffer, sizeof(buffer), "Draw Calls: %u", stats.drawCalls);
			lines[2] = buffer;
			std::snprintf(buffer, sizeof(buffer), "Triangles: %u", stats.triangleCount);
			lines[3] = buffer;
			// Keep the Game View focused on whether the frame is healthy. The
			// detailed meshlet, page, Hi-Z, and packet readbacks now live in the
			// Resource Residency window where they can be grouped without
			// covering the running game.
			lines[4] = stats.detailedRenderDiagnosticsRequested ? "Diagnostics: detailed" : "Diagnostics: compact";
		} else {
			lines[0] = "Stats unavailable";
			lines[1] = "No active camera/render stats";
		}

		float textWidth = 0.0f;
		int lineCount = 0;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const std::string& line : lines) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (line.empty()) {
				continue;
			}
			textWidth = std::max(textWidth, ImGui::CalcTextSize(line.c_str()).x);
			++lineCount;
		}

		const float panelPadding = 8.0f;
		const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
		const float availablePanelWidth = std::max(96.0f, imageSize.x - 20.0f);
		const float panelWidth = std::min(textWidth + panelPadding * 2.0f, availablePanelWidth);
		const float panelHeight = panelPadding * 2.0f + lineHeight * static_cast<float>(lineCount);
		const ImVec2 panelMax = ImVec2(imageMax.x - 10.0f, imageMin.y + 10.0f + panelHeight);
		const ImVec2 panelMin = ImVec2(panelMax.x - panelWidth, imageMin.y + 10.0f);

		drawList->AddRectFilled(panelMin, panelMax, IM_COL32(35, 35, 35, 195), 4.0f);
		float textY = panelMin.y + panelPadding;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const std::string& line : lines) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (line.empty()) {
				continue;
			}
			drawList->AddText(ImVec2(panelMin.x + panelPadding, textY), IM_COL32(232, 232, 232, 255), line.c_str());
			textY += lineHeight;
		}
	}
}
