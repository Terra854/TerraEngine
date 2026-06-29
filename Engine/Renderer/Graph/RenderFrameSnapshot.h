// RenderFrameSnapshot.h - immutable renderer-facing frame data.
//
// Worker render jobs must never read live scene, editor, or resource-manager
// containers. This snapshot is the stable hand-off object: the main thread
// gathers the frame inputs once, then workers consume these value records.

#pragma once

#include "Renderer/RenderTypes.h"
#include "Utilities/UUID.h"
#include "Core/Export.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::Renderer {

	enum class RenderResourceReadinessState
	{
		// Ready resources can be consumed by workers immediately. Other states
		// force the graph to pick a fallback, delay work, or reject the frame
		// before backend submission tries to use an unsafe resource.
		Ready,
		Pending,
		Fallback,
		Failed,
		Retired
	};

	struct RenderFrameViewRequest
	{
		RenderOutputKind outputKind = RenderOutputKind::GameView;
		RenderViewType viewType = RenderViewType::Game;
		glm::uvec2 targetSize = glm::uvec2(0u);
		std::uint32_t outputId = 0u;
	};

	struct VirtualGeometryDrawRange
	{
		std::uint32_t selectedLodIndex = 0u;
		std::uint32_t meshletOffset = 0u;
		std::uint32_t meshletCount = 0u;
		bool virtualGeometryActive = false;
		bool failOpen = false;
		std::string fallbackReason;
		bool hiZSelfOcclusionGuardPassed = false;
	};

	struct RenderFrameRenderableRecord
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
		glm::mat4 modelMatrix = glm::mat4(1.0f);
		VirtualGeometryDrawRange virtualGeometry;
		bool taskShaderCullingEnabled = true;
		bool usingFallback = false;
	};

	struct VirtualGeometryViewDrawRecord
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		VirtualGeometryDrawRange range;
		bool taskShaderCullingEnabled = true;
	};

	struct VirtualGeometryViewSelectionState
	{
		// One state belongs to one camera-like producer. Scene, Game, shadow,
		// and exported-runtime views may see the same object with different LOD,
		// culling, and Hi-Z decisions, so workers read these copied ranges
		// instead of sharing one mutable renderable-side answer.
		RenderOutputKind outputKind = RenderOutputKind::GameView;
		RenderViewType viewType = RenderViewType::Game;
		glm::uvec2 targetSize = glm::uvec2(0u);
		std::uint32_t outputId = 0u;
		bool shadowPass = false;
		bool exportRuntime = false;
		bool hiZOcclusionAllowed = true;
		std::vector<VirtualGeometryViewDrawRecord> draws;
	};

	struct RenderFrameLightRecord
	{
		TerraEngine::UUID lightId = TerraEngine::EMPTY_UUID;
		std::uint32_t lightType = 0u;
		bool castsShadows = false;
		bool usingFallback = false;
	};

	struct RenderFrameResourceRecord
	{
		TerraEngine::UUID resourceId = TerraEngine::EMPTY_UUID;
		std::string debugName;
		bool resident = true;
		bool usingFallback = false;
		bool requiredForFrame = false;
		RenderResourceReadinessState readiness = RenderResourceReadinessState::Ready;
		std::string fallbackReason;
	};

	struct RenderFrameSnapshot
	{
		// RenderSystem builds this value object on the main thread. Worker jobs
		// must read only these copied records, never live scene containers or
		// editor-owned objects that can mutate during the frame.
		std::uint64_t frameId = 0u;
		std::vector<RenderFrameViewRequest> viewRequests;
		std::vector<RenderFrameRenderableRecord> renderables;
		std::vector<RenderFrameLightRecord> lights;
		std::vector<RenderFrameResourceRecord> materials;
		std::vector<RenderFrameResourceRecord> meshes;
		std::vector<RenderFrameResourceRecord> textures;
		std::vector<VirtualGeometryViewSelectionState> virtualGeometryViewSelections;
		RenderSettings renderSettings;
		std::uint64_t resourceRevision = 0u;
		std::uint64_t sceneRevision = 0u;
		bool valid = true;
		// This flag is a defensive tripwire for tests and diagnostics. If a
		// future collector accidentally hands workers live mutable state, graph
		// validation can reject the packet instead of racing the scene.
		bool containsLiveMutableState = false;
	};

	// Builder functions keep construction deterministic. Tests and future
	// backends can create snapshots without needing access to RenderSystem.
	// Deterministic sorting and hashing make worker completion order irrelevant
	// to the final renderer output.
	TERRAENGINE_API void SortSnapshotDeterministically(RenderFrameSnapshot& snapshot);
	TERRAENGINE_API bool SnapshotUsesOnlyResolvedResources(const RenderFrameSnapshot& snapshot);
	TERRAENGINE_API bool SnapshotContainsLiveMutableState(const RenderFrameSnapshot& snapshot);
	TERRAENGINE_API std::uint64_t BuildSnapshotDeterministicHash(const RenderFrameSnapshot& snapshot);
	TERRAENGINE_API std::uint64_t EstimateSnapshotWorkload(const RenderFrameSnapshot& snapshot);
	TERRAENGINE_API VirtualGeometryViewSelectionState BuildVirtualGeometryViewSelectionState(const RenderFrameViewRequest& view, const std::vector<RenderFrameRenderableRecord>& renderables);
	TERRAENGINE_API std::vector<VirtualGeometryViewSelectionState> BuildVirtualGeometryViewSelectionStates(const RenderFrameSnapshot& snapshot);

}
