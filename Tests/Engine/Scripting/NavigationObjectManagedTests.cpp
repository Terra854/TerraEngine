// NavigationObjectManagedTests.cpp - C# object-agent ABI and public surface.

#include "../TestHarness.h"

#include "Components/Navigation/NavigationAgentComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Navigation/NavigationSystem.h"
#include "Scripting/ScriptGlue.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::Tests
{
	namespace
	{
		std::string ReadSource(const char* relativePath)
		{
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath, std::ios::binary);
			std::ostringstream output;
			output << input.rdbuf();
			return output.str();
		}
	}

	void RegisterNavigationObjectManagedTests(TestRegistry& registry)
	{
		registry.Add("Navigation.ObjectManaged.NativeAbiResolvesAndReadsObjectAgent", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& system = Navigation::NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();
			const Object object = objects.createObject("Managed Navigation Object");
			(void)objects.AddComponent<Transform>(object);
			(void)objects.AddComponent<NavigationAgentComponent>(object);
			system.SynchronizeSceneObjectAgents(objects);

			NativeNavigationAgentSnapshot snapshot{};
			snapshot.header = {kNativeNavigationAbiVersion, sizeof(snapshot)};
			const NativeNavigationStatus resolved = Native_Navigation_ResolveObjectAgent(object.entityValue, &snapshot);
			context.Expect(resolved == NativeNavigationStatus::Success && snapshot.agent.owner.ownerId != 0U
				&& snapshot.agent.slotGeneration != 0U && snapshot.poseGeneration == 1U,
				"Managed gameplay must resolve copied runtime state directly from its ordinary Object ID.");

			NativeNavigationAgentSnapshot reread{};
			reread.header = {kNativeNavigationAbiVersion, sizeof(reread)};
			context.Expect(Native_Navigation_ReadAgent(&snapshot.agent, &reread) == NativeNavigationStatus::Success
				&& reread.agent.slotGeneration == snapshot.agent.slotGeneration,
				"The resolved generation-qualified handle must support deterministic state polling.");
			context.Expect(Native_Navigation_ReleaseAgent(&snapshot.agent) == NativeNavigationStatus::Success,
				"Managed wrappers must be able to release retained agent state explicitly.");
			reread.header = {kNativeNavigationAbiVersion, sizeof(reread)};
			context.Expect(Native_Navigation_ReadAgent(&snapshot.agent, &reread) == NativeNavigationStatus::StaleTarget,
				"A released generation must become stale rather than aliasing a future object agent.");
			system.Shutdown();
			objects.clear();
		});

		registry.Add("Navigation.ObjectManaged.CSharpObjectCanSetWorldDestination", [](TestContext& context) {
			const std::string agent = ReadSource("Engine/Managed/Source/Navigation/NavigationAgent.cs");
			const std::string component = ReadSource("Engine/Managed/Source/Navigation/NavigationAgentComponent.cs");
			const std::string facade = ReadSource("Engine/Managed/Source/Navigation/Navigation.cs");
			context.Expect(agent.find("Resolve(Object value)") != std::string::npos
				&& agent.find("SetDestination(WorldPosition destination") != std::string::npos
				&& component.find("RuntimeAgent") != std::string::npos,
				"The installed C# surface must let gameplay resolve an object's component and set a world-space destination.");
			context.Expect(facade.find("ResolveObjectAgent") != std::string::npos
				&& facade.find("ReadAgent") != std::string::npos
				&& facade.find("ReleaseAgent") != std::string::npos,
				"C# object wrappers must expose copied resolve/read/release operations over the native ABI.");
		});
	}
}
