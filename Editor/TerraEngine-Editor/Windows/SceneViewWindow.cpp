// SceneViewWindow.cpp — Scene View Window Implementation
//
// Implements the 3D viewport interface, rendering the scene to an ImGui texture and providing manipulation gizmos.
// It handles editor-specific rendering features like the grid, selection outlines, and camera movement controls.

#include "SceneViewWindow.h"
#include "../Core/EditorContext.h"
#include "../Project/ProjectManager.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "../Subsystems/UndoRedoSystem.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Renderer/RendererRecovery.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/File Formats/TerraEngineMeshFile.h"
#include "Engine/Components/MeshRenderComponent.h"
#include "Engine/Components/Physics/ColliderSetComponent.h"
#include "Engine/Components/Physics/CharacterControllerComponent.h"
#include "Engine/Components/Physics/ConstraintSetComponent.h"
#include "Engine/Components/Physics/RigidBodyComponent.h"
#include "Engine/Input/InputSystem.h"
#include "Engine/Input/InputCodes.h"
#include "Engine/Profiling/Profiler.h"
#include "Engine/Window/WindowSystem.h"
#include <GLFW/glfw3.h>
#include "Engine/Components/Transform.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Components/CameraComponent.h"
#include "Engine/Components/LightComponent.h"
#include "Engine/Scene/SceneMutationQueue.h"
#include "Engine/Scene/TransactionalMutation.h"
#include <imgui.h>
#include <ImGuizmo.h>
#include <entt/entt.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <memory>
#include <utility>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/constants.hpp>

namespace SceneOverlay = TerraEngine::Editor::SceneViewOverlay;

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

glm::vec3 CameraForwardFromEuler(const glm::vec3& rotation)
{
	// Scene View stores camera rotation as pitch/yaw; convert it once so view,
	// input, and orientation-widget code all agree on the forward direction.
	const glm::quat orientation = glm::quat(glm::vec3(rotation.x, rotation.y, 0.0f));
	return SafeNormalize(orientation * glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 0.0f, -1.0f));
}

glm::vec2 CameraEulerFromForward(const glm::vec3& forward)
{
	const glm::vec3 normalizedForward = SafeNormalize(forward, glm::vec3(0.0f, 0.0f, -1.0f));
	const float pitch = std::asin(std::clamp(normalizedForward.y, -0.999f, 0.999f));
	const float yaw = std::atan2(-normalizedForward.x, -normalizedForward.z);
	return glm::vec2(pitch, yaw);
}

glm::vec3 CameraUpFromEuler(const glm::vec3& rotation)
{
	// The up vector comes from the same orientation as forward so snapped top
	// and side views do not accidentally use a stale world-up basis.
	const glm::quat orientation = glm::quat(glm::vec3(rotation.x, rotation.y, 0.0f));
	return SafeNormalize(orientation * glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
}

std::optional<glm::vec3> ComputeEntityWorldPosition(entt::entity entity)
{
	// Walk parent transforms from the selected object to the root so Scene View
	// focus uses the same world-space position that markers and gizmos display.
	auto& objectManager = ObjectManager::instance();
	auto& registry = objectManager.getRegistry();
	if (!registry.valid(entity))
	{
		return std::nullopt;
	}

	glm::mat4 world(1.0f);
	entt::entity current = entity;
	while (current != entt::null && registry.valid(current))
	{
		if (const Transform* transform = registry.try_get<Transform>(current))
		{
			glm::mat4 local(1.0f);
			local = glm::translate(local, transform->position);
			local = local * glm::mat4_cast(transform->rotation);
			local = glm::scale(local, transform->scale);
			world = local * world;
		}
		if (const std::optional<Object> object = objectManager.getObject(current))
		{
			const std::optional<Object> parent = objectManager.getParent(*object);
			current = parent ? parent->EntityId() : entt::null;
		}
		else
		{
			current = entt::null;
		}
	}
	return glm::vec3(world[3]);
}

bool IsPointerInsideRect(const ImVec2& pointer, const ImVec2& min, const ImVec2& max)
{
	return pointer.x >= min.x && pointer.x <= max.x && pointer.y >= min.y && pointer.y <= max.y;
}

float PerspectiveVisibleHeight(float fovDegrees, float distance)
{
	const float clampedDistance = std::max(distance, 1.0f);
	const float clampedFov = std::clamp(fovDegrees, 1.0f, 120.0f);
	return 2.0f * clampedDistance * std::tan(glm::radians(clampedFov) * 0.5f);
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

ImVec2 AddIconVec2(const ImVec2& left, const ImVec2& right)
{
	return ImVec2(left.x + right.x, left.y + right.y);
}

ImVec2 SubtractIconVec2(const ImVec2& left, const ImVec2& right)
{
	return ImVec2(left.x - right.x, left.y - right.y);
}

ImVec2 ScaleIconVec2(const ImVec2& value, float scale)
{
	return ImVec2(value.x * scale, value.y * scale);
}

ImVec2 NormalizeIconVec2(const ImVec2& value)
{
	const float length = std::sqrt((value.x * value.x) + (value.y * value.y));
	if (length <= 0.0001f)
	{
		return ImVec2(1.0f, 0.0f);
	}
	return ImVec2(value.x / length, value.y / length);
}

void DrawIconArrow(ImDrawList* drawList, const ImVec2& start, const ImVec2& end, ImU32 color, float thickness)
{
	const ImVec2 direction = NormalizeIconVec2(SubtractIconVec2(end, start));
	const ImVec2 perpendicular(-direction.y, direction.x);
	const float headLength = thickness * 2.8f;
	drawList->AddLine(start, end, color, thickness);
	drawList->AddTriangleFilled(
		end,
		AddIconVec2(SubtractIconVec2(end, ScaleIconVec2(direction, headLength)), ScaleIconVec2(perpendicular, headLength * 0.55f)),
		SubtractIconVec2(SubtractIconVec2(end, ScaleIconVec2(direction, headLength)), ScaleIconVec2(perpendicular, headLength * 0.55f)),
		color);
}

void DrawToolbarIconShape(ImDrawList* drawList, std::string_view shapeKind, const ImVec2& min, const ImVec2& max, ImU32 iconColor, ImU32 accentColor)
{
	const float width = max.x - min.x;
	const float height = max.y - min.y;
	const float unit = std::min(width, height);
	const ImVec2 center(min.x + width * 0.5f, min.y + height * 0.5f);
	const float radius = unit * 0.28f;
	const float thickness = std::max(1.4f, unit * 0.075f);

	if (shapeKind == "hand-pan")
	{
		// A small palm and four fingers read as the familiar hand-pan tool even
		// without an icon font, keeping the toolbar self-contained.
		const ImVec2 palmMin(center.x - unit * 0.14f, center.y - unit * 0.02f);
		const ImVec2 palmMax(center.x + unit * 0.16f, center.y + unit * 0.24f);
		drawList->AddRect(palmMin, palmMax, iconColor, unit * 0.06f, 0, thickness);
		for (int finger = 0; finger < 4; ++finger)
		{
			const float x = center.x - unit * 0.18f + static_cast<float>(finger) * unit * 0.11f;
			drawList->AddLine(ImVec2(x, center.y + unit * 0.02f), ImVec2(x, center.y - unit * 0.26f), iconColor, thickness);
		}
		drawList->AddLine(ImVec2(center.x - unit * 0.14f, center.y + unit * 0.12f), ImVec2(center.x - unit * 0.34f, center.y - unit * 0.04f), iconColor, thickness);
		return;
	}

	if (shapeKind == "axis-arrows")
	{
		DrawIconArrow(drawList, center, ImVec2(center.x, min.y + unit * 0.18f), iconColor, thickness);
		DrawIconArrow(drawList, center, ImVec2(center.x, max.y - unit * 0.18f), iconColor, thickness);
		DrawIconArrow(drawList, center, ImVec2(min.x + unit * 0.18f, center.y), iconColor, thickness);
		DrawIconArrow(drawList, center, ImVec2(max.x - unit * 0.18f, center.y), iconColor, thickness);
		drawList->AddCircleFilled(center, unit * 0.045f, accentColor, 12);
		return;
	}

	if (shapeKind == "orbit-ring")
	{
		drawList->AddCircle(center, radius, iconColor, 36, thickness);
		DrawIconArrow(drawList, ImVec2(center.x + radius * 0.35f, center.y - radius * 0.94f), ImVec2(center.x + radius * 0.78f, center.y - radius * 0.55f), iconColor, thickness);
		return;
	}

	if (shapeKind == "corner-scale")
	{
		const ImVec2 topLeft(center.x - radius, center.y - radius);
		const ImVec2 bottomRight(center.x + radius, center.y + radius);
		drawList->AddLine(ImVec2(topLeft.x, topLeft.y + radius * 0.55f), topLeft, iconColor, thickness);
		drawList->AddLine(topLeft, ImVec2(topLeft.x + radius * 0.55f, topLeft.y), iconColor, thickness);
		drawList->AddLine(ImVec2(bottomRight.x - radius * 0.55f, bottomRight.y), bottomRight, iconColor, thickness);
		drawList->AddLine(bottomRight, ImVec2(bottomRight.x, bottomRight.y - radius * 0.55f), iconColor, thickness);
		DrawIconArrow(drawList, ImVec2(center.x - radius * 0.35f, center.y - radius * 0.35f), bottomRight, iconColor, thickness);
		return;
	}

	if (shapeKind == "center-dot" || shapeKind == "pivot-dot")
	{
		const bool centerMode = shapeKind == "center-dot";
		drawList->AddCircle(center, radius, iconColor, 28, thickness);
		drawList->AddLine(ImVec2(center.x - radius * 1.2f, center.y), ImVec2(center.x + radius * 1.2f, center.y), iconColor, thickness);
		drawList->AddLine(ImVec2(center.x, center.y - radius * 1.2f), ImVec2(center.x, center.y + radius * 1.2f), iconColor, thickness);
		drawList->AddCircleFilled(centerMode ? center : ImVec2(center.x + radius * 0.55f, center.y - radius * 0.35f), unit * 0.065f, accentColor, 14);
		return;
	}

	if (shapeKind == "space-cube" || shapeKind == "space-world")
	{
		if (shapeKind == "space-world")
		{
			drawList->AddCircle(center, radius, iconColor, 30, thickness);
			drawList->AddLine(ImVec2(center.x - radius, center.y), ImVec2(center.x + radius, center.y), iconColor, thickness * 0.75f);
			drawList->AddLine(ImVec2(center.x, center.y - radius), ImVec2(center.x, center.y + radius), iconColor, thickness * 0.75f);
			return;
		}
		const ImVec2 frontMin(center.x - radius * 0.8f, center.y - radius * 0.55f);
		const ImVec2 frontMax(center.x + radius * 0.45f, center.y + radius * 0.7f);
		const ImVec2 offset(radius * 0.38f, -radius * 0.34f);
		drawList->AddRect(frontMin, frontMax, iconColor, 0.0f, 0, thickness);
		drawList->AddRect(AddIconVec2(frontMin, offset), AddIconVec2(frontMax, offset), iconColor, 0.0f, 0, thickness);
		drawList->AddLine(frontMin, AddIconVec2(frontMin, offset), iconColor, thickness);
		drawList->AddLine(ImVec2(frontMax.x, frontMin.y), AddIconVec2(ImVec2(frontMax.x, frontMin.y), offset), iconColor, thickness);
		drawList->AddLine(frontMax, AddIconVec2(frontMax, offset), iconColor, thickness);
		return;
	}

	if (shapeKind == "magnet-grid")
	{
		drawList->PathArcTo(ImVec2(center.x - radius * 0.35f, center.y), radius * 0.55f, 3.14159265f * 0.5f, 3.14159265f * 1.5f, 18);
		drawList->PathStroke(iconColor, 0, thickness);
		drawList->AddLine(ImVec2(center.x - radius * 0.35f, center.y - radius * 0.55f), ImVec2(center.x + radius * 0.52f, center.y - radius * 0.55f), iconColor, thickness);
		drawList->AddLine(ImVec2(center.x - radius * 0.35f, center.y + radius * 0.55f), ImVec2(center.x + radius * 0.52f, center.y + radius * 0.55f), iconColor, thickness);
		drawList->AddLine(ImVec2(center.x + radius * 0.72f, center.y - radius * 0.65f), ImVec2(center.x + radius * 0.72f, center.y + radius * 0.65f), accentColor, thickness * 0.8f);
		return;
	}

	if (shapeKind == "grid-lines")
	{
		const ImVec2 gridMin(center.x - radius, center.y - radius);
		const ImVec2 gridMax(center.x + radius, center.y + radius);
		for (int line = 0; line < 3; ++line)
		{
			const float t = static_cast<float>(line) / 2.0f;
			const float x = gridMin.x + (gridMax.x - gridMin.x) * t;
			const float y = gridMin.y + (gridMax.y - gridMin.y) * t;
			drawList->AddLine(ImVec2(x, gridMin.y), ImVec2(x, gridMax.y), iconColor, thickness * 0.75f);
			drawList->AddLine(ImVec2(gridMin.x, y), ImVec2(gridMax.x, y), iconColor, thickness * 0.75f);
		}
		return;
	}

	if (shapeKind == "marker-dots")
	{
		drawList->AddCircleFilled(ImVec2(center.x - radius * 0.7f, center.y - radius * 0.1f), unit * 0.075f, iconColor, 14);
		drawList->AddCircleFilled(ImVec2(center.x, center.y - radius * 0.6f), unit * 0.075f, accentColor, 14);
		drawList->AddCircleFilled(ImVec2(center.x + radius * 0.72f, center.y + radius * 0.32f), unit * 0.075f, iconColor, 14);
		drawList->AddLine(ImVec2(center.x - radius * 0.7f, center.y - radius * 0.1f), ImVec2(center.x, center.y - radius * 0.6f), iconColor, thickness * 0.65f);
		drawList->AddLine(ImVec2(center.x, center.y - radius * 0.6f), ImVec2(center.x + radius * 0.72f, center.y + radius * 0.32f), iconColor, thickness * 0.65f);
		return;
	}

	if (shapeKind == "transform-gizmo")
	{
		DrawIconArrow(drawList, center, ImVec2(center.x, center.y - radius * 1.05f), IM_COL32(108, 225, 130, 255), thickness);
		DrawIconArrow(drawList, center, ImVec2(center.x + radius * 1.0f, center.y + radius * 0.56f), IM_COL32(106, 158, 255, 255), thickness);
		DrawIconArrow(drawList, center, ImVec2(center.x - radius * 1.0f, center.y + radius * 0.56f), IM_COL32(255, 108, 108, 255), thickness);
		return;
	}

	if (shapeKind == "projection-persp")
	{
		const ImVec2 nearA(center.x - radius * 0.35f, center.y - radius * 0.25f);
		const ImVec2 nearB(center.x + radius * 0.35f, center.y - radius * 0.25f);
		const ImVec2 farA(center.x - radius, center.y + radius * 0.7f);
		const ImVec2 farB(center.x + radius, center.y + radius * 0.7f);
		drawList->AddLine(nearA, farA, iconColor, thickness);
		drawList->AddLine(nearB, farB, iconColor, thickness);
		drawList->AddLine(farA, farB, iconColor, thickness);
		drawList->AddLine(nearA, nearB, accentColor, thickness);
		return;
	}

	if (shapeKind == "projection-ortho")
	{
		drawList->AddRect(ImVec2(center.x - radius * 0.8f, center.y - radius * 0.8f), ImVec2(center.x + radius * 0.8f, center.y + radius * 0.8f), iconColor, unit * 0.03f, 0, thickness);
		drawList->AddLine(ImVec2(center.x - radius * 0.45f, center.y - radius * 0.8f), ImVec2(center.x - radius * 0.45f, center.y + radius * 0.8f), accentColor, thickness * 0.8f);
		drawList->AddLine(ImVec2(center.x + radius * 0.45f, center.y - radius * 0.8f), ImVec2(center.x + radius * 0.45f, center.y + radius * 0.8f), accentColor, thickness * 0.8f);
		return;
	}

	if (shapeKind == "pulse-bars")
	{
		for (int bar = 0; bar < 4; ++bar)
		{
			const float x = center.x - radius + static_cast<float>(bar) * radius * 0.55f;
			const float barHeight = radius * (0.45f + static_cast<float>((bar + 1) % 3) * 0.28f);
			drawList->AddRectFilled(ImVec2(x, center.y + radius), ImVec2(x + radius * 0.26f, center.y + radius - barHeight), bar == 2 ? accentColor : iconColor, unit * 0.03f);
		}
		return;
	}

	drawList->AddCircle(center, radius * 0.8f, iconColor, 24, thickness);
}

bool DrawOverlayIconButton(const char* id, std::string_view shapeKind, bool active, const char* tooltip, float size)
{
	ImGui::PushID(id);
	const bool pressed = ImGui::InvisibleButton("##scene_toolbar_icon", ImVec2(size, size));
	const bool hovered = ImGui::IsItemHovered();
	const bool held = ImGui::IsItemActive();
	const ImVec2 min = ImGui::GetItemRectMin();
	const ImVec2 max = ImGui::GetItemRectMax();
	ImDrawList* drawList = ImGui::GetWindowDrawList();

	const ImU32 background = active
		? IM_COL32(81, 128, 166, 255)
		: (held ? IM_COL32(76, 82, 88, 255) : (hovered ? IM_COL32(62, 68, 74, 255) : IM_COL32(48, 52, 56, 255)));
	const ImU32 border = active ? IM_COL32(124, 180, 218, 255) : IM_COL32(76, 82, 88, 255);
	const ImU32 icon = active ? IM_COL32(245, 250, 255, 255) : IM_COL32(205, 214, 222, 255);
	const ImU32 accent = active ? IM_COL32(255, 222, 126, 255) : IM_COL32(137, 185, 225, 255);

	// The button face is drawn manually so every toolbar command can be an icon
	// without depending on a bundled icon font or visible text labels.
	drawList->AddRectFilled(min, max, background, 3.0f);
	drawList->AddRect(min, max, border, 3.0f, 0, active ? 1.6f : 1.0f);
	DrawToolbarIconShape(drawList, shapeKind, min, max, icon, accent);

	if (hovered && tooltip != nullptr)
	{
		ImGui::SetTooltip("%s", tooltip);
	}
	ImGui::PopID();
	return pressed;
}

void DrawToolbarSeparator()
{
	ImGui::SameLine(0.0f, 6.0f);
	const ImVec2 min = ImGui::GetCursorScreenPos();
	const float height = ImGui::GetFrameHeight();
	ImGui::Dummy(ImVec2(1.0f, height));
	ImGui::GetWindowDrawList()->AddLine(
		ImVec2(min.x, min.y + 3.0f),
		ImVec2(min.x, min.y + height - 3.0f),
		IM_COL32(95, 101, 106, 180),
		1.0f);
	ImGui::SameLine(0.0f, 6.0f);
}

ImU32 MarkerColor(SceneOverlay::MarkerCategory category, bool selected, bool hovered)
{
	if (selected)
	{
		return IM_COL32(255, 213, 91, 255);
	}
	if (hovered)
	{
		return IM_COL32(116, 196, 255, 255);
	}
	switch (category)
	{
	case SceneOverlay::MarkerCategory::Camera: return IM_COL32(210, 232, 255, 235);
	case SceneOverlay::MarkerCategory::DirectionalLight: return IM_COL32(255, 240, 145, 235);
	case SceneOverlay::MarkerCategory::PointLight: return IM_COL32(255, 222, 119, 235);
	case SceneOverlay::MarkerCategory::SpotLight: return IM_COL32(255, 197, 109, 235);
	case SceneOverlay::MarkerCategory::Physics: return IM_COL32(112, 205, 255, 225);
	case SceneOverlay::MarkerCategory::EmptyObject: return IM_COL32(218, 224, 230, 215);
	default: return IM_COL32(210, 210, 210, 215);
	}
}

const char* MarkerGlyph(SceneOverlay::MarkerCategory category)
{
	switch (category)
	{
	case SceneOverlay::MarkerCategory::Camera: return "C";
	case SceneOverlay::MarkerCategory::DirectionalLight: return "D";
	case SceneOverlay::MarkerCategory::PointLight: return "P";
	case SceneOverlay::MarkerCategory::SpotLight: return "S";
	case SceneOverlay::MarkerCategory::Physics: return "R";
	case SceneOverlay::MarkerCategory::EmptyObject: return "E";
	case SceneOverlay::MarkerCategory::SelectedObject: return "*";
	default: return "?";
	}
}
}

SceneViewWindow::SceneViewWindow(MainEditorSubsystem* owner)
	// EditorWindow is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: EditorWindow("Scene View", true, "Rendering"), m_Owner(owner) {
	m_CurrentGizmoOperation = ImGuizmo::TRANSLATE;
	m_IconDefinitions = SceneOverlay::BuildDefaultIconDefinitions();
}

