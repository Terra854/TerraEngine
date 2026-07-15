// Direct CharacterVirtual regressions. The shared test registration/CMake slice
// owns inclusion; this file keeps the provider exercise isolated and headless.
#include "../TestHarness.h"

#include "Physics/Backend/JoltPhysicsBackend.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <cmath>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;
	using namespace TerraEngine::Physics::Backend;

	namespace {
		class CharacterFixture final {
		public:
			CharacterFixture() : backend(ThreadPool::instance()) {
				ThreadPool::instance().shutdown(); ThreadPool::instance().initialize(); backend.Initialize();
				PhysicsBackendWorldConfiguration configuration; configuration.world = world; backend.CreateWorld(configuration);
				PhysicsBackendStagingToken staging; PhysicsShapeRecipe floorRecipe; floorRecipe.kind = PhysicsShapeKind::Box; floorRecipe.dimensions = {20.0F, 1.0F, 20.0F};
				backend.CreateShape(world, floorShape, floorRecipe, staging); PhysicsBodyCreateRequest floor; floor.handle = floorBody; floor.objectId = TerraEngine::GenerateUUID(); floor.objectGeneration = 1U; floor.componentGeneration = 1U; floor.shape = floorShape; floor.motionType = PhysicsMotionType::Static; floor.pose.position.y = -0.5F;
				backend.CreateBody(floor, staging); backend.CommitStaging(staging);
			}
			~CharacterFixture() { backend.DestroyWorld(world); backend.Shutdown(); ThreadPool::instance().shutdown(); }

			PhysicsCharacterCreateRequest CreateRequest(PhysicsCharacterHandle handle, PhysicsShapeKind kind) {
				PhysicsCharacterCreateRequest request; request.handle = handle; request.objectId = TerraEngine::GenerateUUID(); request.objectGeneration = 1U; request.componentGeneration = 1U; request.shapeKind = kind;
				request.radius = 0.5F; request.height = 2.0F; request.pose.position = {0.0F, 1.1F, 0.0F}; request.stepHeight = 0.45F; request.groundAdhesion = 0.2F; request.allowStairWalking = true;
				return request;
			}

			JoltPhysicsBackend backend; PhysicsWorldHandle world{{42U}, 1U}; PhysicsShapeHandle floorShape{{42U}, 1U, 0U, 1U}; PhysicsBodyHandle floorBody{{42U}, 1U, 0U, 1U};
		};
	}

	void RegisterPhysicsCharacterSimulationTests(TestRegistry& registry) {
		registry.Add("Physics.CharacterSimulation.CapsuleCylinderGroundJumpResizeTeleportAndTeardown", [](TestContext& context) {
			CharacterFixture fixture; const PhysicsCharacterHandle capsule{{42U}, 1U, 0U, 1U}; const PhysicsCharacterHandle cylinder{{42U}, 1U, 1U, 1U};
			PhysicsCharacterCreateRequest capsuleRequest = fixture.CreateRequest(capsule, PhysicsShapeKind::Capsule); PhysicsCharacterCreateRequest cylinderRequest = fixture.CreateRequest(cylinder, PhysicsShapeKind::Cylinder); cylinderRequest.pose.position.x = 3.0F;
			PhysicsBackendStagingToken staging; context.Expect(fixture.backend.CreateCharacter(capsuleRequest, staging).Succeeded() && fixture.backend.CreateCharacter(cylinderRequest, staging).Succeeded(), "Capsule and cylinder virtual characters must stage together.");
			context.Expect(fixture.backend.CommitStaging(staging).Succeeded(), "Character staging must publish atomically.");
			for(PhysicsTick tick = 1U; tick <= 90U; ++tick) {
				PhysicsCharacterUpdateRequest update; update.handle = capsule; update.objectId = capsuleRequest.objectId; update.objectGeneration = 1U; update.componentGeneration = 1U; update.desiredVelocity = {1.0F, 0.0F, 0.0F};
				context.Expect(fixture.backend.UpdateCharacter(update).Succeeded() && fixture.backend.Step({fixture.world, tick, 1.0F / 60.0F, 1U}).Succeeded(), "Ground and stair character updates must remain provider-safe.");
			}
			PhysicsBackendExtraction extraction; extraction.world = fixture.world; fixture.backend.Extract(extraction);
			const auto grounded = std::find_if(extraction.characters.begin(), extraction.characters.end(), [&](const auto& state) { return state.handle == capsule; });
			context.Expect(grounded != extraction.characters.end() && grounded->supported && grounded->supportBody == fixture.floorBody, "Ground support, moving-support velocity publication, and contacts must use engine body handles.");
			context.Expect(std::isfinite(grounded->groundNormal.y) && grounded->contactCount > 0U, "Ground and slope state must remain finite under Debug Jolt tolerances.");
			context.Expect(grounded != extraction.characters.end() && grounded->objectId == capsuleRequest.objectId
				&& grounded->objectGeneration == capsuleRequest.objectGeneration
				&& grounded->componentGeneration == capsuleRequest.componentGeneration,
				"Extracted character state must preserve the copied object and component generations needed to reject stale managed commands.");
			PhysicsCharacterUpdateRequest jump; jump.handle = capsule; jump.objectId = capsuleRequest.objectId; jump.objectGeneration = 1U; jump.componentGeneration = 1U; jump.jumpRequested = true; jump.jumpSpeed = 5.0F;
			context.Expect(fixture.backend.UpdateCharacter(jump).Succeeded(), "A grounded virtual character must accept a generation-validated jump.");
			PhysicsCharacterUpdateRequest resize = jump; resize.jumpRequested = false; resize.resizeRequested = true; resize.resizeHeight = 1.5F; resize.recoverPenetration = true;
			context.Expect(fixture.backend.UpdateCharacter(resize).Succeeded(), "A capsule resize must use CharacterVirtual::SetShape and penetration recovery.");
			PhysicsCharacterUpdateRequest teleport = resize; teleport.resizeRequested = false; teleport.teleportRequested = true; teleport.pose.position = {0.0F, 4.0F, 0.0F};
			context.Expect(fixture.backend.UpdateCharacter(teleport).Succeeded(), "Teleport must refresh virtual-character contacts at the provider boundary.");
			context.Expect(!fixture.backend.DestroyCharacter({{42U}, 1U, 0U, 2U}).Succeeded(), "A stale character generation must never destroy the live controller.");
			context.Expect(fixture.backend.DestroyCharacter(cylinder).Succeeded() && fixture.backend.DestroyCharacter(capsule).Succeeded(), "Virtual-character teardown must release provider state before world teardown.");
		});
	}
}
