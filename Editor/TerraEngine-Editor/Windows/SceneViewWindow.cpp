// SceneViewWindow.cpp — Scene View Window Implementation
//
// Implements the 3D viewport interface, rendering the scene to an ImGui texture and providing manipulation gizmos.
// It handles editor-specific rendering features like the grid, selection outlines, and camera movement controls.

#include "SceneViewWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "../Subsystems/UndoRedoSystem.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/File Formats/TerraEngineMeshFile.h"
#include "Engine/Components/MeshRenderComponent.h"
#include "Engine/Input/InputSystem.h"
#include "Engine/Input/InputCodes.h"
#include "Engine/Window/WindowSystem.h"
#include <GLFW/glfw3.h>
#include "Engine/Components/Transform.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Components/CameraComponent.h"
#include "Engine/Components/LightComponent.h"
#include <imgui.h>
#include <ImGuizmo.h>
#include <entt/entt.hpp>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <string>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/constants.hpp>

namespace
{
// SafeNormalize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
glm::vec3 SafeNormalize(const glm::vec3& value, const glm::vec3& fallback)
{
	if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
	{
		return fallback;
	}

	const float length = glm::length(value);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (length <= 0.0001f)
	{
		return fallback;
	}

	return value / length;
}

// BuildBasisFromDirection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void BuildBasisFromDirection(const glm::vec3& direction, glm::vec3& outRight, glm::vec3& outUp)
{
	const glm::vec3 fallbackUp = (std::abs(glm::dot(direction, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
		? glm::vec3(1.0f, 0.0f, 0.0f)
		: glm::vec3(0.0f, 1.0f, 0.0f);
	outRight = SafeNormalize(glm::cross(direction, fallbackUp), glm::vec3(1.0f, 0.0f, 0.0f));
	outUp = SafeNormalize(glm::cross(outRight, direction), glm::vec3(0.0f, 1.0f, 0.0f));
}

bool ProjectWorldPoint(
	const glm::vec3& worldPosition,
	const glm::mat4& viewProjection,
	const ImVec2& viewportPos,
	const ImVec2& viewportSize,
	ImVec2& outScreenPosition)
{
	const glm::vec4 clipPosition = viewProjection * glm::vec4(worldPosition, 1.0f);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (clipPosition.w <= 0.0001f)
	{
		return false;
	}

	const glm::vec3 ndc = glm::vec3(clipPosition) / clipPosition.w;
	outScreenPosition.x = viewportPos.x + (ndc.x + 1.0f) * 0.5f * viewportSize.x;
	outScreenPosition.y = viewportPos.y + (1.0f - ndc.y) * 0.5f * viewportSize.y;
	return true;
}

void DrawWorldLine(
	ImDrawList* drawList,
	const glm::vec3& worldStart,
	const glm::vec3& worldEnd,
	const glm::mat4& viewProjection,
	const ImVec2& viewportPos,
	const ImVec2& viewportSize,
	ImU32 color,
	float thickness)
{
	ImVec2 screenStart{};
	ImVec2 screenEnd{};
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!ProjectWorldPoint(worldStart, viewProjection, viewportPos, viewportSize, screenStart)
		|| !ProjectWorldPoint(worldEnd, viewProjection, viewportPos, viewportSize, screenEnd))
	{
		return;
	}

	drawList->AddLine(screenStart, screenEnd, color, thickness);
}

void DrawWorldCircle(
	ImDrawList* drawList,
	const glm::vec3& center,
	const glm::vec3& axisA,
	const glm::vec3& axisB,
	float radius,
	int segments,
	const glm::mat4& viewProjection,
	const ImVec2& viewportPos,
	const ImVec2& viewportSize,
	ImU32 color,
	float thickness)
{
	const float step = glm::two_pi<float>() / static_cast<float>(segments);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int segment = 0; segment < segments; ++segment)
	{
		const float angleA = step * static_cast<float>(segment);
		const float angleB = step * static_cast<float>(segment + 1);
		const glm::vec3 pointA = center + (axisA * std::cos(angleA) + axisB * std::sin(angleA)) * radius;
		const glm::vec3 pointB = center + (axisA * std::cos(angleB) + axisB * std::sin(angleB)) * radius;
		DrawWorldLine(drawList, pointA, pointB, viewProjection, viewportPos, viewportSize, color, thickness);
	}
}

void DrawDirectionalLightGizmo(
	ImDrawList* drawList,
	const glm::vec3& position,
	const glm::vec3& direction,
	const glm::mat4& viewProjection,
	const ImVec2& viewportPos,
	const ImVec2& viewportSize,
	ImU32 color)
{
	glm::vec3 right(1.0f, 0.0f, 0.0f);
	glm::vec3 up(0.0f, 1.0f, 0.0f);
	BuildBasisFromDirection(direction, right, up);

	const float coreRadius = 0.35f;
	DrawWorldCircle(drawList, position, right, up, coreRadius, 32, viewProjection, viewportPos, viewportSize, color, 1.5f);

	// Draw radial rays around the icon to match a classic directional-light "sun".
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int rayIndex = 0; rayIndex < 12; ++rayIndex)
	{
		const float angle = glm::two_pi<float>() * (static_cast<float>(rayIndex) / 12.0f);
		const glm::vec3 ringDirection = SafeNormalize(right * std::cos(angle) + up * std::sin(angle), right);
		const glm::vec3 rayStart = position + ringDirection * 0.42f;
		const glm::vec3 rayEnd = position + ringDirection * 0.62f;
		DrawWorldLine(drawList, rayStart, rayEnd, viewProjection, viewportPos, viewportSize, color, 1.5f);
	}

	// Draw multiple parallel lines to communicate directional lighting.
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int lineIndex = 0; lineIndex < 4; ++lineIndex)
	{
		const float offset = (static_cast<float>(lineIndex) - 1.5f) * 0.2f;
		const glm::vec3 start = position + up * offset;
		const glm::vec3 end = start + direction * 2.4f;
		DrawWorldLine(drawList, start, end, viewProjection, viewportPos, viewportSize, color, 1.8f);
	}
}