nlohmann::json SceneViewWindow::GetAutomationState() const
{
	const auto vector = [](const glm::vec3& value) {
		return nlohmann::json::array({value.x, value.y, value.z});
	};
	const auto toolName = [this]() -> const char* {
		switch (m_ToolState.activeTool)
		{
		case SceneOverlay::SceneViewTool::Hand: return "hand";
		case SceneOverlay::SceneViewTool::Move: return "move";
		case SceneOverlay::SceneViewTool::Rotate: return "rotate";
		case SceneOverlay::SceneViewTool::Scale: return "scale";
		case SceneOverlay::SceneViewTool::None: return "none";
		}
		return "none";
	};

	nlohmann::json state{
		{"camera", {
			{"position", vector(m_CameraPos)},
			{"focus", vector(m_CameraFocusInitialized ? m_CameraFocus : ResolveSceneViewFocus(m_CameraFocusDistance))},
			{"forward", vector(GetCameraForward())},
			{"up", vector(GetCameraUp())},
			{"fieldOfViewDegrees", m_Fov}
		}},
		{"tool", toolName()},
		{"orientation", m_ToolState.orientationMode == SceneOverlay::OrientationMode::Local ? "local" : "global"},
		{"projection", m_ToolState.projectionMode == SceneOverlay::ProjectionMode::Perspective ? "perspective" : "orthographic"},
		{"snapping", {
			{"enabled", m_ToolState.snapEnabled},
			{"translation", m_SnapValue[0]},
			{"rotationDegrees", m_RotationSnapValue[0]},
			{"scale", m_ScaleSnapValue[0]}
		}},
		{"visualization", ToString(m_VirtualGeometryDebugMode)},
		{"visible", IsVisible()}
	};
	if (m_Owner != nullptr && m_Owner->GetSelectedEntity().has_value())
	{
		const auto object = ObjectManager::instance().getObject(*m_Owner->GetSelectedEntity());
		if (object.has_value())
		{
			state["selectedObjectId"] = TerraEngine::ToHexString(ObjectManager::instance().getUUID(*object));
		}
	}
	else
	{
		state["selectedObjectId"] = nullptr;
	}
	const nlohmann::json performance = SceneOverlay::SerializeDiagnosticsSnapshot(SceneOverlay::GetLatestDiagnosticsSnapshot());
	state["largeScene"] = performance.value("largeScene", nlohmann::json::object());
	state["markers"] = performance.value("markers", nlohmann::json::object());
	state["stageTimings"] = performance.value("stageTimings", nlohmann::json::array());
	return state;
}

bool SceneViewWindow::SetAutomationCamera(const glm::vec3& position, const glm::vec3& focus, std::string& outError)
{
	if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)
		|| !std::isfinite(focus.x) || !std::isfinite(focus.y) || !std::isfinite(focus.z))
	{
		outError = "Camera position and focus must contain finite numbers.";
		return false;
	}
	const glm::vec3 direction = focus - position;
	if (glm::length(direction) <= 0.0001f)
	{
		outError = "Camera position and focus must not be the same point.";
		return false;
	}
	const glm::vec2 euler = CameraEulerFromForward(direction);
	m_CameraRot = glm::vec3(euler.x, euler.y, 0.0f);
	m_CameraPos = position;
	m_CameraFocus = focus;
	m_CameraFocusDistance = std::clamp(glm::length(direction), 1.0f, 1000.0f);
	m_CameraFocusInitialized = true;
	return true;
}

// Resolve the same parent-composed model matrix used by Scene View gizmos so
// collider handles remain attached to nested objects instead of world origin.
std::optional<glm::mat4> ComputeEntityWorldMatrix(entt::entity entity)
{
	auto& objectManager = ObjectManager::instance();
	auto& registry = objectManager.getRegistry();
	if (!registry.valid(entity))
	{
		return std::nullopt;
	}

	glm::mat4 world(1.0f);
	entt::entity current = entity;
	while (current != entt::null && registry.valid(current))
	{
		if (const Transform* transform = registry.try_get<Transform>(current))
		{
			glm::mat4 local(1.0f);
			local = glm::translate(local, transform->position);
			local = local * glm::mat4_cast(transform->rotation);
			local = glm::scale(local, transform->scale);
			world = local * world;
		}
		const std::optional<Object> object = objectManager.getObject(current);
		const std::optional<Object> parent = object ? objectManager.getParent(*object) : std::nullopt;
		current = parent ? parent->EntityId() : entt::null;
	}
	return world;
}

glm::vec3 ToGlm(const TerraEngine::Physics::PhysicsVector3& value)
{
	return {value.x, value.y, value.z};
}

TerraEngine::Physics::PhysicsVector3 ToPhysics(const glm::vec3& value)
{
	return {value.x, value.y, value.z};
}

glm::quat ToGlm(const TerraEngine::Physics::PhysicsQuaternion& value)
{
	// GLM stores quaternion constructor arguments as W, X, Y, Z while the stable
	// physics contract exposes fields as X, Y, Z, W.
	return glm::normalize(glm::quat(value.w, value.x, value.y, value.z));
}

TerraEngine::Physics::PhysicsQuaternion ToPhysics(const glm::quat& value)
{
	const glm::quat normalized = glm::normalize(value);
	return {normalized.x, normalized.y, normalized.z, normalized.w};
}

// Primitive authoring has different stored dimensions, but the Scene View
// exposes a common local half-extent box for intuitive resize handles.
glm::vec3 ColliderHalfExtents(const TerraEngine::Physics::ColliderRecord& record)
{
	return std::visit([](const auto& shape) -> glm::vec3 {
		using Shape = std::decay_t<decltype(shape)>;
		if constexpr (std::is_same_v<Shape, TerraEngine::Physics::BoxColliderShape>)
		{
			return ToGlm(shape.halfExtents);
		}
		else if constexpr (std::is_same_v<Shape, TerraEngine::Physics::SphereColliderShape>)
		{
			return glm::vec3(shape.radius);
		}
		else if constexpr (std::is_same_v<Shape, TerraEngine::Physics::CapsuleColliderShape>
			|| std::is_same_v<Shape, TerraEngine::Physics::CylinderColliderShape>)
		{
			return {shape.radius, shape.height * 0.5f, shape.radius};
		}
		else if constexpr (std::is_same_v<Shape, TerraEngine::Physics::TaperedCapsuleColliderShape>)
		{
			return {std::max(shape.topRadius, shape.bottomRadius), shape.height * 0.5f, std::max(shape.topRadius, shape.bottomRadius)};
		}
		else
		{
			return glm::vec3(0.5f);
		}
	}, record.shape);
}

bool ApplyColliderHalfExtents(TerraEngine::Physics::ColliderRecord& record, const glm::vec3& extents)
{
	return std::visit([&](auto& shape) -> bool {
		using Shape = std::decay_t<decltype(shape)>;
		if constexpr (std::is_same_v<Shape, TerraEngine::Physics::BoxColliderShape>)
		{
			shape.halfExtents = ToPhysics(extents);
			return true;
		}
		else if constexpr (std::is_same_v<Shape, TerraEngine::Physics::SphereColliderShape>)
		{
			shape.radius = std::max({extents.x, extents.y, extents.z});
			return true;
		}
		else if constexpr (std::is_same_v<Shape, TerraEngine::Physics::CapsuleColliderShape>)
		{
			shape.radius = std::max(extents.x, extents.z);
			shape.height = std::max(extents.y * 2.0f, shape.radius * 2.0f);
			return true;
		}
		else if constexpr (std::is_same_v<Shape, TerraEngine::Physics::CylinderColliderShape>)
		{
			shape.radius = std::max(extents.x, extents.z);
			shape.height = extents.y * 2.0f;
			return true;
		}
		else if constexpr (std::is_same_v<Shape, TerraEngine::Physics::TaperedCapsuleColliderShape>)
		{
			shape.topRadius = std::max(extents.x, extents.z);
			shape.bottomRadius = shape.topRadius;
			shape.height = std::max(extents.y * 2.0f, shape.topRadius * 2.0f);
			return true;
		}
		else
		{
			return false;
		}
	}, record.shape);
}

bool SceneViewWindow::FrameAutomationObject(entt::entity entity, std::string& outError)
{
	const std::optional<glm::vec3> worldPosition = ComputeEntityWorldPosition(entity);
	if (!worldPosition.has_value())
	{
		outError = "The requested Scene View target is stale or has no live transform context.";
		return false;
	}
	if (m_Owner != nullptr)
	{
		m_Owner->SetSelectedEntity(entity);
	}
	const float distance = ResolveSceneViewFocusDistance(*worldPosition, m_CameraFocusDistance);
	RepositionCameraAroundFocus(*worldPosition, distance);
	return true;
}

bool SceneViewWindow::SetAutomationTool(std::string_view tool, std::string& outError)
{
	if (tool == "hand") { m_ToolState.activeTool = SceneOverlay::SceneViewTool::Hand; m_CurrentGizmoOperation = -1; }
	else if (tool == "move") { m_ToolState.activeTool = SceneOverlay::SceneViewTool::Move; m_CurrentGizmoOperation = ImGuizmo::TRANSLATE; }
	else if (tool == "rotate") { m_ToolState.activeTool = SceneOverlay::SceneViewTool::Rotate; m_CurrentGizmoOperation = ImGuizmo::ROTATE; }
	else if (tool == "scale") { m_ToolState.activeTool = SceneOverlay::SceneViewTool::Scale; m_CurrentGizmoOperation = ImGuizmo::SCALE; }
	else if (tool == "none") { m_ToolState.activeTool = SceneOverlay::SceneViewTool::None; m_CurrentGizmoOperation = -1; }
	else { outError = "Tool must be one of: hand, move, rotate, scale, none."; return false; }
	m_ToolState.lastChangedBy = SceneOverlay::LastChangedBy::AutomationDiagnosticReplay;
	return true;
}

