#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

namespace TerraEngine::Tests::RendererEfficiency
{
	// Pure preparation: all generated buffers are embedded in project-local glTF.
	// The caller writes and hashes these files before publishing the ownership marker.
	// P1 is the compatibility default; P2 adds shared lighting and P3 adds
	// production-depth proof with 32 actual baseline lights. P4 instead retains five
	// baseline lights and adds real grouping/conservative-route requests. Earlier recipes stay unchanged.
	struct ShadowInvalidationFixture
	{
		nlohmann::json scene;
		nlohmann::json steps;
		std::vector<std::pair<std::filesystem::path, nlohmann::json>> files;
	};
	ShadowInvalidationFixture BuildShadowInvalidationFixture(const nlohmann::json& foundationScene,
		const nlohmann::json& heroGltf, const nlohmann::json& definition,
		std::string_view manifestHash, std::uint64_t seed, std::string_view requestedStage = "P1");
}