void DrawPointLightGizmo(
	ImDrawList* drawList,
	const glm::vec3& position,
	float range,
	const glm::mat4& viewProjection,
	const ImVec2& viewportPos,
	const ImVec2& viewportSize,
	ImU32 color)
{
	const float radius = std::max(range, 0.1f);
	DrawWorldCircle(drawList, position, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), radius, 48, viewProjection, viewportPos, viewportSize, color, 1.5f);
	DrawWorldCircle(drawList, position, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), radius, 48, viewProjection, viewportPos, viewportSize, color, 1.5f);
	DrawWorldCircle(drawList, position, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), radius, 48, viewProjection, viewportPos, viewportSize, color, 1.5f);
}

void DrawSpotLightGizmo(
	ImDrawList* drawList,
	const glm::vec3& position,
	const glm::vec3& direction,
	float range,
	float innerConeAngle,
	float outerConeAngle,
	const glm::mat4& viewProjection,
	const ImVec2& viewportPos,
	const ImVec2& viewportSize,
	ImU32 color)
{
	glm::vec3 right(1.0f, 0.0f, 0.0f);
	glm::vec3 up(0.0f, 1.0f, 0.0f);
	BuildBasisFromDirection(direction, right, up);

	const float clampedRange = std::max(range, 0.1f);
	const float clampedOuter = std::clamp(outerConeAngle, 0.05f, 1.55f);
	const float clampedInner = std::clamp(innerConeAngle, 0.0f, clampedOuter - 0.01f);
	const glm::vec3 coneCenter = position + direction * clampedRange;
	const float outerRadius = std::tan(clampedOuter) * clampedRange;
	const float innerRadius = std::tan(clampedInner) * clampedRange;

	DrawWorldLine(drawList, position, coneCenter, viewProjection, viewportPos, viewportSize, color, 1.2f);
	DrawWorldCircle(drawList, coneCenter, right, up, outerRadius, 48, viewProjection, viewportPos, viewportSize, color, 1.6f);
	DrawWorldCircle(drawList, coneCenter, right, up, innerRadius, 36, viewProjection, viewportPos, viewportSize, color, 1.1f);

	// Outline a sparse cone wireframe so users can read direction and range quickly.
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int edgeIndex = 0; edgeIndex < 8; ++edgeIndex)
	{
		const float angle = glm::two_pi<float>() * (static_cast<float>(edgeIndex) / 8.0f);
		const glm::vec3 edgeDirection = SafeNormalize(right * std::cos(angle) + up * std::sin(angle), right);
		const glm::vec3 outerPoint = coneCenter + edgeDirection * outerRadius;
		DrawWorldLine(drawList, position, outerPoint, viewProjection, viewportPos, viewportSize, color, 1.2f);
	}
}
}

