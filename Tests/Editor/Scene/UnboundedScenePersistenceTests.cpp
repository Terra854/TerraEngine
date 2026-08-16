#include "../TestHarness.h"

#include "Editor/TerraEngine-Editor/Automation/EditorAutomationTransaction.h"

#include <string>

namespace TerraEngine::EditorTests
{
	void RegisterUnboundedScenePersistenceTests(TestRegistry& registry)
	{
		registry.Add("Editor.UnboundedPersistence.CanonicalClipboardRoundTripsWithoutCellDrift", [](TestContext& context) {
			Scene::WorldPosition original;
			original.cell = {1'234'567'890'123LL, -4'567'890'123'456LL, 77LL};
			original.offsetMetres = {123.125, -321.5, 0.001};
			const nlohmann::json encoded = Editor::Automation::SerializeCanonicalWorldPosition(original);
			Scene::WorldPosition decoded;
			std::string error;
			context.Expect(Editor::Automation::ParseCanonicalWorldPosition(encoded, decoded, error), "Canonical clipboard payload should be valid for replay.");
			context.Expect(decoded == original, "Canonical persistence must retain exact cells and double offsets without drift.");
		});

		registry.Add("Editor.UnboundedPersistence.NonNormalizedReplayFailsClosed", [](TestContext& context) {
			Scene::WorldPosition decoded;
			std::string error;
			const nlohmann::json malformed = {{"cell", {{"x", "0"}, {"y", "0"}, {"z", "0"}}},
				{"offsetMetres", {Scene::kWorldCellHalfEdgeMetres, 0.0, 0.0}}};
			context.Expect(!Editor::Automation::ParseCanonicalWorldPosition(malformed, decoded, error), "Replay must reject a non-normalized pose instead of repairing a persisted record silently.");
		});
	}
}
