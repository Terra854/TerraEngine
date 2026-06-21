#include "ScriptingRuntimeTestFixtures.h"

#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scripting/ScriptingSystem.h"

namespace TerraEngine::Tests {
	ScriptingRuntimeTestScope::ScriptingRuntimeTestScope()
	{
		ObjectManager::instance().clear();
		TerraEngine::Scene::SceneMutationQueue::Instance().Clear();
		ScriptingSystem::ClearCurrentScriptDispatchContext();
		ScriptingSystem::SetScriptingDiagnosticsRequested(false);
	}

	ScriptingRuntimeTestScope::~ScriptingRuntimeTestScope()
	{
		ScriptingSystem::ClearCurrentScriptDispatchContext();
		TerraEngine::Scene::SceneMutationQueue::Instance().Clear();
		ObjectManager::instance().clear();
	}

	Object CreateScriptedRuntimeObject(std::string_view objectName, std::string_view className)
	{
		Object object = ObjectManager::instance().createObject(std::string(objectName));
		ScriptComponent& component = object.addComponent<ScriptComponent>();
		component.AddBinding(TerraEngine::EMPTY_UUID, std::string(className));
		return object;
	}

	std::uint64_t CaptureObjectMetadataGeneration(const Object& object)
	{
		return ObjectManager::instance().getObjectMetadataGeneration(object);
	}

	std::uint64_t CaptureObjectComponentGeneration(const Object& object)
	{
		return ObjectManager::instance().getObjectComponentGeneration(object);
	}

	std::uint64_t CaptureObjectBindingGeneration(const Object& object)
	{
		return ObjectManager::instance().getObjectBindingGeneration(object);
	}
}
