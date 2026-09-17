// Direct CharacterVirtual regressions. The shared test registration/CMake slice
// owns inclusion; this file keeps the provider exercise isolated and headless.
#include "../TestHarness.h"

#include "Physics/Backend/JoltPhysicsBackend.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

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
		registry.Add("Physics.CharacterSimulation.FixedStepCadenceCoalescesRenderRateIntent", [](TestContext& context) {
			CharacterFixture fixture;
			const PhysicsCharacterHandle character{{42U}, 1U, 8U, 1U};
			PhysicsCharacterCreateRequest characterRequest = fixture.CreateRequest(character, PhysicsShapeKind::Capsule);
			characterRequest.useGravity = false;
			PhysicsBackendStagingToken staging;
			context.Expect(fixture.backend.CreateCharacter(characterRequest, staging).Succeeded()
				&& fixture.backend.CommitStaging(staging).Succeeded(),
				"The cadence fixture must publish one controller before testing stored motion intent.");

			PhysicsCharacterUpdateRequest move;
			move.handle = character;
			move.objectId = characterRequest.objectId;
			move.objectGeneration = characterRequest.objectGeneration;
			move.componentGeneration = characterRequest.componentGeneration;
			move.desiredVelocity = {3.0F, 0.0F, 0.0F};
			constexpr float fixedDeltaTime = 1.0F / 60.0F;
			for(PhysicsTick tick = 1U; tick <= 60U; ++tick) {
				// Eight render-rate submissions model a high-frame-rate script while
				// exactly one fixed step consumes the last copied desired velocity.
				for(std::uint32_t submission = 0U; submission < 8U; ++submission) {
					context.Expect(fixture.backend.UpdateCharacter(move).Succeeded(),
						"Each render-rate character intent must be accepted before its shared fixed step.");
				}
				context.Expect(fixture.backend.AdvanceCharacters(fixture.world, tick, fixedDeltaTime).Succeeded()
					&& fixture.backend.Step({fixture.world, tick, fixedDeltaTime, 1U}).Succeeded(),
					"One planned physics step must consume coalesced character intent once.");
			}

			PhysicsBackendExtraction afterBurst;
			afterBurst.world = fixture.world;
			context.Expect(fixture.backend.Extract(afterBurst).Succeeded(),
				"The cadence fixture must extract the fixed-step authoritative pose.");
			const auto burstState = std::find_if(afterBurst.characters.begin(), afterBurst.characters.end(),
				[character](const PhysicsBackendCharacterState& candidate) { return candidate.handle == character; });
			context.Expect(burstState != afterBurst.characters.end()
				&& std::fabs(burstState->pose.position.x - 3.0F) < 0.15F,
				"Eight render submissions per tick must travel the same three metres as one velocity intent per fixed second.");

			for(PhysicsTick tick = 61U; tick <= 90U; ++tick) {
				context.Expect(fixture.backend.AdvanceCharacters(fixture.world, tick, fixedDeltaTime).Succeeded()
					&& fixture.backend.Step({fixture.world, tick, fixedDeltaTime, 1U}).Succeeded(),
					"Stored desired velocity must remain active when a fixed tick has no new render command.");
			}
			PhysicsBackendExtraction afterQuietSteps;
			afterQuietSteps.world = fixture.world;
			context.Expect(fixture.backend.Extract(afterQuietSteps).Succeeded(),
				"The persistent-intent fixture must extract the later fixed-step pose.");
			const auto quietState = std::find_if(afterQuietSteps.characters.begin(), afterQuietSteps.characters.end(),
				[character](const PhysicsBackendCharacterState& candidate) { return candidate.handle == character; });
			context.Expect(quietState != afterQuietSteps.characters.end()
				&& std::fabs(quietState->pose.position.x - 4.5F) < 0.15F,
				"A fixed-step controller must retain desired velocity until an explicit move or stop replaces it.");
			PhysicsCharacterUpdateRequest disable = move;
			disable.enabled = false;
			disable.teleportRequested = true;
			disable.pose.position = {99.0F, 1.1F, 0.0F};
			context.Expect(fixture.backend.UpdateCharacter(disable).Succeeded(),
				"Disabling a controller must accept and discard any concurrently supplied one-shot intent.");
			PhysicsCharacterUpdateRequest reenable = move;
			reenable.desiredVelocity = {};
			reenable.enabled = true;
			context.Expect(fixture.backend.UpdateCharacter(reenable).Succeeded()
				&& fixture.backend.AdvanceCharacters(fixture.world, 91U, fixedDeltaTime).Succeeded()
				&& fixture.backend.Step({fixture.world, 91U, fixedDeltaTime, 1U}).Succeeded(),
				"Re-enabling a controller must begin from explicit new motion intent only.");
			PhysicsBackendExtraction afterReenable;
			afterReenable.world = fixture.world;
			context.Expect(fixture.backend.Extract(afterReenable).Succeeded(),
				"The disable-cancellation fixture must extract its re-enabled controller pose.");
			const auto reenabledState = std::find_if(afterReenable.characters.begin(), afterReenable.characters.end(),
				[character](const PhysicsBackendCharacterState& candidate) { return candidate.handle == character; });
			context.Expect(reenabledState != afterReenable.characters.end()
				&& std::fabs(reenabledState->pose.position.x - 4.5F) < 0.15F,
				"A disabled controller must not replay its discarded teleport or persistent velocity when it returns.");
			context.Expect(fixture.backend.DestroyCharacter(character).Succeeded(),
				"The cadence fixture must release its controller after the fixed-step assertions.");
		});

		registry.Add("Physics.CharacterSimulation.MovingKinematicBodyBlocksCharacterMove", [](TestContext& context) {
			CharacterFixture fixture;
			const PhysicsShapeHandle blockerShape{{42U}, 1U, 2U, 1U};
			const PhysicsBodyHandle blockerBody{{42U}, 1U, 2U, 1U};
			const PhysicsCharacterHandle character{{42U}, 1U, 3U, 1U};
			PhysicsBackendStagingToken staging;
			PhysicsShapeRecipe blockerRecipe;
			blockerRecipe.kind = PhysicsShapeKind::Box;
			blockerRecipe.dimensions = {1.0F, 2.0F, 2.0F};
			context.Expect(fixture.backend.CreateShape(fixture.world, blockerShape, blockerRecipe, staging).Succeeded(),
				"The moving-blocker fixture must stage its kinematic box shape.");
			PhysicsBodyCreateRequest blocker;
			blocker.handle = blockerBody;
			blocker.objectId = TerraEngine::GenerateUUID();
			blocker.objectGeneration = 1U;
			blocker.componentGeneration = 1U;
			blocker.shape = blockerShape;
			blocker.motionType = PhysicsMotionType::Kinematic;
			blocker.pose.position = {2.0F, 1.0F, 0.0F};
			context.Expect(fixture.backend.CreateBody(blocker, staging).Succeeded()
				&& fixture.backend.CommitStaging(staging).Succeeded(),
				"The moving-blocker fixture must publish its kinematic collider before character movement.");

			PhysicsCharacterCreateRequest characterRequest = fixture.CreateRequest(character, PhysicsShapeKind::Capsule);
			characterRequest.useGravity = false;
			PhysicsBackendStagingToken characterStaging;
			context.Expect(fixture.backend.CreateCharacter(characterRequest, characterStaging).Succeeded()
				&& fixture.backend.CommitStaging(characterStaging).Succeeded(),
				"The moving-blocker fixture must publish the character controller.");

			// Move the kinematic body through the same typed provider update used by a
			// script-driven Transform. The character update below must sweep against
			// this current nonstatic pose instead of using a static-only collision path.
			PhysicsBodyUpdateRequest blockerMove;
			blockerMove.handle = blockerBody;
			blockerMove.pose = blocker.pose;
			blockerMove.pose.position.x = 1.5F;
			blockerMove.enabled = true;
			context.Expect(fixture.backend.UpdateBody(blockerMove).Succeeded(),
				"The moving-blocker fixture must update the kinematic blocker before the character sweep.");
			auto moveCharacter = [&](PhysicsTick tick) {
				PhysicsCharacterUpdateRequest move;
				move.handle = character;
				move.objectId = characterRequest.objectId;
				move.objectGeneration = characterRequest.objectGeneration;
				move.componentGeneration = characterRequest.componentGeneration;
				move.desiredVelocity = {10.0F, 0.0F, 0.0F};
				move.fixedDeltaTime = 1.0F / 60.0F;
				return fixture.backend.UpdateCharacter(move).Succeeded()
					&& fixture.backend.AdvanceCharacters(fixture.world, tick, move.fixedDeltaTime).Succeeded()
					&& fixture.backend.Step({fixture.world, tick, move.fixedDeltaTime, 1U}).Succeeded();
			};
			auto readCharacterPosition = [&]() {
				PhysicsBackendExtraction extraction;
				extraction.world = fixture.world;
				if(!fixture.backend.Extract(extraction).Succeeded()) return std::optional<float>{};
				const auto state = std::find_if(extraction.characters.begin(), extraction.characters.end(),
					[character](const PhysicsBackendCharacterState& candidate) { return candidate.handle == character; });
				return state == extraction.characters.end() ? std::optional<float>{}
					: std::optional<float>{state->pose.position.x};
			};
			for(PhysicsTick tick = 1U; tick <= 30U; ++tick) {
				context.Expect(moveCharacter(tick),
					"Each character movement tick must remain provider-safe near a moving kinematic blocker.");
				const std::optional<float> position = readCharacterPosition();
				context.Expect(position.has_value() && *position < 0.75F,
					"Every character pose while the kinematic blocker is present must remain before its collision boundary.");
			}

			blockerMove.pose.position.x = 20.0F;
			context.Expect(fixture.backend.UpdateBody(blockerMove).Succeeded(),
				"Moving the kinematic blocker away must update the same collision body.");
			for(PhysicsTick tick = 31U; tick <= 45U; ++tick) {
				context.Expect(moveCharacter(tick),
					"The character must remain provider-safe after its kinematic blocker moves away.");
			}
			const std::optional<float> resumedPosition = readCharacterPosition();
			context.Expect(resumedPosition.has_value() && *resumedPosition > 1.5F,
				"A character must advance through the former barrier after the kinematic blocker moves away.");

			// Reset the independent character state before exercising an ordinary
			// dynamic body, so this second assertion cannot pass from prior progress.
			PhysicsCharacterUpdateRequest reset;
			reset.handle = character;
			reset.objectId = characterRequest.objectId;
			reset.objectGeneration = characterRequest.objectGeneration;
			reset.componentGeneration = characterRequest.componentGeneration;
			reset.pose = characterRequest.pose;
			reset.fixedDeltaTime = 1.0F / 60.0F;
			reset.teleportRequested = true;
			context.Expect(fixture.backend.UpdateCharacter(reset).Succeeded()
				&& fixture.backend.AdvanceCharacters(fixture.world, 46U, reset.fixedDeltaTime).Succeeded()
				&& fixture.backend.Step({fixture.world, 46U, reset.fixedDeltaTime, 1U}).Succeeded(),
				"The dynamic-blocker fixture must reset the character to its known starting pose.");

			const PhysicsShapeHandle dynamicShape{{42U}, 1U, 4U, 1U};
			const PhysicsBodyHandle dynamicBody{{42U}, 1U, 4U, 1U};
			PhysicsBackendStagingToken dynamicStaging;
			context.Expect(fixture.backend.CreateShape(fixture.world, dynamicShape, blockerRecipe, dynamicStaging).Succeeded(),
				"The dynamic-blocker fixture must stage its heavy box shape.");
			PhysicsBodyCreateRequest dynamicBlocker;
			dynamicBlocker.handle = dynamicBody;
			dynamicBlocker.objectId = TerraEngine::GenerateUUID();
			dynamicBlocker.objectGeneration = 1U;
			dynamicBlocker.componentGeneration = 1U;
			dynamicBlocker.shape = dynamicShape;
			dynamicBlocker.motionType = PhysicsMotionType::Dynamic;
			dynamicBlocker.mass = 1'000'000.0F;
			dynamicBlocker.useGravity = false;
			dynamicBlocker.pose.position = {1.5F, 1.0F, 0.0F};
			context.Expect(fixture.backend.CreateBody(dynamicBlocker, dynamicStaging).Succeeded()
				&& fixture.backend.CommitStaging(dynamicStaging).Succeeded(),
				"The dynamic-blocker fixture must publish an intentionally heavy ordinary rigid body.");
			// The contact must prevent phasing; its solver impulse may legitimately
			// nudge a dynamic body, so this test intentionally does not require it
			// to be perfectly immovable.
			for(PhysicsTick tick = 47U; tick <= 76U; ++tick) {
				context.Expect(moveCharacter(tick),
					"Each character movement tick must remain provider-safe near a heavy dynamic blocker.");
				const std::optional<float> position = readCharacterPosition();
				context.Expect(position.has_value() && *position < 0.75F,
					"Every character pose while the heavy dynamic blocker is present must remain before its collision boundary.");
			}
		});

		registry.Add("Physics.CharacterSimulation.ControllerOnlyCharactersBlockEachOther", [](TestContext& context) {
			CharacterFixture fixture;
			const PhysicsCharacterHandle mover{{42U}, 1U, 6U, 1U};
			const PhysicsCharacterHandle blocker{{42U}, 1U, 7U, 1U};
			PhysicsCharacterCreateRequest moverRequest = fixture.CreateRequest(mover, PhysicsShapeKind::Capsule);
			PhysicsCharacterCreateRequest blockerRequest = fixture.CreateRequest(blocker, PhysicsShapeKind::Capsule);
			moverRequest.useGravity = false;
			blockerRequest.useGravity = false;
			blockerRequest.pose.position.x = 1.5F;
			PhysicsBackendStagingToken staging;
			context.Expect(fixture.backend.CreateCharacter(moverRequest, staging).Succeeded()
				&& fixture.backend.CreateCharacter(blockerRequest, staging).Succeeded()
				&& fixture.backend.CommitStaging(staging).Succeeded(),
				"Two controller-only characters must publish together without requiring ColliderSet-backed rigid bodies.");

			for(PhysicsTick tick = 1U; tick <= 30U; ++tick) {
				PhysicsCharacterUpdateRequest move;
				move.handle = mover;
				move.objectId = moverRequest.objectId;
				move.objectGeneration = moverRequest.objectGeneration;
				move.componentGeneration = moverRequest.componentGeneration;
				move.desiredVelocity = {10.0F, 0.0F, 0.0F};
				move.fixedDeltaTime = 1.0F / 60.0F;
				context.Expect(fixture.backend.UpdateCharacter(move).Succeeded()
					&& fixture.backend.AdvanceCharacters(fixture.world, tick, move.fixedDeltaTime).Succeeded()
					&& fixture.backend.Step({fixture.world, tick, move.fixedDeltaTime, 1U}).Succeeded(),
					"The moving controller must update through the serialized virtual-character boundary.");
			}

			PhysicsBackendExtraction extraction;
			extraction.world = fixture.world;
			context.Expect(fixture.backend.Extract(extraction).Succeeded(),
				"Controller-only collision coverage must extract the authoritative provider state.");
			const auto moved = std::find_if(extraction.characters.begin(), extraction.characters.end(),
				[mover](const PhysicsBackendCharacterState& candidate) { return candidate.handle == mover; });
			const auto stationary = std::find_if(extraction.characters.begin(), extraction.characters.end(),
				[blocker](const PhysicsBackendCharacterState& candidate) { return candidate.handle == blocker; });
			const float moverPosition = moved != extraction.characters.end() ? moved->pose.position.x : -1.0F;
			const std::uint32_t moverContacts = moved != extraction.characters.end() ? moved->contactCount : 0U;
			const PhysicsVector3 stationaryPosition = stationary != extraction.characters.end()
				? stationary->pose.position : PhysicsVector3{};
			const float stationaryDeltaX = stationaryPosition.x - blockerRequest.pose.position.x;
			const float stationaryDeltaY = stationaryPosition.y - blockerRequest.pose.position.y;
			const float stationaryDeltaZ = stationaryPosition.z - blockerRequest.pose.position.z;
			const float stationaryHorizontalDrift = std::sqrt(stationaryDeltaX * stationaryDeltaX
				+ stationaryDeltaZ * stationaryDeltaZ);
			context.Expect(moved != extraction.characters.end() && moverPosition < 0.75F,
				"A controller-only follower must not phase through another active controller on a mutually-colliding layer; observed x="
					+ std::to_string(moverPosition) + ", contacts=" + std::to_string(moverContacts)
					+ ", committedCharacters=" + std::to_string(extraction.characters.size()) + ".");
			context.Expect(stationary != extraction.characters.end()
				&& stationaryHorizontalDrift < 0.05F,
				"A stationary controller may receive only bounded horizontal contact-padding depenetration, never character-to-character movement; observed position=("
					+ std::to_string(stationaryPosition.x) + ", " + std::to_string(stationaryPosition.y) + ", "
					+ std::to_string(stationaryPosition.z) + "), delta=(" + std::to_string(stationaryDeltaX) + ", "
					+ std::to_string(stationaryDeltaY) + ", " + std::to_string(stationaryDeltaZ) + "), horizontalMagnitude="
					+ std::to_string(stationaryHorizontalDrift) + ". Vertical ground resolution is independent of controller collision.");
			context.Expect(fixture.backend.DestroyCharacter(blocker).Succeeded(),
				"Destroying a controller must unregister its world-owned collision entry before provider teardown.");
			for(PhysicsTick tick = 31U; tick <= 45U; ++tick) {
				PhysicsCharacterUpdateRequest move;
				move.handle = mover;
				move.objectId = moverRequest.objectId;
				move.objectGeneration = moverRequest.objectGeneration;
				move.componentGeneration = moverRequest.componentGeneration;
				move.desiredVelocity = {10.0F, 0.0F, 0.0F};
				move.fixedDeltaTime = 1.0F / 60.0F;
				context.Expect(fixture.backend.UpdateCharacter(move).Succeeded()
					&& fixture.backend.AdvanceCharacters(fixture.world, tick, move.fixedDeltaTime).Succeeded()
					&& fixture.backend.Step({fixture.world, tick, move.fixedDeltaTime, 1U}).Succeeded(),
					"The follower must resume safely after its controller blocker is destroyed.");
			}
			PhysicsBackendExtraction afterDestroy;
			afterDestroy.world = fixture.world;
			context.Expect(fixture.backend.Extract(afterDestroy).Succeeded(),
				"Unregistration coverage must read the current controller position.");
			const auto resumed = std::find_if(afterDestroy.characters.begin(), afterDestroy.characters.end(),
				[mover](const PhysicsBackendCharacterState& candidate) { return candidate.handle == mover; });
			context.Expect(resumed != afterDestroy.characters.end() && resumed->pose.position.x > 1.5F,
				"A removed controller must no longer leave a stale collision entry behind its former boundary.");

			PhysicsCharacterUpdateRequest reset;
			reset.handle = mover;
			reset.objectId = moverRequest.objectId;
			reset.objectGeneration = moverRequest.objectGeneration;
			reset.componentGeneration = moverRequest.componentGeneration;
			reset.pose = moverRequest.pose;
			reset.fixedDeltaTime = 1.0F / 60.0F;
			reset.teleportRequested = true;
			context.Expect(fixture.backend.UpdateCharacter(reset).Succeeded()
				&& fixture.backend.AdvanceCharacters(fixture.world, 46U, reset.fixedDeltaTime).Succeeded()
				&& fixture.backend.Step({fixture.world, 46U, reset.fixedDeltaTime, 1U}).Succeeded(),
				"The replacement fixture must reset the follower before testing a reused controller slot.");
			const PhysicsCharacterHandle maskedBlocker{{42U}, 1U, 7U, 2U};
			PhysicsCharacterCreateRequest maskedRequest = fixture.CreateRequest(maskedBlocker, PhysicsShapeKind::Capsule);
			maskedRequest.useGravity = false;
			maskedRequest.pose.position.x = 1.5F;
			maskedRequest.collisionMask = 0U;
			PhysicsBackendStagingToken replacementStaging;
			context.Expect(fixture.backend.CreateCharacter(maskedRequest, replacementStaging).Succeeded()
				&& fixture.backend.CommitStaging(replacementStaging).Succeeded(),
				"A new controller incarnation must register with its own copied collision policy.");
			for(PhysicsTick tick = 47U; tick <= 61U; ++tick) {
				PhysicsCharacterUpdateRequest move;
				move.handle = mover;
				move.objectId = moverRequest.objectId;
				move.objectGeneration = moverRequest.objectGeneration;
				move.componentGeneration = moverRequest.componentGeneration;
				move.desiredVelocity = {10.0F, 0.0F, 0.0F};
				move.fixedDeltaTime = 1.0F / 60.0F;
				context.Expect(fixture.backend.UpdateCharacter(move).Succeeded()
					&& fixture.backend.AdvanceCharacters(fixture.world, tick, move.fixedDeltaTime).Succeeded()
					&& fixture.backend.Step({fixture.world, tick, move.fixedDeltaTime, 1U}).Succeeded(),
					"The follower must update safely near a layer-mask-excluded replacement controller.");
			}
			PhysicsBackendExtraction maskedExtraction;
			maskedExtraction.world = fixture.world;
			context.Expect(fixture.backend.Extract(maskedExtraction).Succeeded(),
				"Layer-mask coverage must read the replacement controller result.");
			const auto maskExcluded = std::find_if(maskedExtraction.characters.begin(), maskedExtraction.characters.end(),
				[mover](const PhysicsBackendCharacterState& candidate) { return candidate.handle == mover; });
			context.Expect(maskExcluded != maskedExtraction.characters.end() && maskExcluded->pose.position.x > 1.5F,
				"A replacement controller excluded by its mask must not block the follower or inherit a stale predecessor entry.");
			context.Expect(fixture.backend.DestroyCharacter(maskedBlocker).Succeeded()
				&& fixture.backend.DestroyCharacter(mover).Succeeded(),
				"Every controller incarnation must unregister before provider teardown.");
		});

		registry.Add("Physics.CharacterSimulation.CapsuleCylinderGroundJumpResizeTeleportAndTeardown", [](TestContext& context) {
			CharacterFixture fixture; const PhysicsCharacterHandle capsule{{42U}, 1U, 5U, 1U}; const PhysicsCharacterHandle cylinder{{42U}, 1U, 1U, 1U};
			PhysicsCharacterCreateRequest capsuleRequest = fixture.CreateRequest(capsule, PhysicsShapeKind::Capsule); PhysicsCharacterCreateRequest cylinderRequest = fixture.CreateRequest(cylinder, PhysicsShapeKind::Cylinder); cylinderRequest.pose.position.x = 3.0F;
			PhysicsBackendStagingToken staging; context.Expect(fixture.backend.CreateCharacter(capsuleRequest, staging).Succeeded() && fixture.backend.CreateCharacter(cylinderRequest, staging).Succeeded(), "Capsule and cylinder virtual characters must stage together.");
			context.Expect(fixture.backend.CommitStaging(staging).Succeeded(), "Character staging must publish atomically.");
			for(PhysicsTick tick = 1U; tick <= 90U; ++tick) {
				PhysicsCharacterUpdateRequest update; update.handle = capsule; update.objectId = capsuleRequest.objectId; update.objectGeneration = 1U; update.componentGeneration = 1U; update.desiredVelocity = {1.0F, 0.0F, 0.0F};
				context.Expect(fixture.backend.UpdateCharacter(update).Succeeded()
					&& fixture.backend.AdvanceCharacters(fixture.world, tick, 1.0F / 60.0F).Succeeded()
					&& fixture.backend.Step({fixture.world, tick, 1.0F / 60.0F, 1U}).Succeeded(),
					"Ground and stair character updates must remain provider-safe.");
			}
			PhysicsBackendExtraction extraction; extraction.world = fixture.world; fixture.backend.Extract(extraction);
			context.Expect(extraction.characters.size() == 2U
				&& extraction.characters[0].handle == cylinder && extraction.characters[1].handle == capsule,
				"Character extraction must reuse ascending committed membership order after reverse-order staging.");
			const auto grounded = std::find_if(extraction.characters.begin(), extraction.characters.end(), [&](const auto& state) { return state.handle == capsule; });
			context.Expect(grounded != extraction.characters.end() && grounded->supported && grounded->supportBody == fixture.floorBody, "Ground support, moving-support velocity publication, and contacts must use engine body handles.");
			context.Expect(std::isfinite(grounded->groundNormal.y) && grounded->contactCount > 0U, "Ground and slope state must remain finite under Debug Jolt tolerances.");
			context.Expect(grounded != extraction.characters.end() && grounded->objectId == capsuleRequest.objectId
				&& grounded->objectGeneration == capsuleRequest.objectGeneration
				&& grounded->componentGeneration == capsuleRequest.componentGeneration,
				"Extracted character state must preserve the copied object and component generations needed to reject stale managed commands.");
			PhysicsCharacterUpdateRequest jump; jump.handle = capsule; jump.objectId = capsuleRequest.objectId; jump.objectGeneration = 1U; jump.componentGeneration = 1U; jump.jumpRequested = true; jump.jumpSpeed = 5.0F;
			context.Expect(fixture.backend.UpdateCharacter(jump).Succeeded()
				&& fixture.backend.AdvanceCharacters(fixture.world, 91U, 1.0F / 60.0F).Succeeded()
				&& fixture.backend.Step({fixture.world, 91U, 1.0F / 60.0F, 1U}).Succeeded(),
				"A grounded virtual character must apply a generation-validated jump once at its fixed step.");
			PhysicsCharacterUpdateRequest resize = jump; resize.jumpRequested = false; resize.resizeRequested = true; resize.resizeHeight = 1.5F; resize.recoverPenetration = true;
			context.Expect(fixture.backend.UpdateCharacter(resize).Succeeded()
				&& fixture.backend.AdvanceCharacters(fixture.world, 92U, 1.0F / 60.0F).Succeeded()
				&& fixture.backend.Step({fixture.world, 92U, 1.0F / 60.0F, 1U}).Succeeded(),
				"A capsule resize must use CharacterVirtual::SetShape and penetration recovery at one fixed step.");
			PhysicsCharacterUpdateRequest teleport = resize; teleport.resizeRequested = false; teleport.teleportRequested = true; teleport.pose.position = {0.0F, 4.0F, 0.0F};
			context.Expect(fixture.backend.UpdateCharacter(teleport).Succeeded()
				&& fixture.backend.AdvanceCharacters(fixture.world, 93U, 1.0F / 60.0F).Succeeded()
				&& fixture.backend.Step({fixture.world, 93U, 1.0F / 60.0F, 1U}).Succeeded(),
				"Teleport must refresh virtual-character contacts at the provider fixed-step boundary.");
			context.Expect(!fixture.backend.DestroyCharacter({{42U}, 1U, 5U, 2U}).Succeeded(), "A stale character generation must never destroy the live controller.");
			context.Expect(fixture.backend.DestroyCharacter(cylinder).Succeeded() && fixture.backend.DestroyCharacter(capsule).Succeeded(), "Virtual-character teardown must release provider state before world teardown.");
		});
	}
}
