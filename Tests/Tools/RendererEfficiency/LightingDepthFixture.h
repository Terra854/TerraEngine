#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

namespace TerraEngine::Tests::RendererEfficiency
{
	// Detached authored inputs only. The installed materializer writes and hashes
	// these files; cooking, protected transactions and GPU proof remain mandatory.
	struct LightingDepthFixture
	{
		nlohmann::json scene;
		nlohmann::json steps;
		std::vector<std::pair<std::filesystem::path, nlohmann::json>> files;
	};
	LightingDepthFixture BuildLightingDepthFixture(const nlohmann::json& foundationScene,
		const nlohmann::json& definition, std::string_view manifestHash, std::uint64_t seed);
}
