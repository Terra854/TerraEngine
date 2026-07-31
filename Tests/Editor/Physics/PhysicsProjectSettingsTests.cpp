// Physics Project Settings authoring contracts.  These tests are intentionally
// unregistered until the owning test-registry task integrates ImplementationStage.

#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Project/ProjectSettingsMetadata.h"

namespace TerraEngine::EditorTests
{
	void RegisterPhysicsProjectSettingsTests(TestRegistry& registry)
	{
		registry.Add("Editor.PhysicsProjectSettings.CategoryAndLayerRenameContract", [](TestContext& context) {
			using namespace TerraEngine::Editor::ProjectSettings;
			context.Expect(FindProjectSettingsCategory(ProjectSettingsCategoryId::Physics) != nullptr, "Physics must be a first-class Project Settings category.");
			const auto matches = FilterProjectSettingsCategories("gravity");
			context.Expect(!matches.empty() && matches.front().id == ProjectSettingsCategoryId::Physics, "Gravity search should route to Physics settings.");
			const auto impact = PreflightPhysicsLayerRename(TerraEngine::Physics::PhysicsSettings::Defaults(), 0U, "Gameplay", 7U);
			context.Expect(impact.accepted, "A unique layer rename should pass preflight.");
			context.ExpectEqual(static_cast<std::size_t>(7), impact.directReferenceCount, "Preflight must preserve the caller's visible reference impact.");
		});

		registry.Add("Editor.PhysicsProjectSettings.DebugDefaultsAndPublicationDto", [](TestContext& context) {
			using namespace TerraEngine::Editor::ProjectSettings;
			PhysicsSettingsPublicationRequest request;
			context.Expect(!request.settings.debugVisualizationEnabled, "Debug capture must be opt-in by default.");
			context.ExpectEqual(static_cast<std::uint32_t>(2), request.settings.retainedDebugTicks, "Default debug history must be bounded to two ticks.");
			context.ExpectEqual(static_cast<std::uint32_t>(100000), request.settings.maxDebugPrimitives, "Default debug primitive budget must be bounded.");
			context.Expect(request.requiresWorldRebuild, "Project settings publication must request protected runtime rebuild handling.");
		});
	}
}