bool SceneViewWindow::SetAutomationOrientation(std::string_view orientation, std::string& outError)
{
	if (orientation == "local" || orientation == "global")
	{
		m_ToolState.orientationMode = orientation == "local" ? SceneOverlay::OrientationMode::Local : SceneOverlay::OrientationMode::Global;
		m_ToolState.lastChangedBy = SceneOverlay::LastChangedBy::AutomationDiagnosticReplay;
		return true;
	}
	if (orientation == "perspective" || orientation == "orthographic")
	{
		const SceneOverlay::ProjectionMode projectionMode = orientation == "perspective"
			? SceneOverlay::ProjectionMode::Perspective
			: SceneOverlay::ProjectionMode::Orthographic;
		SetSceneViewProjectionMode(projectionMode);
		m_OverlayPreference.projectionMode = m_ToolState.projectionMode;
		m_OverlayPreferenceDirty = true;
		PersistOverlayPreferences();
		return true;
	}

	SceneOverlay::OrientationAxis axis = SceneOverlay::OrientationAxis::None;
	if (orientation == "right" || orientation == "+x") axis = SceneOverlay::OrientationAxis::PositiveX;
	else if (orientation == "left" || orientation == "-x") axis = SceneOverlay::OrientationAxis::NegativeX;
	else if (orientation == "top" || orientation == "+y") axis = SceneOverlay::OrientationAxis::PositiveY;
	else if (orientation == "bottom" || orientation == "-y") axis = SceneOverlay::OrientationAxis::NegativeY;
	else if (orientation == "back" || orientation == "+z") axis = SceneOverlay::OrientationAxis::PositiveZ;
	else if (orientation == "front" || orientation == "-z") axis = SceneOverlay::OrientationAxis::NegativeZ;
	if (axis == SceneOverlay::OrientationAxis::None)
	{
		outError = "Orientation must be local, global, perspective, orthographic, front, back, left, right, top, or bottom.";
		return false;
	}
	const glm::vec3 focus = ResolveSceneViewFocus(m_CameraFocusDistance);
	const float distance = ResolveSceneViewFocusDistance(focus, m_CameraFocusDistance);
	const SceneOverlay::OrientationSnapEuler snapped = SceneOverlay::SnapEulerForAxis(axis);
	m_CameraRot = glm::vec3(snapped.pitchRadians, snapped.yawRadians, 0.0f);
	RepositionCameraAroundFocus(focus, distance);
	m_OrientationWidgetState.activeAxis = SceneOverlay::ToString(axis);
	return true;
}

bool SceneViewWindow::SetAutomationSnapping(bool enabled, const std::optional<glm::vec3>& values, std::string& outError)
{
	if (values.has_value() && (!std::isfinite(values->x) || !std::isfinite(values->y) || !std::isfinite(values->z)
		|| values->x <= 0.0f || values->y <= 0.0f || values->z <= 0.0f))
	{
		outError = "Snap values must be finite positive translation, rotation-degree, and scale increments.";
		return false;
	}
	m_ToolState.snapEnabled = enabled;
	m_UsingSnap = enabled;
	m_OverlayPreference.snapEnabled = enabled;
	if (values.has_value())
	{
		m_SnapValue[0] = m_SnapValue[1] = m_SnapValue[2] = values->x;
		m_RotationSnapValue[0] = m_RotationSnapValue[1] = m_RotationSnapValue[2] = values->y;
		m_ScaleSnapValue[0] = m_ScaleSnapValue[1] = m_ScaleSnapValue[2] = values->z;
		m_ToolState.snapValues = {values->x, values->y, values->z};
		m_OverlayPreference.translationSnap = values->x;
		m_OverlayPreference.rotationSnapDegrees = values->y;
		m_OverlayPreference.scaleSnap = values->z;
	}
	m_OverlayPreferenceDirty = true;
	PersistOverlayPreferences();
	return true;
}

bool SceneViewWindow::SetAutomationVisualization(std::string_view mode, std::string& outError)
{
	VirtualGeometryDebugVisualizationMode resolved = VirtualGeometryDebugVisualizationMode::None;
	if (mode == "off" || mode == "none") resolved = VirtualGeometryDebugVisualizationMode::None;
	else if (mode == "lod") resolved = VirtualGeometryDebugVisualizationMode::LOD;
	else if (mode == "cluster") resolved = VirtualGeometryDebugVisualizationMode::Cluster;
	else if (mode == "meshlet_id" || mode == "meshlet-id") resolved = VirtualGeometryDebugVisualizationMode::MeshletId;
	else if (mode == "bounds") resolved = VirtualGeometryDebugVisualizationMode::Bounds;
	else if (mode == "culling") resolved = VirtualGeometryDebugVisualizationMode::Culling;
	else if (mode == "residency") resolved = VirtualGeometryDebugVisualizationMode::Residency;
	else if (mode == "hiz" || mode == "hi-z") resolved = VirtualGeometryDebugVisualizationMode::HiZ;
	else if (mode == "error") resolved = VirtualGeometryDebugVisualizationMode::Error;
	else { outError = "Visualization mode is not allowlisted."; return false; }
	m_VirtualGeometryDebugMode = resolved;
	m_VirtualGeometryDebugModeInitialized = true;
	RenderSystem::Instance().SetVirtualGeometryDebugVisualizationMode(RenderViewType::Scene, 1u, resolved);
	return true;
}

glm::vec3 SceneViewWindow::GetCameraForward() const
{
	return CameraForwardFromEuler(m_CameraRot);
}

glm::vec3 SceneViewWindow::GetCameraUp() const
{
	return CameraUpFromEuler(m_CameraRot);
}

glm::vec3 SceneViewWindow::ResolveSceneViewFocus(float fallbackDistance) const
{
	if (m_Owner != nullptr)
	{
		const std::optional<entt::entity> selectedEntity = m_Owner->GetSelectedEntity();
		if (selectedEntity.has_value())
		{
			if (const std::optional<glm::vec3> selectedWorldPosition = ComputeEntityWorldPosition(*selectedEntity))
			{
				return *selectedWorldPosition;
			}
		}
	}

	const glm::vec3 forward = GetCameraForward();
	if (std::abs(forward.y) > 0.0001f)
	{
		const float groundDistance = -m_CameraPos.y / forward.y;
		if (std::isfinite(groundDistance) && groundDistance > 1.0f)
		{
			// When nothing is selected, the ground-plane intersection gives the
			// artist a predictable Scene View pivot near the visible grid.
			return m_CameraPos + forward * groundDistance;
		}
	}

	if (m_CameraFocusInitialized)
	{
		return m_CameraFocus;
	}

	return m_CameraPos + forward * std::max(fallbackDistance, 1.0f);
}

float SceneViewWindow::ResolveSceneViewFocusDistance(const glm::vec3& focus, float fallbackDistance) const
{
	const glm::vec3 forward = GetCameraForward();
	const float alongView = glm::dot(focus - m_CameraPos, forward);
	const float directDistance = glm::distance(m_CameraPos, focus);
	const float resolvedDistance = alongView > 1.0f ? alongView : directDistance;
	if (!std::isfinite(resolvedDistance) || resolvedDistance <= 1.0f)
	{
		return std::clamp(fallbackDistance, 1.0f, 1000.0f);
	}
	return std::clamp(resolvedDistance, 1.0f, 1000.0f);
}

void SceneViewWindow::RepositionCameraAroundFocus(const glm::vec3& focus, float distance)
{
	m_CameraFocus = focus;
	m_CameraFocusDistance = std::clamp(distance, 1.0f, 1000.0f);
	m_CameraFocusInitialized = true;
	m_CameraPos = m_CameraFocus - GetCameraForward() * m_CameraFocusDistance;
}

void SceneViewWindow::SetSceneViewProjectionMode(SceneOverlay::ProjectionMode projectionMode)
{
	if (projectionMode == m_ToolState.projectionMode)
	{
		return;
	}

	if (projectionMode == SceneOverlay::ProjectionMode::Orthographic)
	{
		// Snapshot the perspective camera before orthographic setup changes its
		// rotation or focus. Direct copies avoid normalization or reconstruction
		// drift when the artist later returns to perspective mode.
		m_PerspectiveCameraState.position = m_CameraPos;
		m_PerspectiveCameraState.rotation = m_CameraRot;
		m_PerspectiveCameraState.focus = m_CameraFocus;
		m_PerspectiveCameraState.focusDistance = m_CameraFocusDistance;
		m_PerspectiveCameraState.fieldOfView = m_Fov;
		m_PerspectiveCameraState.focusInitialized = m_CameraFocusInitialized;
		m_PerspectiveCameraState.valid = true;

		const float fallbackDistance = m_CameraFocusInitialized ? m_CameraFocusDistance : 18.0f;
		const glm::vec3 focus = ResolveSceneViewFocus(fallbackDistance);
		const float focusDistance = ResolveSceneViewFocusDistance(focus, fallbackDistance);
		// Orthographic mode starts at the established readable isometric angle,
		// but this temporary drafting pose no longer replaces the perspective pose.
		m_CameraRot.x = glm::radians(-35.264f);
		m_CameraRot.y = glm::radians(-45.0f);
		m_OrthographicHeight = std::clamp(PerspectiveVisibleHeight(m_Fov, focusDistance) * 0.65f, 2.0f, 160.0f);
		RepositionCameraAroundFocus(focus, focusDistance);
	}
	else if (m_PerspectiveCameraState.valid)
	{
		// Restore all perspective composition values exactly. Orthographic panning,
		// orbiting, and zooming remain isolated in the state being left behind.
		m_CameraPos = m_PerspectiveCameraState.position;
		m_CameraRot = m_PerspectiveCameraState.rotation;
		m_CameraFocus = m_PerspectiveCameraState.focus;
		m_CameraFocusDistance = m_PerspectiveCameraState.focusDistance;
		m_Fov = m_PerspectiveCameraState.fieldOfView;
		m_CameraFocusInitialized = m_PerspectiveCameraState.focusInitialized;
	}

	m_ToolState.projectionMode = projectionMode;
}

bool SceneViewWindow::IsPointerInsideOrientationWidget(const ImVec2& viewportPos, const ImVec2& viewportSize) const
{
	if (!m_OverlayPreference.orientationWidgetVisible)
	{
		return false;
	}

	const float size = 88.0f;
	const ImVec2 min(viewportPos.x + viewportSize.x - size - 14.0f, viewportPos.y + 14.0f);
	const ImVec2 max(min.x + size, min.y + size);
	const ImVec2 mouse = ImGui::GetMousePos();
	return IsPointerInsideRect(mouse, min, max);
}

bool SceneViewWindow::HandleSceneViewScrollZoom()
{
	const float wheel = ImGui::GetIO().MouseWheel;
	if (std::abs(wheel) <= 0.0001f || ImGuizmo::IsUsing() || ImGuizmo::IsUsingViewManipulate())
	{
		return false;
	}

	if (m_ToolState.projectionMode == SceneOverlay::ProjectionMode::Orthographic)
	{
		// Orthographic cameras do not get larger by moving forward, so wheel
		// input changes the projected height directly like common editors.
		m_OrthographicHeight = std::clamp(m_OrthographicHeight * std::pow(0.88f, wheel), 1.0f, 500.0f);
		return true;
	}

	if (m_IsFlyCamActive)
	{
		return false;
	}

	const float fallbackDistance = m_CameraFocusInitialized ? m_CameraFocusDistance : 18.0f;
	const glm::vec3 focus = ResolveSceneViewFocus(fallbackDistance);
	const float focusDistance = ResolveSceneViewFocusDistance(focus, fallbackDistance);
	const float newDistance = std::clamp(focusDistance * std::pow(0.86f, wheel), 0.5f, 1000.0f);
	// Perspective wheel input dollies around the same focus used by panning and
	// the view manipulator, so scroll zoom feels like moving the Scene View
	// camera through the current composition instead of only changing FOV.
	RepositionCameraAroundFocus(focus, newDistance);
	return true;
}

bool SceneViewWindow::HandleSceneViewPan(const ImVec2& viewportSize)
{
	const bool handToolPan = m_ToolState.activeTool == SceneOverlay::SceneViewTool::Hand
		&& ImGui::IsMouseDown(ImGuiMouseButton_Left);
	const bool middleMousePan = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
	const bool wantsPan = (handToolPan || middleMousePan) && !ImGuizmo::IsUsing() && !ImGuizmo::IsUsingViewManipulate();
	if (!wantsPan)
	{
		m_IsSceneViewPanning = false;
		return false;
	}

	m_IsSceneViewPanning = true;
	m_InteractionContext.activeRegion = SceneOverlay::InteractionRegion::Viewport;
	const ImVec2 mouseDelta = ImGui::GetIO().MouseDelta;
	if (std::abs(mouseDelta.x) <= 0.0001f && std::abs(mouseDelta.y) <= 0.0001f)
	{
		return true;
	}

	const float fallbackDistance = m_CameraFocusInitialized ? m_CameraFocusDistance : 18.0f;
	const glm::vec3 focus = ResolveSceneViewFocus(fallbackDistance);
	const float focusDistance = ResolveSceneViewFocusDistance(focus, fallbackDistance);
	const float visibleHeight = m_ToolState.projectionMode == SceneOverlay::ProjectionMode::Orthographic
		? std::max(1.0f, m_OrthographicHeight)
		: PerspectiveVisibleHeight(m_Fov, focusDistance);
	const float pixelsHigh = std::max(viewportSize.y, 1.0f);
	const float worldUnitsPerPixel = visibleHeight / pixelsHigh;
	const glm::vec3 right = SafeNormalize(glm::cross(GetCameraForward(), GetCameraUp()), glm::vec3(1.0f, 0.0f, 0.0f));
	const glm::vec3 up = GetCameraUp();
	// Pan like a drafting canvas: dragging right/down pulls the scene right/down
	// on screen, which means the camera moves left/up in world space.
	const glm::vec3 panWorldDelta =
		-right * (mouseDelta.x * worldUnitsPerPixel)
		+ up * (mouseDelta.y * worldUnitsPerPixel);
	m_CameraPos += panWorldDelta;
	if (m_CameraFocusInitialized)
	{
		m_CameraFocus += panWorldDelta;
	}
	else
	{
		m_CameraFocus = focus + panWorldDelta;
		m_CameraFocusDistance = focusDistance;
		m_CameraFocusInitialized = true;
	}
	return true;
}

