// NavigationAgentSchedulerTests.cpp - scheduler declaration tests for agent work.
//
// This test is registered with the agent runtime work, after the system header
// exists. Its checks exercise scheduler data rather than implementation text.

#include "../TestHarness.h"

#include "Core/Scheduling/SystemScheduler.h"
#include "Core/Scheduling/JobGraph.h"
#include "Core/SystemBase.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Components/NavigationAgentComponent.h"
#include "Components/Transform.h"
#include "Navigation/Agents/NavigationAgentCommandSystem.h"
#include "Navigation/Agents/NavigationAgentSystem.h"
#include "Navigation/Geometry/NavigationGeometryFormat.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationSystem.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		bool Contains(const std::vector<std::string>& values, std::string_view expected) {
			return std::find(values.begin(), values.end(), expected) != values.end();
		}

		const Scheduling::FrameWorkloadDeclaration* FindWorkload(
			const std::vector<Scheduling::FrameWorkloadDeclaration>& workloads, std::string_view descriptor) {
			const auto found = std::find_if(workloads.begin(), workloads.end(), [descriptor](const auto& workload) {
				return workload.stableDescriptorId == descriptor;
			});
			return found == workloads.end() ? nullptr : &*found;
		}

		std::shared_ptr<const Navigation::NavigationGeometrySnapshot> FlatTerrain() {
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

		bool PrepareTerrain(Navigation::NavigationSystem& navigation) {
			const auto terrain = FlatTerrain();
			if(!terrain || !navigation.QueueInitialGroundBuild(Navigation::NavigationProfile::Defaults(), {terrain}))
				return false;
			navigation.Update(0.0F);
			const auto world = navigation.PublishedWorld();
			return world && world->terrainState == Navigation::NavigationTerrainState::Available;
		}

		Navigation::NavigationLocation Location(const Navigation::NavigationWorldHandle& world, double x) {
			Navigation::NavigationLocation location{};
			location.world = world;
			location.position.offsetMetres = {x, 0.0, 0.0};
			return location;
		}

		void Advance(Navigation::NavigationSystem& navigation, Navigation::NavigationAgentSystem& agents,
			std::uint32_t frames = 1U) {
			for(std::uint32_t frame = 0U; frame < frames; ++frame) {
				navigation.Update(1.0F / 60.0F);
				agents.Update(1.0F / 60.0F);
			}
		}

		// Only scheduler metadata is needed for these external ordering anchors. The
		// real WorldSpatial/Scripting implementations are intentionally not started
		// by a focused navigation unit test.
		class SchedulerAnchor final : public SystemsBase {
		public:
			explicit SchedulerAnchor(std::string name) : name_(std::move(name)) {}
			void Initialize() override {}
			void Update(float) override {}
			void Shutdown() override {}
			std::string SystemName() override { return name_; }
			Scheduling::SystemDeclaration SchedulerDeclaration() override { return {}; }

		private:
			std::string name_;
		};

		// Build every system declaration before running the early command boundary.
		// This mirrors the real scheduler's frame construction and catches a demand
		// hint that would otherwise look correct only when Navigation is declared late.
		std::vector<Scheduling::FrameWorkloadDeclaration> BuildAgentNavigationFrame(
			Scheduling::SystemScheduler& scheduler, Navigation::NavigationAgentCommandSystem& commands,
			Navigation::NavigationSystem& navigation, Navigation::NavigationAgentSystem& agents,
			SchedulerAnchor& worldSpatial, SchedulerAnchor& scripting) {
			Scheduling::SystemDeclaration worldDeclaration{};
			worldDeclaration.name = "WorldSpatialSystem";
			worldDeclaration.phase = "Scene";
			worldDeclaration.omitWhenNoFrameWorkloads = true;
			Scheduling::SystemDeclaration scriptingDeclaration{};
			scriptingDeclaration.name = "ScriptingSystem";
			scriptingDeclaration.phase = "Scripting";
			scriptingDeclaration.omitWhenNoFrameWorkloads = true;
			scheduler.RegisterSystem(worldSpatial, std::move(worldDeclaration));
			scheduler.RegisterSystem(commands, commands.SchedulerDeclaration());
			scheduler.RegisterSystem(navigation, navigation.SchedulerDeclaration());
			scheduler.RegisterSystem(agents, agents.SchedulerDeclaration());
			scheduler.RegisterSystem(scripting, std::move(scriptingDeclaration));
			return scheduler.BuildFrameWorkloadDeclarations(0.0F);
		}

		void RunWorkload(const Scheduling::FrameWorkloadDeclaration* workload) {
			if(workload == nullptr || !workload->work) return;
			Scheduling::JobGraphContext context{};
			workload->work(context);
		}
	}

	void RegisterNavigationAgentSchedulerTests(TestRegistry& registry) {
		registry.Add("Navigation.AgentScheduler.DeclaresOrderedCapturePrepareAndPublish", [](TestContext& context) {
			using namespace TerraEngine::Scheduling;
			Navigation::NavigationAgentCommandSystem& commandSystem =
				Navigation::NavigationAgentCommandSystem::Instance();
			const SystemDeclaration commandDeclaration = commandSystem.SchedulerDeclaration();
			Navigation::NavigationAgentSystem& system = Navigation::NavigationAgentSystem::Instance();
			const SystemDeclaration declaration = system.SchedulerDeclaration();
			context.ExpectEqual(std::string("NavigationAgentSystem"), declaration.name,
				"The optional agent coordinator must expose one stable scheduler identity.");
			context.ExpectEqual(std::string("Navigation"), declaration.phase,
				"Agent state must publish in Navigation after route publication and before scripts observe it.");
			context.Expect(Contains(declaration.after, "NavigationSystem")
				&& Contains(declaration.before, "ScriptingSystem"),
				"Agent scheduling must order its capture after routes and its guidance publication before gameplay scripting.");
			context.Expect(!declaration.mainThread
				&& declaration.phaseKind == WorkerSystemPhaseKind::WorkerPrepare
				&& declaration.boundaryClassification == HardBoundaryClassification::PartiallyWorkerSafe
				&& declaration.hardBoundary,
				"Agent work must name worker preparation and protected publication instead of treating component access as worker-safe.");
			context.Expect(Contains(declaration.reads, "NavigationAgentCommands")
				&& Contains(declaration.reads, "NavigationAgentCapture")
				&& Contains(declaration.reads, "NavigationAgentPrepared")
				&& Contains(declaration.reads, "NavigationResults")
				&& Contains(declaration.reads, "NavigationRouteValidity"),
				"Agent scheduling must declare copied command, capture, prepared-route, result, and validity inputs.");
			context.Expect(Contains(declaration.writes, "NavigationAgentState")
				&& Contains(declaration.writes, "NavigationAgentGuidance"),
				"Only protected agent publication may write runtime state and immutable gameplay guidance.");
			context.Expect(commandDeclaration.name == "NavigationAgentCommandSystem"
				&& commandDeclaration.phase == "Navigation"
				&& Contains(commandDeclaration.before, "NavigationSystem")
				&& Contains(commandDeclaration.reads, "NavigationAgentCommands")
				&& Contains(commandDeclaration.reads, "ObjectManager")
				&& Contains(commandDeclaration.reads, "NavigationResults")
				&& Contains(commandDeclaration.reads, "NavigationRouteValidity")
				&& Contains(commandDeclaration.writes, "NavigationRequestQueue")
				&& Contains(commandDeclaration.writes, "NavigationResults")
				&& Contains(commandDeclaration.writes, "NavigationRouteValidity")
				&& commandDeclaration.mainThread
				&& commandDeclaration.phaseKind == WorkerSystemPhaseKind::ProtectedCommit,
				"Fresh agent commands must use a separate protected Navigation-stage owner before route capture; agent guidance remains later after Navigation publication.");
		});

		registry.Add("Navigation.AgentScheduler.PredeclaredEarlyCommandAdmitsIntoSameNavigationCapture", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			Navigation::NavigationAgentCommandSystem& commands = Navigation::NavigationAgentCommandSystem::Instance();
			navigation.Shutdown(); agents.Shutdown(); commands.Shutdown();
			navigation.Initialize(); agents.Initialize(); commands.Initialize();
			const Object owner = objects.createObject("Early command scheduler owner");
			(void)objects.AddComponent<Transform>(owner);
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			const bool terrainReady = PrepareTerrain(navigation);
			agents.Update(0.0F);
			const Navigation::NavigationAgentIdentity identity = agents.FindIdentity(objects, objects.getUUID(owner));
			const std::shared_ptr<const Navigation::NavigationWorldRoot> world = navigation.PublishedWorld();
			const Navigation::NavigationAgentReceipt queued = terrainReady && identity.IsValid() && world
				? agents.QueueLocationDestination(identity, Location(world->world, 6.0),
					0U, 0U, 1U, 9101U)
				: Navigation::NavigationAgentReceipt{};

			Scheduling::SystemScheduler scheduler;
			SchedulerAnchor worldSpatial{"WorldSpatialSystem"};
			SchedulerAnchor scripting{"ScriptingSystem"};
			const auto workloads = BuildAgentNavigationFrame(scheduler, commands, navigation, agents,
				worldSpatial, scripting);
			const auto plan = Scheduling::ValidateFrameWorkloads(workloads, "navigation-agent-early-command-frame");
			const auto* command = FindWorkload(workloads,
				"NavigationAgentCommandSystem/Navigation/CommandCapture");
			const auto* navigationCapture = FindWorkload(workloads, "NavigationSystem/Navigation/Capture");
			const auto* navigationPrepare = FindWorkload(workloads, "NavigationSystem/Navigation/Prepare");
			RunWorkload(command);
			RunWorkload(navigationCapture);
			const Navigation::NavigationSystem::StoreUsageSnapshot usage = navigation.PublishedStoreUsage();
			const auto applied = agents.ReadReceipt(identity, 9101U);

			context.Expect(plan.valid && command != nullptr && navigationCapture != nullptr
				&& navigationPrepare != nullptr && navigationPrepare->estimatedWorkItemCount >= 1U
				&& queued.state == Navigation::NavigationAgentReceiptState::Queued
				&& applied && applied->state == Navigation::NavigationAgentReceiptState::Applied
				&& usage.activeRequests == 1U && usage.inFlightRoutes == 1U
				&& usage.agentRequestHandles == 1U,
				"A destination queued before real frame declarations must keep Navigation capture live, then enter that same frame's bounded route packet after the early command boundary.");
			commands.Shutdown(); agents.Shutdown(); navigation.Shutdown(); objects.clear();
		});

		registry.Add("Navigation.AgentScheduler.StopOnlyDemandDoesNotCreatePhantomRouteWork", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			Navigation::NavigationAgentCommandSystem& commands = Navigation::NavigationAgentCommandSystem::Instance();
			navigation.Shutdown(); agents.Shutdown(); commands.Shutdown();
			navigation.Initialize(); agents.Initialize(); commands.Initialize();
			const Object owner = objects.createObject("Stop-only command owner");
			(void)objects.AddComponent<Transform>(owner);
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			agents.Update(0.0F);
			const Navigation::NavigationAgentIdentity identity = agents.FindIdentity(objects, objects.getUUID(owner));
			// The protected command workload remains available for lifecycle safety, but
			// an empty queue must not make Navigation declare a speculative route packet.
			Scheduling::SystemScheduler emptyScheduler;
			SchedulerAnchor emptyWorldSpatial{"WorldSpatialSystem"};
			SchedulerAnchor emptyScripting{"ScriptingSystem"};
			const auto emptyWorkloads = BuildAgentNavigationFrame(emptyScheduler, commands, navigation, agents,
				emptyWorldSpatial, emptyScripting);
			RunWorkload(FindWorkload(emptyWorkloads,
				"NavigationAgentCommandSystem/Navigation/CommandCapture"));
			const Navigation::NavigationSystem::StoreUsageSnapshot emptyUsage = navigation.PublishedStoreUsage();
			const Navigation::NavigationAgentReceipt stop = identity.IsValid()
				? agents.QueueStop(identity, 0U, 0U, 1U, 9102U) : Navigation::NavigationAgentReceipt{};
			Scheduling::SystemScheduler scheduler;
			SchedulerAnchor worldSpatial{"WorldSpatialSystem"};
			SchedulerAnchor scripting{"ScriptingSystem"};
			const auto workloads = BuildAgentNavigationFrame(scheduler, commands, navigation, agents,
				worldSpatial, scripting);
			RunWorkload(FindWorkload(workloads, "NavigationAgentCommandSystem/Navigation/CommandCapture"));
			const auto applied = agents.ReadReceipt(identity, 9102U);
			const Navigation::NavigationSystem::StoreUsageSnapshot usage = navigation.PublishedStoreUsage();

			context.Expect(FindWorkload(emptyWorkloads,
				"NavigationAgentCommandSystem/Navigation/CommandCapture") != nullptr
				&& FindWorkload(emptyWorkloads, "NavigationSystem/Navigation/Capture") == nullptr
				&& emptyUsage.activeRequests == 0U && emptyUsage.pendingRoutes == 0U
				&& stop.state == Navigation::NavigationAgentReceiptState::Queued
				&& FindWorkload(workloads, "NavigationSystem/Navigation/Capture") == nullptr
				&& applied && applied->state == Navigation::NavigationAgentReceiptState::Applied
				&& agents.ReadStatus(identity).state == Navigation::NavigationAgentState::Stopped
				&& usage.activeRequests == 0U && usage.pendingRoutes == 0U && usage.inFlightRoutes == 0U,
				"Stop-only command traffic must execute at its protected boundary but never manufacture a Navigation route declaration or request.");
			commands.Shutdown(); agents.Shutdown(); navigation.Shutdown(); objects.clear();
		});

		registry.Add("Navigation.AgentScheduler.RevocationClearsPendingAdmissionDemand", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			Navigation::NavigationAgentCommandSystem& commands = Navigation::NavigationAgentCommandSystem::Instance();
			navigation.Shutdown(); agents.Shutdown(); commands.Shutdown();
			navigation.Initialize(); agents.Initialize(); commands.Initialize();
			const Object owner = objects.createObject("Revoked command owner");
			(void)objects.AddComponent<Transform>(owner);
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			agents.Update(0.0F);
			const Navigation::NavigationAgentIdentity identity = agents.FindIdentity(objects, objects.getUUID(owner));
			const std::shared_ptr<const Navigation::NavigationWorldRoot> world = navigation.PublishedWorld();
			const Navigation::NavigationAgentReceipt queued = identity.IsValid() && world
				? agents.QueueLocationDestination(identity, Location(world->world, 4.0), 0U, 0U, 1U, 9106U)
				: Navigation::NavigationAgentReceipt{};
			agents.RevokeAllAgents(Navigation::NavigationReason::SceneReplaced);
			Scheduling::SystemScheduler scheduler;
			SchedulerAnchor worldSpatial{"WorldSpatialSystem"};
			SchedulerAnchor scripting{"ScriptingSystem"};
			const auto workloads = BuildAgentNavigationFrame(scheduler, commands, navigation, agents,
				worldSpatial, scripting);
			const auto receipt = agents.ReadReceipt(identity, 9106U);
			const Navigation::NavigationSystem::StoreUsageSnapshot usage = navigation.PublishedStoreUsage();

			context.Expect(queued.state == Navigation::NavigationAgentReceiptState::Queued
				&& receipt && receipt->state == Navigation::NavigationAgentReceiptState::Superseded
				&& FindWorkload(workloads, "NavigationSystem/Navigation/Capture") == nullptr
				&& usage.activeRequests == 0U && usage.pendingRoutes == 0U && usage.inFlightRoutes == 0U,
				"Owner revocation must supersede copied destination commands and clear their declaration hint before a future Navigation frame can admit stale route work.");
			commands.Shutdown(); agents.Shutdown(); navigation.Shutdown(); objects.clear();
		});

		registry.Add("Navigation.AgentScheduler.CoalescedAndRejectedCommandsLeaveOnlyRealRouteDemand", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			Navigation::NavigationAgentCommandSystem& commands = Navigation::NavigationAgentCommandSystem::Instance();
			navigation.Shutdown(); agents.Shutdown(); commands.Shutdown();
			navigation.Initialize(); agents.Initialize(); commands.Initialize();
			const Object owner = objects.createObject("Coalesced command owner");
			(void)objects.AddComponent<Transform>(owner);
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			const bool terrainReady = PrepareTerrain(navigation);
			agents.Update(0.0F);
			const Navigation::NavigationAgentIdentity identity = agents.FindIdentity(objects, objects.getUUID(owner));
			const std::shared_ptr<const Navigation::NavigationWorldRoot> world = navigation.PublishedWorld();
			const auto queueLocation = [&](double x, std::uint64_t correlation) {
				return terrainReady && identity.IsValid() && world
					? agents.QueueLocationDestination(identity, Location(world->world, x),
						0U, 0U, correlation, correlation)
					: Navigation::NavigationAgentReceipt{};
			};
			const Navigation::NavigationAgentReceipt first = queueLocation(2.0, 9103U);
			const Navigation::NavigationAgentReceipt latest = queueLocation(8.0, 9104U);
			Scheduling::SystemScheduler scheduler;
			SchedulerAnchor worldSpatial{"WorldSpatialSystem"};
			SchedulerAnchor scripting{"ScriptingSystem"};
			const auto coalescedWorkloads = BuildAgentNavigationFrame(scheduler, commands, navigation, agents,
				worldSpatial, scripting);
			const auto* coalescedCapture = FindWorkload(coalescedWorkloads, "NavigationSystem/Navigation/Capture");
			const auto* coalescedPrepare = FindWorkload(coalescedWorkloads, "NavigationSystem/Navigation/Prepare");
			RunWorkload(FindWorkload(coalescedWorkloads,
				"NavigationAgentCommandSystem/Navigation/CommandCapture"));
			RunWorkload(coalescedCapture);
			const Navigation::NavigationSystem::StoreUsageSnapshot coalescedUsage = navigation.PublishedStoreUsage();
			const auto firstReceipt = agents.ReadReceipt(identity, 9103U);
			const auto latestReceipt = agents.ReadReceipt(identity, 9104U);

			Navigation::NavigationLocation staleLocation = world
				? Location(world->world, 10.0) : Navigation::NavigationLocation{};
			if(staleLocation.world.IsValid()) ++staleLocation.world.worldGeneration;
			const Navigation::NavigationAgentReceipt stale = identity.IsValid()
				? agents.QueueLocationDestination(identity, staleLocation, 0U, 0U, 3U, 9105U)
				: Navigation::NavigationAgentReceipt{};
			Scheduling::SystemScheduler staleScheduler;
			SchedulerAnchor staleWorldSpatial{"WorldSpatialSystem"};
			SchedulerAnchor staleScripting{"ScriptingSystem"};
			const auto staleWorkloads = BuildAgentNavigationFrame(staleScheduler, commands, navigation, agents,
				staleWorldSpatial, staleScripting);
			RunWorkload(FindWorkload(staleWorkloads,
				"NavigationAgentCommandSystem/Navigation/CommandCapture"));
			RunWorkload(FindWorkload(staleWorkloads, "NavigationSystem/Navigation/Capture"));
			const Navigation::NavigationSystem::StoreUsageSnapshot staleUsage = navigation.PublishedStoreUsage();
			const auto staleReceipt = agents.ReadReceipt(identity, 9105U);

			context.Expect(first.state == Navigation::NavigationAgentReceiptState::Queued
				&& latest.state == Navigation::NavigationAgentReceiptState::Queued
				&& coalescedCapture != nullptr && coalescedPrepare != nullptr
				&& coalescedPrepare->estimatedWorkItemCount == 1U
				&& firstReceipt && firstReceipt->state == Navigation::NavigationAgentReceiptState::Superseded
				&& latestReceipt && latestReceipt->state == Navigation::NavigationAgentReceiptState::Applied
				&& coalescedUsage.activeRequests == 1U && coalescedUsage.inFlightRoutes == 1U
				&& stale.state == Navigation::NavigationAgentReceiptState::Queued
				&& staleReceipt && staleReceipt->state == Navigation::NavigationAgentReceiptState::Rejected
				&& staleUsage.activeRequests == coalescedUsage.activeRequests
				&& staleUsage.inFlightRoutes == coalescedUsage.inFlightRoutes,
				"Coalescing must advertise and admit only the latest destination, while a protected stale-world rejection consumes its conservative hint without adding a phantom route.");
			commands.Shutdown(); agents.Shutdown(); navigation.Shutdown(); objects.clear();
		});

		registry.Add("Navigation.AgentScheduler.WorkersReceiveOnlyCapturedValues", [](TestContext& context) {
			using namespace TerraEngine::Scheduling;
			Navigation::NavigationAgentSystem& system = Navigation::NavigationAgentSystem::Instance();
			system.Initialize();
			system.SetContractTestWorkAvailable(true);
			const std::vector<FrameWorkloadDeclaration> workloads = system.DeclareFrameWorkloads(1.0F / 60.0F);
			// Validate the actual resource graph, not only descriptor labels: all
			// worker partitions must remain independently schedulable without conflicts.
			const auto plan = ValidateFrameWorkloads(workloads, "navigation-agent-partitions");
			context.Expect(plan.valid, "Agent worker partitions must form a valid ordered resource graph.");
			system.SetContractTestWorkAvailable(false);
			system.Shutdown();

			const FrameWorkloadDeclaration* capture = FindWorkload(workloads, "NavigationAgentSystem/Navigation/Capture");
			const FrameWorkloadDeclaration* prepare = FindWorkload(workloads, "NavigationAgentSystem/Navigation/Prepare");
			const FrameWorkloadDeclaration* publish = FindWorkload(workloads, "NavigationAgentSystem/Navigation/Publish");
			context.Expect(capture != nullptr && prepare != nullptr && publish != nullptr,
				"Enabled agents must declare capture, worker preparation, and protected publication workloads.");
			if(capture == nullptr || prepare == nullptr || publish == nullptr) return;
			context.Expect(capture->mainThreadRequired && !capture->workerEligible
				&& capture->phaseKind == WorkerSystemPhaseKind::ProtectedCommit,
				"Agent capture must resolve component/lifetime/pose values at a protected boundary.");
			context.Expect(prepare->workerEligible && !prepare->mainThreadRequired
				&& prepare->phaseKind == WorkerSystemPhaseKind::WorkerPrepare
				&& Contains(prepare->explicitDependencies, capture->stableDescriptorId),
				"Agent preparation must only read immutable capture values after protected admission.");
			context.Expect(publish->mainThreadRequired && !publish->workerEligible
				&& publish->phaseKind == WorkerSystemPhaseKind::ProtectedCommit
				&& publish->hardBoundary
				&& Contains(publish->explicitDependencies, prepare->stableDescriptorId),
				"Agent guidance must revalidate candidates and publish atomically after worker preparation.");
		});

		registry.Add("Navigation.AgentScheduler.CaptureDiscoversFirstAttachedAgentThroughDeclaredWork", [](TestContext& context) {
			using namespace TerraEngine::Scheduling;
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown();
			navigation.Initialize();
			agents.Shutdown();
			agents.Initialize();
			const Object object = objects.createObject("Declared Agent Capture");
			const auto* component = objects.AddComponent<NavigationAgentComponent>(object);
			const auto workloads = agents.DeclareFrameWorkloads(0.0F);
			JobGraphContext contextValue{};
			if(!workloads.empty() && workloads.front().work) workloads.front().work(contextValue);
			for(std::size_t index = 1U; index + 1U < workloads.size(); ++index)
				if(workloads[index].work) workloads[index].work(contextValue);
			if(!workloads.empty() && workloads.back().work) workloads.back().work(contextValue);
			const auto identity = agents.FindIdentity(objects, objects.getUUID(object));
			const auto status = agents.ReadStatus(identity);
			context.Expect(component != nullptr && identity.IsValid()
				&& status.state == Navigation::NavigationAgentState::Idle,
				"A newly attached valid agent must be discovered by declared capture and published idle state.");
			agents.Shutdown();
			navigation.Shutdown();
			objects.clear();
		});

		registry.Add("Navigation.AgentScheduler.StopPublicationRejectsObsoleteIntent", [](TestContext& context) {
			using namespace TerraEngine::Scheduling;
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown();
			navigation.Initialize();
			agents.Shutdown();
			agents.Initialize();
			const Object object = objects.createObject("Stop Candidate Agent");
			(void)objects.AddComponent<NavigationAgentComponent>(object);
			const auto runDeclaredFrame = [&]() {
				const auto workloads = agents.DeclareFrameWorkloads(0.0F);
				JobGraphContext frameContext{};
				for(const auto& workload : workloads) if(workload.work) workload.work(frameContext);
			};
			runDeclaredFrame();
			const auto identity = agents.FindIdentity(objects, objects.getUUID(object));
			const auto destination = navigation.PublishedWorld()->world;
			Navigation::NavigationLocation location{};
			location.world = destination;
			location.position.offsetMetres = {4.0, 0.0, 0.0};
			const auto queued = agents.QueueLocationDestination(identity, location, 1U, 0U, 1U, 91U);
			const auto stopped = agents.QueueStop(identity, 1U, 0U, 2U, 92U);
			runDeclaredFrame();
			const auto status = agents.ReadStatus(identity);
			const auto stopReceipt = agents.ReadReceipt(identity, stopped.correlationId);
			context.Expect(queued.state == Navigation::NavigationAgentReceiptState::Queued
				&& stopped.state == Navigation::NavigationAgentReceiptState::Queued
				&& status.state == Navigation::NavigationAgentState::Stopped
				&& stopReceipt.has_value(),
				"A declared capture/publish cycle must apply ordered Stop and prevent obsolete destination intent from reviving state.");
			agents.Shutdown();
			navigation.Shutdown();
			objects.clear();
		});

		registry.Add("Navigation.AgentScheduler.PublishRejectsCandidateAfterConfigurationMutation", [](TestContext& context) {
			using namespace TerraEngine::Scheduling;
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown();
			navigation.Initialize();
			agents.Shutdown();
			agents.Initialize();
			const Object object = objects.createObject("Configuration Mutation Candidate");
			NavigationAgentComponent* component = objects.AddComponent<NavigationAgentComponent>(object);
			const auto workloads = agents.DeclareFrameWorkloads(0.0F);
			JobGraphContext frameContext{};
			if(!workloads.empty() && workloads.front().work) workloads.front().work(frameContext);
			for(std::size_t index = 1U; index + 1U < workloads.size(); ++index)
				if(workloads[index].work) workloads[index].work(frameContext);

			// Simulate an authored settings transaction landing after worker
			// preparation. Publication must reject the old candidate rather than
			// restoring guidance prepared under the previous configuration.
			if(component != nullptr) {
				component->arrivalToleranceMetres += 0.25;
				objects.notifyObjectComponentValueChanged(object, NavigationAgentComponent::kComponentName);
			}
			if(!workloads.empty() && workloads.back().work) workloads.back().work(frameContext);
			const auto identity = agents.FindIdentity(objects, objects.getUUID(object));
			const auto status = agents.ReadStatus(identity);
			const auto guidance = agents.ReadGuidance(identity);
			context.Expect(component != nullptr && identity.IsValid()
				&& status.state != Navigation::NavigationAgentState::Following
				&& !guidance.IsUsable(),
				"A configuration mutation between preparation and publication must invalidate the stale candidate and never restore old guidance.");
			agents.Shutdown();
			navigation.Shutdown();
			objects.clear();
		});

		registry.Add("Navigation.AgentScheduler.PublishesBoundedDiagnosticsAndRejectsStaleSelection", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown();
			agents.Shutdown();
			navigation.Initialize();
			agents.Initialize();
			const Object owner = objects.createObject("Published Agent Diagnostics");
			(void)objects.AddComponent<Transform>(owner);
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			const bool terrainReady = PrepareTerrain(navigation);
			Advance(navigation, agents);
			const Navigation::NavigationAgentIdentity identity = agents.FindIdentity(objects, objects.getUUID(owner));
			const auto world = navigation.PublishedWorld();
			Navigation::NavigationLocation destination{};
			if(world) {
				destination.world = world->world;
				destination.position.offsetMetres = {5.0, 0.0, 0.0};
			}
			const auto receipt = terrainReady && identity.IsValid() && world
				? agents.QueueLocationDestination(identity, destination, 0U, 0U, 1U, 4301U)
				: Navigation::NavigationAgentReceipt{};
			Navigation::NavigationAgentGuidanceSnapshot guidance{};
			for(std::uint32_t frame = 0U; frame < 64U && !guidance.IsUsable(); ++frame) {
				Advance(navigation, agents);
				guidance = agents.ReadGuidance(identity);
			}
			const auto diagnostics = agents.PublishedDiagnostics();
			const bool containsIdentity = diagnostics && std::any_of(diagnostics->agents.begin(), diagnostics->agents.end(),
				[identity](const Navigation::NavigationAgentDiagnosticRecord& record) { return record.identity == identity; });
			const auto selected = diagnostics
				? agents.ReadPublishedSelectedDiagnostic(identity, diagnostics->revision) : std::nullopt;
			const auto staleSelection = diagnostics && diagnostics->revision > 1U
				? agents.ReadPublishedSelectedDiagnostic(identity, diagnostics->revision - 1U) : std::nullopt;

			// Object removal is a protected lifecycle boundary. The old selected copy
			// must never retain usable controller guidance after that boundary runs.
			objects.removeObject(owner);
			agents.ObserveProtectedBoundary(Navigation::NavigationProtectedBoundary::FrameEnd,
				world ? world->rootGeneration + 1U : 1U);
			const auto afterBoundary = agents.ReadGuidance(identity);

			context.Expect(receipt.state != Navigation::NavigationAgentReceiptState::Rejected && guidance.IsUsable()
				&& diagnostics && containsIdentity && selected && selected->routeCurrent
				&& selected->agent.guidance.identity == identity,
				"A real scheduler-driven agent must publish one bounded immutable diagnostic record with its state and revisions.");
			context.Expect(!staleSelection && diagnostics->commandCapacity != 0U
				&& diagnostics->currentOwnedMemoryBytes != 0U && diagnostics->lastProtectedPublishMicroseconds < 10'000'000U,
				"Selected diagnostics must reject an old publication revision and expose finite capacity, memory, and publication timing counters.");
			context.Expect(!afterBoundary.IsUsable(),
				"A protected removal boundary must invalidate selected-route guidance before any controller can consume it.");
			agents.Shutdown();
			navigation.Shutdown();
			objects.clear();
		});
	}
}
