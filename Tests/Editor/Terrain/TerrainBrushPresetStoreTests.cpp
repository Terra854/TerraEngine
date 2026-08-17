// TerrainBrushPresetStoreTests.cpp -- Persistence and lifecycle coverage.

#include "TestHarness.h"
#include "TerrainBrushPresetStore.h"

#include <filesystem>

namespace TerraEngine::EditorTests
{
	void RegisterTerrainBrushPresetStoreTests(TestRegistry& registry)
	{
		registry.Add("Terrain.CreateSculpt.BrushPresetsCreateDuplicateUpdateDeleteAndPersist", [](TestContext& context) {
			const auto path = std::filesystem::temp_directory_path() / "TerraEngineTerrainBrushPresets.json";
			std::error_code error;
			std::filesystem::remove(path, error);
			TerraEngine::Editor::TerrainBrushPresetStore store;
			TerraEngine::Editor::TerrainBrushPreset preset;
			preset.name = "Broad Raise";
			std::string id;
			context.Expect(store.Create(preset, id), "A valid named brush preset should be created.");
			std::string duplicateId;
			context.Expect(store.Duplicate(id, "Broad Raise Copy", duplicateId), "A brush preset should be duplicable.");
			auto updated = store.Get(id);
			context.Expect(updated.has_value(), "The created preset should be readable.");
			if (updated)
			{
				updated->strength = 0.5f;
				context.Expect(store.Update(id, *updated), "An existing preset should be updateable.");
			}
			context.Expect(store.Save(path), "Brush presets should persist independently from terrain history.");
			TerraEngine::Editor::TerrainBrushPresetStore reopened;
			context.Expect(reopened.Load(path), "Persisted brush presets should reload.");
			context.Expect(reopened.Get(id).has_value() && reopened.Get(duplicateId).has_value(), "Create and duplicate identities should survive reload.");
			context.Expect(reopened.Delete(duplicateId), "A persisted preset should be deletable from the store.");
			std::filesystem::remove(path, error);
		});
	}
}