void SceneViewWindow::LoadOverlayPreferencesIfNeeded()
{
	if (m_OverlayPreferencesLoaded)
	{
		return;
	}

	m_OverlayPreferencesLoaded = true;
	if (m_Owner != nullptr && m_Owner->GetContext() != nullptr && m_Owner->GetContext()->projectManager != nullptr)
	{
		m_OverlayPreference = SceneOverlay::SanitizePreference(m_Owner->GetContext()->projectManager->GetEditorPreferences().sceneViewOverlay);
	}
	else
	{
		m_OverlayPreference = SceneOverlay::SanitizePreference(m_OverlayPreference);
	}
	m_ToolState = SceneOverlay::BuildToolStateFromPreference(m_OverlayPreference, SceneOverlay::SceneViewTool::Move, SceneOverlay::PivotMode::Center, SceneOverlay::OrientationMode::Local);
	m_ShowVirtualGeometryDiagnostics = m_OverlayPreference.diagnosticsVisible;
	m_UsingSnap = m_ToolState.snapEnabled;
	m_SnapValue[0] = m_SnapValue[1] = m_SnapValue[2] = m_OverlayPreference.translationSnap;
	m_RotationSnapValue[0] = m_RotationSnapValue[1] = m_RotationSnapValue[2] = m_OverlayPreference.rotationSnapDegrees;
	m_ScaleSnapValue[0] = m_ScaleSnapValue[1] = m_ScaleSnapValue[2] = m_OverlayPreference.scaleSnap;
}

void SceneViewWindow::PersistOverlayPreferences()
{
	if (!m_OverlayPreferenceDirty)
	{
		return;
	}
	m_OverlayPreferenceDirty = false;
	m_OverlayPreference = SceneOverlay::SanitizePreference(m_OverlayPreference);
	if (m_Owner != nullptr && m_Owner->GetContext() != nullptr && m_Owner->GetContext()->projectManager != nullptr)
	{
		m_Owner->GetContext()->projectManager->SetSceneViewOverlayPreference(m_OverlayPreference);
	}
}

void SceneViewWindow::DrawSceneToolbar()
{
	const float buttonSize = std::max(26.0f, ImGui::GetFrameHeight());
	m_InteractionContext.hoveredRegion = ImGui::IsWindowHovered() ? SceneOverlay::InteractionRegion::Viewport : SceneOverlay::InteractionRegion::None;

	auto setTool = [&](SceneOverlay::SceneViewTool tool, int operation) {
		m_ToolState.activeTool = tool;
		m_ToolState.lastChangedBy = SceneOverlay::LastChangedBy::User;
		m_CurrentGizmoOperation = operation;
	};

	if (DrawOverlayIconButton("scene_tool_hand", SceneOverlay::ToolbarIconShape(SceneOverlay::SceneViewTool::Hand), m_ToolState.activeTool == SceneOverlay::SceneViewTool::Hand, "Hand Tool\nPan the Scene View with left drag, or use middle mouse from any tool.", buttonSize))
	{
		setTool(SceneOverlay::SceneViewTool::Hand, -1);
	}
	ImGui::SameLine(0.0f, 2.0f);
	if (DrawOverlayIconButton("scene_tool_move", SceneOverlay::ToolbarIconShape(SceneOverlay::SceneViewTool::Move), m_ToolState.activeTool == SceneOverlay::SceneViewTool::Move, "Move Tool\nMove selected objects.", buttonSize))
	{
		setTool(SceneOverlay::SceneViewTool::Move, ImGuizmo::TRANSLATE);
	}
	ImGui::SameLine(0.0f, 2.0f);
	if (DrawOverlayIconButton("scene_tool_rotate", SceneOverlay::ToolbarIconShape(SceneOverlay::SceneViewTool::Rotate), m_ToolState.activeTool == SceneOverlay::SceneViewTool::Rotate, "Rotate Tool\nRotate selected objects.", buttonSize))
	{
		setTool(SceneOverlay::SceneViewTool::Rotate, ImGuizmo::ROTATE);
	}
	ImGui::SameLine(0.0f, 2.0f);
	if (DrawOverlayIconButton("scene_tool_scale", SceneOverlay::ToolbarIconShape(SceneOverlay::SceneViewTool::Scale), m_ToolState.activeTool == SceneOverlay::SceneViewTool::Scale, "Scale Tool\nScale selected objects.", buttonSize))
	{
		setTool(SceneOverlay::SceneViewTool::Scale, ImGuizmo::SCALE);
	}

	DrawToolbarSeparator();
	const bool centerPivotMode = m_ToolState.pivotMode == SceneOverlay::PivotMode::Center;
	if (DrawOverlayIconButton("scene_pivot", centerPivotMode ? "center-dot" : "pivot-dot", false, centerPivotMode ? "Center Mode\nPlace handles at the selection center." : "Pivot Mode\nPlace handles at object pivots.", buttonSize))
	{
		m_ToolState.pivotMode = m_ToolState.pivotMode == SceneOverlay::PivotMode::Center ? SceneOverlay::PivotMode::Pivot : SceneOverlay::PivotMode::Center;
		m_ToolState.lastChangedBy = SceneOverlay::LastChangedBy::User;
	}
	ImGui::SameLine(0.0f, 2.0f);
	const bool localOrientation = m_ToolState.orientationMode == SceneOverlay::OrientationMode::Local;
	if (DrawOverlayIconButton("scene_orientation", localOrientation ? "space-cube" : "space-world", false, localOrientation ? "Local Orientation\nAlign transform handles to the selected object." : "Global Orientation\nAlign transform handles to world axes.", buttonSize))
	{
		m_ToolState.orientationMode = m_ToolState.orientationMode == SceneOverlay::OrientationMode::Local ? SceneOverlay::OrientationMode::Global : SceneOverlay::OrientationMode::Local;
		m_ToolState.lastChangedBy = SceneOverlay::LastChangedBy::User;
	}
	ImGui::SameLine(0.0f, 2.0f);
	if (DrawOverlayIconButton("scene_snap", "magnet-grid", m_ToolState.snapEnabled, m_ToolState.snapEnabled ? "Snapping On\nClick to disable transform snapping." : "Snapping Off\nClick to enable transform snapping.", buttonSize))
	{
		m_ToolState.snapEnabled = !m_ToolState.snapEnabled;
		m_OverlayPreference.snapEnabled = m_ToolState.snapEnabled;
		m_UsingSnap = m_ToolState.snapEnabled;
		m_OverlayPreferenceDirty = true;
	}

	DrawToolbarSeparator();
	if (DrawOverlayIconButton("scene_grid", "grid-lines", m_ToolState.gridVisible, m_ToolState.gridVisible ? "Grid Visible\nClick to hide the Scene View grid." : "Grid Hidden\nClick to show the Scene View grid.", buttonSize))
	{
		m_ToolState.gridVisible = !m_ToolState.gridVisible;
		m_OverlayPreference.gridVisible = m_ToolState.gridVisible;
		m_OverlayPreferenceDirty = true;
	}
	ImGui::SameLine(0.0f, 2.0f);
	if (DrawOverlayIconButton("scene_icons", "marker-dots", m_ToolState.iconsVisible, m_ToolState.iconsVisible ? "Scene Icons Visible\nClick to hide object markers." : "Scene Icons Hidden\nClick to show object markers.", buttonSize))
	{
		m_ToolState.iconsVisible = !m_ToolState.iconsVisible;
		m_OverlayPreference.iconsVisible = m_ToolState.iconsVisible;
		m_OverlayPreferenceDirty = true;
	}
	ImGui::SameLine(0.0f, 2.0f);
	if (DrawOverlayIconButton("scene_gizmos", "transform-gizmo", m_ToolState.gizmosVisible, m_ToolState.gizmosVisible ? "Gizmos Visible\nClick to hide selection gizmos." : "Gizmos Hidden\nClick to show selection gizmos.", buttonSize))
	{
		m_ToolState.gizmosVisible = !m_ToolState.gizmosVisible;
		m_OverlayPreference.gizmosVisible = m_ToolState.gizmosVisible;
		m_OverlayPreferenceDirty = true;
	}

	DrawToolbarSeparator();
	const bool perspectiveProjection = m_ToolState.projectionMode == SceneOverlay::ProjectionMode::Perspective;
	if (DrawOverlayIconButton("scene_projection", perspectiveProjection ? "projection-persp" : "projection-ortho", false, perspectiveProjection ? "Perspective Projection\nClick to switch to orthographic Scene View." : "Orthographic Projection\nClick to switch to perspective Scene View.", buttonSize))
	{
		const bool switchingToOrthographic = m_ToolState.projectionMode == SceneOverlay::ProjectionMode::Perspective;
		SetSceneViewProjectionMode(switchingToOrthographic
			? SceneOverlay::ProjectionMode::Orthographic
			: SceneOverlay::ProjectionMode::Perspective);
		m_OverlayPreference.projectionMode = m_ToolState.projectionMode;
		m_OverlayPreferenceDirty = true;
	}
	ImGui::SameLine(0.0f, 2.0f);
	if (DrawOverlayIconButton("scene_diagnostics", "pulse-bars", m_ShowVirtualGeometryDiagnostics, m_ShowVirtualGeometryDiagnostics ? "Diagnostics Visible\nClick to hide Scene View diagnostics." : "Diagnostics Hidden\nClick to show Scene View diagnostics.", buttonSize))
	{
		m_ShowVirtualGeometryDiagnostics = !m_ShowVirtualGeometryDiagnostics;
		m_OverlayPreference.diagnosticsVisible = m_ShowVirtualGeometryDiagnostics;
		m_OverlayPreferenceDirty = true;
	}
	PersistOverlayPreferences();
}

