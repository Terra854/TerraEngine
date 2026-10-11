#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>
namespace TerraEngine::Tests::RendererEfficiency
{
	// Detached authoring only: cooking and typed protected transactions supply
	// real resource owners before any batching evidence may be accepted.
	struct ShadowBatchingFixture
	{
		nlohmann::json scene, steps;
		std::vector<std::pair<std::filesystem::path,nlohmann::json>> files;
	};
	ShadowBatchingFixture BuildShadowBatchingFixture(const nlohmann::json& foundation,
		const nlohmann::json& hero, const nlohmann::json& definition,
		std::string_view manifestHash, std::uint64_t seed);
}
