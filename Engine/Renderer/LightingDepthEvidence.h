// Immutable validation controls and copied native lighting-depth receipts.
// These declarations create no GPU proof: the native collector fills receipts
// only after the real command, submission, query read and fence have completed.
#pragma once
#include "Renderer/LightingDepthReduction.h"
#include "Renderer/RHI/Vulkan/VulkanLightingDepthTiming.h"
#include <optional>
#include <vector>

namespace TerraEngine::Renderer
{
	struct RendererEfficiencyFixtureDepthControls
	{
		// Ordinary production keeps its real scene depth. Only an owned validation
		// request can provide an isolated R32 oracle image from this typed recipe.
		std::optional<LightingDepthReduction::DepthRecipe> oracleRecipe;
		bool operator==(const RendererEfficiencyFixtureDepthControls&) const = default;
	};
	struct RendererEfficiencyDepthQueryEvidence
	{
		VulkanLightingDepthTiming::Result result{};
		VulkanLightingDepthTiming::QueryRange range{};
		bool cooperative = false;
		// Both flags describe calls actually made by the native collector. A
		// whole-view profiler sample cannot stand in for these isolated markers.
		bool computeStageMarkers = false, noWaitRead = false;
		// Numeric values are captured from the executed Vulkan calls. Exporters
		// can independently reject WAIT, partial results or whole-view stages.
		std::uint32_t queryReadFlags = 0, beginStageMask = 0, endStageMask = 0;
	};
	struct RendererEfficiencyLightingDepthArtifacts
	{
		RendererEfficiencyDepthQueryEvidence productionQuery{};
		std::optional<RendererEfficiencyDepthQueryEvidence> oracleQuery;
		std::uint32_t productionWidth = 0, productionHeight = 0;
		// Bounds are the complete 144 float2 records copied from the actual
		// production buffer. Their nonempty values qualify real stage work.
		std::vector<std::uint8_t> productionBounds;
		// Oracle payloads are copies of actual GPU image/buffer contents, never
		// CPU-generated expected results. Expected values are compared later.
		std::optional<LightingDepthReduction::DepthRecipe> oracleRecipe;
		std::vector<std::uint8_t> oracleInput, oracleBounds, oracleCoverage;
	};
}
