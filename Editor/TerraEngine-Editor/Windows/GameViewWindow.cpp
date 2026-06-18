// GameViewWindow.cpp — Game View Window Implementation
//
// Implements the editor window that renders the live game output, allowing developers to test gameplay within the editor.
// It manages the transition between editor and game camera modes and provides runtime performance statistics.

#include "GameViewWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Components/CameraComponent.h"
#include "Engine/Components/Transform.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/UI/UISystem.h"
#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>

GameViewWindow::GameViewWindow(MainEditorSubsystem* owner)
	: EditorWindow("Game View"), m_Owner(owner) {
	m_AspectRatios.push_back({ "Free Aspect", 0.0f, 0.0f, false });
	m_AspectRatios.push_back({ "16:9 Aspect", 16.0f, 9.0f, false });
	m_AspectRatios.push_back({ "16:10 Aspect", 16.0f, 10.0f, false });
	m_AspectRatios.push_back({ "Full HD (1920x1080)", 1920.0f, 1080.0f, true });
	m_AspectRatios.push_back({ "4K UHD (3840x2160)", 3840.0f, 2160.0f, true });
	m_AspectRatios.push_back({ "Stand. (4:3)", 4.0f, 3.0f, false });
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
	ImGui::Checkbox("Mute Audio", &m_MuteAudio);
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
	UISystem::Instance().SetGameViewportSize(glm::vec2(imageSize.x, imageSize.y));

	const bool isAssetPrefabEditing = m_Owner != nullptr && m_Owner->IsAssetPrefabEditingMode();
	if (isAssetPrefabEditing)
	{
		const auto activePrefabCameras = ObjectManager::instance().getObjectsWithComponents<Transform, CameraComponent>();
		if (activePrefabCameras.empty())
		{
			// Prefab assets opened from the asset browser are edited in isolation.
			// If they have no active authored camera, keep the Game View idle.
			// ObjectManager's component query excludes inactive objects and
			// disabled CameraComponents, so disabled cameras do not count here.
			UISystem::Instance().SetEditorGameViewPointer(glm::vec2(-1.0f, -1.0f), false, false);
			const ImVec2 cursorPos = ImGui::GetCursorScreenPos();
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availableSize.x, cursorPos.y + availableSize.y), ImGui::GetColorU32(ImGuiCol_WindowBg));

			const char* text = "Game View paused while editing prefab asset";
			const ImVec2 textSize = ImGui::CalcTextSize(text);
			ImGui::SetCursorScreenPos(ImVec2(cursorPos.x + (availableSize.x - textSize.x) * 0.5f, cursorPos.y + (availableSize.y - textSize.y) * 0.5f));
			ImGui::Text("%s", text);
			return;
		}

		// An active prefab-local camera is a valid preview source for asset-style
		// prefab editing, so re-enable Game View submission for this frame and
		// let the regular camera collection below submit those prefab cameras.
		RenderSystem::Instance().SetGameViewSuppressed(false);
	}

	// --- Update Cameras ---
	float aspect = (imageSize.y > 0.0001f) ? (imageSize.x / imageSize.y) : 1.0f;
	if (aspect <= 0.0001f) aspect = 1.0f;

	const bool hasPrefabModeSceneCameras = !isAssetPrefabEditing && (m_Owner != nullptr) && m_Owner->TryApplyPrefabModeGameViewCameras(aspect);
	bool hasCameras = hasPrefabModeSceneCameras;
	if (!hasPrefabModeSceneCameras)
	{
		auto cameras = ObjectManager::instance().getObjectsWithComponents<Transform, CameraComponent>();
		hasCameras = !cameras.empty();
		const std::size_t cameraCount = isAssetPrefabEditing ? std::min<std::size_t>(cameras.size(), 1u) : cameras.size();
		// Iterate over the active camera snapshot; asset-prefab preview uses the
		// first active prefab camera only so one camera owns the Game View output.
		for (std::size_t cameraIndex = 0; cameraIndex < cameraCount; ++cameraIndex) {
			auto obj = cameras[cameraIndex];
			Transform* transform = obj.getComponent<Transform>();
			CameraComponent* camera = obj.getComponent<CameraComponent>();

			glm::vec3 pos = transform->position;
			glm::quat rot = transform->rotation;

			const glm::vec3 defaultForward(0.0f, 0.0f, -1.0f);
			const glm::vec3 forward = rot * defaultForward;
			glm::vec3 up = rot * camera->up;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (glm::length(up) < 0.0001f) up = glm::vec3(0.0f, 1.0f, 0.0f);

			glm::mat4 view = glm::lookAt(pos, pos + forward, glm::normalize(up));
			glm::mat4 proj = glm::perspective(glm::radians(camera->verticalFovDegrees), aspect, camera->nearPlane, camera->farPlane);
			proj[1][1] *= -1;
			RenderSystem::Instance().SetGameViewCamera(view, proj, pos);
		}
	}

	// --- Draw Render ---
	void* texID = m_Owner->GetGameTextureID();
	ImVec2 cursorPos = ImGui::GetCursorScreenPos();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availableSize.x, cursorPos.y + availableSize.y), ImGui::GetColorU32(ImGuiCol_WindowBg));

	ImVec2 imageMin = cursorPos;
	ImVec2 imageMax = ImVec2(cursorPos.x + availableSize.x, cursorPos.y + availableSize.y);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (texID && hasCameras) {
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

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_Stats) {
		RenderStatsSnapshot stats{};
		const bool statsReady = (m_Owner != nullptr) && m_Owner->TryGetGameViewRenderStats(stats) && texID != nullptr && hasCameras;

		char line0[96];
		char line1[96];
		char line2[96];
		char line3[96];
		char line4[96];
		char line5[96];
		char line6[96];
		char line7[96];
		char line8[96];
		char line9[96];
		char line10[96];

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (statsReady) {
			const std::uint32_t displayedCulled = stats.taskCullingActive ? stats.meshletsCulled : 0u;
			const std::uint32_t displayedDispatched = stats.taskCullingActive ? stats.meshletsDispatched : stats.meshletsTotal;

			std::snprintf(line0, sizeof(line0), "FPS: %.1f", stats.fps);
			std::snprintf(line1, sizeof(line1), "Frame: %.2f ms", stats.frameTimeMs);
			std::snprintf(line2, sizeof(line2), "Draw Calls: %u", stats.drawCalls);
			std::snprintf(line3, sizeof(line3), "Triangles: %u", stats.triangleCount);
			std::snprintf(line4, sizeof(line4), "Meshlets Total: %u", stats.meshletsTotal);
			std::snprintf(line5, sizeof(line5), "Meshlets Dispatched: %u", displayedDispatched);
			std::snprintf(line6, sizeof(line6), "Meshlets Culled: %u", displayedCulled);
			std::snprintf(line7, sizeof(line7), "  - Culled by Hi-Z: %u", stats.meshletsCulledHiZ);
			std::snprintf(line8, sizeof(line8), "Task Culling: %s", stats.taskCullingActive ? "On" : "Off");
			std::snprintf(line9, sizeof(line9), "Hi-Z Occlusion: %s", stats.hiZActive ? "Active" : "Off");
			std::snprintf(line10, sizeof(line10), "Variable Rate Shading: %s", stats.vrsActive ? "Active" : "Off");
		} else {
			std::snprintf(line0, sizeof(line0), "Stats unavailable");
			std::snprintf(line1, sizeof(line1), "No active camera/render stats");
			line2[0] = '\0';
			line3[0] = '\0';
			line4[0] = '\0';
			line5[0] = '\0';
			line6[0] = '\0';
			line7[0] = '\0';
			line8[0] = '\0';
			line9[0] = '\0';
			line10[0] = '\0';
		}

		const char* lines[] = {line0, line1, line2, line3, line4, line5, line6, line7, line8, line9, line10};
		float textWidth = 0.0f;
		int lineCount = 0;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const char* line : lines) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (line == nullptr || line[0] == '\0') {
				continue;
			}
			textWidth = std::max(textWidth, ImGui::CalcTextSize(line).x);
			++lineCount;
		}

		const float panelPadding = 8.0f;
		const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
		const float panelWidth = textWidth + panelPadding * 2.0f;
		const float panelHeight = panelPadding * 2.0f + lineHeight * static_cast<float>(lineCount);
		const ImVec2 panelMax = ImVec2(imageMax.x - 10.0f, imageMin.y + 10.0f + panelHeight);
		const ImVec2 panelMin = ImVec2(panelMax.x - panelWidth, imageMin.y + 10.0f);

		drawList->AddRectFilled(panelMin, panelMax, IM_COL32(35, 35, 35, 195), 4.0f);
		float textY = panelMin.y + panelPadding;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const char* line : lines) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (line == nullptr || line[0] == '\0') {
				continue;
			}
			drawList->AddText(ImVec2(panelMin.x + panelPadding, textY), IM_COL32(232, 232, 232, 255), line);
			textY += lineHeight;
		}
	}
}
