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
#include "Engine/Scene/WorldStreamingSystem.h"
#include "Engine/Animation/AnimationSystem.h"
#include "Engine/Animation/Assets/SkeletonAsset.h"
#include "Engine/Animation/Runtime/AnimationAttachments.h"
#include "Engine/Scripting/AnimationScriptAbi.h"
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
#include <unordered_map>
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

std::optional<TerraEngine::Scene::WorldPosition> ComputeEntityWorldPosition(entt::entity entity)
{
	// ObjectManager owns propagated canonical poses. Reading that authoritative
	// pose avoids rebuilding a lossy global matrix from parent-local transforms.
	auto& objectManager = ObjectManager::instance();
	const std::optional<Object> object = objectManager.getObject(entity);
	if (!object)
	{
		return std::nullopt;
	}
	const std::optional<TerraEngine::Scene::WorldPose> pose = objectManager.GetCanonicalWorldPose(*object);
	return pose ? std::optional<TerraEngine::Scene::WorldPosition>(pose->position) : std::nullopt;
}

std::optional<glm::mat4> ComputeEntityLocalWorldMatrix(
	entt::entity entity,
	const TerraEngine::Scene::WorldPosition& frameOrigin)
{
	const std::optional<TerraEngine::Scene::WorldPosition> position = ComputeEntityWorldPosition(entity);
	const std::optional<Object> object = ObjectManager::instance().getObject(entity);
	if (!position || !object)
	{
		return std::nullopt;
	}
	const std::optional<TerraEngine::Scene::WorldPose> pose = ObjectManager::instance().GetCanonicalWorldPose(*object);
	const TerraEngine::Scene::WorldLocalizationResult localized =
		TerraEngine::Scene::LocalizeWorldPosition(*position, frameOrigin, TerraEngine::Scene::kTransformSpaceMaximumRelativeRadiusMetres);
	if (!pose || !localized.Succeeded())
	{
		return std::nullopt;
	}

	glm::mat4 local(1.0f);
	local = glm::translate(local, glm::vec3(localized.offsetMetres));
	local *= glm::mat4_cast(pose->rotation);
	return glm::scale(local, pose->scale);
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

glm::mat4 AnimationTransformMatrix(const TerraEngine::Animation::AnimationTransform& transform)
{
	// Animation records use x/y/z/w quaternion order; glm's constructor takes
	// w/x/y/z. Keeping the conversion here avoids duplicating that subtle rule in
	// every Scene View overlay producer.
	glm::mat4 matrix(1.0f);
	matrix = glm::translate(matrix, glm::vec3(
		transform.translation.x, transform.translation.y, transform.translation.z));
	matrix *= glm::mat4_cast(glm::quat(
		transform.rotation.w, transform.rotation.x,
		transform.rotation.y, transform.rotation.z));
	matrix = glm::scale(matrix, glm::vec3(
		transform.scale.x, transform.scale.y, transform.scale.z));
	return matrix;
}

glm::mat4 NativeAnimationTransformMatrix(const NativeAnimationTransform& transform)
{
	glm::mat4 matrix(1.0f);
	matrix = glm::translate(matrix, glm::vec3(
		transform.translation.x, transform.translation.y, transform.translation.z));
	matrix *= glm::mat4_cast(glm::quat(
		transform.rotation.w, transform.rotation.x,
		transform.rotation.y, transform.rotation.z));
	matrix = glm::scale(matrix, glm::vec3(
		transform.scale.x, transform.scale.y, transform.scale.z));
	return matrix;
}

const char* ProceduralGoalLabel(NativeAnimationProceduralGoalKind kind)
{
	switch(kind)
	{
	case NativeAnimationProceduralGoalKind::Position: return "Position goal";
	case NativeAnimationProceduralGoalKind::Rotation: return "Rotation goal";
	case NativeAnimationProceduralGoalKind::PositionAndRotation: return "Position + rotation goal";
	case NativeAnimationProceduralGoalKind::Aim: return "Aim goal";
	case NativeAnimationProceduralGoalKind::Look: return "Look goal";
	case NativeAnimationProceduralGoalKind::FootPlant: return "Foot contact goal";
	case NativeAnimationProceduralGoalKind::CopyTransform: return "Copy-transform goal";
	case NativeAnimationProceduralGoalKind::Twist: return "Twist goal";
	}
	return "Procedural goal";
}

bool ProjectLocalPoint(
	const glm::vec3& referenceFrameLocalPosition,
	const glm::mat4& viewProjection,
	const ImVec2& viewportPos,
	const ImVec2& viewportSize,
	ImVec2& outScreenPosition)
{
	const glm::vec4 clipPosition = viewProjection * glm::vec4(referenceFrameLocalPosition, 1.0f);
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

void DrawLocalLine(
	ImDrawList* drawList,
	const glm::vec3& localStart,
	const glm::vec3& localEnd,
	const glm::mat4& viewProjection,
	const ImVec2& viewportPos,
	const ImVec2& viewportSize,
	ImU32 color,
	float thickness)
{
	ImVec2 screenStart{};
	ImVec2 screenEnd{};
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!ProjectLocalPoint(localStart, viewProjection, viewportPos, viewportSize, screenStart)
		|| !ProjectLocalPoint(localEnd, viewProjection, viewportPos, viewportSize, screenEnd))
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
		DrawLocalLine(drawList, pointA, pointB, viewProjection, viewportPos, viewportSize, color, thickness);
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
		DrawLocalLine(drawList, rayStart, rayEnd, viewProjection, viewportPos, viewportSize, color, 1.5f);
	}

	// Draw multiple parallel lines to communicate directional lighting.
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int lineIndex = 0; lineIndex < 4; ++lineIndex)
	{
		const float offset = (static_cast<float>(lineIndex) - 1.5f) * 0.2f;
		const glm::vec3 start = position + up * offset;
		const glm::vec3 end = start + direction * 2.4f;
		DrawLocalLine(drawList, start, end, viewProjection, viewportPos, viewportSize, color, 1.8f);
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

	DrawLocalLine(drawList, position, coneCenter, viewProjection, viewportPos, viewportSize, color, 1.2f);
	DrawWorldCircle(drawList, coneCenter, right, up, outerRadius, 48, viewProjection, viewportPos, viewportSize, color, 1.6f);
	DrawWorldCircle(drawList, coneCenter, right, up, innerRadius, 36, viewProjection, viewportPos, viewportSize, color, 1.1f);

	// Outline a sparse cone wireframe so users can read direction and range quickly.
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int edgeIndex = 0; edgeIndex < 8; ++edgeIndex)
	{
		const float angle = glm::two_pi<float>() * (static_cast<float>(edgeIndex) / 8.0f);
		const glm::vec3 edgeDirection = SafeNormalize(right * std::cos(angle) + up * std::sin(angle), right);
		const glm::vec3 outerPoint = coneCenter + edgeDirection * outerRadius;
		DrawLocalLine(drawList, position, outerPoint, viewProjection, viewportPos, viewportSize, color, 1.2f);
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
		drawList->AddRect(palmMin, palmMax, iconColor, unit * 0.06f, thickness, 0);
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
		drawList->AddRect(frontMin, frontMax, iconColor, 0.0f, thickness, 0);
		drawList->AddRect(AddIconVec2(frontMin, offset), AddIconVec2(frontMax, offset), iconColor, 0.0f, thickness, 0);
		drawList->AddLine(frontMin, AddIconVec2(frontMin, offset), iconColor, thickness);
		drawList->AddLine(ImVec2(frontMax.x, frontMin.y), AddIconVec2(ImVec2(frontMax.x, frontMin.y), offset), iconColor, thickness);
		drawList->AddLine(frontMax, AddIconVec2(frontMax, offset), iconColor, thickness);
		return;
	}

	if (shapeKind == "magnet-grid")
	{
		drawList->PathArcTo(ImVec2(center.x - radius * 0.35f, center.y), radius * 0.55f, 3.14159265f * 0.5f, 3.14159265f * 1.5f, 18);
		drawList->PathStroke(iconColor, thickness, 0);
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
		drawList->AddRect(ImVec2(center.x - radius * 0.8f, center.y - radius * 0.8f), ImVec2(center.x + radius * 0.8f, center.y + radius * 0.8f), iconColor, unit * 0.03f, thickness, 0);
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
	drawList->AddRect(min, max, border, 3.0f, active ? 1.6f : 1.0f, 0);
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
	const auto position = [](const TerraEngine::Scene::WorldPosition& value) {
		return nlohmann::json{{"cell", {
			{"x", TerraEngine::Scene::EncodeWorldCellAxis(value.cell.x)},
			{"y", TerraEngine::Scene::EncodeWorldCellAxis(value.cell.y)},
			{"z", TerraEngine::Scene::EncodeWorldCellAxis(value.cell.z)}}},
			{"offsetMetres", {value.offsetMetres.x, value.offsetMetres.y, value.offsetMetres.z}}};
	};
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
			{"position", position(m_CameraPosition)},
			{"focus", position(m_CameraFocusInitialized ? m_CameraFocus : ResolveSceneViewFocus(m_CameraFocusDistance))},
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

bool SceneViewWindow::SetAutomationCamera(
	const TerraEngine::Scene::WorldPosition& position,
	const TerraEngine::Scene::WorldPosition& focus,
	std::string& outError)
{
	const TerraEngine::Scene::WorldCoordinateResult normalizedPosition =
		TerraEngine::Scene::NormalizeWorldPosition(position);
	const TerraEngine::Scene::WorldCoordinateResult normalizedFocus =
		TerraEngine::Scene::NormalizeWorldPosition(focus);
	if (!normalizedPosition.Succeeded() || !normalizedFocus.Succeeded())
	{
		outError = "Camera position and focus must be normalized canonical locations.";
		return false;
	}
	const TerraEngine::Scene::WorldLocalizationResult localizedFocus =
		TerraEngine::Scene::LocalizeWorldPosition(normalizedFocus.position, normalizedPosition.position, 1000.0);
	if (!localizedFocus.Succeeded() || glm::length(glm::vec3(localizedFocus.offsetMetres)) <= 0.0001f)
	{
		outError = "Camera focus must be a distinct location within the bounded Scene View working frame.";
		return false;
	}
	const glm::vec2 euler = CameraEulerFromForward(glm::vec3(localizedFocus.offsetMetres));
	m_CameraRot = glm::vec3(euler.x, euler.y, 0.0f);
	m_CameraPosition = normalizedPosition.position;
	m_CameraFocus = normalizedFocus.position;
	m_CameraFocusDistance = std::clamp(glm::length(glm::vec3(localizedFocus.offsetMetres)), 1.0f, 1000.0f);
	m_CameraFocusInitialized = true;
	return true;
}

// Resolve the same parent-composed model matrix used by Scene View gizmos so
// collider handles remain attached to nested objects instead of world origin.
std::optional<glm::mat4> ComputeEntityWorldMatrix(
	entt::entity entity,
	const TerraEngine::Scene::WorldPosition& frameOrigin)
{
	return ComputeEntityLocalWorldMatrix(entity, frameOrigin);
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
	const std::optional<TerraEngine::Scene::WorldPosition> worldPosition = ComputeEntityWorldPosition(entity);
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
	const TerraEngine::Scene::WorldPosition focus = ResolveSceneViewFocus(m_CameraFocusDistance);
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

TerraEngine::Scene::WorldPosition SceneViewWindow::ResolveSceneViewFocus(float fallbackDistance) const
{
	if (m_Owner != nullptr)
	{
		const std::optional<entt::entity> selectedEntity = m_Owner->GetSelectedEntity();
		if (selectedEntity.has_value())
		{
			if (const std::optional<TerraEngine::Scene::WorldPosition> selectedWorldPosition = ComputeEntityWorldPosition(*selectedEntity))
			{
				return *selectedWorldPosition;
			}
		}
	}

	if (m_CameraFocusInitialized)
	{
		return m_CameraFocus;
	}

	// No selection means the focus is a bounded forward displacement from the
	// camera's canonical address; this is safe at every cell in the world.
	const TerraEngine::Scene::WorldCoordinateResult result = TerraEngine::Scene::ApplyWorldDelta(
		m_CameraPosition, {{}, glm::dvec3(GetCameraForward()) * static_cast<double>(std::max(fallbackDistance, 1.0f))});
	return result.Succeeded() ? result.position : m_CameraPosition;
}

float SceneViewWindow::ResolveSceneViewFocusDistance(
	const TerraEngine::Scene::WorldPosition& focus,
	float fallbackDistance) const
{
	const TerraEngine::Scene::WorldLocalizationResult localized = TerraEngine::Scene::LocalizeWorldPosition(
		focus, m_CameraPosition, 1000.0);
	if (!localized.Succeeded())
	{
		return std::clamp(fallbackDistance, 1.0f, 1000.0f);
	}
	const glm::vec3 forward = GetCameraForward();
	const glm::vec3 localFocus(localized.offsetMetres);
	const float alongView = glm::dot(localFocus, forward);
	const float directDistance = glm::length(localFocus);
	const float resolvedDistance = alongView > 1.0f ? alongView : directDistance;
	if (!std::isfinite(resolvedDistance) || resolvedDistance <= 1.0f)
	{
		return std::clamp(fallbackDistance, 1.0f, 1000.0f);
	}
	return std::clamp(resolvedDistance, 1.0f, 1000.0f);
}

void SceneViewWindow::RepositionCameraAroundFocus(
	const TerraEngine::Scene::WorldPosition& focus,
	float distance)
{
	m_CameraFocus = focus;
	m_CameraFocusDistance = std::clamp(distance, 1.0f, 1000.0f);
	m_CameraFocusInitialized = true;
	const TerraEngine::Scene::WorldCoordinateResult position = TerraEngine::Scene::ApplyWorldDelta(
		m_CameraFocus, {{}, -glm::dvec3(GetCameraForward()) * static_cast<double>(m_CameraFocusDistance)});
	if (position.Succeeded())
	{
		m_CameraPosition = position.position;
	}
}

glm::vec3 SceneViewWindow::LocalizeForCamera(const TerraEngine::Scene::WorldPosition& position) const
{
	const TerraEngine::Scene::WorldLocalizationResult localized = TerraEngine::Scene::LocalizeWorldPosition(
		position, m_CameraPosition, TerraEngine::Scene::kTransformSpaceMaximumRelativeRadiusMetres);
	return localized.Succeeded() ? glm::vec3(localized.offsetMetres) : glm::vec3(0.0f);
}

bool SceneViewWindow::TranslateCamera(const glm::dvec3& deltaMetres)
{
	const TerraEngine::Scene::WorldCoordinateResult moved = TerraEngine::Scene::ApplyWorldDelta(
		m_CameraPosition, {{}, deltaMetres});
	if (!moved.Succeeded())
	{
		return false;
	}
	m_CameraPosition = moved.position;
	return true;
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
		m_PerspectiveCameraState.position = m_CameraPosition;
		m_PerspectiveCameraState.rotation = m_CameraRot;
		m_PerspectiveCameraState.focus = m_CameraFocus;
		m_PerspectiveCameraState.focusDistance = m_CameraFocusDistance;
		m_PerspectiveCameraState.fieldOfView = m_Fov;
		m_PerspectiveCameraState.focusInitialized = m_CameraFocusInitialized;
		m_PerspectiveCameraState.valid = true;

		const float fallbackDistance = m_CameraFocusInitialized ? m_CameraFocusDistance : 18.0f;
		const TerraEngine::Scene::WorldPosition focus = ResolveSceneViewFocus(fallbackDistance);
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
		m_CameraPosition = m_PerspectiveCameraState.position;
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
	const TerraEngine::Scene::WorldPosition focus = ResolveSceneViewFocus(fallbackDistance);
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
	const TerraEngine::Scene::WorldPosition focus = ResolveSceneViewFocus(fallbackDistance);
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
	(void)TranslateCamera(glm::dvec3(panWorldDelta));
	if (m_CameraFocusInitialized)
	{
		const TerraEngine::Scene::WorldCoordinateResult movedFocus = TerraEngine::Scene::ApplyWorldDelta(
			m_CameraFocus, {{}, glm::dvec3(panWorldDelta)});
		if (movedFocus.Succeeded()) m_CameraFocus = movedFocus.position;
	}
	else
	{
		const TerraEngine::Scene::WorldCoordinateResult movedFocus = TerraEngine::Scene::ApplyWorldDelta(
			focus, {{}, glm::dvec3(panWorldDelta)});
		m_CameraFocus = movedFocus.Succeeded() ? movedFocus.position : focus;
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
	ImGui::SameLine(0.0f, 2.0f);
	if (DrawOverlayIconButton(
		"scene_animation_overlays", "orbit-ring", m_ShowAnimationOverlays,
		m_ShowAnimationOverlays
			? "Animation Overlays Visible\nClick to hide. Right-click to choose skeleton, socket, root, IK, contact, ragdoll, morph/bounds, or VG coverage kinds."
			: "Animation Overlays Hidden\nClick to show the selected animator. Right-click to choose overlay kinds.",
		buttonSize))
	{
		m_ShowAnimationOverlays = !m_ShowAnimationOverlays;
		m_AnimationOverlayConfigured = false;
	}
	if(ImGui::IsItemClicked(ImGuiMouseButton_Right))
	{
		ImGui::OpenPopup("SceneAnimationOverlayKinds");
	}
	if(ImGui::BeginPopup("SceneAnimationOverlayKinds"))
	{
		ImGui::TextUnformatted("Animation overlay kinds");
		ImGui::Separator();
		auto toggleKind = [&](TerraEngine::Editor::Animation::AnimationOverlayType type, const char* label) {
			const std::uint32_t bit = TerraEngine::Editor::Animation::AnimationOverlayTypeMask(type);
			bool enabled = (m_AnimationOverlayKinds & bit) != 0U;
			if(ImGui::Checkbox(label, &enabled))
			{
				if(enabled) m_AnimationOverlayKinds |= bit;
				else m_AnimationOverlayKinds &= ~bit;
				m_AnimationOverlayConfigured = false;
			}
		};
		toggleKind(TerraEngine::Editor::Animation::AnimationOverlayType::Skeleton, "Skeleton / joints");
		toggleKind(TerraEngine::Editor::Animation::AnimationOverlayType::Socket, "Sockets");
		toggleKind(TerraEngine::Editor::Animation::AnimationOverlayType::RootTrajectory, "Root trajectory");
		toggleKind(TerraEngine::Editor::Animation::AnimationOverlayType::IkTarget, "IK / procedural targets");
		toggleKind(TerraEngine::Editor::Animation::AnimationOverlayType::Contact, "Contacts");
		toggleKind(TerraEngine::Editor::Animation::AnimationOverlayType::Ragdoll, "Ragdoll");
		toggleKind(TerraEngine::Editor::Animation::AnimationOverlayType::MorphBounds, "Morph bounds");
		toggleKind(TerraEngine::Editor::Animation::AnimationOverlayType::SkinnedBounds, "Skinned bounds");
		toggleKind(TerraEngine::Editor::Animation::AnimationOverlayType::VirtualGeometryCoverage, "VG coverage");
		ImGui::EndPopup();
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

	// The scheduler publishes one immutable packet at the protected frame
	// boundary.  The Scene View must only observe that retained packet; draining
	// here would make editor overlays disagree with renderer worker preparation.
	const std::shared_ptr<const TerraEngine::Scene::SceneChangePacket> changePacket =
		objectManager.LoadPublishedSceneChangePacketShared();
	const bool markerFrameChanged = m_MarkerWorkingFrameOrigin != m_CameraPosition;
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
		if(changePacket != nullptr)
		{
			invalidation = m_MarkerPipeline.ApplySceneChanges(*changePacket);
		}
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

	std::vector<TerraEngine::UUID> refreshIds = invalidation.staticRefreshObjectIds;
	refreshIds.insert(refreshIds.end(), invalidation.poseRefreshObjectIds.begin(), invalidation.poseRefreshObjectIds.end());
	sortUnique(refreshIds);
	bool spatialSnapshotDirty = !invalidation.removedObjectIds.empty() || rebuildAll || markerFrameChanged;
	if (markerFrameChanged && !rebuildAll)
	{
		// Camera motion changes only the bounded presentation frame. Re-localize
		// immutable marker poses instead of scanning live Transform components.
		for (auto& [objectId, spatial] : m_MarkerSpatialRecords)
		{
			const SceneOverlay::MarkerPoseRecord* previous = m_MarkerPipeline.FindPoseRecord(objectId);
			if (previous == nullptr)
			{
				continue;
			}
			SceneOverlay::MarkerPoseRecord localized = *previous;
			localized.localPosition = LocalizeForCamera(localized.canonicalPosition);
			localized.localBounds = TerraEngine::Scene::MakeFallbackBounds(localized.localPosition, 0.5f);
			(void)m_MarkerPipeline.UpsertPoseRecord(localized);
			spatial.canonicalOwner = localized.canonicalPosition;
			spatial.localBounds = *localized.localBounds;
		}
	}
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

		const std::optional<TerraEngine::Scene::WorldPose> canonicalPose = objectManager.GetCanonicalWorldPose(*object);
		if (!canonicalPose)
		{
			m_MarkerSpatialRecords.erase(objectId);
			spatialSnapshotDirty = true;
			continue;
		}
		const glm::vec3 localPosition = LocalizeForCamera(canonicalPose->position);
		const TerraEngine::Scene::SpatialAABB bounds = TerraEngine::Scene::MakeFallbackBounds(localPosition, 0.5f);
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
			pose.canonicalPosition = canonicalPose->position;
			pose.localPosition = localPosition;
			pose.localBounds = bounds;
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
		spatial.canonicalOwner = canonicalPose->position;
		spatial.localBounds = bounds;
		// The immutable selectable index is rebuilt as one Scene View frame;
		// its default local reference frame has generation zero by contract.
		spatial.frameGeneration = 0u;
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
	m_MarkerWorkingFrameOrigin = m_CameraPosition;

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
	query.cameraPosition = glm::vec3(0.0f);
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
				TerraEngine::Scene::SpatialSphere{glm::vec3(0.0f), 1000.0f}, filter);
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
			marker.referenceFrameLocalPosition = pose->localPosition;
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
	const TerraEngine::Scene::WorldPosition focus = ResolveSceneViewFocus(fallbackDistance);
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
		view = glm::lookAt(glm::vec3(0.0f), GetCameraForward(), GetCameraUp());
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
	// RenderStats is a completed immutable snapshot, so copying route evidence
	// here cannot expose in-flight policy or Vulkan resource state to the UI.
	const RenderStatsSnapshot sceneStats = RenderSystem::Instance().GetLatestRenderStats(RenderViewType::Scene);
	snapshot.geometryRoute = sceneStats.geometryRoute;
	snapshot.geometryRouteSource = sceneStats.geometryRouteSource;
	snapshot.geometryRoutePolicyIdentity = sceneStats.geometryRoutePolicyIdentity;
	snapshot.geometryRouteCalibrationIdentity = sceneStats.geometryRouteCalibrationIdentity;
	snapshot.geometryRouteHysteresisFrames = sceneStats.geometryRouteHysteresisFrames;
	snapshot.geometryRouteDirectBatchCount = sceneStats.geometryRouteDirectBatchCount;
	snapshot.geometryRouteSharedInstanceCount = sceneStats.geometryRouteSharedInstanceCount;
	snapshot.geometryRouteSuppressedPageRequests = sceneStats.geometryRouteSuppressedPageRequests;
	snapshot.geometryRouteSuppressedDecodeBlocks = sceneStats.geometryRouteSuppressedDecodeBlocks;
	snapshot.geometryRouteSuppressedResidencyRecords = sceneStats.geometryRouteSuppressedResidencyRecords;
	snapshot.geometryRouteFallbackReason = sceneStats.geometryRouteFallbackReason;
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
	if (m_ShowVirtualGeometryDiagnostics && sceneStats.valid && (sceneStats.meshletsTotal > 0u || sceneStats.virtualGeometryClusterPackets > 0u || sceneStats.virtualGeometrySubmittedBatches > 0u || sceneStats.virtualGeometryResidentPagePayloads > 0u || sceneStats.virtualGeometryEmittedHighDetailPages > 0u || sceneStats.virtualGeometryEmittedFallbackPages > 0u || !sceneStats.geometryRoute.empty()))
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
		const std::string routeText = "Geometry Route: " + (sceneStats.geometryRoute.empty() ? "unavailable" : sceneStats.geometryRoute)
			+ " (" + sceneStats.geometryRouteSource + ")";
		const std::string routePolicyText = "Route Policy/Calibration/Hysteresis: "
			+ std::to_string(sceneStats.geometryRoutePolicyIdentity) + "/"
			+ std::to_string(sceneStats.geometryRouteCalibrationIdentity) + "/"
			+ std::to_string(sceneStats.geometryRouteHysteresisFrames);
		const std::string routeSharedText = "Route Direct Batches/Shared Instances: "
			+ std::to_string(sceneStats.geometryRouteDirectBatchCount) + "/"
			+ std::to_string(sceneStats.geometryRouteSharedInstanceCount);
		const std::string routeSuppressionText = "Route Suppressed Page/Decode/Residency: "
			+ std::to_string(sceneStats.geometryRouteSuppressedPageRequests) + "/"
			+ std::to_string(sceneStats.geometryRouteSuppressedDecodeBlocks) + "/"
			+ std::to_string(sceneStats.geometryRouteSuppressedResidencyRecords);
		const std::string routeFallbackText = "Route Fallback: "
			+ (sceneStats.geometryRouteFallbackReason.empty() ? "none" : sceneStats.geometryRouteFallbackReason);
		const char* lines[] = { meshletsConsideredText.c_str(), meshletsSentText.c_str(), meshletsCulledText.c_str(), meshletsCulledHiZText.c_str(), hiZOutcomeText.c_str(), hiZProofText.c_str(), hiZProjectionText.c_str(), packetText.c_str(), batchText.c_str(), gpuPacketText.c_str(), generatedText.c_str(), dispatchText.c_str(), gpuFallbackText.c_str(), compactText.c_str(), rejectedText.c_str(), fallbackText.c_str(), pageSourceText.c_str(), pagePayloadText.c_str(), pageRequestText.c_str(), reuseText.c_str(), hiZBaselineText.c_str(), routeText.c_str(), routePolicyText.c_str(), routeSharedText.c_str(), routeSuppressionText.c_str(), routeFallbackText.c_str() };
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
	view = glm::lookAt(glm::vec3(0.0f), forward, up);
	
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
		view = glm::lookAt(glm::vec3(0.0f), forward, up);
	}

	// Set Scene View Camera for next render after overlay input has had a chance
	// to adjust the editor camera.
	TerraEngine::Scene::ObserverInterest sceneViewInterest;
	sceneViewInterest.observerId = "editor-scene-view";
	sceneViewInterest.kind = TerraEngine::Scene::ObserverInterestKind::Editor;
	sceneViewInterest.position = m_CameraPosition;
	sceneViewInterest.radiusMetres = 1'000.0;
	sceneViewInterest.priority = TerraEngine::Scene::ObserverInterestPriority::Important;
	sceneViewInterest.leaseGeneration = m_MarkerViewGeneration;
	TerraEngine::Scene::WorldStreamingSystem::Instance().SubmitObserverInterest(std::move(sceneViewInterest));
	RenderSystem::Instance().SetSceneViewCamera(view, proj, m_CameraPosition, m_ToolState.projectionMode == SceneOverlay::ProjectionMode::Orthographic);
	ApplyPrefabContextTintForNextRender();

	// Register the transform gizmo before marker hit testing and object picking.
	// Parent objects commonly have an empty-object marker at the exact transform
	// pivot; current-frame ImGuizmo hover must win there so the marker cannot
	// consume the first click intended for a translate/rotate/scale handle.
	DrawGizmos(view, projNoFlip, cursorPos, contentSize);
	DrawAnimationSceneHandles(view, projNoFlip, cursorPos, contentSize);
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
		mesh->runtimeLayout.quantization.positionOffset[0],
		mesh->runtimeLayout.quantization.positionOffset[1],
		mesh->runtimeLayout.quantization.positionOffset[2]
	};
	m_PhysicsShapePreview->localExtents = glm::max(glm::abs(glm::vec3(
		mesh->runtimeLayout.quantization.positionScale[0],
		mesh->runtimeLayout.quantization.positionScale[1],
		mesh->runtimeLayout.quantization.positionScale[2])), glm::vec3(0.001f));
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
	const std::optional<glm::mat4> model = ComputeEntityWorldMatrix(*selectedEntity, m_CameraPosition);
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
		DrawLocalLine(drawList, corners[static_cast<std::size_t>(edge[0])], corners[static_cast<std::size_t>(edge[1])], viewProjection, viewportPos, viewportSize, IM_COL32(255, 206, 72, 235), 1.5f);
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
		|| m_InteractionContext.activeRegion == SceneOverlay::InteractionRegion::Toolbar
		|| m_InteractionContext.activeRegion == SceneOverlay::InteractionRegion::Gizmo
		|| m_InteractionContext.hoveredRegion == SceneOverlay::InteractionRegion::Gizmo)
	{
		return;
	}

	ImVec2 mousePos = ImGui::GetMousePos();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (mousePos.x < viewportPos.x || mousePos.x > viewportPos.x + viewportSize.x ||
		mousePos.y < viewportPos.y || mousePos.y > viewportPos.y + viewportSize.y) return;

	const float localPixelX = std::clamp(mousePos.x - viewportPos.x, 0.0f, viewportSize.x - 1.0f);
	const float localPixelY = std::clamp(mousePos.y - viewportPos.y, 0.0f, viewportSize.y - 1.0f);
	// ImGui mouse coordinates start at the top while Vulkan image copies start
	// at the bottom, so invert Y before requesting the exact attachment texel.
	const glm::uvec2 identityPixel(
		static_cast<std::uint32_t>(localPixelX),
		static_cast<std::uint32_t>(viewportSize.y - 1.0f - localPixelY));
	// A completed identity copy is the only GPU-authoritative selection source.
	// Queue the next non-blocking copy before falling back so a later click can
	// consume a fence-proven result without treating CPU projection as GPU work.
	const std::optional<TerraEngine::Renderer::RenderReferenceFrame> currentReferenceFrame =
		RenderSystem::Instance().GetLatestRenderReferenceFrame(RenderViewType::Scene);
	if (const std::optional<VirtualGeometryVisibilityPick> authoritativePick =
		RenderSystem::Instance().GetLatestAuthoritativeConventionalVisibilityIdentityPick(
			RenderViewType::Scene, identityPixel);
		authoritativePick && currentReferenceFrame
		&& authoritativePick->IsQualifiedFor(*currentReferenceFrame))
	{
		if (const std::optional<Object> pickedObject =
			ObjectManager::instance().getObject(authoritativePick->objectId))
		{
			m_Owner->SetSelectedEntity(pickedObject->EntityId());
			return;
		}
	}
	RenderSystem::Instance().RequestConventionalVisibilityIdentityPick(
		RenderViewType::Scene, identityPixel);
	// CPU-projected packet selection cannot carry the renderer's origin, spatial,
	// and residency identities. Do not promote it to a selectable result: only a
	// qualified GPU readback proves that this click targets the current frame.
	m_Owner->SetSelectedEntity(std::nullopt);
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
			if (ImGui::IsKeyDown(ImGuiKey_W)) (void)TranslateCamera(glm::dvec3(forward * m_CameraSpeed * deltaTime));
			if (ImGui::IsKeyDown(ImGuiKey_S)) (void)TranslateCamera(glm::dvec3(-forward * m_CameraSpeed * deltaTime));
			if (ImGui::IsKeyDown(ImGuiKey_D)) (void)TranslateCamera(glm::dvec3(right * m_CameraSpeed * deltaTime));
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::IsKeyDown(ImGuiKey_A)) (void)TranslateCamera(glm::dvec3(-right * m_CameraSpeed * deltaTime));
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::IsKeyDown(ImGuiKey_E)) (void)TranslateCamera(glm::dvec3(up * m_CameraSpeed * deltaTime));
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::IsKeyDown(ImGuiKey_Q)) (void)TranslateCamera(glm::dvec3(-up * m_CameraSpeed * deltaTime));

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

