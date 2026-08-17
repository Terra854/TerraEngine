// TerrainBrushPresetStore.cpp -- Deterministic JSON persistence for brush presets.

#include "TerrainBrushPresetStore.h"

#include "Utilities/UUID.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace TerraEngine::Editor
{
	using json = nlohmann::json;

	bool TerrainBrushPresetStore::Validate(const TerrainBrushPreset& preset)
	{
		return !preset.name.empty() && !preset.tool.empty() && !preset.falloff.empty()
			&& std::isfinite(preset.radiusMetres) && preset.radiusMetres > 0.0f && preset.radiusMetres <= 100000.0f
			&& std::isfinite(preset.strength) && preset.strength >= 0.0f && preset.strength <= 1.0f
			&& std::isfinite(preset.spacing) && preset.spacing > 0.0f && preset.spacing <= 4.0f
			&& std::isfinite(preset.rotationDegrees) && preset.rotationDegrees >= -360.0f && preset.rotationDegrees <= 360.0f;
	}

	bool TerrainBrushPresetStore::Load(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input) return false;
		json document;
		try { input >> document; } catch (...) { return false; }
		if (!document.is_object() || document.value("schemaVersion", 0u) != 1u || !document["presets"].is_array()) return false;

		std::vector<TerrainBrushPreset> loaded;
		for (const auto& value : document["presets"])
		{
			if (!value.is_object()) return false;
			TerrainBrushPreset preset;
			preset.id = value.value("id", "");
			preset.name = value.value("name", "");
			preset.tool = value.value("tool", "raise");
			preset.falloff = value.value("falloff", "smooth");
			preset.maskAsset = value.value("maskAsset", "");
			preset.radiusMetres = value.value("radiusMetres", 10.0f);
			preset.strength = value.value("strength", 1.0f);
			preset.spacing = value.value("spacing", 0.25f);
			preset.rotationDegrees = value.value("rotationDegrees", 0.0f);
			preset.pressureEnabled = value.value("pressureEnabled", true);
			if (preset.id.empty() || !Validate(preset) || std::any_of(loaded.begin(), loaded.end(), [&](const auto& item) { return item.id == preset.id; })) return false;
			loaded.push_back(std::move(preset));
		}
		presets_ = std::move(loaded);
		return true;
	}

	bool TerrainBrushPresetStore::Save(const std::filesystem::path& path) const
	{
		std::vector<TerrainBrushPreset> ordered = presets_;
		std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) { return left.id < right.id; });
		json document{{"schemaVersion", 1u}, {"presets", json::array()}};
		for (const auto& preset : ordered)
		{
			document["presets"].push_back({
				{"id", preset.id}, {"name", preset.name}, {"tool", preset.tool}, {"falloff", preset.falloff},
				{"maskAsset", preset.maskAsset}, {"radiusMetres", preset.radiusMetres}, {"strength", preset.strength},
				{"spacing", preset.spacing}, {"rotationDegrees", preset.rotationDegrees}, {"pressureEnabled", preset.pressureEnabled}
			});
		}
		std::error_code error;
		std::filesystem::create_directories(path.parent_path(), error);
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if (!output) return false;
		output << document.dump(2) << '\n';
		return static_cast<bool>(output);
	}

	bool TerrainBrushPresetStore::Create(TerrainBrushPreset preset, std::string& outId)
	{
		if (!Validate(preset)) return false;
		preset.id = TerraEngine::ToHexString(TerraEngine::GenerateUUID());
		outId = preset.id;
		presets_.push_back(std::move(preset));
		return true;
	}

	bool TerrainBrushPresetStore::Duplicate(std::string_view sourceId, std::string_view newName, std::string& outId)
	{
		const auto source = Get(sourceId);
		if (!source || newName.empty()) return false;
		TerrainBrushPreset copy = *source;
		copy.id.clear();
		copy.name = std::string(newName);
		return Create(std::move(copy), outId);
	}

	bool TerrainBrushPresetStore::Update(std::string_view id, TerrainBrushPreset preset)
	{
		if (id.empty() || !Validate(preset)) return false;
		const auto found = std::find_if(presets_.begin(), presets_.end(), [&](const auto& item) { return item.id == id; });
		if (found == presets_.end()) return false;
		preset.id = std::string(id);
		*found = std::move(preset);
		return true;
	}

	bool TerrainBrushPresetStore::Delete(std::string_view id)
	{
		const auto oldSize = presets_.size();
		presets_.erase(std::remove_if(presets_.begin(), presets_.end(), [&](const auto& item) { return item.id == id; }), presets_.end());
		return presets_.size() != oldSize;
	}

	std::optional<TerrainBrushPreset> TerrainBrushPresetStore::Get(std::string_view id) const
	{
		const auto found = std::find_if(presets_.begin(), presets_.end(), [&](const auto& item) { return item.id == id; });
		return found == presets_.end() ? std::nullopt : std::optional<TerrainBrushPreset>(*found);
	}

	std::vector<TerrainBrushPreset> TerrainBrushPresetStore::List() const
	{
		std::vector<TerrainBrushPreset> result = presets_;
		std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) { return left.id < right.id; });
		return result;
	}

	void TerrainBrushPresetStore::Clear() { presets_.clear(); }
}
