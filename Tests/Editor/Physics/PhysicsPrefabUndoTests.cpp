// Prefab/undo physics authoring contracts.  Registry ownership remains outside
// this task, so these tests do not modify the central EditorTests entry point.

#include "TestHarness.h"
#include "Scene/PrefabComposition.h"

#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests
{
	void RegisterPhysicsPrefabUndoTests(TestRegistry& registry)
	{
		registry.Add("Editor.PhysicsPrefabUndo.NestedPhysicsApplyAndRevertAreAtomic", [](TestContext& context) {
			Scene::PrefabSource source;
			source.sourceId = GenerateUUID();
			source.sourceGraph.sceneId = GenerateUUID();
			const UUID sourceObject = GenerateUUID();
			Scene::SceneGraphObject graphObject;
			graphObject.objectId = sourceObject;
			graphObject.name = "Physics Prefab";
			source.sourceGraph.rootObjects.push_back(sourceObject);
			source.sourceGraph.objects.emplace(sourceObject, graphObject);
			const UUID colliderRecord = GenerateUUID();
			source.componentDefaults[sourceObject]["ColliderSetComponent"] = {
				{"schemaVersion", 1U},
				{"colliders", nlohmann::json::array({{
					{"recordId", ToHexString(colliderRecord)},
					{"localCenter", {{"x", 0.0F}, {"y", 0.0F}, {"z", 0.0F}}},
					{"enabled", true}
				}})}
			};
			Scene::PrefabInstance instance;
			instance.sourceId = source.sourceId;
			instance.sourceToInstanceObjectIds.emplace(sourceObject, GenerateUUID());
			Scene::PrefabOverride centerOverride;
			centerOverride.targetObjectId = sourceObject;
			centerOverride.componentName = "ColliderSetComponent";
			centerOverride.recordId = colliderRecord;
			centerOverride.propertyPath = "localCenter.x";
			centerOverride.authoredValue = "2.5";

			std::vector<Scene::SceneDiagnostic> diagnostics;
			context.Expect(Scene::TryApplyOverridesToDefaults(source, instance, {centerOverride}, diagnostics), "A valid stable-record override must update prefab component defaults.");
			context.ExpectEqual(2.5F, source.componentDefaults.at(sourceObject).at("ColliderSetComponent").at("colliders").at(0).at("localCenter").at("x").get<float>(), "Apply must change the addressed nested physics property.");

			Scene::PrefabOverride instanceOverride = centerOverride;
			instanceOverride.authoredValue = "4.0";
			instance.overrides = {instanceOverride};
			const Scene::PrefabCompositionResult overridden = Scene::ResolvePrefabInstance(source, instance);
			context.Expect(overridden.success, "A valid nested physics instance override must resolve.");
			const UUID instanceObject = instance.sourceToInstanceObjectIds.at(sourceObject);
			context.ExpectEqual(4.0F, overridden.resolvedComponentDefaults.at(instanceObject).at("ColliderSetComponent").at("colliders").at(0).at("localCenter").at("x").get<float>(), "Instance composition must apply the nested stable-record override over prefab defaults.");
			Scene::ResetOverridesToDefaults(instance, sourceObject);
			const Scene::PrefabCompositionResult reverted = Scene::ResolvePrefabInstance(source, instance);
			context.ExpectEqual(2.5F, reverted.resolvedComponentDefaults.at(instanceObject).at("ColliderSetComponent").at("colliders").at(0).at("localCenter").at("x").get<float>(), "Revert must expose the applied prefab default without retaining an instance override.");

			const nlohmann::json beforeRejectedBatch = source.componentDefaults.at(sourceObject).at("ColliderSetComponent");
			const std::uint64_t revisionBeforeRejectedBatch = source.sourceGraph.revision;
			Scene::PrefabOverride missingRecord = centerOverride;
			missingRecord.recordId = GenerateUUID();
			diagnostics.clear();
			context.Expect(!Scene::TryApplyOverridesToDefaults(source, instance, {centerOverride, missingRecord}, diagnostics), "One missing stable record must reject the complete prefab-default batch.");
			context.Expect(source.componentDefaults.at(sourceObject).at("ColliderSetComponent") == beforeRejectedBatch, "Rejected prefab-default batches must leave all component payloads unchanged.");
			context.ExpectEqual(revisionBeforeRejectedBatch, source.sourceGraph.revision, "Rejected prefab-default batches must not advance source revision.");
		});

		registry.Add("Editor.PhysicsPrefabUndo.ConnectedUuidRemapIsStable", [](TestContext& context) {
			Scene::PrefabSource source;
			source.sourceId = GenerateUUID();
			Scene::PrefabInstance instance;
			const UUID sourceObject = GenerateUUID();
			const UUID connectedSourceObject = GenerateUUID();
			const UUID instanceObject = GenerateUUID();
			const UUID connectedInstanceObject = GenerateUUID();
			instance.sourceToInstanceObjectIds.emplace(sourceObject, instanceObject);
			instance.sourceToInstanceObjectIds.emplace(connectedSourceObject, connectedInstanceObject);
			const auto remapped = Scene::RemapConnectedObjectUuid(instance, sourceObject);
			context.Expect(remapped.has_value() && *remapped == instanceObject, "Connected constraint UUIDs must remap through prefab identity, not entity slots.");
			context.Expect(!Scene::RemapConnectedObjectUuid(instance, GenerateUUID()).has_value(), "Missing connected-object UUIDs must reject instead of silently binding another object.");

			source.sourceGraph.sceneId = GenerateUUID();
			for (const UUID objectId : {sourceObject, connectedSourceObject})
			{
				Scene::SceneGraphObject object;
				object.objectId = objectId;
				object.name = "Constraint endpoint";
				source.sourceGraph.rootObjects.push_back(objectId);
				source.sourceGraph.objects.emplace(objectId, object);
			}
			const UUID constraintRecord = GenerateUUID();
			source.componentDefaults[sourceObject]["ConstraintSetComponent"] = {
				{"schemaVersion", 1U},
				{"constraints", nlohmann::json::array({{
					{"recordId", ToHexString(constraintRecord)},
					{"firstObjectId", ToHexString(sourceObject)},
					{"secondObjectId", ToHexString(connectedSourceObject)}
				}})}
			};
			const Scene::PrefabCompositionResult resolved = Scene::ResolvePrefabInstance(source, instance);
			context.Expect(resolved.success, "Connected constraint defaults with complete mappings must resolve.");
			context.ExpectEqual(ToHexString(connectedInstanceObject), resolved.resolvedComponentDefaults.at(instanceObject).at("ConstraintSetComponent").at("constraints").at(0).at("secondObjectId").get<std::string>(), "Resolved constraint endpoints must use the connected instance UUID.");
		});

		registry.Add("Editor.PhysicsPrefabUndo.InvalidOverrideBatchIsAtomic", [](TestContext& context) {
			Scene::PrefabSource source;
			source.sourceId = GenerateUUID();
			source.nestedSourceIds.push_back(source.sourceId);
			const std::uint64_t oldRevision = source.sourceGraph.revision;
			Scene::PrefabInstance instance;
			instance.sourceId = source.sourceId;
			Scene::PrefabOverride invalid;
			invalid.targetObjectId = GenerateUUID();
			invalid.connectedObjectSourceId = GenerateUUID();
			std::vector<Scene::SceneDiagnostic> diagnostics;
			context.Expect(!Scene::TryApplyOverridesToDefaults(source, instance, { invalid }, diagnostics), "Invalid connected or cyclic override batches must reject atomically.");
			context.ExpectEqual(oldRevision, source.sourceGraph.revision, "Rejected prefab batches must not change source revision.");
			context.Expect(!diagnostics.empty(), "Rejected prefab batches need an actionable diagnostic.");
		});
	}
}
