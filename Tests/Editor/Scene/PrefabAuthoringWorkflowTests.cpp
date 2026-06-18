#include "../TestHarness.h"
#include "PrefabAuthoringTestFixtures.h"

#include "PrefabAuthoringSubsystem.h"

namespace TerraEngine::EditorTests {
	void RegisterPrefabAuthoringWorkflowTests(TestRegistry& registry)
	{
		registry.Add("PrefabAuthoring.CreateOpenSaveDeleteWorkflow", [](TestContext& context) {
			PrefabAuthoringSubsystem subsystem;
			auto source = PrefabFixtures::MakePrefabSource();
			const TerraEngine::UUID sourceId = source.sourceId;

			const auto session = subsystem.CreatePrefabFromSelection(source);
			context.Expect(session.sourceId == sourceId, "Create should return an editing session for the new prefab source.");
			context.Expect(subsystem.OpenPrefab(sourceId).has_value(), "Open should find a created prefab source.");
			context.Expect(subsystem.SavePrefab(sourceId), "Save should validate and keep a created prefab source.");

			const auto diagnostics = subsystem.DeletePrefab(sourceId, false);
			context.Expect(diagnostics.empty(), "Deleting a source with no live instances should succeed.");
			context.Expect(!subsystem.OpenPrefab(sourceId).has_value(), "Deleted prefab sources should not reopen.");
		});

		registry.Add("PrefabAuthoring.ApplyAndResetOverrides", [](TestContext& context) {
			PrefabAuthoringSubsystem subsystem;
			auto source = PrefabFixtures::MakePrefabSource();
			const TerraEngine::UUID sourceId = source.sourceId;
			subsystem.CreatePrefabFromSelection(source);

			auto instance = PrefabFixtures::MakePrefabInstance(subsystem.GetSources().at(sourceId));
			TerraEngine::Scene::PrefabOverride renameOverride;
			renameOverride.targetObjectId = instance.rootObjectId;
			renameOverride.propertyPath = "name";
			renameOverride.authoredValue = "DefaultedRoot";
			instance.overrides.push_back(renameOverride);

			subsystem.ApplyInstanceOverridesToDefaults(sourceId, instance, instance.overrides);
			context.ExpectEqual(std::string("DefaultedRoot"), subsystem.GetSources().at(sourceId).sourceGraph.objects.at(instance.rootObjectId).name, "Apply-to-defaults should update the prefab source value.");

			subsystem.ResetInstanceOverrides(instance);
			context.Expect(instance.overrides.empty(), "Reset-to-defaults should remove all instance overrides.");
		});

		registry.Add("PrefabAuthoring.DeleteCancelsWhenInstancesRemain", [](TestContext& context) {
			auto source = PrefabFixtures::MakePrefabSource();
			auto instance = PrefabFixtures::MakePrefabInstance(source);
			const auto diagnostics = TerraEngine::Scene::ValidatePrefabDelete(source, {instance}, false);
			context.Expect(!diagnostics.empty(), "Default delete should cancel when live instances reference the source.");

			const auto convertedDiagnostics = TerraEngine::Scene::ValidatePrefabDelete(source, {instance}, true);
			context.Expect(convertedDiagnostics.empty(), "Explicit conversion should allow prefab source deletion.");
		});
	}
}
