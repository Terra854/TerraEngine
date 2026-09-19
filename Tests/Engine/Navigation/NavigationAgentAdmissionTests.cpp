// Admission contracts for direct and NavigationAgent route requests.
#include "../TestHarness.h"

#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationSystem.h"
#include "Core/Object/Object.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::Tests
{
	using namespace TerraEngine::Navigation;

	namespace
	{
		std::shared_ptr<const NavigationGeometrySnapshot> FlatTerrain()
		{
			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{-32.0F, 0.0F, -32.0F}, {32.0F, 0.0F, 32.0F},
				{32.0F, 0.0F, -32.0F}, {-32.0F, 0.0F, 32.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceRevision = 1U;
			descriptor.canonicalPlacementRevision = 1U;
			descriptor.physicalReadyRevision = 1U;
			descriptor.sourceKind = NavigationSourceKind::Terrain;
			return NavigationGeometrySnapshot::Create(descriptor, document).snapshot;
		}

		bool PrepareTerrain(NavigationSystem& system)
		{
			const auto terrain = FlatTerrain();
			if(!terrain || !system.QueueInitialGroundBuild(NavigationProfile::Defaults(), {terrain})) return false;
			system.Update(0.0F);
			return system.PublishedWorld() != nullptr;
		}

		NavigationLocation Destination(NavigationWorldHandle world, double x)
		{
			NavigationLocation location{};
			location.world = world;
			location.position.offsetMetres = {x, 0.0, 0.0};
			return location;
		}
	}

	void RegisterNavigationAgentAdmissionTests(TestRegistry& registry)
	{
		registry.Add("Navigation.AgentAdmission.DirectAndAgentReplacementNamespacesAreDisjoint", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance(); objects.clear();
			NavigationSystem& system = NavigationSystem::Instance(); system.Shutdown(); system.Initialize();
			const Object source = objects.createObject("Admission Namespace Source");
			(void)objects.AddComponent<Transform>(source);
			const bool prepared = PrepareTerrain(system);
			const auto world = system.PublishedWorld();
			const UUID sourceId = objects.getUUID(source);
			const NavigationObjectIdentity sourceIdentity{sourceId, objects.getObjectLifetimeGeneration(source)};
			const std::uint64_t agentKey = 0xA600000000000000ULL
				| (MakeObjectStableHash(sourceId) & 0x00FFFFFFFFFFFFFFULL);
			const auto direct = prepared && world ? system.RequestRoute(objects, sourceId,
				Destination(world->world, 5.0), NavigationProfile::Defaults(),
				NavigationRoutePolicy::ExactDestination, agentKey) : NavigationSystem::ObjectRouteReceipt{};
			const auto agent = prepared && world ? system.RequestAgentRoute(objects, sourceIdentity,
				Destination(world->world, 6.0), NavigationProfile::Defaults()) : NavigationSystem::ObjectRouteReceipt{};
			const auto directRead = system.ReadRequest(direct.request);
			context.Expect(direct.Succeeded() && agent.Succeeded() && directRead.record.has_value()
				&& directRead.record->outcome != NavigationOutcome::Cancelled,
				"An arbitrary direct replacement token that matches the agent key bit pattern must not supersede an agent or be superseded by it.");
			if(direct.Succeeded()) { (void)system.CancelRequest(direct.request); (void)system.ReleaseRequestResult(direct.request); }
			if(agent.Succeeded()) { (void)system.CancelRequest(agent.request); (void)system.ReleaseRequestResult(agent.request); }
			system.Shutdown(); objects.clear();
		});

		registry.Add("Navigation.AgentAdmission.ReclaimsOneThousandCycledResults", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance(); objects.clear();
			NavigationSystem& system = NavigationSystem::Instance(); system.Shutdown(); system.Initialize();
			const Object source = objects.createObject("Admission Reclamation Source");
			(void)objects.AddComponent<Transform>(source);
			const bool prepared = PrepareTerrain(system);
			const auto world = system.PublishedWorld();
			const NavigationObjectIdentity sourceIdentity{objects.getUUID(source), objects.getObjectLifetimeGeneration(source)};
			std::size_t agentAdmitted = 0U;
			std::size_t agentReleased = 0U;
			for(std::uint32_t cycle = 0U; cycle < 1000U && prepared && world; ++cycle) {
				const auto receipt = system.RequestAgentRoute(objects, sourceIdentity,
					Destination(world->world, 4.0 + static_cast<double>(cycle % 8U)), NavigationProfile::Defaults());
				if(!receipt.Succeeded()) continue;
				++agentAdmitted;
				(void)system.CancelRequest(receipt.request);
				if(system.ReleaseRequestResult(receipt.request)) ++agentReleased;
			}
			context.Expect(agentAdmitted == 1000U && agentReleased == 1000U,
				"Agent route replacement and release must reclaim every result in a 1,000-cycle workload without exhausting the bounded store.");
			system.Shutdown(); objects.clear();
		});

		registry.Add("Navigation.AgentAdmission.DirectRequestsLeaveReservedAgentCapacity", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance(); objects.clear();
			NavigationSystem& system = NavigationSystem::Instance(); system.Shutdown(); system.Initialize();
			const bool prepared = PrepareTerrain(system);
			const auto world = system.PublishedWorld();
			std::vector<NavigationRequestHandle> directRequests;
			directRequests.reserve(768U);
			std::size_t directAdmitted = 0U;
			for(std::uint32_t index = 0U; index < 768U && prepared && world; ++index) {
				Ground::GroundRouteQuery::Request request{};
				request.owner = {world->world, NavigationOwnerId{index + 1U}, 1U};
				request.start = Destination(world->world, 0.0);
				request.destination = Destination(world->world, 4.0);
				request.profile = NavigationProfile::Defaults();
				request.maxExpansions = 128U;
				const NavigationRequestHandle handle = system.SubmitOneShotRoute(std::move(request));
				if(handle.IsValid()) { ++directAdmitted; directRequests.push_back(handle); }
			}
		Ground::GroundRouteQuery::Request overflowRequest{};
		if(world) {
			overflowRequest.owner = {world->world, NavigationOwnerId{9001U}, 1U};
			overflowRequest.start = Destination(world->world, 0.0);
			overflowRequest.destination = Destination(world->world, 4.0);
		}
		overflowRequest.profile = NavigationProfile::Defaults(); overflowRequest.maxExpansions = 128U;
		const NavigationRequestHandle overflow = prepared && world ? system.SubmitOneShotRoute(std::move(overflowRequest)) : NavigationRequestHandle{};
			const Object agentOne = objects.createObject("Reserved Agent One");
			const Object agentTwo = objects.createObject("Reserved Agent Two");
			(void)objects.AddComponent<Transform>(agentOne); (void)objects.AddComponent<Transform>(agentTwo);
			const auto firstAgent = prepared && world ? system.RequestAgentRoute(objects,
				{objects.getUUID(agentOne), objects.getObjectLifetimeGeneration(agentOne)}, Destination(world->world, 5.0), NavigationProfile::Defaults()) : NavigationSystem::ObjectRouteReceipt{};
			const auto secondAgent = prepared && world ? system.RequestAgentRoute(objects,
				{objects.getUUID(agentTwo), objects.getObjectLifetimeGeneration(agentTwo)}, Destination(world->world, 6.0), NavigationProfile::Defaults()) : NavigationSystem::ObjectRouteReceipt{};
			context.Expect(directAdmitted == 768U && !overflow.IsValid()
				&& firstAgent.Succeeded() && secondAgent.Succeeded(),
				"Point/direct admission must stop at the shared reservation boundary while reserved agent slots remain available.");
			for(const NavigationRequestHandle request : directRequests) {
				(void)system.CancelRequest(request); (void)system.ReleaseRequestResult(request);
			}
			if(firstAgent.Succeeded()) { (void)system.CancelRequest(firstAgent.request); (void)system.ReleaseRequestResult(firstAgent.request); }
			if(secondAgent.Succeeded()) { (void)system.CancelRequest(secondAgent.request); (void)system.ReleaseRequestResult(secondAgent.request); }
			system.Shutdown(); objects.clear();
		});
	}
}
