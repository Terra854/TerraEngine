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
#include <cmath>

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
	const glm::uvec2 gameTargetSize(
		static_cast<std::uint32_t>(std::max(1.0f, std::ceil(imageSize.x))),
		static_cast<std::uint32_t>(std::max(1.0f, std::ceil(imageSize.y))));
	RenderSystem::Instance().SetGameViewTargetSize(gameTargetSize);
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
		auto& objectManager = ObjectManager::instance();
		auto cameras = objectManager.getObjectsWithComponents<Transform, CameraComponent>();
		hasCameras = !cameras.empty();
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

	// The VG toolbar combo still controls visualization modes, but the retired
	// top-left diagnostic panel is intentionally absent. It was sourced from
	// renderer summary snapshots that can lag behind the live Game View path,
	// while the right-side Stats panel reads the completed draw counters.

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_Stats) {
		RenderStatsSnapshot stats{};
		const bool statsReady = (m_Owner != nullptr) && m_Owner->TryGetGameViewRenderStats(stats) && texID != nullptr && hasCameras;

		char line0[96];
		char line1[96];
		char line2[96];
		char line3[96];
		char line4[192];
		char line5[192];
		char line6[192];
		char line7[192];
		char line8[96];
		char line9[192];
		char line10[96];
		char line11[160];
		char line12[160];
		char line13[160];
		char line14[160];
		char line15[160];
		char line16[160];
		char line17[160];
		char line18[192];
		char line19[192];
		char line20[160];
		char line21[192];
		char line22[192];
		char line23[192];
		char line24[192];
		char line25[192];
		char line26[192];
		char line27[192];
		char line28[192];

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (statsReady) {
			const std::uint32_t displayedCulled = stats.taskCullingActive ? stats.meshletsCulled : 0u;
			const std::uint32_t displayedDispatched = stats.taskCullingActive ? stats.meshletsDispatched : stats.meshletsTotal;

			std::snprintf(line0, sizeof(line0), "FPS: %.1f", stats.fps);
			std::snprintf(line1, sizeof(line1), "Frame: %.2f ms", stats.frameTimeMs);
			std::snprintf(line2, sizeof(line2), "Draw Calls: %u", stats.drawCalls);
			std::snprintf(line3, sizeof(line3), "Triangles: %u", stats.triangleCount);
			const std::string selectedMeshletText = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
				"Meshlets Selected",
				stats.meshletsTotal,
				stats.meshletCounterSource,
				stats.meshletCounterReason);
			std::snprintf(line4, sizeof(line4), "%s", selectedMeshletText.c_str());
			// Task-culling debug overlays can intentionally keep culled meshlets
			// visible so users can inspect them. When the selected and dispatched
			// counts match, printing both rows looks like duplicated/conflicting
			// data, so show the dispatch row only when it adds information.
			if (displayedDispatched != stats.meshletsTotal)
			{
				const std::string dispatchedMeshletText = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
					"Meshlets Dispatched",
					displayedDispatched,
					stats.meshletCounterSource,
					stats.meshletCounterReason);
				std::snprintf(line5, sizeof(line5), "%s", dispatchedMeshletText.c_str());
			}
			else
			{
				line5[0] = '\0';
			}
			const std::string culledMeshletText = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
				"Meshlets Culled",
				displayedCulled,
				stats.meshletCounterSource,
				stats.meshletCounterReason);
			std::snprintf(line6, sizeof(line6), "%s", culledMeshletText.c_str());
			const std::string hiZCulledText = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
				"  - Culled by Hi-Z",
				stats.meshletsCulledHiZ,
				stats.meshletCounterSource,
				stats.meshletCounterReason);
			std::snprintf(line7, sizeof(line7), "%s", hiZCulledText.c_str());
			std::snprintf(line8, sizeof(line8), "Task Culling: %s", stats.taskCullingActive ? "On" : "Off");
			const std::string hiZStatusText = TerraEngine::Renderer::FormatVirtualGeometryHiZOverlayStatus(
				stats.hiZState,
				stats.hiZActive,
				stats.hiZStatusReason);
			std::snprintf(line9, sizeof(line9), "Hi-Z Occlusion: %s", hiZStatusText.c_str());
			std::snprintf(line10, sizeof(line10), "Variable Rate Shading: %s", stats.vrsActive ? "Active" : "Off");
			const std::string packetText = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
				"VG Cluster Packets",
				stats.virtualGeometryClusterPackets,
				stats.meshletCounterSource,
				stats.meshletCounterReason);
			std::snprintf(line11, sizeof(line11), "%s", packetText.c_str());
			const std::string batchText = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
				"VG Submitted Batches",
				stats.virtualGeometrySubmittedBatches,
				stats.meshletCounterSource,
				stats.meshletCounterReason);
			std::snprintf(line12, sizeof(line12), "%s", batchText.c_str());
			std::snprintf(line13, sizeof(line13), "VG Legacy Fallback Batches: %u", stats.virtualGeometryLegacyFallbackBatches);
			std::snprintf(line14, sizeof(line14), "VG Candidate Packets: %u", stats.virtualGeometryCandidatePackets);
			std::snprintf(line15, sizeof(line15), "VG Compacted Packets: %u", stats.virtualGeometryCompactedPackets);
			std::snprintf(line16, sizeof(line16), "VG Rejected Packets: %u", stats.virtualGeometryRejectedPackets);
			std::snprintf(line17, sizeof(line17), "VG Fail-Open Packets: %u", stats.virtualGeometryFailOpenPackets);
			std::snprintf(line18, sizeof(line18), "VG Pages R/Q/M/F: %u/%u/%u/%u",
				stats.virtualGeometryResidentPages,
				stats.virtualGeometryRequestedPages,
				stats.virtualGeometryMissingPages,
				stats.virtualGeometryFailedPages);
			std::snprintf(line19, sizeof(line19), "VG Evicted/Pinned Fallback: %u/%u",
				stats.virtualGeometryEvictedPages,
				stats.virtualGeometryPinnedFallbackPages);
			std::snprintf(line20, sizeof(line20), "Hi-Z Baseline: %s, risks %u",
				stats.hiZBaselineMatched ? "Matched" : "Mismatch",
				stats.hiZFalseOcclusionRiskWarnings);
			const std::string gpuPacketText = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
				"VG GPU Packet Records",
				stats.virtualGeometryGpuPacketRecords,
				stats.meshletCounterSource,
				stats.meshletCounterReason);
			std::snprintf(line21, sizeof(line21), "%s", gpuPacketText.c_str());
			std::snprintf(line22, sizeof(line22), "VG Generated Submissions: %u valid, %u invalid",
				stats.virtualGeometryValidGeneratedSubmissions,
				stats.virtualGeometryInvalidGeneratedSubmissions);
			std::snprintf(line23, sizeof(line23), "VG Compaction Dispatches/Readbacks: %u/%u",
				stats.virtualGeometryGpuCompactionDispatches,
				stats.virtualGeometryGpuCounterReadbacks);
			std::snprintf(line24, sizeof(line24), "VG GPU Fallbacks: %u packets, %u batches",
				stats.virtualGeometryGpuFallbackAffectedPackets,
				stats.virtualGeometryGpuFallbackAffectedBatches);
			std::snprintf(line25, sizeof(line25), "Visible Cache: %s, unique touches %u, deduped %u",
				stats.visibleCommandCacheHit ? "hit" : "miss",
				stats.visibleStreamingUniqueMeshes + stats.visibleStreamingUniqueMaterials + stats.visibleStreamingUniqueTextures,
				stats.visibleStreamingDuplicateTouchesEliminated);
			std::snprintf(line26, sizeof(line26), "VG Page Sources H/F/E/M: %u/%u/%u/%u",
				stats.virtualGeometryEmittedHighDetailPages,
				stats.virtualGeometryEmittedFallbackPages,
				stats.virtualGeometryEmittedEmbeddedFallbackPages,
				stats.virtualGeometryMissingPageSources);
			std::snprintf(line27, sizeof(line27), "VG Page Payload Bytes R/F/E: %llu/%llu/%llu",
				static_cast<unsigned long long>(stats.virtualGeometryResidentPagePayloadBytes),
				static_cast<unsigned long long>(stats.virtualGeometryFallbackPagePayloadBytes),
				static_cast<unsigned long long>(stats.virtualGeometryEmbeddedFallbackBytes));
			std::snprintf(line28, sizeof(line28), "VG Page Requests/Retires: %u/%u",
				stats.virtualGeometryPageSourceRequests,
				stats.virtualGeometryPageSourceRetirements);
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
			line11[0] = '\0';
			line12[0] = '\0';
			line13[0] = '\0';
			line14[0] = '\0';
			line15[0] = '\0';
			line16[0] = '\0';
			line17[0] = '\0';
			line18[0] = '\0';
			line19[0] = '\0';
			line20[0] = '\0';
			line21[0] = '\0';
			line22[0] = '\0';
			line23[0] = '\0';
			line24[0] = '\0';
			line25[0] = '\0';
			line26[0] = '\0';
			line27[0] = '\0';
			line28[0] = '\0';
		}

		const char* lines[] = {line0, line1, line2, line3, line4, line5, line6, line7, line11, line12, line21, line22, line23, line24, line13, line14, line15, line16, line17, line18, line19, line26, line27, line28, line25, line20, line8, line9, line10};
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