SceneViewWindow::SceneViewWindow(MainEditorSubsystem* owner)
	// EditorWindow is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: EditorWindow("Scene View"), m_Owner(owner) {
	m_CurrentGizmoOperation = ImGuizmo::TRANSLATE;
}

// SceneViewWindow::OnGui is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SceneViewWindow::OnGui() {
	ImGuizmo::BeginFrame();

	void* texID = m_Owner->GetSceneTextureID();

	constexpr std::uint32_t kSceneViewVirtualGeometryOutputId = 1u;
	if (!m_VirtualGeometryDebugModeInitialized)
	{
		m_VirtualGeometryDebugMode = RenderSystem::Instance().GetVirtualGeometryDebugVisualizationMode(RenderViewType::Scene, kSceneViewVirtualGeometryOutputId);
		m_VirtualGeometryDebugModeInitialized = true;
	}
	const VirtualGeometryDebugVisualizationMode normalizedDebugMode = NormalizeVirtualGeometryDebugVisualizationMode(m_VirtualGeometryDebugMode);
	if (normalizedDebugMode != m_VirtualGeometryDebugMode)
	{
		m_VirtualGeometryDebugMode = normalizedDebugMode;
		RenderSystem::Instance().SetVirtualGeometryDebugVisualizationMode(RenderViewType::Scene, kSceneViewVirtualGeometryOutputId, m_VirtualGeometryDebugMode);
	}
	int virtualGeometryDebugMode = static_cast<int>(m_VirtualGeometryDebugMode);
	ImGui::SetNextItemWidth(115.0f);
	const VirtualGeometryDebugVisualizationMode currentVirtualGeometryDebugMode =
		static_cast<VirtualGeometryDebugVisualizationMode>(virtualGeometryDebugMode);
	const bool virtualGeometryComboOpen = ImGui::BeginCombo("VG", ToString(currentVirtualGeometryDebugMode));
	if (ImGui::IsItemHovered())
	{
		// Scene View uses the same compact toolbar as Game View; the hover text
		// carries the user-facing color contract for the selected mode.
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
				RenderSystem::Instance().SetVirtualGeometryDebugVisualizationMode(RenderViewType::Scene, kSceneViewVirtualGeometryOutputId, m_VirtualGeometryDebugMode);
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
	ImGui::SameLine();
	ImGui::Checkbox("VG Stats", &m_ShowVirtualGeometryDiagnostics);
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Show virtual-geometry packet and Hi-Z baseline diagnostics in the Scene View.");
	}
	
	// We draw the image filling the content region
	ImVec2 contentSize = ImGui::GetContentRegionAvail();
	contentSize.x = std::max(contentSize.x, 1.0f);
	contentSize.y = std::max(contentSize.y, 1.0f);
	ImVec2 windowPos = ImGui::GetWindowPos();
	ImVec2 cursorPos = ImGui::GetCursorScreenPos(); // Absolute position for ImGuizmo
	const glm::uvec2 sceneTargetSize(
		static_cast<std::uint32_t>(std::max(1.0f, std::ceil(contentSize.x))),
		static_cast<std::uint32_t>(std::max(1.0f, std::ceil(contentSize.y))));
	RenderSystem::Instance().SetSceneViewTargetSize(sceneTargetSize);
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (texID) {
		ImGui::Image((ImTextureID)texID, contentSize);
	} else {
		ImGui::Text("Scene rendering not available.");
	}
	RenderStatsSnapshot sceneStats = RenderSystem::Instance().GetLatestRenderStats(RenderViewType::Scene);
	if (m_ShowVirtualGeometryDiagnostics && sceneStats.valid && (sceneStats.virtualGeometryClusterPackets > 0u || sceneStats.virtualGeometrySubmittedBatches > 0u))
	{
		const std::string packetText = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
			"VG Cluster Packets",
			sceneStats.virtualGeometryClusterPackets,
			sceneStats.meshletCounterSource,
			sceneStats.meshletCounterReason);
		const std::string batchText = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
			"VG Submitted Batches",
			sceneStats.virtualGeometrySubmittedBatches,
			sceneStats.meshletCounterSource,
			sceneStats.meshletCounterReason);
		const std::string compactText = "VG Compacted Packets: " + std::to_string(sceneStats.virtualGeometryCompactedPackets);
		const std::string rejectedText = "VG Rejected Packets: " + std::to_string(sceneStats.virtualGeometryRejectedPackets);
		const std::string fallbackText = "VG Legacy Fallback Batches: " + std::to_string(sceneStats.virtualGeometryLegacyFallbackBatches);
		const std::string hiZBaselineText = "Hi-Z Baseline: "
			+ std::string(sceneStats.hiZBaselineMatched ? "Matched" : "Mismatch")
			+ ", risks "
			+ std::to_string(sceneStats.hiZFalseOcclusionRiskWarnings);
		const char* lines[] = { packetText.c_str(), batchText.c_str(), compactText.c_str(), rejectedText.c_str(), fallbackText.c_str(), hiZBaselineText.c_str() };
		float textWidth = 0.0f;
		for (const char* line : lines)
		{
			textWidth = std::max(textWidth, ImGui::CalcTextSize(line).x);
		}

		const float panelPadding = 8.0f;
		const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
		const ImVec2 panelMin(cursorPos.x + 12.0f, cursorPos.y + 12.0f);
		const ImVec2 panelMax(panelMin.x + textWidth + panelPadding * 2.0f, panelMin.y + lineHeight * 6.0f + panelPadding * 2.0f);
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(panelMin, panelMax, IM_COL32(34, 40, 46, 190), 4.0f);
		ImVec2 textPos(panelMin.x + panelPadding, panelMin.y + panelPadding);
		for (const char* line : lines)
		{
			drawList->AddText(textPos, IM_COL32(235, 241, 246, 255), line);
			textPos.y += lineHeight;
		}
	}

	// Prepare for Gizmos
	ImGuizmo::SetOrthographic(false);
	ImGuizmo::SetDrawlist();
	ImGuizmo::SetRect(cursorPos.x, cursorPos.y, contentSize.x, contentSize.y);

	const bool isFocused = ImGui::IsWindowFocused();
	const bool isHovered = ImGui::IsWindowHovered();
	// Apply editor camera input before deriving matrices so the submitted view
	// matrix and camera world position describe the same camera state.
	if (isFocused || isHovered) {
		HandleCameraInput(ImGui::GetIO().DeltaTime);
	}

	// Calculate Camera Matrices
	glm::mat4 view = glm::mat4(1.0f);
	glm::mat4 proj = glm::mat4(1.0f);
	
	// m_CameraRot.x = pitch, .y = yaw
	glm::quat orientation = glm::quat(glm::vec3(m_CameraRot.x, m_CameraRot.y, 0.0f));
	glm::vec3 forward = orientation * glm::vec3(0, 0, -1);
	glm::vec3 up = orientation * glm::vec3(0, 1, 0);
	view = glm::lookAt(m_CameraPos, m_CameraPos + forward, up);
	
	float aspect = (contentSize.y > 0.0001f) ? (contentSize.x / contentSize.y) : 1.0f;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (aspect <= 0.0001f) aspect = 1.0f;
	// Scene View feeds the same projection into picking, gizmos, rendering, and
	// Hi-Z validation. Make the depth convention explicit so all those users
	// agree on Vulkan's 0..1 clip-space depth.
	glm::mat4 projNoFlip = glm::perspectiveRH_ZO(glm::radians(std::max(m_Fov, 0.1f)), aspect, 0.1f, 1000.0f);
	proj = projNoFlip;
	proj[1][1] *= -1; // Vulkan flip

	// Set Scene View Camera for next render
	RenderSystem::Instance().SetSceneViewCamera(view, proj, m_CameraPos);
	ApplyPrefabContextTintForNextRender();

	HandlePicking(view, projNoFlip, cursorPos, contentSize);

	DrawGizmos(view, projNoFlip, cursorPos, contentSize);
}