void SceneViewWindow::BuildSceneObjectMarkers(const glm::mat4& viewProjection, const ImVec2& viewportPos, const ImVec2& viewportSize)
{
	if (!m_ToolState.iconsVisible)
	{
		m_ObjectMarkers.clear();
		return;
	}

	auto& objectManager = ObjectManager::instance();
	auto& registry = objectManager.getRegistry();
	const std::optional<entt::entity> selectedEntity = m_Owner != nullptr ? m_Owner->GetSelectedEntity() : std::nullopt;
	const std::uint8_t markerPreferenceSignature = (m_OverlayPreference.cameraIconsVisible ? 1U : 0U)
		| (m_OverlayPreference.lightIconsVisible ? 2U : 0U)
		| (m_OverlayPreference.emptyObjectIconsVisible ? 4U : 0U);
	const std::size_t preferenceHash = std::hash<std::string>{}(nlohmann::json(m_OverlayPreference).dump());
	const bool categoryVisibilityChanged = markerPreferenceSignature != m_MarkerPreferenceSignature;
	if (preferenceHash != m_LastMarkerPreferenceHash)
	{
		m_LastMarkerPreferenceHash = preferenceHash;
		++m_MarkerPreferenceGeneration;
	}

	const TerraEngine::Scene::SceneChangePacket changePacket = objectManager.PublishSceneChangePacket();
	const bool rebuildAll = m_MarkerSceneGeneration != objectManager.GetSceneGeneration()
		|| m_MarkerSpatialSnapshot == nullptr
		|| categoryVisibilityChanged;
	SceneOverlay::MarkerInvalidationResult invalidation;
	if (rebuildAll)
	{
		m_MarkerPipeline.Clear();
		m_MarkerSpatialRecords.clear();
		m_MarkerSceneGeneration = objectManager.GetSceneGeneration();
		invalidation.sceneReplaced = true;
		for (entt::entity entity : registry.view<Transform>())
		{
			if (const std::optional<Object> object = objectManager.getObject(entity))
			{
				const TerraEngine::UUID objectId = objectManager.getUUID(*object);
				invalidation.staticRefreshObjectIds.push_back(objectId);
				invalidation.poseRefreshObjectIds.push_back(objectId);
			}
		}
	}
	else
	{
		invalidation = m_MarkerPipeline.ApplySceneChanges(changePacket);
	}

	// Selection is editor state rather than scene state. Refresh only the old and
	// new selected marker metadata so selection remains reserved without turning
	// a click into a whole-scene cache rebuild.
	if (selectedEntity != m_MarkerSelection)
	{
		for (const std::optional<entt::entity> entity : {m_MarkerSelection, selectedEntity})
		{
			if (entity.has_value())
			{
				if (const std::optional<Object> object = objectManager.getObject(*entity))
				{
					invalidation.staticRefreshObjectIds.push_back(objectManager.getUUID(*object));
				}
			}
		}
	}
	auto sortUnique = [](std::vector<TerraEngine::UUID>& values) {
		std::sort(values.begin(), values.end(), [](const TerraEngine::UUID& left, const TerraEngine::UUID& right) {
			return TerraEngine::ToHexString(left) < TerraEngine::ToHexString(right);
		});
		values.erase(std::unique(values.begin(), values.end()), values.end());
	};
	sortUnique(invalidation.staticRefreshObjectIds);
	sortUnique(invalidation.poseRefreshObjectIds);
	for (const TerraEngine::UUID& removed : invalidation.removedObjectIds)
	{
		m_MarkerSpatialRecords.erase(removed);
	}

	auto computeWorldMatrix = [&](entt::entity entity) {
		glm::mat4 world(1.0f);
		entt::entity current = entity;
		std::size_t remaining = registry.storage<entt::entity>().size();
		while (current != entt::null && registry.valid(current) && remaining-- > 0u)
		{
			if (const Transform* transform = registry.try_get<Transform>(current))
			{
				glm::mat4 local(1.0f);
				local = glm::translate(local, transform->position);
				local = local * glm::mat4_cast(transform->rotation);
				local = glm::scale(local, transform->scale);
				world = local * world;
			}
			const std::optional<Object> object = objectManager.getObject(current);
			const std::optional<Object> parent = object ? objectManager.getParent(*object) : std::nullopt;
			current = parent ? parent->EntityId() : entt::null;
		}
		return world;
	};

	std::vector<TerraEngine::UUID> refreshIds = invalidation.staticRefreshObjectIds;
	refreshIds.insert(refreshIds.end(), invalidation.poseRefreshObjectIds.begin(), invalidation.poseRefreshObjectIds.end());
	sortUnique(refreshIds);
	bool spatialSnapshotDirty = !invalidation.removedObjectIds.empty() || rebuildAll;
	for (const TerraEngine::UUID& objectId : refreshIds)
	{
		const std::optional<Object> object = objectManager.getObject(objectId);
		if (!object || !registry.valid(object->EntityId()) || !registry.any_of<Transform>(object->EntityId()))
		{
			m_MarkerSpatialRecords.erase(objectId);
			spatialSnapshotDirty = true;
			continue;
		}

		const entt::entity entity = object->EntityId();
		const LightComponent* light = registry.try_get<LightComponent>(entity);
		SceneOverlay::MarkerComponentClassification components;
		components.selected = selectedEntity.has_value() && *selectedEntity == entity;
		components.hasCamera = registry.any_of<CameraComponent>(entity);
		components.hasDirectionalLight = light != nullptr && light->type == LightType::Directional;
		components.hasSpotLight = light != nullptr && light->type == LightType::Spot;
		components.hasPointLight = light != nullptr && !components.hasDirectionalLight && !components.hasSpotLight;
		components.hasRenderable = registry.any_of<MeshRenderComponent>(entity);
		components.hasRigidBody = registry.any_of<RigidBodyComponent>(entity);
		components.hasColliderSet = registry.any_of<ColliderSetComponent>(entity);
		components.hasConstraintSet = registry.any_of<ConstraintSetComponent>(entity);
		components.hasCharacterController = registry.any_of<CharacterControllerComponent>(entity);
		const SceneOverlay::MarkerCategory category = SceneOverlay::ClassifyMarkerCategory(components);
		const bool lightMarker = category == SceneOverlay::MarkerCategory::DirectionalLight
			|| category == SceneOverlay::MarkerCategory::PointLight
			|| category == SceneOverlay::MarkerCategory::SpotLight;
		const bool categoryVisible = category != SceneOverlay::MarkerCategory::Unsupported
			&& (category != SceneOverlay::MarkerCategory::Camera || m_OverlayPreference.cameraIconsVisible)
			&& (!lightMarker || m_OverlayPreference.lightIconsVisible)
			&& (category != SceneOverlay::MarkerCategory::EmptyObject || m_OverlayPreference.emptyObjectIconsVisible);
		if (!categoryVisible)
		{
			m_MarkerSpatialRecords.erase(objectId);
			spatialSnapshotDirty = true;
			continue;
		}

		const glm::mat4 worldMatrix = computeWorldMatrix(entity);
		const glm::vec3 worldPosition = glm::vec3(worldMatrix[3]);
		const TerraEngine::Scene::SpatialAABB bounds = TerraEngine::Scene::MakeFallbackBounds(worldPosition, 0.5f);
		if (std::binary_search(invalidation.staticRefreshObjectIds.begin(), invalidation.staticRefreshObjectIds.end(), objectId, [](const TerraEngine::UUID& left, const TerraEngine::UUID& right) {
			return TerraEngine::ToHexString(left) < TerraEngine::ToHexString(right);
		}))
		{
			SceneOverlay::MarkerStaticInput input;
			input.objectId = objectId;
			input.objectLifetimeGeneration = objectManager.getObjectLifetimeGeneration(*object);
			input.entityReference = entity;
			input.componentGeneration = objectManager.getObjectComponentGeneration(*object);
			input.metadataGeneration = objectManager.getObjectMetadataGeneration(*object);
			input.category = category;
			input.displayName = objectManager.getObjectName(*object);
			input.tooltip = input.displayName + " (" + SceneOverlay::ToString(category) + ")";
			input.selected = components.selected;
			(void)m_MarkerPipeline.UpsertStaticRecord(input);
		}
		if (std::binary_search(invalidation.poseRefreshObjectIds.begin(), invalidation.poseRefreshObjectIds.end(), objectId, [](const TerraEngine::UUID& left, const TerraEngine::UUID& right) {
			return TerraEngine::ToHexString(left) < TerraEngine::ToHexString(right);
		}))
		{
			SceneOverlay::MarkerPoseRecord pose;
			pose.objectId = objectId;
			pose.objectLifetimeGeneration = objectManager.getObjectLifetimeGeneration(*object);
			pose.transformRevision = objectManager.GetObjectTransformRevision(*object);
			pose.hierarchyGeneration = objectManager.GetHierarchyRevision();
			pose.parentWorldGeneration = objectManager.GetHierarchyRevision();
			pose.worldPosition = worldPosition;
			pose.worldBounds = bounds;
			pose.movable = !objectManager.isObjectStatic(*object);
			if (const std::optional<Object> parent = objectManager.getParent(*object))
			{
				pose.parentObjectId = objectManager.getUUID(*parent);
			}
			(void)m_MarkerPipeline.UpsertPoseRecord(pose);
		}

		TerraEngine::Scene::SpatialObjectRecord spatial;
		spatial.objectId = objectId;
		spatial.entity = entity;
		// Inherited inactivity also removes child markers from normal scene queries;
		// the child's own authored checkbox remains available in the Inspector.
		spatial.active = objectManager.isObjectActiveInHierarchy(*object);
		spatial.isStatic = objectManager.isObjectStatic(*object);
		spatial.categoryMask = TerraEngine::Scene::SpatialCategoryEditorSelectable;
		spatial.worldBounds = bounds;
		spatial.revision = objectManager.GetObjectSpatialRevision(*object);
		m_MarkerSpatialRecords[objectId] = spatial;
		spatialSnapshotDirty = true;
	}

	if (spatialSnapshotDirty)
	{
		std::vector<TerraEngine::Scene::SpatialObjectRecord> records;
		records.reserve(m_MarkerSpatialRecords.size());
		for (const auto& [objectId, record] : m_MarkerSpatialRecords)
		{
			(void)objectId;
			records.push_back(record);
		}
		++m_MarkerSpatialPublicationGeneration;
		(void)m_MarkerSpatialPublisher.Publish(std::move(records), m_MarkerSpatialPublicationGeneration);
		m_MarkerSpatialSnapshot = m_MarkerSpatialPublisher.Latest();
	}

	const glm::ivec4 viewport(
		static_cast<int>(std::lround(viewportPos.x)),
		static_cast<int>(std::lround(viewportPos.y)),
		static_cast<int>(std::lround(viewportSize.x)),
		static_cast<int>(std::lround(viewportSize.y)));
	bool projectionChanged = viewport != m_LastMarkerViewport;
	for (glm::length_t column = 0; column < 4 && !projectionChanged; ++column)
	{
		for (glm::length_t row = 0; row < 4; ++row)
		{
			projectionChanged = projectionChanged || m_LastMarkerViewProjection[column][row] != viewProjection[column][row];
		}
	}
	if (projectionChanged)
	{
		m_LastMarkerViewProjection = viewProjection;
		m_LastMarkerViewport = viewport;
		++m_MarkerViewGeneration;
	}

	SceneOverlay::MarkerViewportQuery query;
	query.viewportKey.viewGeneration = m_MarkerViewGeneration;
	query.viewportKey.projectionGeneration = m_MarkerViewGeneration;
	query.viewportKey.viewportRectangle = viewport;
	query.viewportKey.projectionMode = m_ToolState.projectionMode;
	query.viewportKey.overlayPreferenceGeneration = m_MarkerPreferenceGeneration;
	query.viewportKey.markerSpatialPublicationGeneration = m_MarkerSpatialSnapshot ? m_MarkerSpatialSnapshot->Generation() : 0u;
	query.viewportKey.markerPosePublicationGeneration = m_MarkerPipeline.PosePublicationGeneration();
	query.viewProjection = viewProjection;
	query.cameraPosition = m_CameraPos;
	query.viewportPosition = glm::vec2(viewportPos.x, viewportPos.y);
	query.viewportSize = glm::vec2(viewportSize.x, viewportSize.y);
	if (m_MarkerSpatialSnapshot)
	{
		TerraEngine::Scene::SpatialQueryFilter filter;
		// Scene View is an authoring surface: inactive authored objects remain
		// selectable even though runtime renderer/physics queries exclude them.
		filter.includeInactive = true;
		filter.categoryMask = TerraEngine::Scene::SpatialCategoryEditorSelectable;
		filter.sortMode = TerraEngine::Scene::SpatialSortMode::StableIdentity;
		query.spatialCandidates = m_MarkerSpatialSnapshot->QueryFrustum(TerraEngine::Scene::ExtractFrustum(viewProjection), filter);
		if (query.spatialCandidates.hits.empty() && m_MarkerSpatialSnapshot->RecordCount() > 0u)
		{
			// A malformed or temporarily stale viewport matrix must fail open for
			// editor selection. Query the same immutable index with a conservative
			// camera-radius volume, then let the pipeline's exact clip-space test
			// reject off-screen entries. This avoids a whole-scene scan and prevents
			// one bad frustum publication from making every object unselectable.
			query.spatialCandidates = m_MarkerSpatialSnapshot->QuerySphere(
				TerraEngine::Scene::SpatialSphere{m_CameraPos, 1000.0f}, filter);
		}
		m_MarkerSpatialQueryRejectedCount = query.spatialCandidates.rejectedCount;
		m_MarkerSpatialQueryFallbackCount = query.spatialCandidates.fallbackCount;
	}
	m_MarkerFrame = m_MarkerPipeline.PrepareViewport(query, m_OverlayPreference);
	m_ObjectMarkers.clear();
	for (const SceneOverlay::MarkerPresentationBatch& batch : m_MarkerFrame.presentationBatches)
	{
		for (const SceneOverlay::MarkerPresentationItem& item : batch.items)
		{
			const SceneOverlay::MarkerStaticRecord* staticRecord = m_MarkerPipeline.FindStaticRecord(item.objectId);
			const SceneOverlay::MarkerPoseRecord* pose = m_MarkerPipeline.FindPoseRecord(item.objectId);
			if (staticRecord == nullptr || pose == nullptr)
			{
				continue;
			}
			SceneOverlay::SceneObjectMarker marker;
			marker.markerId = TerraEngine::ToHexString(item.objectId);
			marker.objectId = item.objectId;
			marker.entity = staticRecord->entityReference;
			marker.displayName = std::string(m_MarkerPipeline.ResolveString(item.displayNameId));
			marker.tooltip = std::string(m_MarkerPipeline.ResolveString(item.tooltipId));
			marker.category = item.category;
			marker.worldPosition = pose->worldPosition;
			marker.screenPosition = item.screenPosition;
			marker.depth = item.depth;
			marker.visible = true;
			marker.selected = staticRecord->alwaysReserved && item.category == SceneOverlay::MarkerCategory::SelectedObject;
			marker.visualPriority = static_cast<std::uint32_t>(item.primitiveTier);
			marker.overlapGroup = item.clusterMemberCount;
			if (item.clusterMemberCount > 1u)
			{
				marker.tooltip += " (" + std::to_string(item.clusterMemberCount) + " nearby markers; zoom in to reveal)";
			}
			m_ObjectMarkers.push_back(std::move(marker));
		}
	}
	m_MarkerPreferenceSignature = markerPreferenceSignature;
	m_MarkerSelection = selectedEntity;
}

void SceneViewWindow::DrawSceneObjectMarkers()
{
	if (!m_ToolState.iconsVisible)
	{
		return;
	}

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	const ImVec2 mouse = ImGui::GetMousePos();
	const glm::vec2 pointer(mouse.x, mouse.y);
	// The orientation widget is drawn over the viewport. When the pointer is in
	// that box, markers underneath it should not steal hover or click ownership.
	const bool orientationWidgetOwnsPointer =
		m_InteractionContext.hoveredRegion == SceneOverlay::InteractionRegion::OrientationWidget
		|| m_InteractionContext.activeRegion == SceneOverlay::InteractionRegion::OrientationWidget;
	std::optional<std::size_t> hovered;
	if (!orientationWidgetOwnsPointer)
	{
		const SceneOverlay::MarkerHitTestResult hit = m_MarkerPipeline.HitTest(m_MarkerFrame.screenIndex, pointer, 13.0f);
		m_MarkerFrame.stageTelemetry.push_back(hit.telemetry);
		if (hit.objectId.has_value())
		{
			const auto marker = std::find_if(m_ObjectMarkers.begin(), m_ObjectMarkers.end(), [&](const SceneOverlay::SceneObjectMarker& candidate) {
				return candidate.objectId == *hit.objectId;
			});
			if (marker != m_ObjectMarkers.end())
			{
				hovered = static_cast<std::size_t>(std::distance(m_ObjectMarkers.begin(), marker));
			}
		}
	}
	m_InteractionContext.candidateMarkers.clear();
	m_InteractionContext.hoveredRegion = hovered.has_value() ? SceneOverlay::InteractionRegion::Marker : m_InteractionContext.hoveredRegion;

	for (std::size_t index = 0u; index < m_ObjectMarkers.size(); ++index)
	{
		SceneOverlay::SceneObjectMarker& marker = m_ObjectMarkers[index];
		if (!marker.visible)
		{
			continue;
		}
		marker.hovered = hovered.has_value() && *hovered == index;
		if (marker.hovered)
		{
			m_InteractionContext.candidateMarkers.push_back(marker.markerId);
			m_InteractionContext.activeObjectId = marker.markerId;
		}
		const ImVec2 center(marker.screenPosition.x, marker.screenPosition.y);
		const ImU32 color = MarkerColor(marker.category, marker.selected, marker.hovered);
		const SceneOverlay::MarkerPrimitiveTier tier = static_cast<SceneOverlay::MarkerPrimitiveTier>(marker.visualPriority);
		const float radius = marker.selected ? 10.0f : (tier == SceneOverlay::MarkerPrimitiveTier::Point ? 3.0f : 8.0f);
		drawList->AddCircleFilled(center, radius, tier == SceneOverlay::MarkerPrimitiveTier::Point ? color : IM_COL32(20, 24, 28, 210), 24);
		if (tier != SceneOverlay::MarkerPrimitiveTier::Point)
		{
			drawList->AddCircle(center, radius, color, 24, 2.0f);
			const char* glyph = tier == SceneOverlay::MarkerPrimitiveTier::Cluster ? "+" : MarkerGlyph(marker.category);
			const ImVec2 glyphSize = ImGui::CalcTextSize(glyph);
			drawList->AddText(ImVec2(center.x - glyphSize.x * 0.5f, center.y - glyphSize.y * 0.5f), color, glyph);
		}
	}

	if (hovered.has_value())
	{
		const SceneOverlay::SceneObjectMarker& marker = m_ObjectMarkers[*hovered];
		ImGui::SetTooltip("%s", marker.tooltip.c_str());
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_Owner != nullptr
			&& (!m_PhysicsShapePreview || !m_PhysicsShapePreview->active))
		{
			m_Owner->SetSelectedEntity(marker.entity);
			m_InteractionContext.activeRegion = SceneOverlay::InteractionRegion::Marker;
			m_InteractionContext.selectionChangePending = true;
		}
	}
}

