// NavigationAgentAutomationTests.cpp - end-to-end editor automation parity for navigation agents.

#include "../TestHarness.h"

#include "Editor/TerraEngine-Editor/Automation/EditorAutomationService.h"
#include "Editor/TerraEngine-Editor/Core/EditorContext.h"
#include "Editor/TerraEngine-Editor/Project/ProjectManager.h"
#include "Editor/TerraEngine-Editor/Subsystems/UndoRedoSystem.h"
#include "Engine/Components/NavigationAgentComponent.h"
#include "Engine/Components/Transform.h"
#include "Engine/Core/Object/ComponentRegistry.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Navigation/Agents/NavigationAgentSystem.h"
#include "Engine/Navigation/Geometry/NavigationGeometryFormat.h"
#include "Engine/Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Engine/Navigation/NavigationSystem.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Scene/Serializer.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace TerraEngine::EditorTests
{
	using TerraEngine::Editor::Automation::EditorAutomationService;
	using TerraEngine::Editor::Automation::WriteMode;

	namespace
	{
		// This fixture opens the smallest real project and scene accepted by the
		// editor service. It ensures the test dispatches the public JSON tools,
		// rather than calling a private handler or substituting a mock registry.
		class NavigationAutomationFixture
		{
		public:
			NavigationAutomationFixture()
				: root(std::filesystem::temp_directory_path() / "TerraEngineNavigationAutomation" / ToHexString(GenerateUUID()))
			{
				UndoRedoSystem::Instance().Clear();
				std::filesystem::create_directories(root);
				context.projectManager = std::make_shared<ProjectManager>();
				std::string error;
				ready = context.projectManager->CreateProject(root, "Agent Commands", ProjectTemplate::Blank, &error);
				if(!ready) return;

				const std::filesystem::path scenePath = context.projectManager->GetProjectRoot() / "Assets/Scenes/NavigationAutomation.tescene";
				std::filesystem::create_directories(scenePath.parent_path());
				std::ofstream scene(scenePath, std::ios::binary);
				scene << nlohmann::json{{"format", "TerraSceneUnboundedV1"}, {"name", "Navigation Automation"},
					{"sceneSettings", nlohmann::json::object()}, {"objects", nlohmann::json::array()}}.dump(2);
				scene.close();

				SceneManager::Instance().UnloadAll();
				SceneManager::Instance().SetSceneRoot(context.projectManager->GetProjectRoot());
				const UUID sceneId = SceneManager::Instance().LoadScene(scenePath);
				SceneManager::Instance().SetActiveScene(sceneId);
				service.SetEditorContext(&context);
			}

			~NavigationAutomationFixture()
			{
				UndoRedoSystem::Instance().Clear();
				ObjectManager::instance().clear();
				SceneManager::Instance().UnloadAll();
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
			}

			std::filesystem::path root;
			EditorContext context;
			EditorAutomationService service;
			bool ready = false;
		};

		std::shared_ptr<const Navigation::NavigationGeometrySnapshot> FlatTerrain()
		{
			Navigation::NavigationGeometryDocument document{};
			document.kind = Navigation::NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{-16.0F, 0.0F, -16.0F}, {16.0F, 0.0F, 16.0F},
				{16.0F, 0.0F, -16.0F}, {-16.0F, 0.0F, 16.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			Navigation::NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceRevision = 1U;
			descriptor.canonicalPlacementRevision = 1U;
			descriptor.physicalReadyRevision = 1U;
			descriptor.sourceKind = Navigation::NavigationSourceKind::Terrain;
			return Navigation::NavigationGeometrySnapshot::Create(descriptor, document).snapshot;
		}

		bool PrepareTerrain(Navigation::NavigationSystem& navigation)
		{
			const auto terrain = FlatTerrain();
			if(!terrain || !navigation.QueueInitialGroundBuild(Navigation::NavigationProfile::Defaults(), {terrain})) return false;
			navigation.Update(0.0F);
			const auto world = navigation.PublishedWorld();
			return world != nullptr && world->terrainState == Navigation::NavigationTerrainState::Available;
		}

		void Advance(Navigation::NavigationSystem& navigation, Navigation::NavigationAgentSystem& agents, std::uint32_t frames)
		{
			for(std::uint32_t frame = 0U; frame < frames; ++frame)
			{
				navigation.Update(1.0F / 60.0F);
				agents.Update(1.0F / 60.0F);
			}
		}
	}

	void RegisterNavigationAgentAutomationTests(TestRegistry& registry)
	{
		registry.Add("Editor.Navigation.AgentAutomation.DispatchesGenerationQualifiedRuntimeCommands", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown();
			agents.Shutdown();
			navigation.Initialize();
			agents.Initialize();
			NavigationAutomationFixture fixture;
			context.Expect(fixture.ready, "The editor automation fixture must create its isolated project before dispatching tools.");
			if(!fixture.ready)
			{
				agents.Shutdown();
				navigation.Shutdown();
				return;
			}
			const Object owner = objects.createObject("Automation Agent");
			(void)objects.AddComponent<Transform>(owner);
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			const bool terrainReady = PrepareTerrain(navigation);
			Advance(navigation, agents, 2U);
			const auto world = navigation.PublishedWorld();

			const nlohmann::json bind = fixture.service.HandleTool("navigation.agent.bind", {
				{"objectId", ToHexString(objects.getUUID(owner))}}, WriteMode::ReadOnly, "navigation-agent-bind");
			context.Expect(bind.value("ok", false) && bind["data"].contains("agent"),
				"The public bind tool must resolve only the already-published agent snapshot.");
			if(!terrainReady || !world || !bind.value("ok", false))
			{
				context.Expect(false, "A flat terrain world and a published navigation agent are required for editor automation parity.");
				objects.clear();
				agents.Shutdown();
				navigation.Shutdown();
				return;
			}

			const nlohmann::json agent = bind["data"]["agent"];
			Navigation::NavigationLocation destination{};
			destination.world = world->world;
			destination.position.offsetMetres = {6.0, 0.0, 0.0};
			const nlohmann::json setDestination = fixture.service.HandleTool("navigation.agent.destination.location", {
				{"agent", agent}, {"location", {{"worldId", world->world.worldId.value}, {"worldGeneration", world->world.worldGeneration},
					{"position", Serializer::SerializeWorldPosition(destination.position)}}},
				{"producerOrderingToken", 71U}, {"correlationId", 7001U}}, WriteMode::WritesEnabled, "navigation-agent-location");
			const nlohmann::json destinationReceipt = setDestination.value("data", nlohmann::json::object())
				.value("commitReceipt", nlohmann::json::object());
			context.Expect(setDestination.value("ok", false)
				&& destinationReceipt.value("state", std::string{}) == "Queued",
				"The JSON destination command must report Queued until the protected agent boundary applies it.");

			const nlohmann::json queuedReceipt = fixture.service.HandleTool("navigation.agent.receipt", {
				{"agent", agent}, {"correlationId", 7001U}}, WriteMode::ReadOnly, "navigation-agent-receipt");
			const nlohmann::json queuedStatus = queuedReceipt.value("data", nlohmann::json::object())
				.value("status", nlohmann::json::object());
			context.Expect(queuedReceipt.value("ok", false)
				&& queuedStatus.value("state", std::string{}) == "Queued",
				"Receipt reads must preserve the exact queued-versus-applied command state.");

			Advance(navigation, agents, 2U);
			const nlohmann::json appliedReceipt = fixture.service.HandleTool("navigation.agent.receipt", {
				{"agent", agent}, {"correlationId", 7001U}}, WriteMode::ReadOnly, "navigation-agent-applied-receipt");
			const nlohmann::json appliedStatus = appliedReceipt.value("data", nlohmann::json::object())
				.value("status", nlohmann::json::object());
			context.Expect(appliedReceipt.value("ok", false)
				&& appliedStatus.value("state", std::string{}) == "Applied",
				"A later receipt read must report Applied only after the protected agent boundary has committed the queued intent.");

			// The high word makes this an impossible exact precondition. A 32-bit JSON
			// conversion would turn it into the wildcard zero and incorrectly apply it.
			const nlohmann::json mismatchedPrecondition = fixture.service.HandleTool("navigation.agent.destination.location", {
				{"agent", agent}, {"location", {{"worldId", world->world.worldId.value}, {"worldGeneration", world->world.worldGeneration},
					{"position", Serializer::SerializeWorldPosition(destination.position)}}},
				{"expectedConfigurationRevision", (std::uint64_t{1} << 32U)},
				{"producerOrderingToken", 73U}, {"correlationId", 7003U}}, WriteMode::WritesEnabled, "navigation-agent-wide-precondition");
			Advance(navigation, agents, 2U);
			const nlohmann::json rejectedReceipt = fixture.service.HandleTool("navigation.agent.receipt", {
				{"agent", agent}, {"correlationId", 7003U}}, WriteMode::ReadOnly, "navigation-agent-wide-precondition-receipt");
			const nlohmann::json rejectedStatus = rejectedReceipt.value("data", nlohmann::json::object())
				.value("status", nlohmann::json::object());
			context.Expect(mismatchedPrecondition.value("ok", false)
				&& rejectedReceipt.value("ok", false)
				&& rejectedStatus.value("state", std::string{}) == "Rejected",
				"A high-word configuration precondition must survive JSON parsing and reject rather than becoming a wildcard command.");
			const nlohmann::json malformedPrecondition = fixture.service.HandleTool("navigation.agent.stop", {
				{"agent", agent}, {"expectedConfigurationRevision", "not-a-generation"},
				{"producerOrderingToken", 74U}, {"correlationId", 7004U}}, WriteMode::WritesEnabled, "navigation-agent-malformed-precondition");
			context.Expect(!malformedPrecondition.value("ok", true)
				&& malformedPrecondition.value("error", nlohmann::json::object()).value("category", std::string{}) == "validation",
				"Malformed generation strings must return a validation response instead of escaping as a JSON conversion exception.");

			const nlohmann::json expiredReceipt = fixture.service.HandleTool("navigation.agent.receipt", {
				{"agent", agent}, {"correlationId", 7999U}}, WriteMode::ReadOnly, "navigation-agent-expired-receipt");
			const nlohmann::json expiredStatus = expiredReceipt.value("data", nlohmann::json::object())
				.value("status", nlohmann::json::object());
			context.Expect(expiredReceipt.value("ok", false)
				&& expiredStatus.value("state", std::string{}) == "Expired"
				&& expiredStatus.value("outcome", std::string{}) == "Expired"
				&& expiredStatus.value("correlationId", std::uint64_t{0}) == 7999U,
				"A missing bounded receipt must remain an explicit Expired result with its caller correlation retained.");

			// The route request is asynchronous. Advance the same bounded maximum
			// used by native agent tests before querying the public JSON guidance.
			Advance(navigation, agents, 64U);
			const nlohmann::json guidance = fixture.service.HandleTool("navigation.agent.guidance", {{"agent", agent}},
				WriteMode::ReadOnly, "navigation-agent-guidance");
			const nlohmann::json status = fixture.service.HandleTool("navigation.agent.status", {{"agent", agent}},
				WriteMode::ReadOnly, "navigation-agent-status");
			const nlohmann::json guidanceData = guidance.value("data", nlohmann::json::object())
				.value("guidance", nlohmann::json::object());
			context.Expect(guidance.value("ok", false) && status.value("ok", false)
				&& guidanceData.value("usable", false)
				&& guidanceData.contains("identity") && guidanceData.contains("observation")
				&& guidanceData.contains("destination") && guidanceData.contains("routeGeneration")
				&& guidanceData["identity"] == agent,
				"Guidance reads must expose terrain-backed advisory direction with the full generation-qualified route and observation contract.");

			const nlohmann::json stop = fixture.service.HandleTool("navigation.agent.stop", {
				{"agent", agent}, {"producerOrderingToken", 72U}, {"correlationId", 7002U}},
				WriteMode::WritesEnabled, "navigation-agent-stop");
			Advance(navigation, agents, 2U);
			const nlohmann::json stoppedStatus = fixture.service.HandleTool("navigation.agent.status", {{"agent", agent}},
				WriteMode::ReadOnly, "navigation-agent-stopped-status");
			const nlohmann::json stopReceipt = stop.value("data", nlohmann::json::object())
				.value("commitReceipt", nlohmann::json::object());
			const nlohmann::json terminalStatus = stoppedStatus.value("data", nlohmann::json::object())
				.value("status", nlohmann::json::object());
			context.Expect(stop.value("ok", false)
				&& stopReceipt.value("state", std::string{}) == "Queued"
				&& stoppedStatus.value("ok", false)
				&& terminalStatus.value("state", std::string{}) == "Stopped",
				"Stop must queue against the original identity and clear following state without resolving a fresh object attachment.");
			context.Expect(!UndoRedoSystem::Instance().CanUndo(),
				"Runtime navigation-agent commands queue transient intent only and must never create editor history entries.");

			objects.clear();
			agents.Shutdown();
			navigation.Shutdown();
		});
	}
}
