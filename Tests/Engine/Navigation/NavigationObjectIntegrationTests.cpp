// NavigationObjectIntegrationTests.cpp - ordinary scene-object route ownership.

#include "../TestHarness.h"

#include "Components/Navigation/NavigationAgentComponent.h"
#include "Components/Navigation/NavigationSourceComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Navigation/Geometry/NavigationGeometryFormat.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/Integration/NavigationRuntimeSceneCapture.h"
#include "Navigation/Integration/NavigationSceneBootstrap.h"
#include "Navigation/Ground/GroundRouteQuery.h"
#include "Navigation/NavigationSystem.h"
#include "Resource/ResourceManager.h"
#include "Scene/SceneMutationQueue.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace TerraEngine::Tests
{
	using namespace TerraEngine::Navigation;
	using namespace TerraEngine::Navigation::Ground;

	namespace
	{
		NavigationLocation Location(const NavigationWorldHandle world, double x)
		{
			NavigationLocation location{};
			location.world = world;
			location.position.offsetMetres = {x, 0.0, 0.0};
			return location;
		}

		std::shared_ptr<const NavigationGeometrySnapshot> FlatGround()
		{
			// Clockwise winding viewed from above creates upward-facing walkable
			// support. The snapshot is copied before any worker preparation begins.
			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{-16.0F, 0.0F, -16.0F}, {16.0F, 0.0F, 16.0F},
				{16.0F, 0.0F, -16.0F}, {-16.0F, 0.0F, 16.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceRevision = 1U;
			descriptor.canonicalPlacementRevision = 1U;
			descriptor.physicalReadyRevision = 1U;
			return NavigationGeometrySnapshot::Create(descriptor, document).snapshot;
		}

		TerraEngine::UUID FixedSourceId(std::uint8_t seed)
		{
			TerraEngine::UUID id{};
			id[0] = seed;
			id[15] = static_cast<std::uint8_t>(seed + 1U);
			return id;
		}
	}

	void RegisterNavigationObjectIntegrationTests(TestRegistry& registry)
	{
		registry.Add("Navigation.ObjectIntegration.EnabledComponentRegistersGenerationQualifiedAgent", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();

			const Object object = objects.createObject("Navigation Route Object");
			Transform* transform = objects.AddComponent<Transform>(object);
			NavigationAgentComponent* authoring = objects.AddComponent<NavigationAgentComponent>(object);
			system.SynchronizeSceneObjectAgents(objects);
			const NavigationAgentHandle runtime = system.ResolveObjectAgent(objects.getUUID(object));
			const auto read = system.ReadAgent(runtime);
			context.Expect(transform != nullptr && authoring != nullptr && runtime.IsValid(),
				"An enabled NavigationAgentComponent and Transform must publish one runtime agent for the ordinary object.");
			context.Expect(read.record.has_value() && transform != nullptr
				&& read.record->currentPosition.position == transform->worldPose.position
				&& read.record->poseGeneration == 1U,
				"The runtime agent must start from the object's canonical Transform pose and expose a generation-qualified snapshot.");

			if(authoring != nullptr) authoring->enabled = false;
			system.SynchronizeSceneObjectAgents(objects);
			context.Expect(!system.ResolveObjectAgent(objects.getUUID(object)).IsValid()
				&& system.ReadAgent(runtime).status != NavigationHandleStatus::Alive,
				"Disabling the authoring component must revoke both the object binding and its prior runtime generation.");
			system.Shutdown();
			objects.clear();
		});

		registry.Add("Navigation.ObjectIntegration.SetDestinationPublishesRouteFromObjectPose", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();
			const auto world = system.PublishedWorld();
			const auto geometry = FlatGround();
			context.Expect(world != nullptr && geometry != nullptr,
				"The object route fixture requires a live navigation world and immutable walkable support.");
			if(!world || !geometry) { system.Shutdown(); objects.clear(); return; }
			context.Expect(system.QueueInitialGroundBuild(NavigationProfile::Defaults(), {geometry}),
				"Walkable scene geometry must be accepted for background ground-data preparation.");
			system.Update(0.0F);

			const Object object = objects.createObject("Destination Object");
			Transform* transform = objects.AddComponent<Transform>(object);
			NavigationAgentComponent* authoring = objects.AddComponent<NavigationAgentComponent>(object);
			if(transform != nullptr) transform->worldPose.position.offsetMetres = {0.0, 0.0, 0.0};
			system.SynchronizeSceneObjectAgents(objects);
			const NavigationAgentHandle runtime = system.ResolveObjectAgent(objects.getUUID(object));
			const NavigationLocation destination = Location(world->world, 5.0);
			const auto command = system.QueueSetDestination(runtime, destination,
				authoring ? authoring->GetRoutePolicyValue() : 0U, 0U);
			context.Expect(runtime.IsValid() && command.command.IsValid(),
				"A normal object runtime agent must accept a set-destination command.");

			// The first tick atomically applies the destination and admits a route;
			// the next worker/publish pair computes and exposes the immutable points.
			system.Update(0.0F);
			const auto applied = system.ReadCommand(command.command);
			context.Expect(applied.record.has_value() && applied.record->state == NavigationCommandState::Applied
				&& applied.record->resultingRequest.IsValid(),
				"The protected boundary must turn the command into one observable route request.");
			system.Update(0.0F);
			const auto state = system.ReadAgent(runtime);
			const auto route = state.record ? system.ReadRoute(state.record->activeRoute) : GroundRouteQuery::RetainedRoute{};
			context.Expect(state.record.has_value() && state.record->activeRoute.IsValid()
				&& route.IsValid() && !route.points.empty(),
				"A destination on published ground must produce an active route that gameplay can read from the object agent.");
			context.Expect(route.IsValid() && transform != nullptr
				&& route.points.front().position == transform->worldPose.position
				&& route.points.back() == destination,
				"The ordered route must begin at the object's captured pose and end at its requested location.");

			// Navigation owns intent and route data only. The object's transform remains
			// unchanged until a gameplay controller consumes the points and moves it.
			context.Expect(transform != nullptr && transform->worldPose.position.offsetMetres.x == 0.0,
				"Pathfinding must not silently move the object; gameplay remains the locomotion authority.");
			system.Shutdown();
			objects.clear();
		});

		registry.Add("Navigation.ObjectIntegration.GameplayMovesNavigatingObjectToTargetObject", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			Scene::SceneMutationQueue& mutations = Scene::SceneMutationQueue::Instance();
			objects.clear();
			mutations.Clear();
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();
			const auto world = system.PublishedWorld();
			const auto geometry = FlatGround();
			context.Expect(world != nullptr && geometry != nullptr
				&& system.QueueInitialGroundBuild(NavigationProfile::Defaults(), {geometry}),
				"The object-to-object movement fixture requires published walkable ground.");
			if(!world || !geometry) { system.Shutdown(); objects.clear(); mutations.Clear(); return; }
			system.Update(0.0F);

			const Object navigator = objects.createObject("Navigating Object");
			Transform* navigatorTransform = objects.AddComponent<Transform>(navigator);
			NavigationAgentComponent* navigatorAuthoring = objects.AddComponent<NavigationAgentComponent>(navigator);
			const Object target = objects.createObject("Target Object");
			Transform* targetTransform = objects.AddComponent<Transform>(target);
			if(navigatorTransform != nullptr) navigatorTransform->worldPose.position.offsetMetres = {0.0, 0.0, 0.0};
			if(targetTransform != nullptr) targetTransform->worldPose.position.offsetMetres = {5.0, 0.0, 0.0};
			context.Expect(navigatorTransform != nullptr && navigatorAuthoring != nullptr && targetTransform != nullptr,
				"Both ordinary objects must expose canonical transforms and the navigator must expose an agent component.");
			if(navigatorTransform == nullptr || navigatorAuthoring == nullptr || targetTransform == nullptr) {
				system.Shutdown(); objects.clear(); mutations.Clear(); return;
			}

			// Gameplay targets the other object's published canonical pose. Navigation
			// receives only a copied world location, so no worker retains a Transform.
			system.SynchronizeSceneObjectAgents(objects);
			const NavigationAgentHandle agent = system.ResolveObjectAgent(objects.getUUID(navigator));
			const NavigationLocation destination{world->world, targetTransform->worldPose.position};
			const auto command = system.QueueSetDestination(
				agent, destination, navigatorAuthoring->GetRoutePolicyValue(), 0U);
			for(std::uint32_t frame = 0U; frame < 3U; ++frame) system.Update(0.0F);
			const auto agentBeforeMovement = system.ReadAgent(agent);
			const GroundRouteQuery::RetainedRoute route = agentBeforeMovement.record
				? system.ReadRoute(agentBeforeMovement.record->activeRoute)
				: GroundRouteQuery::RetainedRoute{};
			context.Expect(command.command.IsValid() && route.IsValid() && route.points.size() >= 2U
				&& route.points.front().position == navigatorTransform->worldPose.position
				&& route.points.back() == destination,
				"The navigating object must first compute an ordered route to the target object's current pose.");
			if(!route.IsValid() || route.points.size() < 2U) {
				system.Shutdown(); objects.clear(); mutations.Clear(); return;
			}

			// This loop stands in for a gameplay/character controller. It consumes each
			// published waypoint and moves through the transactional scene boundary;
			// navigation remains responsible only for route computation and observation.
			bool everyMovementCommitted = true;
			std::size_t movementStepCount = 0U;
			const TerraEngine::UUID navigatorId = objects.getUUID(navigator);
			for(std::size_t pointIndex = 1U; pointIndex < route.points.size(); ++pointIndex) {
				navigatorTransform = objects.GetComponent<Transform>(navigator);
				if(navigatorTransform == nullptr) { everyMovementCommitted = false; break; }
				Scene::WorldPose nextPose = navigatorTransform->worldPose;
				nextPose.position = route.points[pointIndex].position;
				Scene::TransactionalMutationBuilder movement(
					"navigation-gameplay-step-" + std::to_string(pointIndex),
					Scene::TransactionOrigin::Test, "NavigationGameplayController");
				movement.SetWorldTransform(objects, navigatorId, nextPose);
				mutations.EnqueueTransaction(std::move(movement).Build());
				const Scene::SceneMutationBatchResult committed = mutations.Apply(
					objects, "Navigation gameplay route movement");
				const bool stepCommitted = committed.transactionResults.size() == 1U
					&& committed.transactionResults.front().status == Scene::TransactionStatus::Committed
					&& committed.transactionResults.front().appliedWriteCount == 1U;
				everyMovementCommitted = everyMovementCommitted && stepCommitted;
				if(!stepCommitted) break;
				++movementStepCount;
				system.SynchronizeSceneObjectAgents(objects);
			}

			navigatorTransform = objects.GetComponent<Transform>(navigator);
			targetTransform = objects.GetComponent<Transform>(target);
			const auto agentAfterMovement = system.ReadAgent(agent);
			context.Expect(everyMovementCommitted && movementStepCount == route.points.size() - 1U,
				"Gameplay must commit every route waypoint through the protected transactional movement path.");
			context.Expect(navigatorTransform != nullptr && targetTransform != nullptr
				&& navigatorTransform->worldPose.position == targetTransform->worldPose.position,
				"The navigating object's final canonical position must match the target object's canonical position.");
			context.Expect(agentAfterMovement.record.has_value() && targetTransform != nullptr
				&& agentAfterMovement.record->currentPosition.position == targetTransform->worldPose.position,
				"Navigation must observe the navigator at the target after gameplay publishes its movement.");

			system.Shutdown();
			objects.clear();
			mutations.Clear();
		});

		registry.Add("Navigation.ObjectIntegration.TerrainCapabilityDoesNotRequireTerrainInEveryScene", [](TestContext& context) {
			NavigationWorldHandle world{NavigationWorldId{19U}, 2U};
			const NavigationLocation start = Location(world, 0.0);
			const NavigationLocation destination = Location(world, 8.0);
			GroundRouteQuery::Request request{};
			request.owner = {world, NavigationOwnerId{41U}, 1U};
			request.start = start;
			request.destination = destination;
			request.profile = NavigationProfile::Defaults();
			request.maxExpansions = 128U;
			const GroundRouteQuery::Result terrainBacked = GroundRouteQuery::Search(
				request, GroundRouteQuery::Snapshot::StraightGround(start, destination));
			const GroundRouteQuery::Result meshBacked = GroundRouteQuery::Search(
				request, GroundRouteQuery::Snapshot::StraightGround(start, destination));
			context.Expect(terrainBacked.outcome == NavigationOutcome::Complete
				&& meshBacked.outcome == NavigationOutcome::Complete,
				"Terrain support is mandatory, while mesh/collider-only scenes must remain valid ground-navigation inputs.");
			context.Expect(!terrainBacked.points.empty() && terrainBacked.points.front() == start
				&& terrainBacked.points.back() == destination,
				"Provider-neutral route points must preserve the canonical object start and destination.");
		});

		registry.Add("Navigation.ObjectIntegration.SceneBootstrapPublishesTerrainAndMeshCoverageInputs", [](TestContext& context) {
			NavigationSceneBootstrapInput input{};
			NavigationBootstrapSourceInput terrainSource{};
			terrainSource.component.sourceId = FixedSourceId(7U);
			terrainSource.component.sourceKind = NavigationSourceKind::Terrain;
			terrainSource.component.geometryRevision = 3U;
			terrainSource.terrain = TerrainNavigationPublishedSamples{};
			terrainSource.terrain->terrainPresent = true;
			terrainSource.terrain->descriptor.sourceUuid = {terrainSource.component.sourceId};
			terrainSource.terrain->descriptor.sourceRevision = 3U;
			terrainSource.terrain->descriptor.canonicalPlacementRevision = 1U;
			terrainSource.terrain->descriptor.physicalReadyRevision = 3U;
			terrainSource.terrain->descriptor.sourceKind = NavigationSourceKind::Terrain;
			terrainSource.terrain->sampleColumns = 2U;
			terrainSource.terrain->sampleRows = 2U;
			terrainSource.terrain->sampleSpacingMetres = 4.0F;
			terrainSource.terrain->heights = {0.0F, 0.0F, 0.0F, 0.0F};
			input.sources.push_back(terrainSource);

			NavigationBootstrapAgentInput agent{};
			agent.objectId = FixedSourceId(19U);
			input.agents.push_back(agent);
			const NavigationSceneBootstrapCapture captured = CaptureNavigationSceneBootstrap(input);
			context.Expect(captured.accepted && captured.builds.size() == 1U
				&& captured.builds.front().sources.size() == 1U
				&& captured.agents.size() == 1U && captured.agents.front().resolution.accepted,
				"Protected scene bootstrap must copy an eligible terrain source and resolve an ordinary object's default ground profile into one build input.");

			NavigationSceneBootstrapInput meshOnly{};
			NavigationBootstrapSourceInput meshSource{};
			meshSource.component.sourceId = FixedSourceId(31U);
			meshSource.component.sourceKind = NavigationSourceKind::Mesh;
			meshSource.component.geometryRevision = 4U;
			MeshColliderNavigationSourceInput meshInput{};
			meshInput.descriptor.sourceUuid = {meshSource.component.sourceId};
			meshInput.descriptor.sourceRevision = 4U;
			meshInput.descriptor.canonicalPlacementRevision = 1U;
			meshInput.descriptor.physicalReadyRevision = 4U;
			meshInput.descriptor.sourceKind = NavigationSourceKind::Mesh;
			meshInput.geometry.kind = NavigationGeometryPayloadKind::TriangleMesh;
			meshInput.geometry.vertices = {{-2.0F, 0.0F, -2.0F}, {2.0F, 0.0F, 2.0F},
				{2.0F, 0.0F, -2.0F}, {-2.0F, 0.0F, 2.0F}};
			meshInput.geometry.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			meshSource.meshOrCollider = meshInput;
			meshOnly.sources.push_back(meshSource);
			const NavigationSceneBootstrapCapture noTerrain = CaptureNavigationSceneBootstrap(meshOnly);
			context.Expect(noTerrain.accepted && noTerrain.builds.size() == 1U,
				"The same bootstrap boundary must accept a mesh-only scene without fabricating a terrain object.");

			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();
			const bool queued = captured.accepted && system.QueueInitialGroundBuild(
				captured.builds.front().profile, captured.builds.front().sources);
			system.Update(0.0F);
			system.Update(0.0F);
			const auto published = system.PublishedWorld();
			context.Expect(queued && published && !published->coverage.empty(),
				"Captured scene sources must flow through the real navigation build boundary into routable published coverage.");
			system.Shutdown();
		});

		registry.Add("Navigation.ObjectIntegration.RuntimeCaptureLoadsReferencedGeometryCompanionOnDemand", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			ResourceManager& resources = ResourceManager::Instance();
			objects.clear();
			const TerraEngine::UUID assetId = FixedSourceId(43U);
			(void)resources.ReleaseNavigationGeometrySnapshot(assetId);

			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{-4.0F, 0.0F, -4.0F}, {4.0F, 0.0F, 4.0F},
				{4.0F, 0.0F, -4.0F}, {-4.0F, 0.0F, 4.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			const NavigationGeometryEncodeResult encoded = EncodeNavigationGeometry(document);
			const std::filesystem::path companion = std::filesystem::temp_directory_path()
				/ "terra-navigation-runtime-capture.tenavgeom";
			{
				// The fixture publishes the exact checked companion bytes that the editor
				// registers. Runtime capture must perform the first load itself.
				std::ofstream output(companion, std::ios::binary | std::ios::trunc);
				if(encoded.accepted && output) {
					output.write(reinterpret_cast<const char*>(encoded.bytes.data()),
						static_cast<std::streamsize>(encoded.bytes.size()));
				}
			}
			resources.RegisterNavigationGeometryPath(assetId, companion);

			const Object sourceObject = objects.createObject("Runtime Navigation Ground");
			(void)objects.AddComponent<Transform>(sourceObject);
			NavigationSourceComponent* source = objects.AddComponent<NavigationSourceComponent>(sourceObject);
			if(source != nullptr) {
				source->sourceId = FixedSourceId(47U);
				source->sourceAssetId = assetId;
				source->sourceKind = NavigationSourceKind::Mesh;
				source->geometryRevision = 5U;
			}
			const auto capture = CaptureNavigationRuntimeScene(objects, resources, {}, {});
			context.Expect(encoded.accepted && source != nullptr && capture.accepted
				&& capture.bootstrap.builds.size() == 1U
				&& capture.bootstrap.builds.front().sources.size() == 1U,
				"A referenced mesh companion must load automatically during the protected production scene capture.");
			context.Expect(resources.GetNavigationGeometrySnapshot(assetId) != nullptr,
				"The on-demand companion load must publish one immutable reusable resource generation.");

			(void)resources.ReleaseNavigationGeometrySnapshot(assetId);
			std::error_code removeError;
			std::filesystem::remove(companion, removeError);
			objects.clear();
		});

		registry.Add("Navigation.ObjectIntegration.CommittedSourceRemovalMarksActiveRouteCoverageDirty", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();
			const auto world = system.PublishedWorld();
			const auto geometry = FlatGround();
			context.Expect(world != nullptr && geometry != nullptr
				&& system.QueueInitialGroundBuild(NavigationProfile::Defaults(), {geometry}),
				"A committed source-change test must begin with real published walkable support.");
			if(!world || !geometry) { system.Shutdown(); objects.clear(); return; }
			system.Update(0.0F);
			const Object object = objects.createObject("Dynamic Route Object");
			Transform* transform = objects.AddComponent<Transform>(object);
			(void)objects.AddComponent<NavigationAgentComponent>(object);
			const Object unaffectedObject = objects.createObject("Unaffected Route Object");
			Transform* unaffectedTransform = objects.AddComponent<Transform>(unaffectedObject);
			(void)objects.AddComponent<NavigationAgentComponent>(unaffectedObject);
			if(unaffectedTransform != nullptr) unaffectedTransform->worldPose.position.offsetMetres.x = 10.0;
			system.SynchronizeSceneObjectAgents(objects);
			const NavigationAgentHandle agent = system.ResolveObjectAgent(objects.getUUID(object));
			const NavigationAgentHandle unaffectedAgent = system.ResolveObjectAgent(objects.getUUID(unaffectedObject));
			const NavigationWorldHandle worldHandle = world->world;
			const NavigationOwnerHandle owner = agent.owner;
			const NavigationOwnerHandle unaffectedOwner = unaffectedAgent.owner;
			const NavigationLocation destination = Location(worldHandle, 4.0);
			const auto command = system.QueueSetDestination(agent, destination, 0U, 0U);
			const auto unaffectedCommand = system.QueueSetDestination(
				unaffectedAgent, Location(worldHandle, 12.0), 0U, 0U);
			// Two agents may finish on different worker/publication turns. Advance a
			// small bounded number of frames before observing both immutable routes.
			for(std::uint32_t frame = 0U; frame < 4U; ++frame) system.Update(0.0F);
			const auto agentState = system.ReadAgent(agent);
			const auto unaffectedState = system.ReadAgent(unaffectedAgent);
			context.Expect(transform != nullptr && command.command.IsValid()
				&& agentState.record.has_value() && agentState.record->activeRoute.IsValid(),
				"A dynamic-source test must retain an active route owned by an ordinary scene object before removal.");
			context.Expect(unaffectedTransform != nullptr && unaffectedCommand.command.IsValid()
				&& unaffectedState.record.has_value() && unaffectedState.record->activeRoute.IsValid(),
				"The fixture must also retain a spatially separate route for dependency-aware invalidation.");

			Scene::NavigationSourceMutationReceipt removal{};
			removal.accepted = true;
			removal.transactionStatus = Scene::TransactionStatus::Committed;
			removal.intent.sourceId = FixedSourceId(7U);
			removal.intent.operation = Scene::NavigationSourceMutationOperation::Delete;
			removal.intent.committedSourceRevision = 4U;
			removal.intent.physicalReadyRevision = 4U;
			removal.intent.oldBounds.valid = true;
			removal.intent.oldBounds.minimum.offsetMetres = {-2.0, 0.0, -2.0};
			removal.intent.oldBounds.maximum.offsetMetres = {6.0, 2.0, 2.0};
			const std::array<Scene::NavigationSourceMutationReceipt, 1U> receipts{removal};
			const std::size_t accepted = system.ObserveSceneMutationReceipts(receipts);
			const auto diagnostics = system.PublishedDiagnostics();
			context.Expect(accepted == 1U && diagnostics
				&& diagnostics->dirtyCoverageCount > 0U,
				"A committed source removal must atomically mark the affected old bounds dirty and queue bounded replacement preparation.");
			context.Expect(!system.DynamicDirtyIndexForTesting().ObserveAtProtectedBoundary().entries.empty(),
				"The removal tombstone must be visible at the next protected boundary before replacement geometry is ready.");
			const auto invalidations = system.ReadInvalidations(owner, 0U);
			const auto unaffectedInvalidations = system.ReadInvalidations(unaffectedOwner, 0U);
			const auto unaffectedAfterChange = system.ReadAgent(unaffectedAgent);
			context.Expect(!invalidations.records.empty(),
				"An active object route affected by committed source removal must receive an explicit invalidation before movement consumes it.");
			context.Expect(unaffectedInvalidations.records.empty() && unaffectedAfterChange.record.has_value()
				&& unaffectedAfterChange.record->activeRoute.IsValid(),
				"A spatially unrelated object route must remain usable when committed dirty coverage does not intersect its swept corridor.");
			system.Shutdown();
			objects.clear();
		});
	}
}