bool SceneViewWindow::DrawOrientationWidget(glm::mat4& view, const glm::mat4& projection, const ImVec2& viewportPos, const ImVec2& viewportSize)
{
	if (!m_OverlayPreference.orientationWidgetVisible)
	{
		m_OrientationWidgetState.visible = false;
		return false;
	}

	const float size = 88.0f;
	const ImVec2 min(viewportPos.x + viewportSize.x - size - 14.0f, viewportPos.y + 14.0f);
	const ImVec2 max(min.x + size, min.y + size);
	SceneOverlay::SceneViewOverlayCacheKey cacheKey;
	cacheKey.viewportGeneration = m_MarkerViewGeneration;
	cacheKey.sceneGeneration = m_MarkerSceneGeneration;
	cacheKey.selectionGeneration = m_MarkerSelection.has_value()
		? static_cast<std::uint64_t>(static_cast<std::uint32_t>(*m_MarkerSelection)) + 1u
		: 0u;
	cacheKey.simulationGeneration = m_MarkerSpatialPublicationGeneration;
	cacheKey.preferenceGeneration = m_MarkerPreferenceGeneration;
	m_OrientationWidgetState = SceneOverlay::BuildOrientationWidgetStateCached(
		m_OrientationWidgetCache,
		cacheKey,
		GetCameraForward(),
		GetCameraUp(),
		m_ToolState.projectionMode,
		glm::vec4(min.x, min.y, max.x, max.y));

	ImGui::PushID("scene_orientation_widget");
	glm::mat4 widgetModel(1.0f);
	const float fallbackDistance = m_CameraFocusInitialized ? m_CameraFocusDistance : 18.0f;
	const glm::vec3 focus = ResolveSceneViewFocus(fallbackDistance);
	const float focusDistance = ResolveSceneViewFocusDistance(focus, fallbackDistance);
	// ImGuizmo already ships a view-manipulation cube with face picking,
	// interpolation, and drag orbiting. Feed it the same view/projection pair as
	// the editor gizmos, then convert the edited view matrix back to our compact
	// pitch/yaw camera state below.
	ImGuizmo::ViewManipulate(
		glm::value_ptr(view),
		glm::value_ptr(projection),
		ImGuizmo::TRANSLATE,
		ImGuizmo::WORLD,
		glm::value_ptr(widgetModel),
		focusDistance,
		min,
		ImVec2(size, size),
		IM_COL32(24, 28, 32, 150));

	const bool widgetHovered = ImGuizmo::IsViewManipulateHovered();
	const bool widgetUsing = ImGuizmo::IsUsingViewManipulate();
	m_OrientationWidgetState.hoveredAxis = widgetHovered ? "view-manipulate" : "none";
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->AddRect(min, max, IM_COL32(170, 184, 198, 170), 4.0f);
	drawList->AddText(ImVec2(min.x + 8.0f, max.y - 20.0f), IM_COL32(232, 238, 244, 255), m_OrientationWidgetState.label.c_str());

	if (widgetHovered)
	{
		m_InteractionContext.hoveredRegion = SceneOverlay::InteractionRegion::OrientationWidget;
	}
	if (widgetUsing)
	{
		m_InteractionContext.activeRegion = SceneOverlay::InteractionRegion::OrientationWidget;
		const glm::mat4 inverseView = glm::inverse(view);
		const glm::vec3 editedForward = SafeNormalize(glm::vec3(inverseView * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)), GetCameraForward());
		const glm::vec2 editedEuler = CameraEulerFromForward(editedForward);
		m_CameraRot.x = std::clamp(editedEuler.x, glm::radians(-89.0f), glm::radians(89.0f));
		m_CameraRot.y = editedEuler.y;
		// Keep the selected scene focus stable while the widget rotates the view,
		// matching how editor view cubes orbit content instead of drifting away.
		RepositionCameraAroundFocus(focus, focusDistance);
		view = glm::lookAt(m_CameraPos, m_CameraPos + GetCameraForward(), GetCameraUp());
		m_OrientationWidgetState.activeAxis = "view-manipulate";
	}
	ImGui::PopID();
	return widgetUsing;
}

void SceneViewWindow::PublishSceneViewDiagnostics(bool rendererAvailable, const std::string& disabledOverlayReason)
{
	SceneOverlay::SceneViewDiagnosticsSnapshot snapshot;
	snapshot.toolState = m_ToolState;
	snapshot.overlayPreferences = m_OverlayPreference;
	snapshot.orientationWidget = m_OrientationWidgetState;
	snapshot.projectionMode = m_ToolState.projectionMode;
	snapshot.lastInteractionOwner = m_InteractionContext.activeRegion != SceneOverlay::InteractionRegion::None
		? m_InteractionContext.activeRegion
		: m_InteractionContext.hoveredRegion;
	snapshot.rendererAvailable = rendererAvailable;
	snapshot.disabledOverlayReason = disabledOverlayReason;
	snapshot.physicsDebugPacketCount = m_PhysicsDebugPacketCount;
	snapshot.physicsDebugPrimitiveCount = m_PhysicsDebugPrimitiveCount;
	snapshot.physicsDebugVisible = m_PhysicsDebugVisible;
	snapshot.gameViewAffected = false;
	snapshot.largeSceneState = SceneOverlay::ResolveLargeSceneState(m_OverlayPreference, m_MarkerFrame.candidates.sourceCount);
	snapshot.sourceMarkerCount = m_MarkerFrame.candidates.sourceCount;
	snapshot.spatialRecordCount = m_MarkerSpatialSnapshot
		? static_cast<std::uint32_t>(std::min<std::size_t>(m_MarkerSpatialSnapshot->RecordCount(), std::numeric_limits<std::uint32_t>::max()))
		: 0u;
	snapshot.spatialQueryRejectedCount = m_MarkerSpatialQueryRejectedCount;
	snapshot.spatialQueryFallbackCount = m_MarkerSpatialQueryFallbackCount;
	snapshot.candidateMarkerCount = static_cast<std::uint32_t>(
		m_MarkerFrame.candidates.reservedMarkerIds.size() + m_MarkerFrame.candidates.generalCandidateIds.size());
	snapshot.viewRejectedMarkerCount = m_MarkerFrame.candidates.frustumRejectedCount
		+ m_MarkerFrame.candidates.distanceRejectedCount
		+ m_MarkerFrame.candidates.sizeRejectedCount;
	snapshot.budgetRejectedMarkerCount = m_MarkerFrame.candidates.budgetRejectedCount;
	snapshot.presentedMarkerCount = static_cast<std::uint32_t>(m_ObjectMarkers.size());
	snapshot.presentationBatchCount = static_cast<std::uint32_t>(m_MarkerFrame.presentationBatches.size());
	snapshot.densityConsolidatedMarkerCount = m_MarkerFrame.screenIndex.entries.size() > m_ObjectMarkers.size()
		? static_cast<std::uint32_t>(m_MarkerFrame.screenIndex.entries.size() - m_ObjectMarkers.size())
		: 0u;
	snapshot.stageTimings = m_MarkerFrame.stageTelemetry;
	// Publish typed value records while EditorSystem still runs inside the current
	// profiler frame. This copies no window, cache, or presentation ownership.
	TerraEngine::Profiling::Profiler::Instance().RecordSceneViewStages(snapshot.stageTimings);
	for (const TerraEngine::UUID& reservedId : m_MarkerFrame.candidates.reservedMarkerIds)
	{
		const SceneOverlay::MarkerStaticRecord* record = m_MarkerPipeline.FindStaticRecord(reservedId);
		if (record == nullptr)
		{
			continue;
		}
		if (record->category == SceneOverlay::MarkerCategory::SelectedObject)
		{
			++snapshot.selectedExemptionCount;
		}
		else if (record->category == SceneOverlay::MarkerCategory::Camera)
		{
			++snapshot.cameraExemptionCount;
		}
		else if (record->category == SceneOverlay::MarkerCategory::DirectionalLight
			|| record->category == SceneOverlay::MarkerCategory::PointLight
			|| record->category == SceneOverlay::MarkerCategory::SpotLight)
		{
			++snapshot.lightExemptionCount;
		}
	}
	if (snapshot.largeSceneState.effective)
	{
		snapshot.activeReductions.push_back("bounded_marker_budget");
		snapshot.activeReductions.push_back("dense_marker_consolidation");
		if (m_OverlayPreference.densePrimitiveTier != SceneOverlay::DensePrimitiveTier::Full)
		{
			snapshot.activeReductions.push_back("dense_marker_simplification");
		}
	}
	for (const SceneOverlay::SceneObjectMarker& marker : m_ObjectMarkers)
	{
		if (marker.hovered)
		{
			snapshot.hoveredMarkerId = marker.markerId;
		}
		if (marker.selected)
		{
			snapshot.selectedMarkerId = marker.markerId;
		}
	}
	snapshot = SceneOverlay::CountMarkersIntoSnapshot(snapshot, m_ObjectMarkers);
	SceneOverlay::PublishLatestDiagnosticsSnapshot(snapshot);
}

// SceneViewWindow::OnGui is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SceneViewWindow::OnGui() {
	ImGuizmo::BeginFrame();
	LoadOverlayPreferencesIfNeeded();
	m_InteractionContext = SceneOverlay::ViewportInteractionContext{};
	m_InteractionContext.hoveredRegion = ImGui::IsWindowHovered() ? SceneOverlay::InteractionRegion::Viewport : SceneOverlay::InteractionRegion::None;
	DrawSceneToolbar();
	ImGui::SameLine();

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
	ImGui::SetNextItemWidth(96.0f);
	const VirtualGeometryDebugVisualizationMode currentVirtualGeometryDebugMode =
		static_cast<VirtualGeometryDebugVisualizationMode>(virtualGeometryDebugMode);
	const bool virtualGeometryComboOpen = ImGui::BeginCombo("##scene_vg_visualizer", ToString(currentVirtualGeometryDebugMode));
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
	RenderSystem::Instance().SetSceneViewGridVisible(m_ToolState.gridVisible);

	const TerraEngine::Renderer::RendererRecoverySnapshot recoverySnapshot =
		RenderSystem::Instance().GetLatestRendererRecoverySnapshot();
	if (TerraEngine::Renderer::IsRendererRecoveryActive(recoverySnapshot.state))
	{
		// Device-loss recovery must remain readable without sampling the last
		// scene texture. Draw a CPU-only panel and leave scene/object state alone.
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + contentSize.x, cursorPos.y + contentSize.y), ImGui::GetColorU32(ImGuiCol_WindowBg));
		const std::string status = std::string("Renderer unavailable: ")
			+ TerraEngine::Renderer::ToString(recoverySnapshot.state)
			+ (recoverySnapshot.message.empty() ? std::string{} : " - " + recoverySnapshot.message);
		const ImVec2 textSize = ImGui::CalcTextSize(status.c_str(), nullptr, false, contentSize.x - 32.0f);
		ImGui::SetCursorScreenPos(ImVec2(
			cursorPos.x + std::max(16.0f, (contentSize.x - textSize.x) * 0.5f),
			cursorPos.y + std::max(16.0f, (contentSize.y - textSize.y) * 0.5f)));
		ImGui::PushTextWrapPos(cursorPos.x + contentSize.x - 16.0f);
		ImGui::TextUnformatted(status.c_str());
		ImGui::PopTextWrapPos();
		m_OrientationWidgetState.visible = false;
		// Do not report the prior frame's physics overlay as visible while the
		// renderer is unavailable; no packet is submitted on this early return.
		m_PhysicsDebugPacketCount = 0U;
		m_PhysicsDebugPrimitiveCount = 0U;
		m_PhysicsDebugVisible = false;
		PublishSceneViewDiagnostics(false, status);
		return;
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (texID) {
		ImGui::Image((ImTextureID)texID, contentSize);
	} else {
		ImGui::Text("Scene rendering not available.");
	}
	RenderStatsSnapshot sceneStats = RenderSystem::Instance().GetLatestRenderStats(RenderViewType::Scene);
	if (m_ShowVirtualGeometryDiagnostics && sceneStats.valid && (sceneStats.meshletsTotal > 0u || sceneStats.virtualGeometryClusterPackets > 0u || sceneStats.virtualGeometrySubmittedBatches > 0u || sceneStats.virtualGeometryResidentPagePayloads > 0u || sceneStats.virtualGeometryEmittedHighDetailPages > 0u || sceneStats.virtualGeometryEmittedFallbackPages > 0u))
	{
		// Meshlet counters are resolved from the completed Scene View frame slot.
		// Keep the source label on the primary rows so delayed or unavailable GPU
		// readback can never look like an authoritative zero in the overlay.
		const std::string meshletsConsideredText = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
			"Meshlets Considered",
			sceneStats.meshletsTotal,
			sceneStats.meshletCounterSource,
			sceneStats.meshletCounterReason);
		const std::string meshletsSentText = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
			"Meshlets Sent",
			sceneStats.meshletsDispatched,
			sceneStats.meshletCounterSource,
			sceneStats.meshletCounterReason);
		const std::string meshletsCulledText = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
			"Meshlets Culled",
			sceneStats.meshletsCulled,
			sceneStats.meshletCounterSource,
			sceneStats.meshletCounterReason);
		const std::string meshletsCulledHiZText = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
			"Meshlets Culled by Hi-Z",
			sceneStats.meshletsCulledHiZ,
			sceneStats.meshletCounterSource,
			sceneStats.meshletCounterReason);
		const std::string hiZOutcomeText = "Hi-Z Tests/Culled/No Proof: "
			+ std::to_string(sceneStats.hiZTests)
			+ "/"
			+ std::to_string(sceneStats.meshletsCulledHiZ)
			+ "/"
			+ std::to_string(sceneStats.hiZNoOcclusionProof);
		const std::string hiZProofText = "Hi-Z Proofs Full/Grid/Center/Vertex: "
			+ std::to_string(sceneStats.hiZFullRectCulled)
			+ "/"
			+ std::to_string(sceneStats.hiZRectCoverageCulled)
			+ "/"
			+ std::to_string(sceneStats.hiZCenterCulled)
			+ "/"
			+ std::to_string(sceneStats.hiZVertexCoverageCulled);
		const std::string hiZProjectionText = "Hi-Z Projection Failed: " + std::to_string(sceneStats.hiZProjectionFailed);
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
		const std::string fallbackText = "VG Single-Packet Fallback Batches: " + std::to_string(sceneStats.virtualGeometrySinglePacketFallbackBatches);
		const std::string gpuPacketText = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
			"VG GPU Packet Records",
			sceneStats.virtualGeometryGpuPacketRecords,
			sceneStats.meshletCounterSource,
			sceneStats.meshletCounterReason);
		const std::string generatedText = "VG Generated Submissions: "
			+ std::to_string(sceneStats.virtualGeometryValidGeneratedSubmissions)
			+ " valid, "
			+ std::to_string(sceneStats.virtualGeometryInvalidGeneratedSubmissions)
			+ " invalid";
		const std::string dispatchText = "VG Compaction Dispatches/Readbacks: "
			+ std::to_string(sceneStats.virtualGeometryGpuCompactionDispatches)
			+ "/"
			+ std::to_string(sceneStats.virtualGeometryGpuCounterReadbacks);
		const std::string gpuFallbackText = "VG GPU Fallbacks: "
			+ std::to_string(sceneStats.virtualGeometryGpuFallbackAffectedPackets)
			+ " packets, "
			+ std::to_string(sceneStats.virtualGeometryGpuFallbackAffectedBatches)
			+ " batches";
		const std::string hiZBaselineText = "Hi-Z Baseline: "
			+ std::string(sceneStats.hiZBaselineMatched ? "Matched" : "Mismatch")
			+ ", risks "
			+ std::to_string(sceneStats.hiZFalseOcclusionRiskWarnings);
		const std::string reuseText = "Visible Cache: "
			+ std::string(sceneStats.visibleCommandCacheHit ? "hit" : "miss")
			+ ", unique touches "
			+ std::to_string(sceneStats.visibleStreamingUniqueMeshes + sceneStats.visibleStreamingUniqueMaterials + sceneStats.visibleStreamingUniqueTextures)
			+ ", deduped "
			+ std::to_string(sceneStats.visibleStreamingDuplicateTouchesEliminated);
		const std::string pageSourceText = "VG Page Sources H/F/E/M: "
			+ std::to_string(sceneStats.virtualGeometryEmittedHighDetailPages)
			+ "/"
			+ std::to_string(sceneStats.virtualGeometryEmittedFallbackPages)
			+ "/"
			+ std::to_string(sceneStats.virtualGeometryEmittedEmbeddedFallbackPages)
			+ "/"
			+ std::to_string(sceneStats.virtualGeometryMissingPageSources);
		const std::string pagePayloadText = "VG Page Payload Bytes R/F/E: "
			+ std::to_string(sceneStats.virtualGeometryResidentPagePayloadBytes)
			+ "/"
			+ std::to_string(sceneStats.virtualGeometryFallbackPagePayloadBytes)
			+ "/"
			+ std::to_string(sceneStats.virtualGeometryEmbeddedFallbackBytes);
		const std::string pageRequestText = "VG Page Requests/Retires: "
			+ std::to_string(sceneStats.virtualGeometryPageSourceRequests)
			+ "/"
			+ std::to_string(sceneStats.virtualGeometryPageSourceRetirements);
		const char* lines[] = { meshletsConsideredText.c_str(), meshletsSentText.c_str(), meshletsCulledText.c_str(), meshletsCulledHiZText.c_str(), hiZOutcomeText.c_str(), hiZProofText.c_str(), hiZProjectionText.c_str(), packetText.c_str(), batchText.c_str(), gpuPacketText.c_str(), generatedText.c_str(), dispatchText.c_str(), gpuFallbackText.c_str(), compactText.c_str(), rejectedText.c_str(), fallbackText.c_str(), pageSourceText.c_str(), pagePayloadText.c_str(), pageRequestText.c_str(), reuseText.c_str(), hiZBaselineText.c_str() };
		float textWidth = 0.0f;
		for (const char* line : lines)
		{
			textWidth = std::max(textWidth, ImGui::CalcTextSize(line).x);
		}

		const float panelPadding = 8.0f;
		const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
		const ImVec2 panelMin(cursorPos.x + 12.0f, cursorPos.y + 12.0f);
		const ImVec2 panelMax(panelMin.x + textWidth + panelPadding * 2.0f, panelMin.y + lineHeight * static_cast<float>(sizeof(lines) / sizeof(lines[0])) + panelPadding * 2.0f);
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
	ImGuizmo::SetOrthographic(m_ToolState.projectionMode == SceneOverlay::ProjectionMode::Orthographic);
	ImGuizmo::SetDrawlist();
	ImGuizmo::SetRect(cursorPos.x, cursorPos.y, contentSize.x, contentSize.y);

	const bool isFocused = ImGui::IsWindowFocused();
	const bool isHovered = ImGui::IsWindowHovered();
	const bool sceneViewAcceptsViewportInput = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
	const ImVec2 mousePos = ImGui::GetMousePos();
	const bool pointerInsideViewport =
		mousePos.x >= cursorPos.x && mousePos.x <= cursorPos.x + contentSize.x
		&& mousePos.y >= cursorPos.y && mousePos.y <= cursorPos.y + contentSize.y;
	const bool viewportOwnsPointer = sceneViewAcceptsViewportInput && pointerInsideViewport;
	const bool orientationWidgetHovered = viewportOwnsPointer && IsPointerInsideOrientationWidget(cursorPos, contentSize);
	const bool orientationWidgetBusy = orientationWidgetHovered || ImGuizmo::IsUsingViewManipulate();
	if (orientationWidgetHovered)
	{
		m_InteractionContext.hoveredRegion = SceneOverlay::InteractionRegion::OrientationWidget;
	}
	if (viewportOwnsPointer && !orientationWidgetBusy)
	{
		// Pixel bounds alone are not enough here: docked editor panels can cover
		// the Scene View while the cursor is still inside its old rectangle, and
		// wheel input should belong to the topmost hovered window.
		HandleSceneViewScrollZoom();
	}
	// Apply editor camera input before deriving matrices so the submitted view
	// matrix and camera world position describe the same camera state.
	if (!orientationWidgetBusy && (isFocused || isHovered) && (viewportOwnsPointer || m_IsFlyCamActive || m_IsSceneViewPanning)) {
		HandleCameraInput(ImGui::GetIO().DeltaTime, contentSize);
	}

	// Calculate Camera Matrices
	glm::mat4 view = glm::mat4(1.0f);
	glm::mat4 proj = glm::mat4(1.0f);
	
	// m_CameraRot.x = pitch, .y = yaw.
	glm::vec3 forward = GetCameraForward();
	glm::vec3 up = GetCameraUp();
	view = glm::lookAt(m_CameraPos, m_CameraPos + forward, up);
	
	float aspect = (contentSize.y > 0.0001f) ? (contentSize.x / contentSize.y) : 1.0f;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (aspect <= 0.0001f) aspect = 1.0f;
	// Scene View feeds the same projection into picking, gizmos, rendering, and
	// Hi-Z validation. Make the depth convention explicit so all those users
	// agree on Vulkan's 0..1 clip-space depth.
	glm::mat4 projNoFlip = glm::mat4(1.0f);
	if (m_ToolState.projectionMode == SceneOverlay::ProjectionMode::Orthographic)
	{
		const float halfHeight = std::max(1.0f, m_OrthographicHeight) * 0.5f;
		const float halfWidth = halfHeight * aspect;
		projNoFlip = glm::orthoRH_ZO(-halfWidth, halfWidth, -halfHeight, halfHeight, 0.1f, 1000.0f);
	}
	else
	{
		projNoFlip = glm::perspectiveRH_ZO(glm::radians(std::max(m_Fov, 0.1f)), aspect, 0.1f, 1000.0f);
	}
	proj = projNoFlip;
	proj[1][1] *= -1; // Vulkan flip

	BuildSceneObjectMarkers(projNoFlip * view, cursorPos, contentSize);
	const bool orientationChanged = DrawOrientationWidget(view, projNoFlip, cursorPos, contentSize);
	if (orientationChanged)
	{
		// Submit the manipulated camera to the renderer immediately so the next
		// Scene View texture reflects the view-cube drag/click without an extra
		// frame of stale camera state.
		forward = GetCameraForward();
		up = GetCameraUp();
		view = glm::lookAt(m_CameraPos, m_CameraPos + forward, up);
	}

	// Set Scene View Camera for next render after overlay input has had a chance
	// to adjust the editor camera.
	RenderSystem::Instance().SetSceneViewCamera(view, proj, m_CameraPos, m_ToolState.projectionMode == SceneOverlay::ProjectionMode::Orthographic);
	ApplyPrefabContextTintForNextRender();

	// Register the transform gizmo before marker hit testing and object picking.
	// Parent objects commonly have an empty-object marker at the exact transform
	// pivot; current-frame ImGuizmo hover must win there so the marker cannot
	// consume the first click intended for a translate/rotate/scale handle.
	DrawGizmos(view, projNoFlip, cursorPos, contentSize);
	DrawSceneObjectMarkers();
	HandlePicking(view, projNoFlip, cursorPos, contentSize);

	DrawPhysicsDebugOverlay(view, projNoFlip, cursorPos, contentSize);
	PublishSceneViewDiagnostics(true);
}

