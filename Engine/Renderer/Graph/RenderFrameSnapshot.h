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

	struct RenderFrameViewRequest
	{
		RenderOutputKind outputKind = RenderOutputKind::GameView;
		RenderViewType viewType = RenderViewType::Game;
		glm::uvec2 targetSize = glm::uvec2(0u);
		std::uint32_t outputId = 0u;
	};

	struct RenderFrameRenderableRecord
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
		glm::mat4 modelMatrix = glm::mat4(1.0f);
		bool taskShaderCullingEnabled = true;
		bool usingFallback = false;
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
	};

	struct RenderFrameSnapshot
	{
		std::uint64_t frameId = 0u;
		std::vector<RenderFrameViewRequest> viewRequests;
		std::vector<RenderFrameRenderableRecord> renderables;
		std::vector<RenderFrameLightRecord> lights;
		std::vector<RenderFrameResourceRecord> materials;
		std::vector<RenderFrameResourceRecord> meshes;
		std::vector<RenderFrameResourceRecord> textures;
		RenderSettings renderSettings;
		std::uint64_t resourceRevision = 0u;
		std::uint64_t sceneRevision = 0u;
		bool valid = true;
	};

	// Builder functions keep construction deterministic. Tests and future
	// backends can create snapshots without needing access to RenderSystem.
	TERRAENGINE_API void SortSnapshotDeterministically(RenderFrameSnapshot& snapshot);
	TERRAENGINE_API bool SnapshotUsesOnlyResolvedResources(const RenderFrameSnapshot& snapshot);
	TERRAENGINE_API std::uint64_t EstimateSnapshotWorkload(const RenderFrameSnapshot& snapshot);

}
