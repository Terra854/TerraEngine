// VirtualGeometryBuildRecipe.h - Canonical identities for resumable cooks.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace TerraEngine::Editor
{
	// Every input that can change cooked bytes participates in the recipe.
	// Keeping these facts explicit prevents timestamp-only cache hits.
	struct VirtualGeometryBuildRecipe
	{
		std::string sourceHash;
		std::string topologyHash;
		std::string attributeHash;
		std::string settingsHash;
		std::string importerVersion;
		std::string compilerVersion;
		std::string toolchainVersion;
		std::string codecProfile;
		std::string quantizationProfile = "Balanced";
		std::uint32_t quantizationProfileVersion = 1u;
		std::string quantizationSettingsHash = "balanced-production-v1";
		std::uint32_t pageSizeBytes = 64u * 1024u;

		[[nodiscard]] bool Complete() const noexcept;
		[[nodiscard]] std::string Key() const;
		[[nodiscard]] std::string StageKey(std::string_view stage) const;
	};

	// A stage cache record is valid only after its private output has been
	// verified. cacheHit and hitReason are provenance, not proof of validity.
	struct VirtualGeometryStageCacheRecord
	{
		std::string stage;
		std::string inputKey;
		std::string outputHash;
		std::string hitReason;
		bool valid = false;
		bool cacheHit = false;

		[[nodiscard]] bool Matches(std::string_view expectedStage, std::string_view expectedInputKey) const noexcept;
	};
} // namespace TerraEngine::Editor