// RayAABB is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool RayAABB(const glm::vec3& rayOrigin, const glm::vec3& rayDir, const glm::vec3& aabbMin, const glm::vec3& aabbMax, float& t)
{
	float tmin = 0.0f;
	float tmax = std::numeric_limits<float>::max();

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int i = 0; i < 3; i++)
	{
		if (std::abs(rayDir[i]) < 0.0001f)
		{
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (rayOrigin[i] < aabbMin[i] || rayOrigin[i] > aabbMax[i]) return false;
		}
		else
		{
			float invD = 1.0f / rayDir[i];
			float t1 = (aabbMin[i] - rayOrigin[i]) * invD;
			float t2 = (aabbMax[i] - rayOrigin[i]) * invD;

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (t1 > t2) std::swap(t1, t2);

			tmin = std::max(tmin, t1);
			tmax = std::min(tmax, t2);

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (tmin > tmax) return false;
		}
	}

	t = tmin;
	return true;
}

// SceneViewWindow::HandlePicking is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SceneViewWindow::HandlePicking(const glm::mat4& view, const glm::mat4& proj, const ImVec2& viewportPos, const ImVec2& viewportSize) {
	if (!ImGui::IsMouseClicked(0) || ImGuizmo::IsOver() || m_IsFlyCamActive || !ImGui::IsWindowHovered()) return;

	ImVec2 mousePos = ImGui::GetMousePos();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (mousePos.x < viewportPos.x || mousePos.x > viewportPos.x + viewportSize.x ||
		mousePos.y < viewportPos.y || mousePos.y > viewportPos.y + viewportSize.y) return;

	// NDC coordinates
	float x = ((mousePos.x - viewportPos.x) / viewportSize.x) * 2.0f - 1.0f;
	float y = 1.0f - ((mousePos.y - viewportPos.y) / viewportSize.y) * 2.0f;

	glm::mat4 invVP = glm::inverse(proj * view);
	glm::vec4 rayStart = invVP * glm::vec4(x, y, 0.01f, 1.0f);
	glm::vec4 rayEnd = invVP * glm::vec4(x, y, 1.0f, 1.0f);

	glm::vec3 rayOrigin = glm::vec3(rayStart) / rayStart.w;
	glm::vec3 rayDirection = glm::normalize((glm::vec3(rayEnd) / rayEnd.w) - rayOrigin);

	entt::entity bestEntity = entt::null;
	float bestDist = std::numeric_limits<float>::max();

	auto& registry = ObjectManager::instance().getRegistry();
	
	auto computeWorldMatrix = [&](entt::entity entity) -> glm::mat4 {
		glm::mat4 world(1.0f);
		entt::entity current = entity;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		while (current != entt::null && registry.valid(current)) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (auto* t = registry.try_get<Transform>(current)) {
				glm::mat4 local(1.0f);
				local = glm::translate(local, t->position);
				local = local * glm::mat4_cast(t->rotation);
				local = glm::scale(local, t->scale);
				world = local * world;
			}
			if (std::optional<Object> currentObject = ObjectManager::instance().getObject(current))
			{
				std::optional<Object> parent = ObjectManager::instance().getParent(*currentObject);
				current = parent ? parent->EntityId() : entt::null;
			}
			else
			{
				current = entt::null;
			}
		}
		return world;
	};

	auto viewTransforms = registry.view<Transform>();
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto entity : viewTransforms) {
		glm::mat4 worldMatrix = computeWorldMatrix(entity);
		glm::vec3 worldPos = glm::vec3(worldMatrix[3]);

		bool hit = false;
		float t = 0.0f;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto* meshComp = registry.try_get<MeshRenderComponent>(entity)) {
			if (Mesh* mesh = ResourceManager::Instance().GetMesh(meshComp->meshId)) {
				// Transform ray to local space
				glm::mat4 invWorld = glm::inverse(worldMatrix);
				glm::vec3 localRayOrigin = glm::vec3(invWorld * glm::vec4(rayOrigin, 1.0f));
				glm::vec3 localRayDir = glm::normalize(glm::vec3(invWorld * glm::vec4(rayDirection, 0.0f)));

				glm::vec3 aabbMin(
					mesh->header.quantization.positionOffset[0] - mesh->header.quantization.positionScale[0],
					mesh->header.quantization.positionOffset[1] - mesh->header.quantization.positionScale[1],
					mesh->header.quantization.positionOffset[2] - mesh->header.quantization.positionScale[2]
				);
				glm::vec3 aabbMax(
					mesh->header.quantization.positionOffset[0] + mesh->header.quantization.positionScale[0],
					mesh->header.quantization.positionOffset[1] + mesh->header.quantization.positionScale[1],
					mesh->header.quantization.positionOffset[2] + mesh->header.quantization.positionScale[2]
				);

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (RayAABB(localRayOrigin, localRayDir, aabbMin, aabbMax, t)) {
					// Convert t back to world distance (approx)
					glm::vec3 hitPointLocal = localRayOrigin + localRayDir * t;
					glm::vec3 hitPointWorld = glm::vec3(worldMatrix * glm::vec4(hitPointLocal, 1.0f));
					t = glm::distance(rayOrigin, hitPointWorld);
					hit = true;
				}
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!hit) {
			// Fallback to point check
			glm::vec3 v = worldPos - rayOrigin;
			t = glm::dot(v, rayDirection);
			if (t >= 0) {
				glm::vec3 closestPoint = rayOrigin + rayDirection * t;
				float d = glm::length(worldPos - closestPoint);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (d < 0.5f) {
					hit = true;
				}
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (hit && t < bestDist) {
			bestDist = t;
			bestEntity = entity;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (bestEntity != entt::null) {
		m_Owner->SetSelectedEntity(bestEntity);
	} else {
		m_Owner->SetSelectedEntity(std::nullopt);
	}
}

// SceneViewWindow::HandleCameraInput is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SceneViewWindow::HandleCameraInput(float deltaTime) {
	// Need to include WindowSystem and GLFW for this
	auto* window = static_cast<GLFWwindow*>(WindowSystem::Instance().GetNativeHandle());

	if (m_IsFlyCamActive) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
			m_IsFlyCamActive = false;
			
			// Restore cursor visibility
			glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
			ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
		} else {
			// Calculate delta manually to avoid ImGui delta issues with re-centering
			auto currentPos = InputSystem::Instance().GetCursorPosition();
			double deltaX = currentPos.first - m_StoredCursorPos.first;
			double deltaY = currentPos.second - m_StoredCursorPos.second;

			// Reset cursor to stored position to keep it locked
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (deltaX != 0.0 || deltaY != 0.0) {
				InputSystem::Instance().SetCursorPosition(m_StoredCursorPos.first, m_StoredCursorPos.second);
			}

			// Fly Cam Movement
			glm::quat orientation = glm::quat(glm::vec3(m_CameraRot.x, m_CameraRot.y, 0.0f));
			glm::vec3 forward = orientation * glm::vec3(0, 0, -1);
			glm::vec3 right = orientation * glm::vec3(1, 0, 0);
			glm::vec3 up = orientation * glm::vec3(0, 1, 0);

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::IsKeyDown(ImGuiKey_W)) m_CameraPos += forward * m_CameraSpeed * deltaTime;
			if (ImGui::IsKeyDown(ImGuiKey_S)) m_CameraPos -= forward * m_CameraSpeed * deltaTime;
			if (ImGui::IsKeyDown(ImGuiKey_D)) m_CameraPos += right * m_CameraSpeed * deltaTime;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::IsKeyDown(ImGuiKey_A)) m_CameraPos -= right * m_CameraSpeed * deltaTime;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::IsKeyDown(ImGuiKey_E)) m_CameraPos += up * m_CameraSpeed * deltaTime;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::IsKeyDown(ImGuiKey_Q)) m_CameraPos -= up * m_CameraSpeed * deltaTime;

			// Mouse Look
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (deltaX != 0.0 || deltaY != 0.0) {
				m_CameraRot.y -= (float)deltaX * m_MouseSensitivity * 0.01f;
				m_CameraRot.x -= (float)deltaY * m_MouseSensitivity * 0.01f;
				
				// Clamp pitch
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (m_CameraRot.x > glm::radians(89.0f)) m_CameraRot.x = glm::radians(89.0f);
				if (m_CameraRot.x < glm::radians(-89.0f)) m_CameraRot.x = glm::radians(-89.0f);
			}
		}
	} else {
		// Only activate if clicked THIS frame while hovered and not using gizmo
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::IsWindowHovered() && !ImGuizmo::IsUsing()) {
			m_IsFlyCamActive = true;
			m_StoredCursorPos = InputSystem::Instance().GetCursorPosition();
			
			// Hide the cursor explicitly since we tell ImGui not to touch it
			glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
			ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
		}
	}
	
	// Gizmo Shortcuts
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
		if (ImGui::IsKeyPressed(ImGuiKey_W)) m_CurrentGizmoOperation = ImGuizmo::TRANSLATE;
		if (ImGui::IsKeyPressed(ImGuiKey_E)) m_CurrentGizmoOperation = ImGuizmo::ROTATE;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::IsKeyPressed(ImGuiKey_R)) m_CurrentGizmoOperation = ImGuizmo::SCALE;
	}
}

