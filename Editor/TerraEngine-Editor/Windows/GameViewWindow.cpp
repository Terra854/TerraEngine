#include "GameViewWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Components/CameraComponent.h"
#include "Engine/Components/Transform.h"
#include "Engine/Core/Object/ObjectManager.h"
#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
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

void GameViewWindow::OnGui() {
	// --- Toolbar ---
	ImGui::SetNextItemWidth(150.0f);
	if (ImGui::BeginCombo("##aspect", m_AspectRatios[m_SelectedAspectRatioIndex].Name.c_str())) {
		for (int i = 0; i < static_cast<int>(m_AspectRatios.size()); i++) {
			bool isSelected = (m_SelectedAspectRatioIndex == i);
			if (ImGui::Selectable(m_AspectRatios[i].Name.c_str(), isSelected)) {
				m_SelectedAspectRatioIndex = i;
			}
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
	if (option.Width > 0 && option.Height > 0) {
		float targetRatio = option.Width / option.Height;
		float currentRatio = availableSize.x / availableSize.y;
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

	auto cameras = ObjectManager::instance().getObjectsWithComponents<Transform, CameraComponent>();
	bool hasCameras = !cameras.empty();
	for (auto obj : cameras) {
		Transform* transform = obj.getComponent<Transform>();
		CameraComponent* camera = obj.getComponent<CameraComponent>();

		glm::vec3 pos = transform->position;
		glm::quat rot = transform->rotation;

		const glm::vec3 defaultForward(0.0f, 0.0f, -1.0f);
		const glm::vec3 forward = rot * defaultForward;
		glm::vec3 up = rot * camera->up;
		if (glm::length(up) < 0.0001f) up = glm::vec3(0.0f, 1.0f, 0.0f);

		glm::mat4 view = glm::lookAt(pos, pos + forward, glm::normalize(up));
		glm::mat4 proj = glm::perspective(glm::radians(camera->verticalFovDegrees), aspect, camera->nearPlane, camera->farPlane);
		proj[1][1] *= -1;
		RenderSystem::Instance().SetGameViewCamera(view, proj, pos);
	}

	// --- Draw Render ---
	void* texID = m_Owner->GetGameTextureID();
	ImVec2 cursorPos = ImGui::GetCursorScreenPos();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availableSize.x, cursorPos.y + availableSize.y), ImGui::GetColorU32(ImGuiCol_WindowBg));

	ImVec2 imageMin = cursorPos;
	ImVec2 imageMax = ImVec2(cursorPos.x + availableSize.x, cursorPos.y + availableSize.y);
	if (texID && hasCameras) {
		float xOffset = (availableSize.x - imageSize.x) * 0.5f;
		float yOffset = (availableSize.y - imageSize.y) * 0.5f;
		imageMin = ImVec2(cursorPos.x + xOffset, cursorPos.y + yOffset);
		imageMax = ImVec2(imageMin.x + imageSize.x, imageMin.y + imageSize.y);

		ImGui::PushClipRect(cursorPos, ImVec2(cursorPos.x + availableSize.x, cursorPos.y + availableSize.y), true);
		ImGui::SetCursorScreenPos(imageMin);
		ImGui::Image((ImTextureID)texID, imageSize);
		ImGui::PopClipRect();
	} else {
		const char* text = !hasCameras ? "No objects with cameras in the hierarchy" : "No Camera Rendering";
		ImVec2 textSize = ImGui::CalcTextSize(text);
		ImGui::SetCursorScreenPos(ImVec2(cursorPos.x + (availableSize.x - textSize.x) * 0.5f, cursorPos.y + (availableSize.y - textSize.y) * 0.5f));
		ImGui::Text("%s", text);
	}

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
		for (const char* line : lines) {
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
		for (const char* line : lines) {
			if (line == nullptr || line[0] == '\0') {
				continue;
			}
			drawList->AddText(ImVec2(panelMin.x + panelPadding, textY), IM_COL32(232, 232, 232, 255), line);
			textY += lineHeight;
		}
	}
}

