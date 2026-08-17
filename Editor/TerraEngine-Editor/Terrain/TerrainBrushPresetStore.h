// TerrainBrushPresetStore.h -- Durable editor brush presets.
//
// Presets describe tool settings only. They are editor preferences and are
// deliberately persisted independently from terrain edit history, so changing
// a preset never creates an authored terrain transaction or dirty asset state.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace TerraEngine::Editor
{
	struct TerrainBrushPreset
	{
		std::string id;
		std::string name;
		std::string tool = "raise";
		std::string falloff = "smooth";
		std::string maskAsset;
		float radiusMetres = 10.0f;
		float strength = 1.0f;
		float spacing = 0.25f;
		float rotationDegrees = 0.0f;
		bool pressureEnabled = true;
	};

	class TerrainBrushPresetStore
	{
	public:
		[[nodiscard]] bool Load(const std::filesystem::path& path);
		[[nodiscard]] bool Save(const std::filesystem::path& path) const;

		[[nodiscard]] bool Create(TerrainBrushPreset preset, std::string& outId);
		[[nodiscard]] bool Duplicate(std::string_view sourceId, std::string_view newName, std::string& outId);
		[[nodiscard]] bool Update(std::string_view id, TerrainBrushPreset preset);
		[[nodiscard]] bool Delete(std::string_view id);
		[[nodiscard]] std::optional<TerrainBrushPreset> Get(std::string_view id) const;
		[[nodiscard]] std::vector<TerrainBrushPreset> List() const;
		void Clear();

	private:
		[[nodiscard]] static bool Validate(const TerrainBrushPreset& preset);
		std::vector<TerrainBrushPreset> presets_;
	};
}
