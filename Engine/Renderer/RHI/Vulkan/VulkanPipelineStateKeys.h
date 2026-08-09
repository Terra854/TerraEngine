// VulkanPipelineStateKeys.h - deterministic state keys for worker prep.

#pragma once

#include "Renderer/Graph/RenderFramePublication.h"
#include "Resource/MaterialRuntimeContract.h"
#include "Core/Export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::Renderer::Vulkan {

	struct PipelineStateKey
	{
		std::uint64_t sortKey = 0u;
		std::uint64_t compatibilitySignature = 0u;
		std::uint32_t shadingBin = 0u;
		// The identity fragment variant has a second typed output, so it must
		// remain distinct from target0-only shadow/default pipeline state.
		bool usesConventionalVisibilityIdentity = false;
		bool usesIndexedFallback = true;
		std::string label;
	};

	// Validation happens before worker pipeline preparation. A malformed material
	// signature therefore selects the bounded indexed fallback instead of making
	// an unbounded or incompatible programmable key.
	TERRAENGINE_API PipelineStateKey BuildPipelineStateKey(
		const TerraEngine::UUID& materialId,
		const TerraEngine::UUID& meshId,
		const MaterialCapabilitySignature& capability,
		bool usesConventionalVisibilityIdentity = false);
	TERRAENGINE_API std::vector<PipelineStateKey> BuildPipelineStateKeys(
		const RenderFramePublication& publication);

}