void SceneViewWindow::CollectFocusedPrefabObjectIds(entt::entity entity, std::unordered_set<TerraEngine::UUID>& objectIds)
{
	auto& objectManager = ObjectManager::instance();
	std::optional<Object> object = objectManager.getObject(entity);
	if (!object)
	{
		return;
	}

	objectIds.insert(objectManager.getUUID(*object));
	if (const auto* mesh = objectManager.GetComponent<MeshRenderComponent>(*object); mesh != nullptr && mesh->spatialObjectId != TerraEngine::EMPTY_UUID)
	{
		// RenderSystem keys draw commands by spatial object id when a mesh has
		// one. Include that id too so prefab-focus tinting matches the renderer
		// instead of accidentally washing out the selected object.
		objectIds.insert(mesh->spatialObjectId);
	}
	for (Object child : objectManager.getChildren(*object))
	{
		CollectFocusedPrefabObjectIds(child.EntityId(), objectIds);
	}
}

void SceneViewWindow::ApplyPrefabContextTintForNextRender()
{
	std::unordered_set<TerraEngine::UUID> focusedObjectIds;
	if (m_Owner == nullptr || !m_Owner->ShouldSceneViewUseSceneContextForPrefabEdit())
	{
		RenderSystem::Instance().SetSceneViewContextTint(focusedObjectIds);
		return;
	}

	const auto selectedEntity = m_Owner->GetSelectedEntity();
	if (!selectedEntity.has_value())
	{
		RenderSystem::Instance().SetSceneViewContextTint(focusedObjectIds);
		return;
	}

	auto& registry = ObjectManager::instance().getRegistry();
	if (!registry.valid(*selectedEntity))
	{
		RenderSystem::Instance().SetSceneViewContextTint(focusedObjectIds);
		return;
	}

	// Include the selected prefab root and every child in the active prefab
	// branch so nested prefab objects stay fully colored while scene context
	// around them is washed out by the renderer.
	CollectFocusedPrefabObjectIds(*selectedEntity, focusedObjectIds);
	RenderSystem::Instance().SetSceneViewContextTint(focusedObjectIds);
}

