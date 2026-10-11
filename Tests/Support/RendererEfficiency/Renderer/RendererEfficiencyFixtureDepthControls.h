// Pure admission for owned, copied depth recipes. Live scene storage and GPU
// state are deliberately absent; protected capture separately validates cameras.
#pragma once
#include "Renderer/LightingDepthEvidence.h"
#include <algorithm>
#include <span>
#include <string>

namespace TerraEngine::Renderer
{
	inline bool ValidateRendererEfficiencyFixtureDepthControls(
		const std::optional<RendererEfficiencyFixtureDepthControls>& controls,
		bool ownedValidation, std::span<const LightingDepthReduction::Extent> cameras, std::string& reason)
	{
		if (!controls) { reason.clear(); return true; }
		if (!ownedValidation || cameras.empty() || cameras.size() > 2
			|| std::any_of(cameras.begin(),cameras.end(),[](const auto& camera){return !camera.width || !camera.height;}))
		{ reason="Depth controls require owned sealed output cameras."; return false; }
		if (!controls->oracleRecipe) { reason.clear(); return true; }
		const auto& recipe=*controls->oracleRecipe;
		if (!recipe.extent.width || !recipe.extent.height
			|| recipe.extent.width > LightingDepthReduction::kMaximumWidth
			|| recipe.extent.height > LightingDepthReduction::kMaximumHeight)
		{ reason="Oracle dimensions exceed the validation image capacity."; return false; }
		if (std::any_of(cameras.begin(),cameras.end(),[&](const auto& camera){return camera!=recipe.extent;}))
		{ reason="Oracle dimensions differ from an actual sealed output camera."; return false; }
		// Bound sparse work independently of the dense image. All coordinates are
		// validated before the caller may publish controls or prepare GPU bytes.
		if (recipe.overrides.size() > 1024 || recipe.overrides.size() > std::uint64_t(recipe.extent.width)*recipe.extent.height)
		{ reason="Oracle sparse overrides exceed the bounded admission capacity."; return false; }
		std::vector<std::uint64_t> coordinates; coordinates.reserve(recipe.overrides.size());
		for (const auto& pixel:recipe.overrides) {
			if (pixel.x>=recipe.extent.width || pixel.y>=recipe.extent.height)
			{ reason="Oracle override lies outside its actual image."; return false; }
			coordinates.push_back(std::uint64_t(pixel.y)*recipe.extent.width+pixel.x);
		}
		std::sort(coordinates.begin(),coordinates.end());
		if (std::adjacent_find(coordinates.begin(),coordinates.end())!=coordinates.end())
		{ reason="Oracle recipe repeats a pixel coordinate."; return false; }
		reason.clear(); return true;
	}
}
