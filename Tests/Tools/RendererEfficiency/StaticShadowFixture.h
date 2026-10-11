#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>
namespace TerraEngine::Tests::RendererEfficiency
{
	// Detached authoring only. Actual imports, protected transactions and native
	// completed frames remain mandatory before this definition becomes evidence.
	struct StaticShadowFixture
	{
		nlohmann::json scene, steps;
		std::vector<std::pair<std::filesystem::path,nlohmann::json>> files;
	};
	StaticShadowFixture BuildStaticShadowFixture(const nlohmann::json& foundation,
		const nlohmann::json& hero, const nlohmann::json& definition,
		std::string_view manifestHash, std::uint64_t seed);
}