void SceneViewWindow::DrawAnimationSceneHandles(
	const glm::mat4& view,
	const glm::mat4& proj,
	const ImVec2& viewportPos,
	const ImVec2& viewportSize)
{
	using TerraEngine::Editor::Animation::AnimationHandleKind;
	using TerraEngine::Editor::Animation::AnimationOverlayPrimitive;
	using TerraEngine::Editor::Animation::AnimationOverlayStatus;
	using TerraEngine::Editor::Animation::AnimationOverlayType;
	using TerraEngine::Editor::Animation::AnimationSceneHandle;
	using TerraEngine::Animation::AnimationStatus;

	m_AnimationSceneHandles.SetRendererAvailable(true);
	if(!m_ShowAnimationOverlays || m_Owner == nullptr)
	{
		(void)m_AnimationSceneHandles.Set({});
		m_AnimationSceneHandles.ClearSelection();
		m_SelectedAnimationGoalId = 0U;
		return;
	}
	if(m_AnimationSceneHandles.ActiveEdit() != nullptr)
	{
		// The detached procedural target owns ImGuizmo until Apply or Cancel so an
		// object transform cannot be changed by the same drag.
		return;
	}

	const std::optional<entt::entity> selectedEntity = m_Owner->GetSelectedEntity();
	const std::optional<Object> selectedObject = selectedEntity
		? ObjectManager::instance().getObject(*selectedEntity) : std::nullopt;
	if(!selectedObject)
	{
		(void)m_AnimationSceneHandles.Set({});
		m_AnimationHandleStatus = "Select an object with a published Animator pose.";
		return;
	}

	const TerraEngine::UUID objectId = ObjectManager::instance().getUUID(*selectedObject);
	const auto animator = TerraEngine::Animation::AnimationSystem::Instance()
		.ResolveAnimatorForObject(objectId);
	const std::shared_ptr<const TerraEngine::Animation::AnimationAuthorityFrame> authority =
		TerraEngine::Animation::AnimationSystem::Instance().LoadPublishedAuthorityFrame();
	const TerraEngine::Animation::AnimationAuthorityRow* published = authority
		? authority->Find(animator) : nullptr;
	if(!published || !published->snapshot || !published->snapshot->pose
		|| !published->snapshot->pose->IsSealed())
	{
		(void)m_AnimationSceneHandles.Set({});
		m_AnimationHandleStatus = "The selected Animator has no complete immutable pose publication.";
		return;
	}

	if(!m_AnimationOverlayConfigured)
	{
		TerraEngine::Editor::Animation::AnimationOverlayOptions options;
		options.enabled = true;
		options.expanded = true;
		options.rendererAvailable = true;
		options.enabledKinds = m_AnimationOverlayKinds;
		options.maximumPrimitives =
			TerraEngine::Editor::Animation::kMaximumAnimationOverlayPrimitives;
		m_AnimationOverlays.Configure(options);
		m_AnimationOverlayConfigured = true;
	}

	const glm::mat4 ownerWorld = published->sourceObjectWorldTransformCaptured
		? AnimationTransformMatrix(published->sourceObjectWorldTransform)
		: glm::mat4(1.0F);
	const std::span<const TerraEngine::Animation::AnimationTransform> modelTransforms =
		published->snapshot->pose->ModelTransforms();
	const std::size_t jointCount = std::min(
		modelTransforms.size(), published->jointStableIds.size());

	// Resolve stable skeleton parent IDs through a checked copied publication.
	// This work is opt-in and selected-object-only; no cache-owned pointer escapes.
	TerraEngine::Animation::SkeletonAsset skeleton;
	TerraEngine::Animation::AnimationGeneration skeletonGeneration = 0U;
	const bool copiedSkeleton = ResourceManager::Instance().CopySkeletonPublication(
		published->skeleton.assetId.value, skeleton, skeletonGeneration)
		&& skeletonGeneration == published->skeleton.generation;
	std::unordered_map<std::uint64_t, std::size_t> publishedJointIndices;
	publishedJointIndices.reserve(jointCount);
	for(std::size_t index = 0U; index < jointCount; ++index)
	{
		publishedJointIndices.emplace(published->jointStableIds[index], index);
	}
	std::unordered_map<std::uint64_t, const TerraEngine::Animation::SkeletonJoint*> skeletonJoints;
	if(copiedSkeleton)
	{
		skeletonJoints.reserve(skeleton.joints.size());
		for(const TerraEngine::Animation::SkeletonJoint& joint : skeleton.joints)
		{
			skeletonJoints.emplace(joint.stableId, &joint);
		}
	}

	std::vector<NativeAnimationRagdollSnapshot> ragdollSnapshots;
	std::uint32_t ragdollCount = 0U;
	AnimationStatus ragdollStatus = TerraEngine::Animation::AnimationSystem::Instance()
		.CopyRagdollSnapshots(published->key.Handle(), {}, ragdollCount);
	if(ragdollStatus == AnimationStatus::BufferTooSmall && ragdollCount > 0U)
	{
		ragdollSnapshots.resize(ragdollCount);
		ragdollStatus = TerraEngine::Animation::AnimationSystem::Instance()
			.CopyRagdollSnapshots(published->key.Handle(), ragdollSnapshots, ragdollCount);
	}
	const bool ragdollActive = ragdollStatus == AnimationStatus::Ok
		&& std::any_of(ragdollSnapshots.begin(), ragdollSnapshots.end(),
			[](const NativeAnimationRagdollSnapshot& snapshot) {
				return snapshot.blendWeight > 0.0F;
			});
	const float ragdollWeight = ragdollActive
		? std::clamp(ragdollSnapshots.front().blendWeight, 0.0F, 1.0F) : 0.0F;

	std::vector<AnimationSceneHandle> handles;
	handles.reserve(std::min<std::size_t>(
		jointCount + skeleton.sockets.size() + 32U,
		TerraEngine::Editor::Animation::kMaximumAnimationSceneHandles));
	std::vector<glm::vec3> jointWorldPositions(jointCount);
	glm::vec3 jointBoundsMinimum(std::numeric_limits<float>::max());
	glm::vec3 jointBoundsMaximum(std::numeric_limits<float>::lowest());
	for(std::size_t index = 0U; index < jointCount; ++index)
	{
		const glm::mat4 world = ownerWorld * AnimationTransformMatrix(modelTransforms[index]);
		jointWorldPositions[index] = glm::vec3(world[3]);
		jointBoundsMinimum = glm::min(jointBoundsMinimum, jointWorldPositions[index]);
		jointBoundsMaximum = glm::max(jointBoundsMaximum, jointWorldPositions[index]);
	}
	std::size_t activeMorphCount = 0U;
	for(const TerraEngine::Animation::AnimationNamedFloatValue& morph
		: published->snapshot->pose->MorphValues())
	{
		if(std::abs(morph.value) > 0.0001F) ++activeMorphCount;
	}
	const std::vector<TerraEngine::Renderer::OcclusionDebugSample> virtualGeometrySamples =
		RenderSystem::Instance().GetLatestVirtualGeometryOcclusionDebugSamples(
			RenderViewType::Scene,
			TerraEngine::Renderer::kVirtualGeometryOcclusionDebugSampleCapacity);
	const std::size_t selectedVirtualGeometrySamples = static_cast<std::size_t>(
		std::count_if(virtualGeometrySamples.begin(), virtualGeometrySamples.end(),
			[&objectId](const TerraEngine::Renderer::OcclusionDebugSample& sample) {
				return sample.objectId == objectId;
			}));
	const std::uint64_t objectOverlayId = std::max<std::uint64_t>(
		1U, std::hash<std::string>{}(TerraEngine::ToHexString(objectId)));

	for(std::size_t index = 0U; index < jointCount && handles.size()
		< TerraEngine::Editor::Animation::kMaximumAnimationSceneHandles; ++index)
	{
		AnimationSceneHandle handle;
		handle.kind = ragdollActive ? AnimationHandleKind::RagdollBody : AnimationHandleKind::Joint;
		handle.ownerId = published->jointStableIds[index];
		handle.x = jointWorldPositions[index].x;
		handle.y = jointWorldPositions[index].y;
		handle.z = jointWorldPositions[index].z;
		handle.weight = ragdollActive ? ragdollWeight : 1.0F;
		handle.sourceGeneration = published->componentGeneration;
		handle.label = "Joint " + std::to_string(handle.ownerId);

		const auto skeletonJoint = skeletonJoints.find(handle.ownerId);
		if(skeletonJoint != skeletonJoints.end())
		{
			if(!skeletonJoint->second->displayName.empty())
			{
				handle.label = skeletonJoint->second->displayName;
			}
			const std::int32_t parentIndex = skeletonJoint->second->parentIndex;
			if(parentIndex >= 0 && static_cast<std::size_t>(parentIndex) < skeleton.joints.size())
			{
				const std::uint64_t parentId = skeleton.joints[static_cast<std::size_t>(parentIndex)].stableId;
				const auto publishedParent = publishedJointIndices.find(parentId);
				if(publishedParent != publishedJointIndices.end())
				{
					const glm::vec3 parent = jointWorldPositions[publishedParent->second];
					handle.endX = parent.x;
					handle.endY = parent.y;
					handle.endZ = parent.z;
					handle.hasEndPoint = true;
				}
			}
		}
		handles.push_back(std::move(handle));
	}

	TerraEngine::Animation::AnimationSocketReadSnapshot sockets;
	if(TerraEngine::Animation::AnimationAttachmentSnapshotPublisher::Instance()
		.CopySocketSnapshot(published->key.Handle(), sockets))
	{
		for(const TerraEngine::Animation::AnimationAttachmentTransformSnapshot& socket : sockets.sockets)
		{
			if(handles.size() == TerraEngine::Editor::Animation::kMaximumAnimationSceneHandles) break;
			const glm::mat4 socketWorld = ownerWorld * AnimationTransformMatrix(socket.modelTransform);
			AnimationSceneHandle handle;
			handle.kind = AnimationHandleKind::Socket;
			handle.ownerId = socket.stableId;
			handle.x = socketWorld[3].x;
			handle.y = socketWorld[3].y;
			handle.z = socketWorld[3].z;
			handle.sourceGeneration = published->componentGeneration;
			handle.label = "Socket " + std::to_string(socket.stableId);
			handles.push_back(std::move(handle));
		}
	}

	std::vector<NativeAnimationProceduralGoalCommand> goals;
	std::uint32_t goalCount = 0U;
	AnimationStatus goalStatus = TerraEngine::Animation::AnimationSystem::Instance()
		.CopyProceduralGoals(published->key.Handle(), {}, goalCount);
	if(goalStatus == AnimationStatus::BufferTooSmall && goalCount > 0U)
	{
		goals.resize(goalCount);
		goalStatus = TerraEngine::Animation::AnimationSystem::Instance()
			.CopyProceduralGoals(published->key.Handle(), goals, goalCount);
	}
	if(goalStatus == AnimationStatus::Ok)
	{
		for(const NativeAnimationProceduralGoalCommand& goal : goals)
		{
			if(goal.command != NativeAnimationProceduralCommandKind::SetGoal
				|| handles.size() == TerraEngine::Editor::Animation::kMaximumAnimationSceneHandles) continue;
			const bool worldSpace = goal.coordinateSpace
				== NativeAnimationProceduralCoordinateSpace::World;
			const bool modelSpace = goal.coordinateSpace
				== NativeAnimationProceduralCoordinateSpace::Model;
			const glm::mat4 goalMatrix = NativeAnimationTransformMatrix(goal.goalTransform);
			const glm::mat4 worldGoal = worldSpace ? goalMatrix : ownerWorld * goalMatrix;
			AnimationSceneHandle handle;
			handle.kind = goal.goalKind == NativeAnimationProceduralGoalKind::FootPlant
				? AnimationHandleKind::Contact : AnimationHandleKind::IkGoal;
			handle.ownerId = goal.goalId;
			handle.x = worldGoal[3].x;
			handle.y = worldGoal[3].y;
			handle.z = worldGoal[3].z;
			handle.rotationX = goal.goalTransform.rotation.x;
			handle.rotationY = goal.goalTransform.rotation.y;
			handle.rotationZ = goal.goalTransform.rotation.z;
			handle.rotationW = goal.goalTransform.rotation.w;
			handle.weight = std::clamp(std::max(goal.positionWeight, goal.rotationWeight), 0.0F, 1.0F);
			handle.sourceGeneration = published->componentGeneration;
			handle.label = std::string(ProceduralGoalLabel(goal.goalKind)) + " #"
				+ std::to_string(goal.goalId);
			// Parent/local goal positions need the evaluated parent snapshot to be
			// invertible. They remain visible but read-only rather than guessing a
			// write-back space from the live hierarchy.
			handle.editable = worldSpace || modelSpace;
			handles.push_back(std::move(handle));
		}
	}

	if(const auto& rootMotion = published->snapshot->pose->RootMotionDelta(); rootMotion)
	{
		AnimationSceneHandle handle;
		handle.kind = AnimationHandleKind::RootMotion;
		handle.ownerId = std::max<std::uint64_t>(
			1U, std::hash<std::string>{}(TerraEngine::ToHexString(objectId)));
		handle.x = ownerWorld[3].x;
		handle.y = ownerWorld[3].y;
		handle.z = ownerWorld[3].z;
		handle.endX = handle.x + rootMotion->worldTranslation.x;
		handle.endY = handle.y + rootMotion->worldTranslation.y;
		handle.endZ = handle.z + rootMotion->worldTranslation.z;
		handle.hasEndPoint = true;
		handle.sourceGeneration = published->componentGeneration;
		handle.label = "Root motion delta";
		if(handles.size() < TerraEngine::Editor::Animation::kMaximumAnimationSceneHandles)
		{
			handles.push_back(std::move(handle));
		}
	}

	if(!m_AnimationSceneHandles.Set(handles))
	{
		m_AnimationHandleStatus = "Animation overlay collection exceeded a bound or contained non-finite data.";
		return;
	}

	TerraEngine::Editor::Animation::AnimationOverlayPreparedPacket prepared;
	const std::uint64_t sourceGeneration = std::max<std::uint64_t>(
		1U, published->authorityPublicationGeneration);
	const AnimationOverlayStatus prepareStatus = m_AnimationOverlays.Prepare(
		{sourceGeneration, 1U},
		[&](std::uint32_t enabledKinds, std::size_t maximum,
			std::vector<AnimationOverlayPrimitive>& output) {
			output.clear();
			output.reserve(std::min(maximum,
				m_AnimationSceneHandles.Snapshot().size() + 3U));
			for(const AnimationSceneHandle& handle : m_AnimationSceneHandles.Snapshot())
			{
				if(output.size() == maximum) break;
				AnimationOverlayPrimitive primitive;
				switch(handle.kind)
				{
				case AnimationHandleKind::Joint:
				case AnimationHandleKind::JointLimit:
					primitive.type = AnimationOverlayType::Skeleton;
					primitive.color = {0.25F, 0.72F, 1.0F, 0.95F};
					break;
				case AnimationHandleKind::Socket:
					primitive.type = AnimationOverlayType::Socket;
					primitive.color = {0.20F, 1.0F, 0.92F, 1.0F};
					break;
				case AnimationHandleKind::RootMotion:
					primitive.type = AnimationOverlayType::RootTrajectory;
					primitive.color = {0.35F, 1.0F, 0.35F, 1.0F};
					break;
				case AnimationHandleKind::IkGoal:
					primitive.type = AnimationOverlayType::IkTarget;
					primitive.color = {1.0F, 0.82F, 0.20F, 1.0F};
					break;
				case AnimationHandleKind::Contact:
					primitive.type = AnimationOverlayType::Contact;
					primitive.color = {1.0F, 0.48F, 0.16F, 1.0F};
					break;
				case AnimationHandleKind::RagdollBody:
					primitive.type = AnimationOverlayType::Ragdoll;
					primitive.color = {0.95F, 0.35F, 0.95F, 1.0F};
					break;
				}
				primitive.stableId = handle.ownerId;
				primitive.start = {handle.x, handle.y, handle.z};
				primitive.end = handle.hasEndPoint
					? TerraEngine::Editor::Animation::AnimationOverlayVec3{
						handle.endX, handle.endY, handle.endZ}
					: primitive.start;
				primitive.label = handle.label;
				primitive.accessibilityText = handle.label;
				output.push_back(std::move(primitive));
			}

			// Bounds and coverage records are derived only from immutable pose and
			// renderer diagnostic copies captured above. A diagonal is intentionally
			// used as the smallest bounded primitive understood by the existing Scene
			// View renderer; its label makes the conservative meaning explicit.
			auto appendDiagnosticPrimitive = [&](AnimationOverlayType type,
				std::uint64_t identitySalt,
				const glm::vec3& start,
				const glm::vec3& end,
				TerraEngine::Editor::Animation::AnimationOverlayColor color,
				std::string label) {
				if(output.size() == maximum
					|| (enabledKinds & TerraEngine::Editor::Animation::AnimationOverlayTypeMask(type)) == 0U) {
					return;
				}
				AnimationOverlayPrimitive primitive;
				primitive.type = type;
				primitive.stableId = objectOverlayId ^ identitySalt;
				if(primitive.stableId == 0U) primitive.stableId = identitySalt;
				primitive.start = {start.x, start.y, start.z};
				primitive.end = {end.x, end.y, end.z};
				primitive.color = color;
				primitive.label = std::move(label);
				primitive.accessibilityText = std::string(
					TerraEngine::Editor::Animation::ToString(type)) + ": " + primitive.label;
				output.push_back(std::move(primitive));
			};

			if(jointCount > 0U)
			{
				appendDiagnosticPrimitive(AnimationOverlayType::SkinnedBounds,
					0x51A11E01ULL, jointBoundsMinimum, jointBoundsMaximum,
					{0.36F, 0.72F, 1.0F, 0.9F},
					"Conservative published joint-position bounds");
				if(activeMorphCount > 0U)
				{
					appendDiagnosticPrimitive(AnimationOverlayType::MorphBounds,
						0xA40F0001ULL, jointBoundsMinimum, jointBoundsMaximum,
						{1.0F, 0.42F, 0.72F, 0.9F},
						"Current pose envelope with " + std::to_string(activeMorphCount)
							+ " active morph channel(s)");
				}
			}
			if(selectedVirtualGeometrySamples > 0U)
			{
				const glm::vec3 origin(ownerWorld[3]);
				appendDiagnosticPrimitive(AnimationOverlayType::VirtualGeometryCoverage,
					0x760C0001ULL, origin, origin,
					{0.42F, 1.0F, 0.58F, 1.0F},
					std::to_string(selectedVirtualGeometrySamples)
						+ " bounded Scene View VG coverage sample(s)");
			}
			return AnimationOverlayStatus::Ok;
		}, prepared);
	if(prepareStatus == AnimationOverlayStatus::Ok)
	{
		(void)m_AnimationOverlays.Publish(prepared);
	}

	const TerraEngine::Editor::Animation::AnimationOverlaySnapshot overlay =
		m_AnimationOverlays.Snapshot();
	const glm::mat4 viewProjection = proj * view;
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	for(const AnimationOverlayPrimitive& primitive : overlay.primitives)
	{
		const ImU32 color = ImGui::GetColorU32(ImVec4(
			primitive.color.red, primitive.color.green,
			primitive.color.blue, primitive.color.alpha));
		const glm::vec3 start(primitive.start.x, primitive.start.y, primitive.start.z);
		const glm::vec3 end(primitive.end.x, primitive.end.y, primitive.end.z);
		const glm::vec3 primitiveDelta = end - start;
		if(glm::dot(primitiveDelta, primitiveDelta) > 0.0000001F)
		{
			DrawLocalLine(drawList, start, end, viewProjection,
				viewportPos, viewportSize, color, 2.0F);
		}
		ImVec2 screen{};
		if(ProjectLocalPoint(start, viewProjection, viewportPos, viewportSize, screen))
		{
			drawList->AddCircleFilled(screen, primitive.type == AnimationOverlayType::Skeleton
				? 3.0F : 5.0F, color, 16);
			if(primitive.type != AnimationOverlayType::Skeleton)
			{
				drawList->AddText(ImVec2(screen.x + 7.0F, screen.y - 7.0F), color,
					primitive.label.c_str());
			}
		}
	}

	// Select only editable procedural targets. The nearest projected handle wins,
	// and the viewport marks the region as a gizmo so ordinary object picking does
	// not consume the same click.
	const ImVec2 mouse = ImGui::GetMousePos();
	const AnimationSceneHandle* hoveredGoal = nullptr;
	float hoveredDistanceSquared = 100.0F;
	for(const AnimationSceneHandle& handle : m_AnimationSceneHandles.Snapshot())
	{
		if(!handle.editable) continue;
		const AnimationOverlayType overlayType = handle.kind == AnimationHandleKind::Contact
			? AnimationOverlayType::Contact : AnimationOverlayType::IkTarget;
		if((m_AnimationOverlayKinds
			& TerraEngine::Editor::Animation::AnimationOverlayTypeMask(overlayType)) == 0U) continue;
		ImVec2 screen{};
		if(!ProjectLocalPoint(glm::vec3(handle.x, handle.y, handle.z), viewProjection,
			viewportPos, viewportSize, screen)) continue;
		const float dx = screen.x - mouse.x;
		const float dy = screen.y - mouse.y;
		const float distanceSquared = dx * dx + dy * dy;
		if(distanceSquared < hoveredDistanceSquared)
		{
			hoveredDistanceSquared = distanceSquared;
			hoveredGoal = &handle;
		}
	}
	if(hoveredGoal != nullptr)
	{
		m_InteractionContext.hoveredRegion = SceneOverlay::InteractionRegion::Gizmo;
		if(ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsOver())
		{
			if(m_AnimationSceneHandles.Select(hoveredGoal->kind, hoveredGoal->ownerId)
				&& m_AnimationSceneHandles.BeginEdit(hoveredGoal->sourceGeneration))
			{
				m_SelectedAnimationGoalId = hoveredGoal->ownerId;
				m_AnimationHandleStatus = "Detached procedural goal preview; Apply queues the edit.";
			}
		}
	}

	const TerraEngine::Editor::Animation::AnimationSceneHandleEdit* activeEdit =
		m_AnimationSceneHandles.ActiveEdit();
	if(activeEdit == nullptr)
	{
		return;
	}

	glm::mat4 editMatrix = glm::translate(glm::mat4(1.0F), glm::vec3(
		activeEdit->candidate.x, activeEdit->candidate.y, activeEdit->candidate.z));
	if(ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
		ImGuizmo::TRANSLATE, ImGuizmo::WORLD, glm::value_ptr(editMatrix), nullptr,
		m_UsingSnap ? m_SnapValue : nullptr))
	{
		const glm::vec3 translation(editMatrix[3]);
		(void)m_AnimationSceneHandles.UpdateEditPosition(
			translation.x, translation.y, translation.z);
	}

	ImGui::SetNextWindowPos(ImVec2(
		viewportPos.x + viewportSize.x - 330.0F,
		viewportPos.y + 42.0F), ImGuiCond_Always);
	if(ImGui::Begin("Animation Handle Controls##SceneView", nullptr,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse))
	{
		activeEdit = m_AnimationSceneHandles.ActiveEdit();
		if(activeEdit != nullptr)
		{
			ImGui::TextUnformatted(activeEdit->candidate.label.c_str());
			float position[3] = {
				activeEdit->candidate.x, activeEdit->candidate.y, activeEdit->candidate.z};
			if(ImGui::DragFloat3("World position", position, 0.01F))
			{
				(void)m_AnimationSceneHandles.UpdateEditPosition(
					position[0], position[1], position[2]);
			}
			float rotation[4] = {
				activeEdit->candidate.rotationX, activeEdit->candidate.rotationY,
				activeEdit->candidate.rotationZ, activeEdit->candidate.rotationW};
			if(ImGui::DragFloat4("Goal-space rotation", rotation, 0.005F, -1.0F, 1.0F))
			{
				glm::quat normalized(rotation[3], rotation[0], rotation[1], rotation[2]);
				if(glm::dot(normalized, normalized) > 0.000001F)
				{
					normalized = glm::normalize(normalized);
					(void)m_AnimationSceneHandles.UpdateEditRotation(
						normalized.x, normalized.y, normalized.z, normalized.w);
				}
			}
			ImGui::TextWrapped("%s", m_AnimationHandleStatus.c_str());

			if(ImGui::Button("Apply"))
			{
				AnimationSceneHandle committed;
				if(!m_AnimationSceneHandles.CommitEdit(
					published->componentGeneration, committed))
				{
					m_AnimationHandleStatus = "The Animator component generation changed; the edit was rejected.";
				}
				else
				{
					const auto command = std::find_if(goals.begin(), goals.end(),
						[&](const NativeAnimationProceduralGoalCommand& candidate) {
							return candidate.command == NativeAnimationProceduralCommandKind::SetGoal
								&& candidate.goalId == committed.ownerId;
						});
					if(command == goals.end())
					{
						m_AnimationHandleStatus = "The procedural goal retired before Apply.";
					}
					else
					{
						NativeAnimationProceduralGoalCommand update = *command;
						glm::vec3 goalPosition(committed.x, committed.y, committed.z);
						if(update.coordinateSpace != NativeAnimationProceduralCoordinateSpace::World)
						{
							goalPosition = glm::vec3(glm::inverse(ownerWorld)
								* glm::vec4(goalPosition, 1.0F));
						}
						update.goalTransform.translation = {
							goalPosition.x, goalPosition.y, goalPosition.z};
						update.goalTransform.rotation = {
							committed.rotationX, committed.rotationY,
							committed.rotationZ, committed.rotationW};
						++m_AnimationHandleCommandSequence;
						if(m_AnimationHandleCommandSequence == 0U) ++m_AnimationHandleCommandSequence;
						update.producerId = 0x45444954U; // Stable native editor producer identity: EDIT.
						update.producerSequence = m_AnimationHandleCommandSequence;
						update.correlationId = m_AnimationHandleCommandSequence;
						update.idempotencyKey = m_AnimationHandleCommandSequence
							^ (update.goalId * 0x9e3779b97f4a7c15ULL);
						if(update.idempotencyKey == 0U) update.idempotencyKey = 1U;
						update.cancellationToken = m_AnimationHandleCommandSequence;
						update.targetTick = published->snapshot->tick + 1;
						update.expectedPublicationGeneration =
							published->authorityPublicationGeneration;
						update.expectedPoseGeneration =
							published->snapshot->pose->Generation().poseGeneration;
						const NativeAnimationProceduralGoalReceipt receipt =
							TerraEngine::Animation::AnimationSystem::Instance()
								.SubmitProceduralGoal(update);
						m_AnimationHandleStatus = receipt.status == NativeAnimationStatus::Queued
							? "Queued; the new target becomes visible after the animation publication boundary."
							: "The typed procedural command rejected the edit with status "
								+ std::to_string(static_cast<std::uint16_t>(receipt.status)) + ".";
					}
				}
				m_AnimationSceneHandles.ClearSelection();
				m_SelectedAnimationGoalId = 0U;
			}
			ImGui::SameLine();
			if(ImGui::Button("Cancel"))
			{
				m_AnimationSceneHandles.CancelEdit();
				m_AnimationSceneHandles.ClearSelection();
				m_SelectedAnimationGoalId = 0U;
				m_AnimationHandleStatus = "Procedural goal preview cancelled; runtime state was unchanged.";
			}
		}
	}
	ImGui::End();
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
				const std::optional<glm::mat4> localizedModel =
					ComputeEntityLocalWorldMatrix(*selectedEntity, m_CameraPosition);
				if (!localizedModel)
				{
					return;
				}
				glm::mat4 model = *localizedModel;

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
							m_GizmoStartPose = transform->worldPose;
						}

						std::optional<Object> selectedObject = ObjectManager::instance().getObject(*selectedEntity);

						// ImGuizmo edits the bounded camera-local matrix. Reconstruct the
						// canonical address from that bounded displacement before one
						// protected ObjectManager pose commit.
						glm::vec3 scale;
						glm::quat rotation;
						glm::vec3 translation;
						glm::vec3 skew;
						glm::vec4 perspective;
						glm::decompose(model, scale, rotation, translation, skew, perspective);
						if (selectedObject)
						{
							const TerraEngine::Scene::WorldCoordinateResult position =
								TerraEngine::Scene::ApplyWorldDelta(m_CameraPosition, {{}, glm::dvec3(translation)});
							if (position.Succeeded())
							{
								TerraEngine::Scene::WorldPose pose = transform->worldPose;
								pose.position = position.position;
								pose.rotation = glm::normalize(rotation);
								pose.scale = scale;
								(void)ObjectManager::instance().SetCanonicalWorldPose(*selectedObject, pose);
							}
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
						// History records are migrated by the strict automation/history
						// workstream. Do not write a legacy float transform command here.
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