void SceneViewWindow::BeginPhysicsColliderPreview(entt::entity entity, const TerraEngine::UUID& recordId)
{
	auto& objects = ObjectManager::instance();
	const std::optional<Object> object = objects.getObject(entity);
	ColliderSetComponent* set = object ? objects.GetComponent<ColliderSetComponent>(*object) : nullptr;
	if (!object || set == nullptr || recordId == TerraEngine::EMPTY_UUID)
	{
		m_PhysicsPreviewStatus = "Collider target is unavailable.";
		return;
	}

	auto lock = set->acquireReadLock();
	const auto record = std::find_if(set->colliders.begin(), set->colliders.end(), [&](const TerraEngine::Physics::ColliderRecord& candidate) {
		return candidate.recordId == recordId;
	});
	if (record == set->colliders.end())
	{
		m_PhysicsPreviewStatus = "Collider record changed before it could be selected.";
		return;
	}

	m_SelectedColliderRecordId = recordId;
	SceneOverlay::PhysicsShapeEditPreview preview;
	preview.objectId = objects.getUUID(*object);
	preview.colliderRecordId = recordId;
	preview.localCenter = ToGlm(record->localCenter);
	preview.localRotation = ToGlm(record->localRotation);
	preview.localExtents = ColliderHalfExtents(*record);
	preview.active = true;
	m_PhysicsLastAuthoredCenter = preview.localCenter;
	m_PhysicsLastAuthoredRotation = preview.localRotation;
	m_PhysicsLastAuthoredExtents = preview.localExtents;
	m_PhysicsShapePreview = std::move(preview);
	m_PhysicsPreviewStatus = "Editing collider shape; Apply publishes one queued transaction.";
}

bool SceneViewWindow::ReadPhysicsColliderPreview(
	const TerraEngine::UUID& objectId,
	const TerraEngine::UUID& recordId,
	glm::vec3& outCenter,
	glm::vec3& outHalfExtents) const
{
	if (!m_PhysicsShapePreview || !m_PhysicsShapePreview->active
		|| m_PhysicsShapePreview->objectId != objectId
		|| m_PhysicsShapePreview->colliderRecordId != recordId)
	{
		return false;
	}
	outCenter = m_PhysicsShapePreview->localCenter;
	outHalfExtents = m_PhysicsShapePreview->localExtents;
	return true;
}

void SceneViewWindow::UpdatePhysicsColliderPreview(
	const TerraEngine::UUID& objectId,
	const TerraEngine::UUID& recordId,
	const glm::vec3& center,
	const glm::vec3& halfExtents)
{
	if (!m_PhysicsShapePreview || !m_PhysicsShapePreview->active
		|| m_PhysicsShapePreview->objectId != objectId
		|| m_PhysicsShapePreview->colliderRecordId != recordId)
	{
		return;
	}

	// Numeric Inspector edits update the same detached values as viewport drags.
	// Clamping here keeps every input path within the primitive validation bound.
	m_PhysicsShapePreview->localCenter = center;
	m_PhysicsShapePreview->localExtents = glm::max(halfExtents, glm::vec3(0.001f));
	m_PhysicsPreviewStatus = "Editing collider shape; Apply publishes one queued transaction.";
}

bool SceneViewWindow::IsPhysicsColliderPreviewSelection(const TerraEngine::UUID& recordId) const
{
	return recordId != TerraEngine::EMPTY_UUID && m_SelectedColliderRecordId == recordId;
}

void SceneViewWindow::RequestPhysicsColliderFitToRenderMesh(entt::entity entity)
{
	if (!m_PhysicsShapePreview || !m_PhysicsShapePreview->active)
	{
		m_PhysicsPreviewStatus = "Select a collider before fitting it to a render mesh.";
		return;
	}

	auto& registry = ObjectManager::instance().getRegistry();
	const MeshRenderComponent* renderable = registry.valid(entity) ? registry.try_get<MeshRenderComponent>(entity) : nullptr;
	Mesh* mesh = renderable ? ResourceManager::Instance().GetMesh(renderable->meshId) : nullptr;
	if (mesh == nullptr)
	{
		m_PhysicsPreviewStatus = "Fit rejected: the selected object has no loaded render mesh bounds.";
		return;
	}

	// TEMF quantization stores an AABB as center plus half extents. Copy those
	// values into preview state only; the live collider stays untouched until
	// the explicit Apply action creates a protected transaction.
	m_PhysicsShapePreview->localCenter = {
		mesh->header.quantization.positionOffset[0],
		mesh->header.quantization.positionOffset[1],
		mesh->header.quantization.positionOffset[2]
	};
	m_PhysicsShapePreview->localExtents = glm::max(glm::abs(glm::vec3(
		mesh->header.quantization.positionScale[0],
		mesh->header.quantization.positionScale[1],
		mesh->header.quantization.positionScale[2])), glm::vec3(0.001f));
	m_PhysicsShapePreview->fitToRenderMeshRequested = true;
	m_PhysicsPreviewStatus = "Render-mesh bounds are staged for the collider edit.";
}

void SceneViewWindow::CancelPhysicsColliderPreview()
{
	m_PhysicsShapePreview.reset();
	m_PhysicsPreviewStatus = "Collider edit canceled; no scene or physics state changed.";
}

void SceneViewWindow::CommitPhysicsColliderPreview()
{
	if (!m_PhysicsShapePreview)
	{
		m_PhysicsPreviewStatus = "No collider edit is ready to apply.";
		return;
	}

	const SceneOverlay::PhysicsShapeAuthoringCommit commit = SceneOverlay::FinalizePhysicsShapeEdit(*m_PhysicsShapePreview);
	if (!commit.valid)
	{
		m_PhysicsPreviewStatus = "Collider edit is invalid and was rejected without mutation.";
		return;
	}

	auto& objects = ObjectManager::instance();
	const std::optional<Object> object = objects.getObject(commit.preview.objectId);
	ColliderSetComponent* set = object ? objects.GetComponent<ColliderSetComponent>(*object) : nullptr;
	if (!object || set == nullptr)
	{
		m_PhysicsPreviewStatus = "Collider target is stale; no transaction was queued.";
		return;
	}

	std::vector<TerraEngine::Physics::ColliderRecord> updatedRecords;
	std::uint32_t schemaVersion = TerraEngine::Physics::kColliderSetComponentSchemaVersion;
	bool componentActive = true;
	{
		auto lock = set->acquireReadLock();
		updatedRecords = set->colliders;
		schemaVersion = set->schemaVersion;
		componentActive = set->active;
	}
	const auto record = std::find_if(updatedRecords.begin(), updatedRecords.end(), [&](const TerraEngine::Physics::ColliderRecord& candidate) {
		return candidate.recordId == commit.preview.colliderRecordId;
	});
	if (record == updatedRecords.end())
	{
		m_PhysicsPreviewStatus = "Collider record is stale; transaction rejected atomically.";
		return;
	}

	// Revalidate a detached copy before publishing. The stable record UUID is
	// preserved, and asset-backed colliders reject dimension edits instead of
	// inventing a primitive that would silently change authored intent.
	record->localCenter = ToPhysics(commit.preview.localCenter);
	record->localRotation = ToPhysics(commit.preview.localRotation);
	if (!ApplyColliderHalfExtents(*record, commit.preview.localExtents))
	{
		m_PhysicsPreviewStatus = "Asset-backed collider dimensions cannot be edited with primitive handles.";
		return;
	}
	ColliderSetComponent candidate;
	candidate.schemaVersion = schemaVersion;
	candidate.active = componentActive;
	candidate.colliders = updatedRecords;
	const TerraEngine::Physics::ColliderSetValidationResult validation = candidate.Validate();
	if (!validation.accepted)
	{
		m_PhysicsPreviewStatus = "Collider edit rejected: " + validation.message;
		return;
	}

	// This is the sole durable mutation for a completed drag or fit. The read
	// set makes a stale object or intervening component edit reject the complete
	// update at the queue's protected flush boundary rather than partially
	// changing ObjectManager or the provider during UI interaction.
	TerraEngine::Scene::TransactionalMutationBuilder builder(
		"scene-view-physics-shape-" + TerraEngine::ToHexString(commit.preview.objectId) + "-"
			+ TerraEngine::ToHexString(commit.preview.colliderRecordId) + "-" + std::to_string(++m_PhysicsShapeCommitSequence),
		TerraEngine::Scene::TransactionOrigin::Editor,
		"SceneViewPhysicsColliderHandles");
	builder.ObserveScene(objects)
		.ObserveObject(objects, commit.preview.objectId)
		.SetUndoGroup(commit.transactionLabel)
		.UpdateComponentProperty(
			commit.preview.objectId,
			std::string(ColliderSetComponent::kComponentName),
			"colliders",
			TerraEngine::Reflection::Variant{std::move(updatedRecords)});
	TerraEngine::Scene::SceneMutationQueue::Instance().EnqueueTransaction(builder.Build());
	m_PhysicsShapePreview.reset();
	m_PhysicsPreviewStatus = "Collider edit queued for the protected transactional flush.";
}

