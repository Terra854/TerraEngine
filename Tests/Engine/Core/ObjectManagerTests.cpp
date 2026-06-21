#include "TestHarness.h"

#include "Components/ScriptComponent.h"
#include "Components/Transform.h"
#include "Core/Object/Component.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		bool NearlyEqual(float left, float right, float epsilon = 0.0001f) {
			return std::abs(left - right) <= epsilon;
		}

		bool NearlyEqualVec3(const glm::vec3& left, const glm::vec3& right) {
			return NearlyEqual(left.x, right.x) && NearlyEqual(left.y, right.y) && NearlyEqual(left.z, right.z);
		}

		bool HasObject(const std::vector<Object>& objects, const Object& expected) {
			return std::any_of(objects.begin(), objects.end(), [&expected](const Object& object) {
				return object == expected;
			});
		}

		ComponentAccessFunctions GetRegisteredComponentAccess(const std::string& name) {
			// The registry is shared process state, so tests copy the access
			// functions while holding the mutex and then release the lock.
			std::lock_guard lock(ComponentAccessRegistryMutex());
			auto& registry = ComponentAccessRegistry();
			const auto iter = registry.find(name);
			return iter != registry.end() ? iter->second : ComponentAccessFunctions{};
		}

		void ResetObjectManager() {
			// Clear callbacks before data so stale lambdas from one test cannot
			// observe cleanup done by the next test.
			ObjectManager& manager = ObjectManager::instance();
			manager.SetOnModifiedCallback({});
			manager.SetOnSpatialObjectDirtyCallback({});
			manager.clear();
		}
	}

	void RegisterObjectManagerTests(TestRegistry& registry) {
		registry.Add("Engine.ObjectManager.CreateObjectTracksMetadataUuidAndCallbacks", [](TestContext& context) {
			ResetObjectManager();
			ObjectManager& manager = ObjectManager::instance();
			int modifiedCount = 0;
			int spatialDirtyCount = 0;
			entt::entity lastSpatialEntity = entt::null;

			manager.SetOnModifiedCallback([&modifiedCount]() {
				++modifiedCount;
			});
			manager.SetOnSpatialObjectDirtyCallback([&spatialDirtyCount, &lastSpatialEntity](entt::entity entity) {
				++spatialDirtyCount;
				lastSpatialEntity = entity;
			});

			Object object = manager.createObject("Player");
			const TerraEngine::UUID originalUuid = object.getUUID();

			context.Expect(object.getComponent<Transform>() != nullptr, "Created objects should receive a Transform component by default.");
			context.Expect(manager.getObject(originalUuid).has_value(), "Created object UUIDs should resolve back to live objects.");
			context.ExpectEqual(std::string("Player"), object.getName(), "Created objects should keep their authored name.");
			context.ExpectEqual(std::string("Untagged"), object.getTag(), "Created objects should use the default tag.");
			context.ExpectEqual(std::string("Default"), object.getLayer(), "Created objects should use the default layer.");
			context.Expect(object.isActive(), "Created objects should start active.");
			context.Expect(!object.isStatic(), "Created objects should start non-static.");

			object.setName("Hero");
			object.setTag("Gameplay");
			object.setLayer("Characters");
			object.setActive(false);
			object.setStatic(true);

			context.ExpectEqual(std::string("Hero"), object.getName(), "Name updates should be stored in object metadata.");
			context.ExpectEqual(std::string("Gameplay"), object.getTag(), "Tag updates should be stored in object metadata.");
			context.ExpectEqual(std::string("Characters"), object.getLayer(), "Layer updates should be stored in object metadata.");
			context.Expect(!object.isActive(), "Active-state updates should be stored in object metadata.");
			context.Expect(object.isStatic(), "Static-state updates should be stored in object metadata.");
			context.Expect(manager.FindObjectByName("Hero").has_value(), "Name lookup should find live objects by display name.");
			context.Expect(!manager.FindObjectByTag("Gameplay").has_value(), "Tag lookup should ignore inactive objects.");

			object.setActive(true);
			context.Expect(manager.FindObjectByTag("Gameplay").has_value(), "Tag lookup should include active objects.");
			context.Expect(modifiedCount >= 6, "Metadata mutations should notify the scene modified callback.");
			context.Expect(spatialDirtyCount >= 3, "Creation and spatial metadata mutations should notify spatial dirty callbacks.");
			context.Expect(lastSpatialEntity == object.EntityId(), "Spatial dirty callbacks should report the modified entity.");

			ResetObjectManager();
		});

		registry.Add("Engine.ObjectManager.ParentingPreservesWorldTransformAndRejectsCycles", [](TestContext& context) {
			ResetObjectManager();
			ObjectManager& manager = ObjectManager::instance();
			Object root = manager.createObject("Root");
			Object child = manager.createObject("Child");
			Object grandchild = manager.createObject("Grandchild");

			Transform* rootTransform = root.getComponent<Transform>();
			Transform* childTransform = child.getComponent<Transform>();
			rootTransform->position = glm::vec3(10.0f, 0.0f, 0.0f);
			rootTransform->scale = glm::vec3(2.0f, 2.0f, 2.0f);
			childTransform->position = glm::vec3(4.0f, 0.0f, 0.0f);

			context.Expect(manager.setParent(child, root), "Parenting a root object under another live object should succeed.");
			context.Expect(manager.setParent(grandchild, child), "Nested parenting should succeed when no cycle is created.");
			context.Expect(NearlyEqualVec3(childTransform->position, glm::vec3(-3.0f, 0.0f, 0.0f)), "Parenting should convert the child transform so world position is preserved.");
			context.Expect(HasObject(manager.getChildren(root), child), "The parent should list the child after parenting.");
			context.Expect(!HasObject(manager.getRootObjects(), child), "Parented children should not be reported as root objects.");

			context.Expect(!manager.setParent(root, grandchild), "Parenting an ancestor under a descendant should be rejected.");
			context.Expect(!manager.setParent(child, child), "Self-parenting should be rejected.");

			context.Expect(manager.setParent(child, std::nullopt), "Passing an empty parent should detach the object to the root.");
			context.Expect(NearlyEqualVec3(childTransform->position, glm::vec3(4.0f, 0.0f, 0.0f)), "Detaching should preserve the child's world-space position.");
			context.Expect(manager.getChildren(root).empty(), "Detaching should remove the child from its old parent's child list.");
			context.Expect(HasObject(manager.getRootObjects(), child), "Detached children should become root objects again.");

			ResetObjectManager();
		});

		registry.Add("Engine.ObjectManager.RemovingParentDeletesChildBranchAndUuidLookups", [](TestContext& context) {
			ResetObjectManager();
			ObjectManager& manager = ObjectManager::instance();
			Object root = manager.createObject("Root");
			Object child = manager.createObject("Child");
			Object grandchild = manager.createObject("Grandchild");
			const TerraEngine::UUID rootUuid = root.getUUID();
			const TerraEngine::UUID childUuid = child.getUUID();
			const TerraEngine::UUID grandchildUuid = grandchild.getUUID();

			context.Expect(manager.setParent(child, root), "The child should parent under the root before removal.");
			context.Expect(manager.setParent(grandchild, child), "The grandchild should parent under the child before removal.");

			manager.removeObject(root);

			context.Expect(!manager.getObject(rootUuid).has_value(), "Removing an object should clear its UUID lookup.");
			context.Expect(!manager.getObject(childUuid).has_value(), "Removing a parent should recursively clear child UUID lookups.");
			context.Expect(!manager.getObject(grandchildUuid).has_value(), "Removing a parent should recursively clear grandchild UUID lookups.");
			context.Expect(manager.getRootObjects().empty(), "Removing the only hierarchy branch should leave no root objects.");

			ResetObjectManager();
		});

		registry.Add("Engine.ObjectManager.NameAndTagIndexesInvalidateOnMetadataChanges", [](TestContext& context) {
			ResetObjectManager();
			ObjectManager& manager = ObjectManager::instance();
			Object first = manager.createObject("Player");
			Object second = manager.createObject("Player");

			first.setTag("Enemy");
			second.setTag("Enemy");
			context.Expect(manager.FindObjectByName("Player").has_value(), "The name index should find newly created objects.");
			context.Expect(manager.FindObjectByTag("Enemy").has_value(), "The tag index should find active tagged objects.");

			first.setName("Hero");
			context.Expect(manager.FindObjectByName("Hero").has_value(), "Renaming should insert the object into the new name bucket.");
			context.Expect(manager.FindObjectByName("Player").has_value(), "Renaming one duplicate should preserve other objects in the old name bucket.");

			second.setName("Villain");
			context.Expect(!manager.FindObjectByName("Player").has_value(), "Renaming the final duplicate should remove the old name bucket.");

			first.setTag("Player");
			context.Expect(manager.FindObjectByTag("Player").has_value(), "Changing tags should insert the object into the new tag bucket.");
			first.setActive(false);
			context.Expect(!manager.FindObjectByTag("Player").has_value(), "Inactive objects should not be returned from the tag index.");

			manager.removeObject(second);
			context.Expect(!manager.FindObjectByName("Villain").has_value(), "Removing an object should erase its name index entry.");
			context.Expect(manager.FindObjectByTag("Enemy").has_value() == false, "Removing the last active tagged object should erase the tag result.");

			manager.clear();
			context.Expect(!manager.FindObjectByName("Hero").has_value(), "Clearing the manager should clear name indexes.");
			context.Expect(!manager.FindObjectByTag("Player").has_value(), "Clearing the manager should clear tag indexes.");
		});

		registry.Add("Engine.ObjectManager.CacheGenerationsTrackMetadataAndStructure", [](TestContext& context) {
			ResetObjectManager();
			ObjectManager& manager = ObjectManager::instance();
			Object object = manager.createObject("Generation Target");
			const std::uint64_t initialMetadataGeneration = manager.getObjectMetadataGeneration(object);
			const std::uint64_t initialComponentGeneration = manager.getObjectComponentGeneration(object);
			const std::uint64_t initialBindingGeneration = manager.getObjectBindingGeneration(object);

			manager.setObjectName(object, "Generation Target Renamed");
			context.Expect(manager.getObjectMetadataGeneration(object) > initialMetadataGeneration, "Object name edits should advance the metadata generation used by managed Object.Name caches.");
			context.ExpectEqual(initialComponentGeneration, manager.getObjectComponentGeneration(object), "Name edits should not invalidate component wrapper caches.");

			object.addComponent<ScriptComponent>();
			manager.notifyObjectStructureChanged(object);
			context.Expect(manager.getObjectComponentGeneration(object) > initialComponentGeneration, "Component changes should advance the component generation used by managed GetComponent caches.");
			context.Expect(manager.getObjectBindingGeneration(object) > initialBindingGeneration, "Script binding capable structure changes should advance binding cache generations.");

			ResetObjectManager();
		});

		registry.Add("Engine.ComponentAccess.TransformPropertiesRoundTripThroughReflection", [](TestContext& context) {
			ResetObjectManager();
			ComponentRegistry::RegisterAllComponents();
			Object object = ObjectManager::instance().createObject("Reflected");
			ComponentAccessFunctions transformAccess = GetRegisteredComponentAccess("Transform");

			context.Expect(TerraEngine::Reflection::IsValid(transformAccess.reflectionType), "RegisterAllComponents should register Transform reflection metadata.");
			context.ExpectEqual(std::string("Transform"), TerraEngine::Reflection::GetTypeName(transformAccess.reflectionType), "Transform should keep its stable reflected display name.");
			context.Expect(transformAccess.hasComponent(object), "The reflected Transform accessor should detect the default component.");
			context.Expect(transformAccess.setter(object, "Position", glm::vec3(3.0f, 4.0f, 5.0f)), "The reflected setter should update vec3 properties.");
			context.Expect(transformAccess.setter(object, "Scale", glm::vec3(2.0f, 3.0f, 4.0f)), "The reflected setter should update scale properties.");
			context.Expect(!transformAccess.setter(object, "Missing", 7), "The reflected setter should fail closed for unknown properties.");

			TerraEngine::Reflection::Variant positionValue = transformAccess.getter(object, "Position");
			context.Expect(TerraEngine::Reflection::IsType<glm::vec3>(positionValue), "The reflected getter should return a typed vec3 value.");
			context.Expect(NearlyEqualVec3(TerraEngine::Reflection::GetValue<glm::vec3>(positionValue), glm::vec3(3.0f, 4.0f, 5.0f)), "The reflected getter should read the updated position.");
			context.Expect(!TerraEngine::Reflection::IsValid(transformAccess.getter(object, "Missing")), "The reflected getter should return an empty value for unknown properties.");

			nlohmann::json serialized = transformAccess.serialize(object);
			context.Expect(serialized.contains("Position") && serialized["Position"].is_array(), "Transform serialization should include vec3 properties as arrays.");
			transformAccess.deserialize(object, nlohmann::json{
				{"Position", {7.0f, 8.0f, 9.0f}},
				{"Scale", {1.0f, 1.5f, 2.0f}},
				{"active", false}
			});

			Transform* transform = object.getComponent<Transform>();
			TerraEngine::Reflection::Variant deserializedPosition = transformAccess.getter(object, "Position");
			context.Expect(NearlyEqualVec3(transform->position, glm::vec3(7.0f, 8.0f, 9.0f)), "Deserialization should write reflected vec3 values back to the component.");
			context.Expect(NearlyEqualVec3(transform->scale, glm::vec3(1.0f, 1.5f, 2.0f)), "Deserialization should write reflected scale values back to the component.");
			context.Expect(NearlyEqualVec3(TerraEngine::Reflection::GetValue<glm::vec3>(deserializedPosition), glm::vec3(7.0f, 8.0f, 9.0f)), "Registered component accessors should read reflected values after deserialization.");
			context.Expect(!transform->active, "Deserialization should update reflected base component fields.");

			transformAccess.removeComponent(object);
			context.Expect(!transformAccess.hasComponent(object), "The reflected remover should detach the component.");
			transformAccess.addComponent(object);
			context.Expect(transformAccess.hasComponent(object), "The reflected adder should restore a missing component.");

			ResetObjectManager();
		});

		registry.Add("Engine.UUID.HexRoundTripRejectsMalformedText", [](TestContext& context) {
			TerraEngine::UUID uuid{};
			for(std::uint8_t index = 0; index < uuid.size(); ++index) {
				uuid[index] = index;
			}

			const std::string encoded = TerraEngine::ToHexString(uuid);
			const std::optional<TerraEngine::UUID> parsed = TerraEngine::ParseUUID(encoded);

			context.ExpectEqual(std::string("000102030405060708090a0b0c0d0e0f"), encoded, "UUID formatting should use stable lower-case hexadecimal text.");
			context.Expect(parsed.has_value() && *parsed == uuid, "UUID parsing should round-trip formatted identifiers.");
			context.Expect(!TerraEngine::ParseUUID("abc").has_value(), "UUID parsing should reject strings with the wrong length.");
			context.Expect(!TerraEngine::ParseUUID("000102030405060708090a0b0c0d0e0g").has_value(), "UUID parsing should reject non-hex characters.");
		});
	}
}
