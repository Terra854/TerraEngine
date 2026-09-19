// NavigationAgentControllerTests.cpp - native consumer proof for advisory agent guidance.
//
// This test deliberately owns the controller loop. Navigation publishes only
// copied guidance; a gameplay controller is responsible for turning that advice
// into desired velocity and for publishing its authoritative pose at a protected
// boundary.

#include "../TestHarness.h"

#include "Components/NavigationAgentComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Navigation/Agents/NavigationAgentSystem.h"
#include "Navigation/Geometry/NavigationGeometryFormat.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationSystem.h"
#include "Physics/Backend/JoltPhysicsBackend.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;
	using namespace TerraEngine::Physics::Backend;

	namespace {
		constexpr float kFixedDeltaTime = 1.0F / 60.0F;
		constexpr float kControllerSpeed = 2.0F;

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

		bool PrepareTerrain(Navigation::NavigationSystem& navigation, const Navigation::NavigationProfile& profile) {
			const auto terrain = FlatTerrain();
			if(!terrain || !navigation.QueueInitialGroundBuild(profile, {terrain}))
				return false;
			navigation.Update(0.0F);
			const auto world = navigation.PublishedWorld();
			return world != nullptr && world->terrainState == Navigation::NavigationTerrainState::Available;
		}

		class ControllerFixture final {
		public:
			ControllerFixture()
				: backend(ThreadPool::instance()) {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				backend.Initialize();
				PhysicsBackendWorldConfiguration configuration{};
				configuration.world = world;
				backend.CreateWorld(configuration);

				PhysicsBackendStagingToken staging{};
				PhysicsShapeRecipe floorRecipe{};
				floorRecipe.kind = PhysicsShapeKind::Box;
				floorRecipe.dimensions = {20.0F, 1.0F, 20.0F};
				backend.CreateShape(world, floorShape, floorRecipe, staging);
				PhysicsBodyCreateRequest floor{};
				floor.handle = floorBody;
				floor.objectId = GenerateUUID();
				floor.objectGeneration = 1U;
				floor.componentGeneration = 1U;
				floor.shape = floorShape;
				floor.motionType = PhysicsMotionType::Static;
				floor.pose.position.y = -0.5F;
				backend.CreateBody(floor, staging);
				backend.CommitStaging(staging);
			}

			~ControllerFixture() {
				backend.DestroyWorld(world);
				backend.Shutdown();
				ThreadPool::instance().shutdown();
				if(previousWorkerCount != 0U) ThreadPool::instance().initialize(previousWorkerCount);
			}

			PhysicsCharacterCreateRequest CharacterRequest(PhysicsCharacterHandle handle, UUID objectId) const {
				PhysicsCharacterCreateRequest request{};
				request.handle = handle;
				request.objectId = objectId;
				request.objectGeneration = 1U;
				request.componentGeneration = 1U;
				request.shapeKind = PhysicsShapeKind::Capsule;
				request.radius = 0.5F;
				request.height = 2.0F;
				// Keep the provider's CharacterVirtual position at the scene ground
				// origin. Its collision capsule is centred half a height above it.
				request.localCenter = {0.0F, request.height * 0.5F, 0.0F};
				request.pose.position = {0.0F, 0.0F, 0.0F};
				request.stepHeight = 0.45F;
				// Keep physical contact padding within the profile clearance used to
				// build route support, rather than claiming unwalkable clearance.
				request.contactPadding = 0.01F;
				request.groundAdhesion = 0.2F;
				request.allowStairWalking = true;
				request.useGravity = false;
				return request;
			}

			bool AddWall(float x) {
				PhysicsBackendStagingToken staging{};
				PhysicsShapeRecipe recipe{};
				recipe.kind = PhysicsShapeKind::Box;
				recipe.dimensions = {1.0F, 2.0F, 2.0F};
				PhysicsBodyCreateRequest wall{};
				wall.handle = wallBody;
				wall.objectId = GenerateUUID();
				wall.objectGeneration = 1U;
				wall.componentGeneration = 1U;
				wall.shape = wallShape;
				wall.motionType = PhysicsMotionType::Static;
				wall.pose.position = {x, 1.0F, 0.0F};
				return backend.CreateShape(world, wallShape, recipe, staging).Succeeded()
					&& backend.CreateBody(wall, staging).Succeeded()
					&& backend.CommitStaging(staging).Succeeded();
			}

			std::optional<PhysicsBackendCharacterState> ReadCharacter(PhysicsCharacterHandle handle) const {
				PhysicsBackendExtraction extraction{};
				extraction.world = world;
				if(!backend.Extract(extraction).Succeeded()) return std::nullopt;
				const auto found = std::find_if(extraction.characters.begin(), extraction.characters.end(),
					[handle](const PhysicsBackendCharacterState& state) { return state.handle == handle; });
				return found == extraction.characters.end() ? std::nullopt
					: std::optional<PhysicsBackendCharacterState>{*found};
			}

			JoltPhysicsBackend backend;
			PhysicsWorldHandle world{{741U}, 1U};
			PhysicsShapeHandle floorShape{{741U}, 1U, 0U, 1U};
			PhysicsBodyHandle floorBody{{741U}, 1U, 0U, 1U};
			PhysicsShapeHandle wallShape{{741U}, 1U, 1U, 1U};
			PhysicsBodyHandle wallBody{{741U}, 1U, 1U, 1U};
			const std::size_t previousWorkerCount = ThreadPool::instance().getThreadCount();
		};

		bool PublishControllerPose(ObjectManager& objects, const Object& owner,
			const PhysicsBackendCharacterState& state,
			Navigation::NavigationSystem& navigation, Navigation::NavigationAgentSystem& agents,
			std::uint64_t boundaryGeneration) {
			auto pose = objects.GetCanonicalWorldPose(owner);
			if(!pose) return false;
			// CharacterVirtual extraction returns its object-origin position; its
			// local capsule centre is already accounted for by the create request.
			pose->position.offsetMetres = {state.pose.position.x, state.pose.position.y, state.pose.position.z};
			if(objects.SetCanonicalWorldPose(owner, *pose) != Scene::WorldCoordinateError::None)
				return false;
			// This test is the controller/presentation boundary. It is the only place
			// it makes the provider's authoritative pose visible to Navigation.
			navigation.ObserveProtectedBoundary(Navigation::NavigationProtectedBoundary::FrameEnd, boundaryGeneration);
			agents.ObserveProtectedBoundary(Navigation::NavigationProtectedBoundary::FrameEnd, boundaryGeneration);
			return true;
		}

		bool StepController(ControllerFixture& fixture, PhysicsCharacterHandle handle,
			const PhysicsCharacterCreateRequest& request, const glm::dvec3& direction,
			PhysicsTick tick) {
			PhysicsCharacterUpdateRequest update{};
			update.handle = handle;
			update.objectId = request.objectId;
			update.objectGeneration = request.objectGeneration;
			update.componentGeneration = request.componentGeneration;
			update.desiredVelocity = {static_cast<float>(direction.x) * kControllerSpeed,
				static_cast<float>(direction.y) * kControllerSpeed, static_cast<float>(direction.z) * kControllerSpeed};
			update.fixedDeltaTime = kFixedDeltaTime;
			return fixture.backend.UpdateCharacter(update).Succeeded()
				&& fixture.backend.AdvanceCharacters(fixture.world, tick, kFixedDeltaTime).Succeeded()
				&& fixture.backend.Step({fixture.world, tick, kFixedDeltaTime, 1U}).Succeeded();
		}

		Navigation::NavigationAgentGuidanceSnapshot WaitForGuidance(
			Navigation::NavigationSystem& navigation, Navigation::NavigationAgentSystem& agents,
			Navigation::NavigationAgentIdentity identity) {
			for(std::uint32_t frame = 0U; frame < 96U; ++frame) {
				navigation.Update(kFixedDeltaTime);
				agents.Update(kFixedDeltaTime);
				const auto guidance = agents.ReadGuidance(identity);
				if(guidance.IsUsable()) return guidance;
			}
			return {};
		}
	}

	void RegisterNavigationAgentControllerTests(TestRegistry& registry) {
		registry.Add("Navigation.AgentNativeController.ReachesStopsAndRespectsCollisionWithStaleAdvice", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown();
			agents.Shutdown();
			navigation.Initialize();
			agents.Initialize();

			const Object owner = objects.createObject("Native Controller Navigation Owner");
			(void)objects.AddComponent<Transform>(owner);
			auto* agentComponent = objects.AddComponent<NavigationAgentComponent>(owner);
			if(agentComponent != nullptr) {
				// The route profile must be no less conservative than the real capsule.
				agentComponent->radiusMetres = 0.5;
				agentComponent->heightMetres = 2.0;
			}
			const Navigation::NavigationProfile controllerProfile = agentComponent != nullptr
				? agentComponent->ToNavigationProfile() : Navigation::NavigationProfile::Defaults();
			const bool terrainReady = PrepareTerrain(navigation, controllerProfile);
			navigation.Update(kFixedDeltaTime);
			agents.Update(kFixedDeltaTime);
			const Navigation::NavigationAgentIdentity identity = agents.FindIdentity(objects, objects.getUUID(owner));
			const auto navigationWorld = navigation.PublishedWorld();

			ControllerFixture physics;
			const PhysicsCharacterHandle character{{741U}, 1U, 2U, 1U};
			const PhysicsCharacterCreateRequest controller = physics.CharacterRequest(character, objects.getUUID(owner));
			PhysicsBackendStagingToken characterStaging{};
			const bool controllerReady = physics.backend.CreateCharacter(controller, characterStaging).Succeeded()
				&& physics.backend.CommitStaging(characterStaging).Succeeded();

			Navigation::NavigationLocation arrivalDestination{};
			if(navigationWorld) {
				arrivalDestination.world = navigationWorld->world;
				arrivalDestination.position.offsetMetres = {2.0, 0.0, 0.0};
			}
			const auto submitted = terrainReady && controllerReady && identity.IsValid()
				? agents.QueueLocationDestination(identity, arrivalDestination, 0U, 0U, 1U, 3901U)
				: Navigation::NavigationAgentReceipt{};
			Navigation::NavigationAgentGuidanceSnapshot guidance = WaitForGuidance(navigation, agents, identity);

			bool anyPosePublished = false;
			bool allPosePublicationsSucceeded = true;
			bool arrived = false;
			PhysicsTick tick = 1U;
			for(; tick <= 240U && guidance.IsUsable() && !arrived; ++tick) {
				const bool stepped = StepController(physics, character, controller, guidance.suggestedDirection, tick);
				const auto authoritative = physics.ReadCharacter(character);
				const bool published = stepped && authoritative
					&& PublishControllerPose(objects, owner, *authoritative, navigation, agents, tick);
				anyPosePublished = anyPosePublished || published;
				allPosePublicationsSucceeded = allPosePublicationsSucceeded && published;
				navigation.Update(kFixedDeltaTime);
				agents.Update(kFixedDeltaTime);
				guidance = agents.ReadGuidance(identity);
				arrived = agents.ReadStatus(identity).state == Navigation::NavigationAgentState::Arrived;
			}

			const auto beforeStopPose = physics.ReadCharacter(character);
			const auto beforeStop = agents.ReadStatus(identity);
			const auto stop = agents.QueueStop(identity, beforeStop.configurationRevision,
				beforeStop.intentSequence, 2U, 3902U);
			navigation.Update(kFixedDeltaTime);
			agents.Update(kFixedDeltaTime);
			const auto stoppedStatus = agents.ReadStatus(identity);
			for(; tick <= 270U; ++tick) {
				const bool stepped = StepController(physics, character, controller, {}, tick);
				const auto authoritative = physics.ReadCharacter(character);
				const bool published = stepped && authoritative
					&& PublishControllerPose(objects, owner, *authoritative, navigation, agents, tick);
				anyPosePublished = anyPosePublished || published;
				allPosePublicationsSucceeded = allPosePublicationsSucceeded && published;
			}
			const auto afterStopPose = physics.ReadCharacter(character);

			Navigation::NavigationLocation blockedDestination = arrivalDestination;
			blockedDestination.position.offsetMetres.x = 8.0;
			const auto resumed = agents.QueueLocationDestination(identity, blockedDestination, 0U, 0U, 3U, 3903U);
			const Navigation::NavigationAgentGuidanceSnapshot staleAdvice = WaitForGuidance(navigation, agents, identity);
			const bool wallReady = physics.AddWall(4.0F);
			bool navigationInvalidatedAdvice = false;
			for(; tick <= 390U && staleAdvice.IsUsable(); ++tick) {
				const bool stepped = StepController(physics, character, controller, staleAdvice.suggestedDirection, tick);
				const auto authoritative = physics.ReadCharacter(character);
				if(!stepped || !authoritative) {
					allPosePublicationsSucceeded = false;
					break;
				}
				const bool published = PublishControllerPose(objects, owner, *authoritative, navigation, agents, tick);
				anyPosePublished = anyPosePublished || published;
				allPosePublicationsSucceeded = allPosePublicationsSucceeded && published;
				navigationInvalidatedAdvice = navigationInvalidatedAdvice || !agents.ReadGuidance(identity).IsUsable();
			}
			const auto blockedPose = physics.ReadCharacter(character);

			const float stopDistance = beforeStopPose && afterStopPose
				? std::fabs(afterStopPose->pose.position.x - beforeStopPose->pose.position.x) : std::numeric_limits<float>::infinity();
			const float blockedX = blockedPose ? blockedPose->pose.position.x : std::numeric_limits<float>::infinity();
			context.Expect(terrainReady && controllerReady && submitted.state != Navigation::NavigationAgentReceiptState::Rejected
				&& anyPosePublished && allPosePublicationsSucceeded && arrived,
				"A native gameplay controller must reach the agent's advised destination only by publishing extracted authoritative poses.");
			context.Expect(stop.state != Navigation::NavigationAgentReceiptState::Rejected
				&& stoppedStatus.state == Navigation::NavigationAgentState::Stopped
				&& stopDistance < 0.02F,
				"Stop must replace the controller's desired velocity with zero and leave its authoritative pose stationary.");
			context.Expect(resumed.state != Navigation::NavigationAgentReceiptState::Rejected && wallReady
				&& staleAdvice.IsUsable() && navigationInvalidatedAdvice && blockedX < 3.0F,
				"A real Jolt wall must block a controller that continues to consume retained stale advice until Navigation invalidates it.");

			objects.clear();
			agents.Shutdown();
			navigation.Shutdown();
		});
	}
}
