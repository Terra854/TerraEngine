#include "../TestHarness.h"

#include "Editor/TerraEngine-Editor/Automation/EditorAutomationTransaction.h"

#include <string>

namespace TerraEngine::EditorTests
{
	void RegisterUnboundedWorldAutomationTests(TestRegistry& registry)
	{
		registry.Add("Editor.Automation.UnboundedWorld.StrictPositionRejectsLegacyVector", [](TestContext& context) {
			Scene::WorldPosition position;
			std::string error;
			const bool accepted = Editor::Automation::ParseCanonicalWorldPosition(
				nlohmann::json::array({1.0, 2.0, 3.0}), position, error);
			context.Expect(!accepted, "Three-number vectors must never be promoted into canonical world positions.");
			context.Expect(!error.empty(), "Rejected legacy locations should explain the required canonical fields.");
		});

		registry.Add("Editor.Automation.UnboundedWorld.StrictPositionPreservesExactCell", [](TestContext& context) {
			Scene::WorldPosition position;
			std::string error;
			const nlohmann::json payload = {{"cell", {{"x", "9223372036854775806"}, {"y", "-9223372036854775807"}, {"z", "0"}}},
				{"offsetMetres", {1.25, -2.5, 0.0}}};
			context.Expect(Editor::Automation::ParseCanonicalWorldPosition(payload, position, error), "Exact decimal cells with normalized double offsets should decode.");
			context.ExpectEqual(9223372036854775806LL, position.cell.x, "Positive near-limit cells must remain exact.");
			context.ExpectEqual(-9223372036854775807LL, position.cell.y, "Negative near-limit cells must remain exact.");
		});

		registry.Add("Editor.Automation.UnboundedWorld.TransactionRequiresVersion", [](TestContext& context) {
			const Editor::Automation::AutomationTransactionPreflight legacy =
				Editor::Automation::PreflightAutomationTransaction({{"operations", nlohmann::json::array({{{"kind", "world.translate"}}})}});
			const Editor::Automation::AutomationTransactionPreflight strict =
				Editor::Automation::PreflightAutomationTransaction({{"format", "TerraSpatialTransactionV1"}, {"operations", nlohmann::json::array({{{"kind", "world.translate"}}})}});
			context.Expect(!legacy.valid, "Unversioned automation transactions must be rejected before live object access.");
			context.Expect(strict.valid, "The strict transaction envelope should pass immutable shape preflight.");
		});
	}
}
