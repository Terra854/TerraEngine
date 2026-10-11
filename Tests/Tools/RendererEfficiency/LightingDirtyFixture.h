#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

namespace TerraEngine::Tests::RendererEfficiency
{
	// Detached preparation only: the caller writes and hashes these values before
	// publishing its owned project marker. No live scene or renderer is accessed.
	struct LightingDirtyFixture
	{
		nlohmann::json scene;
		nlohmann::json steps;
		std::vector<std::pair<std::filesystem::path, nlohmann::json>> files;
	};
	LightingDirtyFixture BuildLightingDirtyFixture(const nlohmann::json& foundationScene,
		const nlohmann::json& definition, std::string_view manifestHash, std::uint64_t seed);
}