// SceneViewWindow::DrawGizmos is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SceneViewWindow::DrawGizmos(const glm::mat4& view, const glm::mat4& proj, const ImVec2& viewportPos, const ImVec2& viewportSize) {
	// Selection
	auto selectedEntity = m_Owner->GetSelectedEntity();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (selectedEntity.has_value()) {
		auto& registry = ObjectManager::instance().getRegistry();
		if (registry.valid(*selectedEntity)) {
			Transform* transform = registry.try_get<Transform>(*selectedEntity);
			CameraComponent* camera = registry.try_get<CameraComponent>(*selectedEntity);
			LightComponent* light = registry.try_get<LightComponent>(*selectedEntity);

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (transform || camera || light) {
				// Helper to compute world transform
				auto computeWorldMatrix = [&](entt::entity entity) -> glm::mat4 {
					glm::mat4 world(1.0f);
					entt::entity current = entity;
					// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
					while (current != entt::null && registry.valid(current)) {
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (auto* t = registry.try_get<Transform>(current)) {
							glm::mat4 local(1.0f);
							local = glm::translate(local, t->position);
							local = local * glm::mat4_cast(t->rotation);
							local = glm::scale(local, t->scale);
							world = local * world; // Parent * Child
						}
						if (std::optional<Object> currentObject = ObjectManager::instance().getObject(current))
						{
							std::optional<Object> parent = ObjectManager::instance().getParent(*currentObject);
							current = parent ? parent->EntityId() : entt::null;
						}
						else
						{
							current = entt::null;
						}
					}
					return world;
				};

				glm::mat4 model = computeWorldMatrix(*selectedEntity);

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (transform) {
					if (ImGuizmo::Manipulate(
						glm::value_ptr(view), 
						glm::value_ptr(proj), 
						(ImGuizmo::OPERATION)m_CurrentGizmoOperation, 
						ImGuizmo::LOCAL, 
						glm::value_ptr(model),
						nullptr, 
						m_UsingSnap ? m_SnapValue : nullptr)) 
					{
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (!m_WasUsingGizmo) {
							m_WasUsingGizmo = true;
							m_GizmoStartPos = transform->position;
							m_GizmoStartRot = transform->rotation;
							m_GizmoStartScale = transform->scale;
						}

						// Convert updated World Matrix back to Local Matrix
						glm::mat4 parentMatrix(1.0f);
						std::optional<Object> selectedObject = ObjectManager::instance().getObject(*selectedEntity);
						std::optional<Object> parent = selectedObject ? ObjectManager::instance().getParent(*selectedObject) : std::nullopt;
						if (parent) {
							parentMatrix = computeWorldMatrix(parent->EntityId());
						}
						
						glm::mat4 localMatrix = glm::inverse(parentMatrix) * model;

						// Decompose and update
						glm::vec3 scale;
						glm::quat rotation;
						glm::vec3 translation;
						glm::vec3 skew;
						glm::vec4 perspective;
						glm::decompose(localMatrix, scale, rotation, translation, skew, perspective);
						
						transform->position = translation;
						transform->rotation = rotation;
						transform->scale = scale;
					}

					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (m_WasUsingGizmo && !ImGuizmo::IsUsing()) {
						m_WasUsingGizmo = false;
						// SceneView owns transient transform preview only. The
						// structural editor operations named in the STM plan
						// (create/delete/rename/reparent/component edits) are
						// routed through Hierarchy, Inspector, and Automation
						// command paths, where transaction grouping can wrap the
						// resulting undo commands without disturbing gizmo drag
						// feedback.
						UndoRedoSystem::Instance().PushCommand(std::make_unique<ChangeTransformCommand>(
							 *selectedEntity,
							m_GizmoStartPos, m_GizmoStartRot, m_GizmoStartScale,
							transform->position, transform->rotation, transform->scale
						));
					}
				}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (camera) {
					float nearP = std::max(0.001f, camera->nearPlane);
					float farP = std::max(nearP + 0.001f, camera->farPlane);

					float h_n = tanf(glm::radians(camera->verticalFovDegrees) * 0.5f) * nearP;
					float w_n = h_n * camera->aspectRatio;
					float h_f = tanf(glm::radians(camera->verticalFovDegrees) * 0.5f) * farP;
					float w_f = h_f * camera->aspectRatio;

					glm::vec3 corners[8] = {
						{ -w_n, -h_n, -nearP }, {  w_n, -h_n, -nearP },
						{  w_n,  h_n, -nearP }, { -w_n,  h_n, -nearP },
						{ -w_f, -h_f, -farP  }, {  w_f, -h_f, -farP  },
						{  w_f,  h_f, -farP  }, { -w_f,  h_f, -farP  }
					};

					ImVec2 projected[8];
					bool behind[8];
					glm::mat4 viewProj = proj * view;

					// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
					for (int i = 0; i < 8; i++) {
						glm::vec4 worldPos = model * glm::vec4(corners[i], 1.0f);
						glm::vec4 clipPos = viewProj * worldPos;
						
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (clipPos.w <= 0.0f) {
							behind[i] = true;
							continue;
						}
						behind[i] = false;
						glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
						projected[i].x = viewportPos.x + (ndc.x + 1.0f) * 0.5f * viewportSize.x;
						projected[i].y = viewportPos.y + (1.0f - ndc.y) * 0.5f * viewportSize.y;
					}

					auto drawLine = [&](int i, int j) {
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (!behind[i] && !behind[j]) {
							ImGui::GetWindowDrawList()->AddLine(projected[i], projected[j], IM_COL32(255, 255, 255, 255), 2.0f);
						}
					};

					// Near plane
					drawLine(0, 1); drawLine(1, 2); drawLine(2, 3); drawLine(3, 0);
					// Far plane
					drawLine(4, 5); drawLine(5, 6); drawLine(6, 7); drawLine(7, 4);
					// Sides
					drawLine(0, 4); drawLine(1, 5); drawLine(2, 6); drawLine(3, 7);
				}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (light && transform) {
					const glm::mat4 viewProjection = proj * view;
					const glm::vec3 lightPosition = glm::vec3(model[3]);
					const glm::vec3 lightDirection = SafeNormalize(glm::vec3(model * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)), glm::vec3(0.0f, -1.0f, 0.0f));
					const ImU32 lightColor = IM_COL32(255, 244, 214, 255);

					const LightType lightType = light->type;

					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (lightType == LightType::Directional)
					{
						DrawDirectionalLightGizmo(ImGui::GetWindowDrawList(), lightPosition, lightDirection, viewProjection, viewportPos, viewportSize, lightColor);
					}
					// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
					else if (lightType == LightType::Point)
					{
						DrawPointLightGizmo(ImGui::GetWindowDrawList(), lightPosition, light->range, viewProjection, viewportPos, viewportSize, lightColor);
					}
					// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
					else if (lightType == LightType::Spot)
					{
						DrawSpotLightGizmo(
							ImGui::GetWindowDrawList(),
							lightPosition,
							lightDirection,
							light->range,
							light->innerConeAngle,
							light->outerConeAngle,
							viewProjection,
							viewportPos,
							viewportSize,
							lightColor);
					}
				}
			}
		}
	}
}
