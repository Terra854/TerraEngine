#include "../TestHarness.h"

#include "Components/SkyComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneManager.h"
#include "Scene/Serializer.h"
#include "Utilities/UUID.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace TerraEngine::Tests {
	namespace
	{
		TerraEngine::UUID MakeUuid(std::uint8_t fillValue)
		{
			TerraEngine::UUID uuid{};
			uuid.fill(fillValue);
			return uuid;
		}

		bool NearlyEqual(float lhs, float rhs)
		{
			return std::fabs(lhs - rhs) < 0.0001f;
		}

		std::filesystem::path TestRoot()
		{
			const std::filesystem::path root = std::filesystem::temp_directory_path() / "TerraEngineSkyComponentTests";
			std::filesystem::create_directories(root);
			return root;
		}

		nlohmann::json MakeSkyComponentJson(std::string sourceAsset)
		{
			return {
				{ "active", true },
				{ "sourceAsset", std::move(sourceAsset) },
				{ "exposure", 1.25f },
				{ "rotationOffsetDegrees", 15.0f },
				{ "horizonTint", { 0.1f, 0.2f, 0.3f } },
				{ "zenithTint", { 0.4f, 0.5f, 0.6f } },
				{ "groundTint", { 0.7f, 0.8f, 0.9f } }
			};
		}

		nlohmann::json MakeObjectJson(const TerraEngine::UUID& uuid, std::string name, nlohmann::json skyComponent = nullptr)
		{
			nlohmann::json objectJson = {
				{ "uuid", TerraEngine::ToHexString(uuid) },
				{ "name", std::move(name) },
				{ "tag", "Untagged" },
				{ "layer", "Default" },
				{ "active", true },
				{ "isStatic", false },
				{ "components", nlohmann::json::object() }
			};
			if (!skyComponent.is_null())
			{
				objectJson["components"]["SkyComponent"] = std::move(skyComponent);
			}
			return objectJson;
		}

		nlohmann::json MakeSceneJson(nlohmann::json objects, nlohmann::json sceneSettings = nlohmann::json::object())
		{
			return {
				{ "name", "Sky Component Test Scene" },
				{ "sceneSettings", std::move(sceneSettings) },
				{ "objects", std::move(objects) }
			};
		}

		std::filesystem::path WriteSceneFile(const std::string& fileName, const nlohmann::json& sceneJson)
		{
			const std::filesystem::path path = TestRoot() / fileName;
			std::ofstream output(path);
			output << sceneJson.dump(4);
			return path;
		}

		bool LoadAndActivateScene(const std::filesystem::path& scenePath)
		{
			SkyComponent::registerType();
			SceneManager& scenes = SceneManager::Instance();
			scenes.UnloadAll();
			scenes.SetSceneRoot(scenePath.parent_path());
			const TerraEngine::UUID sceneId = scenes.LoadScene(scenePath);
			return sceneId != TerraEngine::EMPTY_UUID && scenes.SetActiveScene(sceneId);
		}

		void ResetSceneState()
		{
			SceneManager::Instance().UnloadAll();
			ObjectManager::instance().clear();
		}
	}

	void RegisterSkyComponentSerializationTests(TestRegistry& registry)
	{
		registry.Add("Scene.SkyComponent.SerializesUnderObjectComponents", [](TestContext& context) {
			ResetSceneState();
			SkyComponent::registerType();

			Object skyObject = ObjectManager::instance().createObject("Sky");
			SkyComponent& sky = *ObjectManager::instance().AddComponent<SkyComponent>(skyObject.EntityId());
			SetComponentProperty(sky, "sourceAsset", std::string("Textures/Sky/Test.tetf"));

			nlohmann::json objectJson = Serializer::SerializeObject(skyObject);
			context.Expect(objectJson["components"].contains("SkyComponent"), "Sky data should serialize through the object component map.");
			const nlohmann::json& componentJson = objectJson["components"]["SkyComponent"];
			context.Expect(componentJson.contains("sourceAsset") && componentJson["sourceAsset"].is_string(), "Sky source asset should be serialized as a string: " + objectJson.dump());
			context.Expect(!componentJson.contains("intensity"), "Sky intensity should live on the same object's LightComponent, not SkyComponent: " + objectJson.dump());
			context.ExpectEqual(std::string("Textures/Sky/Test.tetf"), componentJson["sourceAsset"].get<std::string>(), "Sky source asset should be persisted on SkyComponent.");
			ResetSceneState();
		});

		registry.Add("Scene.SkyComponent.SaveOmitsSceneSettingsSky", [](TestContext& context) {
			ResetSceneState();
			const std::filesystem::path scenePath = WriteSceneFile(
				"save-omits-scene-sky.tescene",
				MakeSceneJson(nlohmann::json::array()));
			context.Expect(LoadAndActivateScene(scenePath), "Scene with no initial objects should load before save verification.");

			Object skyObject = ObjectManager::instance().createObject("Saved Sky");
			SkyComponent& sky = *ObjectManager::instance().AddComponent<SkyComponent>(skyObject.EntityId());
			SetComponentProperty(sky, "sourceAsset", std::string("Textures/Sky/Saved.tetf"));
			SceneManager::Instance().ForceMarkActiveSceneDirty();
			context.Expect(SceneManager::Instance().SaveScene(), "Saving a scene with SkyComponent should succeed.");

			std::ifstream savedScene(scenePath);
			const nlohmann::json savedJson = nlohmann::json::parse(savedScene);
			context.Expect(!savedJson["sceneSettings"].contains("sky"), "New-format scene settings must not write sceneSettings.sky.");
			context.Expect(savedJson["objects"][0]["components"].contains("SkyComponent"), "Saved sky data should remain under object components.");
			ResetSceneState();
		});

		registry.Add("Scene.SkyComponent.ActiveComponentMatchesAuthoringAfterLoad", [](TestContext& context) {
			ResetSceneState();
			const std::filesystem::path scenePath = WriteSceneFile(
				"active-payload.tescene",
				MakeSceneJson(nlohmann::json::array({
					MakeObjectJson(MakeUuid(2), "Sky", MakeSkyComponentJson("Textures/Sky/Loaded.tetf"))
				})));
			context.Expect(LoadAndActivateScene(scenePath), "Scene with SkyComponent should load.");

			const SkyComponent* activeSky = SceneManager::Instance().GetActiveSkyComponent();
			context.Expect(SceneManager::Instance().HasActiveSkyComponent(), "Active sky component should be present when an active SkyComponent exists.");
			context.Expect(activeSky != nullptr, "Active sky lookup should return the selected SkyComponent.");
			context.ExpectEqual(std::string("Textures/Sky/Loaded.tetf"), activeSky->sourceAsset, "Active sky lookup should use the component source asset.");
			ResetSceneState();
		});

		registry.Add("Scene.SkyComponent.ObsoleteSceneSettingsSkyDoesNotCreateComponent", [](TestContext& context) {
			ResetSceneState();
			nlohmann::json sceneSettings = {
				{ "sky", MakeSkyComponentJson("Textures/Sky/Obsolete.tetf") }
			};
			const std::filesystem::path scenePath = WriteSceneFile(
				"obsolete-without-component.tescene",
				MakeSceneJson(nlohmann::json::array({ MakeObjectJson(MakeUuid(3), "Plain Object") }), sceneSettings));
			context.Expect(LoadAndActivateScene(scenePath), "Scene with obsolete sceneSettings.sky should still load.");

			const std::vector<entt::entity> skyObjects = ObjectManager::instance().getEntitiesWithComponents<SkyComponent>();
			context.Expect(skyObjects.empty(), "Obsolete sceneSettings.sky must not create a SkyComponent.");
			context.Expect(!SceneManager::Instance().HasActiveSkyComponent(), "No SkyComponent should leave the active sky component absent.");
			ResetSceneState();
		});

		registry.Add("Scene.SkyComponent.ObsoleteSceneSettingsSkyDoesNotOverrideComponent", [](TestContext& context) {
			ResetSceneState();
			nlohmann::json sceneSettings = {
				{ "sky", MakeSkyComponentJson("Textures/Sky/Obsolete.tetf") }
			};
			const std::filesystem::path scenePath = WriteSceneFile(
				"obsolete-with-component.tescene",
				MakeSceneJson(nlohmann::json::array({
					MakeObjectJson(MakeUuid(4), "Sky", MakeSkyComponentJson("Textures/Sky/Component.tetf"))
				}), sceneSettings));
			context.Expect(LoadAndActivateScene(scenePath), "Scene with obsolete and component sky data should load.");

			const SkyComponent* activeSky = SceneManager::Instance().GetActiveSkyComponent();
			context.Expect(activeSky != nullptr, "SkyComponent should remain the active sky source beside obsolete scene settings.");
			context.ExpectEqual(std::string("Textures/Sky/Component.tetf"), activeSky->sourceAsset, "Obsolete sceneSettings.sky should not override SkyComponent source data.");
			ResetSceneState();
		});

		registry.Add("Scene.SkyComponent.MalformedObsoleteSceneSettingsSkyIsIgnored", [](TestContext& context) {
			ResetSceneState();
			nlohmann::json sceneSettings = {
				{ "sky", "not an object anymore" },
				{ "indirectStrength", 2.0f }
			};
			const std::filesystem::path scenePath = WriteSceneFile(
				"malformed-obsolete.tescene",
				MakeSceneJson(nlohmann::json::array({ MakeObjectJson(MakeUuid(5), "Plain Object") }), sceneSettings));
			context.Expect(LoadAndActivateScene(scenePath), "Malformed obsolete sceneSettings.sky should not fail otherwise valid scene loading.");
			context.Expect(NearlyEqual(2.0f, SceneManager::Instance().GetActiveSceneRenderSettings().indirectStrength), "Non-sky scene settings should still load beside ignored obsolete sky data.");
			ResetSceneState();
		});

		registry.Add("Scene.SkyComponent.EditSaveReloadPreservesValuesAcrossCycles", [](TestContext& context) {
			ResetSceneState();
			std::filesystem::path scenePath = WriteSceneFile(
				"edit-save-reload.tescene",
				MakeSceneJson(nlohmann::json::array({
					MakeObjectJson(MakeUuid(6), "Sky", MakeSkyComponentJson("Textures/Sky/Cycle0.tetf"))
				})));

			for (int cycle = 1; cycle <= 10; ++cycle)
			{
				context.Expect(LoadAndActivateScene(scenePath), "SkyComponent scene should load for every edit-save-reload cycle.");
				std::vector<entt::entity> skyObjects = ObjectManager::instance().getEntitiesWithComponents<SkyComponent>();
				context.ExpectEqual(static_cast<std::size_t>(1), skyObjects.size(), "Exactly one SkyComponent should exist during edit-save-reload.");
				SkyComponent* sky = ObjectManager::instance().GetComponent<SkyComponent>(skyObjects.front());
				context.Expect(sky != nullptr, "Sky object should expose SkyComponent during edit-save-reload.");
				SetComponentProperty(*sky, "sourceAsset", "Textures/Sky/Cycle" + std::to_string(cycle) + ".tetf");
				SceneManager::Instance().ForceMarkActiveSceneDirty();
				context.Expect(SceneManager::Instance().SaveScene(), "Edited SkyComponent values should save.");
			}

			context.Expect(LoadAndActivateScene(scenePath), "Final SkyComponent scene should reload after repeated cycles.");
			const SkyComponent* activeSky = SceneManager::Instance().GetActiveSkyComponent();
			context.Expect(activeSky != nullptr, "Final reload should expose the active SkyComponent.");
			context.ExpectEqual(std::string("Textures/Sky/Cycle10.tetf"), activeSky->sourceAsset, "Final sky source should match the last saved component value.");
			ResetSceneState();
		});

		registry.Add("Scene.SkyComponent.MultipleComponentsUseDeterministicTraversalOrder", [](TestContext& context) {
			ResetSceneState();
			const std::filesystem::path scenePath = WriteSceneFile(
				"multiple-components.tescene",
				MakeSceneJson(nlohmann::json::array({
					MakeObjectJson(MakeUuid(9), "Later Sky", MakeSkyComponentJson("Textures/Sky/Later.tetf")),
					MakeObjectJson(MakeUuid(1), "Earlier Sky", MakeSkyComponentJson("Textures/Sky/Earlier.tetf"))
				})));
			context.Expect(LoadAndActivateScene(scenePath), "Scene with multiple SkyComponents should load.");

			const SkyComponent* activeSky = SceneManager::Instance().GetActiveSkyComponent();
			context.Expect(activeSky != nullptr, "Multiple sky components should still resolve one active component.");
			context.ExpectEqual(std::string("Textures/Sky/Earlier.tetf"), activeSky->sourceAsset, "Active sky should use the first SkyComponent in UUID-sorted traversal order.");
			ResetSceneState();
		});
	}
}