void SceneViewWindow::DrawPhysicsColliderEditHandles(const glm::mat4& view, const glm::mat4& proj, const ImVec2& viewportPos, const ImVec2& viewportSize)
{
	if (m_Owner == nullptr)
	{
		return;
	}

	const std::optional<entt::entity> selectedEntity = m_Owner->GetSelectedEntity();
	auto& objects = ObjectManager::instance();
	auto& registry = objects.getRegistry();
	if (!selectedEntity || !registry.valid(*selectedEntity))
	{
		if (m_PhysicsShapePreview) CancelPhysicsColliderPreview();
		return;
	}
	const std::optional<Object> object = objects.getObject(*selectedEntity);
	ColliderSetComponent* set = object ? objects.GetComponent<ColliderSetComponent>(*object) : nullptr;
	if (!object || set == nullptr)
	{
		if (m_PhysicsShapePreview) CancelPhysicsColliderPreview();
		return;
	}
	if (!m_PhysicsShapePreview || !m_PhysicsShapePreview->active)
	{
		return;
	}

	std::optional<TerraEngine::Physics::ColliderRecord> authoredRecord;
	{
		auto lock = set->acquireReadLock();
		const auto record = std::find_if(set->colliders.begin(), set->colliders.end(), [&](const TerraEngine::Physics::ColliderRecord& candidate) {
			return candidate.recordId == m_PhysicsShapePreview->colliderRecordId;
		});
		if (record != set->colliders.end()) authoredRecord = *record;
	}
	const TerraEngine::UUID selectedObjectId = objects.getUUID(*object);
	if (m_PhysicsShapePreview->objectId != selectedObjectId || !authoredRecord)
	{
		// Selection and record identity are both stable UUIDs. Clearing preview on
		// either mismatch prevents an Apply click in a newly selected object from
		// publishing an edit prepared for a stale object or removed record.
		m_PhysicsShapePreview.reset();
		m_SelectedColliderRecordId = TerraEngine::EMPTY_UUID;
		return;
	}

	const glm::vec3 authoredCenter = ToGlm(authoredRecord->localCenter);
	const glm::quat authoredRotation = ToGlm(authoredRecord->localRotation);
	const glm::vec3 authoredExtents = ColliderHalfExtents(*authoredRecord);
	if (authoredCenter != m_PhysicsLastAuthoredCenter
		|| authoredRotation != m_PhysicsLastAuthoredRotation
		|| authoredExtents != m_PhysicsLastAuthoredExtents)
	{
		// Undo/redo publishes a new live ColliderSet value at the protected flush.
		// Keep edit mode active, but replace the detached visual state so handles
		// and Inspector preview fields show the exact restored collider immediately.
		m_PhysicsShapePreview->localCenter = authoredCenter;
		m_PhysicsShapePreview->localRotation = authoredRotation;
		m_PhysicsShapePreview->localExtents = authoredExtents;
		m_PhysicsLastAuthoredCenter = authoredCenter;
		m_PhysicsLastAuthoredRotation = authoredRotation;
		m_PhysicsLastAuthoredExtents = authoredExtents;
		m_PhysicsPreviewStatus = "Collider shape refreshed from the restored undo/redo state.";
	}
	const std::optional<glm::mat4> model = ComputeEntityWorldMatrix(*selectedEntity);
	if (!model)
	{
		return;
	}

	const glm::mat4 viewProjection = proj * view;
	const glm::vec3 localCenter = m_PhysicsShapePreview->localCenter;
	const glm::quat localRotation = glm::normalize(m_PhysicsShapePreview->localRotation);
	const glm::vec3 extents = glm::max(m_PhysicsShapePreview->localExtents, glm::vec3(0.001f));
	const glm::mat4 colliderPose = *model
		* glm::translate(glm::mat4(1.0F), localCenter)
		* glm::mat4_cast(localRotation);
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	std::array<glm::vec3, 8> corners{};
	for (int index = 0; index < 8; ++index)
	{
		const glm::vec3 signedExtent(
			(index & 1) == 0 ? -extents.x : extents.x,
			(index & 2) == 0 ? -extents.y : extents.y,
			(index & 4) == 0 ? -extents.z : extents.z);
		corners[static_cast<std::size_t>(index)] = glm::vec3(colliderPose * glm::vec4(signedExtent, 1.0f));
	}
	constexpr std::array<std::array<int, 2>, 12> edges = {{{0, 1}, {0, 2}, {0, 4}, {1, 3}, {1, 5}, {2, 3}, {2, 6}, {3, 7}, {4, 5}, {4, 6}, {5, 7}, {6, 7}}};
	for (const auto& edge : edges)
	{
		DrawWorldLine(drawList, corners[static_cast<std::size_t>(edge[0])], corners[static_cast<std::size_t>(edge[1])], viewProjection, viewportPos, viewportSize, IM_COL32(255, 206, 72, 235), 1.5f);
	}

	// Collider Move/Rotate/Scale uses the same active toolbar operation as object
	// editing. Handle changes made by W, E, and R therefore take effect in this
	// frame while the detached preview remains the only mutable state.
	glm::mat4 manipulatedModel = colliderPose * glm::scale(glm::mat4(1.0F), extents);
	const float* snap = m_UsingSnap
		? (m_CurrentGizmoOperation == ImGuizmo::ROTATE
			? m_RotationSnapValue
			: (m_CurrentGizmoOperation == ImGuizmo::SCALE ? m_ScaleSnapValue : m_SnapValue))
		: nullptr;
	if (m_CurrentGizmoOperation >= 0 && ImGuizmo::Manipulate(
		glm::value_ptr(view),
		glm::value_ptr(proj),
		(ImGuizmo::OPERATION)m_CurrentGizmoOperation,
		m_ToolState.orientationMode == SceneOverlay::OrientationMode::Local ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
		glm::value_ptr(manipulatedModel),
		nullptr,
		snap))
	{
		glm::vec3 editedExtents;
		glm::quat editedRotation;
		glm::vec3 editedCenter;
		glm::vec3 skew;
		glm::vec4 perspective;
		const glm::mat4 editedLocal = glm::inverse(*model) * manipulatedModel;
		if (glm::decompose(editedLocal, editedExtents, editedRotation, editedCenter, skew, perspective))
		{
			// Decomposition returns the collider's local authoring values. Absolute
			// scale prevents an axis crossing from publishing invalid dimensions.
			m_PhysicsShapePreview->localCenter = editedCenter;
			m_PhysicsShapePreview->localRotation = glm::normalize(editedRotation);
			m_PhysicsShapePreview->localExtents = glm::max(glm::abs(editedExtents), glm::vec3(0.001F));
			m_InteractionContext.activeRegion = SceneOverlay::InteractionRegion::Gizmo;
			m_PhysicsPreviewStatus = "Editing collider with Move/Rotate/Scale; Apply publishes one queued transaction.";
		}
	}
}

void SceneViewWindow::DrawPhysicsDebugOverlay(const glm::mat4& view, const glm::mat4& proj, const ImVec2& viewportPos, const ImVec2& viewportSize)
{
	m_PhysicsDebugPacketCount = 0U;
	m_PhysicsDebugPrimitiveCount = 0U;
	m_PhysicsDebugVisible = false;
	// Physics overlay is opt-in and no producer work is requested here. Opening
	// this Scene View merely reads packets that physics already published.
	if (!m_OverlayPreference.diagnosticsVisible)
	{
		return;
	}

	const std::shared_ptr<const TerraEngine::Physics::PhysicsDiagnosticsSnapshot> diagnostics =
		TerraEngine::Physics::AcquirePhysicsDiagnosticsSnapshot();
	if (!diagnostics || !diagnostics->world.IsValid())
	{
		return;
	}
	const std::vector<std::shared_ptr<const TerraEngine::Physics::PhysicsDebugPacket>> packets =
		TerraEngine::Physics::PublishedPhysicsDebugPackets().Select(diagnostics->world, diagnostics->tick, m_PhysicsDebugSelection);
	m_PhysicsDebugPacketCount = packets.size();
	m_PhysicsDebugPrimitiveCount = SceneOverlay::CountPhysicsDebugPrimitives(packets, m_PhysicsDebugSelection);
	if (packets.empty())
	{
		return;
	}

	// The renderer records the immutable submission only when its RHI exists.
	// CPU drawing below intentionally remains a no-op when the caller returned
	// through the renderer-unavailable branch earlier in OnGui.
	if (!RenderSystem::Instance().SubmitSceneViewPhysicsDebugPackets(packets, m_PhysicsDebugSelection))
	{
		return;
	}
	m_PhysicsDebugVisible = true;
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	const auto project = [&](const TerraEngine::Physics::PhysicsVector3& point, ImVec2& screen) {
		const glm::vec4 clip = proj * view * glm::vec4(point.x, point.y, point.z, 1.0f);
		if (clip.w <= 0.0001f)
		{
			return false;
		}
		const glm::vec3 ndc = glm::vec3(clip) / clip.w;
		screen = ImVec2(viewportPos.x + (ndc.x * 0.5f + 0.5f) * viewportSize.x, viewportPos.y + (-ndc.y * 0.5f + 0.5f) * viewportSize.y);
		return std::isfinite(screen.x) && std::isfinite(screen.y);
	};
	const auto drawSegment = [&](const TerraEngine::Physics::PhysicsVector3& first, const TerraEngine::Physics::PhysicsVector3& second, ImU32 color, float thickness = 1.5f) {
		ImVec2 start;
		ImVec2 end;
		if (project(first, start) && project(second, end))
		{
			drawList->AddLine(start, end, color, thickness);
		}
	};
	for (const std::shared_ptr<const TerraEngine::Physics::PhysicsDebugPacket>& packet : packets)
	{
		if (!packet) { continue; }
		if (m_PhysicsDebugSelection.showLines && packet->lines)
		{
			for (const auto& line : *packet->lines) { drawSegment(line.start, line.end, line.color); }
		}
		if (m_PhysicsDebugSelection.showTriangles && packet->triangles)
		{
			for (const auto& triangle : *packet->triangles)
			{
				drawSegment(triangle.a, triangle.b, triangle.color);
				drawSegment(triangle.b, triangle.c, triangle.color);
				drawSegment(triangle.c, triangle.a, triangle.color);
			}
		}
		if (m_PhysicsDebugSelection.showContacts && packet->contacts)
		{
			for (const auto& contact : *packet->contacts)
			{
				const TerraEngine::Physics::PhysicsVector3 end{contact.position.x + contact.normal.x * 0.25F, contact.position.y + contact.normal.y * 0.25F, contact.position.z + contact.normal.z * 0.25F};
				drawSegment(contact.position, end, IM_COL32(255, 208, 64, 255), 2.0f);
			}
		}
		if (m_PhysicsDebugSelection.showConstraints && packet->constraints)
		{
			for (const auto& constraint : *packet->constraints) { drawSegment(constraint.firstAnchor, constraint.secondAnchor, constraint.broken ? IM_COL32(255, 64, 64, 255) : IM_COL32(96, 220, 255, 255), 2.0f); }
		}
		if (m_PhysicsDebugSelection.showCharacterSupport && packet->characters)
		{
			for (const auto& character : *packet->characters)
			{
				const TerraEngine::Physics::PhysicsVector3 end{character.position.x + character.up.x * character.height, character.position.y + character.up.y * character.height, character.position.z + character.up.z * character.height};
				drawSegment(character.position, end, IM_COL32(128, 255, 128, 255), 2.0f);
			}
		}
		if (m_PhysicsDebugSelection.showRecentQueries && packet->queries)
		{
			for (const auto& query : *packet->queries)
			{
				const TerraEngine::Physics::PhysicsVector3 end{query.origin.x + query.direction.x * query.distance, query.origin.y + query.direction.y * query.distance, query.origin.z + query.direction.z * query.distance};
				drawSegment(query.origin, end, IM_COL32(180, 128, 255, 255));
			}
		}
	}
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
	if (m_PhysicsShapePreview && m_PhysicsShapePreview->active)
	{
		// Empty-space and object clicks inside Scene View must not silently abandon
		// a detached collider edit. Apply or Cancel in the Inspector owns the edit
		// decision; stale object/record validation remains the only automatic exit.
		return;
	}
	if (m_ToolState.activeTool == SceneOverlay::SceneViewTool::Hand || m_IsSceneViewPanning)
	{
		return;
	}
	if (m_InteractionContext.activeRegion == SceneOverlay::InteractionRegion::Marker
		|| m_InteractionContext.activeRegion == SceneOverlay::InteractionRegion::OrientationWidget
		|| m_InteractionContext.hoveredRegion == SceneOverlay::InteractionRegion::OrientationWidget
		|| m_InteractionContext.activeRegion == SceneOverlay::InteractionRegion::Toolbar)
	{
		return;
	}

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
void SceneViewWindow::HandleCameraInput(float deltaTime, const ImVec2& viewportSize) {
	// Need to include WindowSystem and GLFW for this
	auto* window = static_cast<GLFWwindow*>(WindowSystem::Instance().GetNativeHandle());
	if (HandleSceneViewPan(viewportSize))
	{
		return;
	}

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
		if (ImGui::IsKeyPressed(ImGuiKey_W)) { m_CurrentGizmoOperation = ImGuizmo::TRANSLATE; m_ToolState.activeTool = SceneOverlay::SceneViewTool::Move; }
		if (ImGui::IsKeyPressed(ImGuiKey_E)) { m_CurrentGizmoOperation = ImGuizmo::ROTATE; m_ToolState.activeTool = SceneOverlay::SceneViewTool::Rotate; }
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::IsKeyPressed(ImGuiKey_R)) { m_CurrentGizmoOperation = ImGuizmo::SCALE; m_ToolState.activeTool = SceneOverlay::SceneViewTool::Scale; }
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
	if (!m_ToolState.gizmosVisible)
	{
		return;
	}
	// Collider manipulation is independent of the transform-tool selection, so
	// its handles remain available when the normal transform gizmo is disabled.
	DrawPhysicsColliderEditHandles(view, proj, viewportPos, viewportSize);
	if (m_PhysicsShapePreview && m_PhysicsShapePreview->active)
	{
		// Collider preview handles own viewport manipulation until Apply or Cancel.
		// Suppressing the object transform gizmo prevents two overlapping control
		// sets from competing for the same pointer input and visual position.
		return;
	}
	if (m_CurrentGizmoOperation < 0)
	{
		return;
	}
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
						m_ToolState.orientationMode == SceneOverlay::OrientationMode::Local ? ImGuizmo::LOCAL : ImGuizmo::WORLD, 
						glm::value_ptr(model),
						nullptr, 
						m_UsingSnap ? (m_CurrentGizmoOperation == ImGuizmo::ROTATE ? m_RotationSnapValue : (m_CurrentGizmoOperation == ImGuizmo::SCALE ? m_ScaleSnapValue : m_SnapValue)) : nullptr)) 
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
						if (selectedObject)
						{
							// The renderer caches propagated world transforms by ObjectManager
							// revision. Notify it for every preview update so moving a parent
							// invalidates and recomputes the complete descendant branch.
							ObjectManager::instance().MarkObjectTransformDirty(*selectedObject);
						}
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
