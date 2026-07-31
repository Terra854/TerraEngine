#include "VirtualGeometryBuildRecipe.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace TerraEngine::Editor
{
	namespace
	{
		std::string NormalizeToken(std::string_view input)
		{
			std::string normalized;
			normalized.reserve(input.size());
			for (const unsigned char character : input)
			{
				if (!std::isspace(character))
				{
					normalized.push_back(static_cast<char>(std::tolower(character)));
				}
			}
			return normalized;
		}

		void HashToken(std::uint64_t &hash, std::string_view label, std::string_view value)
		{
			// Length-delimited fields avoid ambiguous concatenations such as
			// ("ab", "c") and ("a", "bc").
			const std::string normalized = NormalizeToken(value);
			const std::string field = std::string(label) + ":" + std::to_string(normalized.size()) + ":" + normalized + ";";
			for (const unsigned char byte : field)
			{
				hash = (hash ^ byte) * 1099511628211ull;
			}
		}

		std::string HexHash(std::uint64_t hash)
		{
			std::ostringstream output;
			output << std::hex << std::setfill('0') << std::setw(16) << hash;
			return output.str();
		}
	} // namespace

	bool VirtualGeometryBuildRecipe::Complete() const noexcept
	{
		return !sourceHash.empty() && !topologyHash.empty() && !attributeHash.empty() && !settingsHash.empty() && !importerVersion.empty() &&
			   !compilerVersion.empty() && !toolchainVersion.empty() && !codecProfile.empty() && !quantizationProfile.empty()
			   && quantizationProfileVersion != 0u && !quantizationSettingsHash.empty()
			   && (pageSizeBytes == 32u * 1024u || pageSizeBytes == 64u * 1024u || pageSizeBytes == 128u * 1024u);
	}

	std::string VirtualGeometryBuildRecipe::Key() const
	{
		if (!Complete())
		{
			return {};
		}

		std::uint64_t hash = 1469598103934665603ull;
		HashToken(hash, "source", sourceHash);
		HashToken(hash, "topology", topologyHash);
		HashToken(hash, "attributes", attributeHash);
		HashToken(hash, "settings", settingsHash);
		HashToken(hash, "importer", importerVersion);
		HashToken(hash, "compiler", compilerVersion);
		HashToken(hash, "toolchain", toolchainVersion);
		HashToken(hash, "codec", codecProfile);
		HashToken(hash, "quantization-profile", quantizationProfile);
		HashToken(hash, "quantization-version", std::to_string(quantizationProfileVersion));
		HashToken(hash, "quantization-settings", quantizationSettingsHash);
		HashToken(hash, "page-size", std::to_string(pageSizeBytes));
		return "vg-recipe-v1-" + HexHash(hash);
	}

	std::string VirtualGeometryBuildRecipe::StageKey(std::string_view stage) const
	{
		const std::string recipeKey = Key();
		if (recipeKey.empty() || stage.empty())
		{
			return {};
		}

		std::uint64_t hash = 1469598103934665603ull;
		HashToken(hash, "stage", stage);
		HashToken(hash, "recipe", recipeKey);
		return "vg-stage-v1-" + HexHash(hash);
	}

	bool VirtualGeometryStageCacheRecord::Matches(std::string_view expectedStage, std::string_view expectedInputKey) const noexcept
	{
		return valid && !outputHash.empty() && stage == expectedStage && inputKey == expectedInputKey;
	}
} // namespace TerraEngine::Editor
