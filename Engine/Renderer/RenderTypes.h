#pragma once

#include <array>
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include "Utilities/UUID.h"

enum class RenderViewType : std::uint8_t
{
	Game = 0,
	Scene = 1
};

struct DrawCommand
{
	TerraEngine::UUID meshId;
	TerraEngine::UUID materialId;
	glm::mat4 modelMatrix;
	bool taskShaderCullingEnabled = true;
};

struct FrameRenderData
{
	glm::mat4 viewMatrix;
	glm::mat4 projectionMatrix;
	glm::vec3 cameraWorldPosition = glm::vec3(0.0f);
	float deltaTimeSeconds = 0.0f;
	bool globalTaskShaderCullingEnabled = true;
	float cullingEpsilon = 0.001f;
	std::vector<DrawCommand> commands;
};

// GPU-facing frustum/camera payload consumed by the task shader path.
struct alignas(16) FrameCullingDataGPU
{
	std::array<glm::vec4, 6> frustumPlanes{};
	glm::vec4 cameraPositionAndEpsilon = glm::vec4(0.0f, 0.0f, 0.0f, 0.001f);
	std::uint32_t globalTaskCullingEnabled = 1u;
	std::uint32_t taskShaderSupported = 0u;
	std::uint32_t reserved0 = 0u;
	std::uint32_t reserved1 = 0u;
};
static_assert(sizeof(FrameCullingDataGPU) == 128, "Frame culling payload must stay tightly packed for GPU uploads.");

// GPU counters written by task shader atomic operations per view/frame slot.
struct alignas(16) MeshletCullingCountersGPU
{
	std::uint32_t meshletsTotal = 0u;
	std::uint32_t meshletsDispatched = 0u;
	std::uint32_t meshletsCulled = 0u;
	std::uint32_t reserved = 0u;
};
static_assert(sizeof(MeshletCullingCountersGPU) == 16, "Meshlet counter payload must keep a stable 16-byte layout.");

// UI-facing immutable snapshot returned by the renderer. The renderer owns
// writes while editor windows read copies, so no caller mutates shared state.
struct RenderStatsSnapshot
{
	RenderViewType viewType = RenderViewType::Game;
	float frameTimeMs = 0.0f;
	float fps = 0.0f;
	std::uint32_t drawCalls = 0u;
	std::uint32_t triangleCount = 0u;
	std::uint32_t meshletsTotal = 0u;
	std::uint32_t meshletsDispatched = 0u;
	std::uint32_t meshletsCulled = 0u;
	bool taskCullingActive = false;
	bool valid = false;
};
