// PhysicsConstraintCharacterStress.cpp - installed headless readiness physics probe.
//
// The fixture uses only TerraEngine's provider-neutral request and extraction
// records. The concrete Jolt backend is selected by the executable, but no Jolt
// header, identifier, callback, or pointer escapes this direct runtime client.

#include "Physics/Backend/JoltPhysicsBackend.h"
#include "Physics/PhysicsWorld.h"
#include "ThreadPool/ThreadPool.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

namespace {
	using namespace TerraEngine;
	using namespace TerraEngine::Physics;
	using namespace TerraEngine::Physics::Backend;

	// These values make the Debug executable a meaningful phase workload without
	// turning a normal installed-runtime smoke run into a long benchmark.
	constexpr std::uint32_t kChainBodyCount = 48U;
	constexpr std::uint32_t kCharacterCount = 4U;
	constexpr PhysicsTick kFixedSteps = 180U;
	constexpr float kFixedDeltaTime = 1.0F / 60.0F;
	constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
	constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
	constexpr std::string_view kProviderVersion = "5.6.1";
	constexpr std::string_view kProviderRevision = "2e28006e4a367624bb512bf2c37ca99a93d9d1ea";

	struct CharacterIdentity {
		PhysicsCharacterHandle handle;
		UUID objectId{};
		PhysicsGeneration objectGeneration = 1U;
		PhysicsGeneration componentGeneration = 1U;
	};

	struct FixtureCounts {
		std::uint32_t bodies = 0U;
		std::uint32_t constraints = 0U;
		std::uint32_t characters = 0U;
		std::uint32_t supportedCharacters = 0U;
		bool movementJumpResizeTeleport = false;
		std::uint64_t stateHash = kFnvOffset;
	};

	UUID MakeStableId(std::uint32_t family, std::uint32_t ordinal) {
		// Stable UUID bytes make printed hashes comparable across fresh processes.
		UUID id{0x54U, 0x45U, 0x52U, 0x52U, 0x41U, 0x50U, 0x48U, 0x59U,
			0x53U, 0x55U, 0x34U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
		id[11] = static_cast<std::uint8_t>(family);
		id[12] = static_cast<std::uint8_t>(ordinal >> 24U);
		id[13] = static_cast<std::uint8_t>(ordinal >> 16U);
		id[14] = static_cast<std::uint8_t>(ordinal >> 8U);
		id[15] = static_cast<std::uint8_t>(ordinal);
		return id;
	}

	void MixBytes(std::uint64_t& hash, const void* bytes, std::size_t count) {
		const auto* value = static_cast<const std::uint8_t*>(bytes);
		for(std::size_t index = 0U; index < count; ++index) {
			hash ^= value[index];
			hash *= kFnvPrime;
		}
	}

	template<typename Value>
	void MixValue(std::uint64_t& hash, const Value& value) {
		MixBytes(hash, &value, sizeof(Value));
	}

	void MixFloat(std::uint64_t& hash, float value) {
		MixValue(hash, std::bit_cast<std::uint32_t>(value));
	}

	void MixVector(std::uint64_t& hash, PhysicsVector3 value) {
		MixFloat(hash, value.x);
		MixFloat(hash, value.y);
		MixFloat(hash, value.z);
	}

	bool IsFinite(PhysicsVector3 value) {
		return value.IsFinite();
	}

	bool IsFinite(const PhysicsBackendBodyState& state) {
		return state.pose.IsFinite() && IsFinite(state.linearVelocity) && IsFinite(state.angularVelocity)
			&& std::isfinite(state.mass);
	}

	bool IsFinite(const PhysicsBackendConstraintState& state) {
		return std::isfinite(state.linearImpulseMagnitude) && std::isfinite(state.angularImpulseMagnitude);
	}

	bool IsFinite(const PhysicsBackendCharacterState& state) {
		return state.pose.IsFinite() && IsFinite(state.linearVelocity) && IsFinite(state.groundPosition)
			&& IsFinite(state.groundNormal) && IsFinite(state.supportVelocity);
	}

	PhysicsBackendResult CreateBoxShape(
		IPhysicsBackend& backend,
		PhysicsWorldHandle world,
		PhysicsShapeHandle handle,
		PhysicsVector3 dimensions,
		PhysicsBackendStagingToken& staging) {
		PhysicsShapeRecipe recipe;
		recipe.kind = PhysicsShapeKind::Box;
		recipe.dimensions = dimensions;
		return backend.CreateShape(world, handle, recipe, staging);
	}

	PhysicsBackendResult CreateBody(
		IPhysicsBackend& backend,
		PhysicsBackendStagingToken& staging,
		PhysicsWorldHandle world,
		PhysicsShapeHandle shape,
		PhysicsSlotIndex slot,
		PhysicsMotionType motion,
		PhysicsVector3 position,
		std::uint32_t identity) {
		PhysicsBodyCreateRequest request;
		request.handle = {world.worldId, world.worldGeneration, slot, 1U};
		request.objectId = MakeStableId(1U, identity);
		request.objectGeneration = 1U;
		request.componentGeneration = 1U;
		request.shape = shape;
		request.motionType = motion;
		request.pose.position = position;
		request.useGravity = motion == PhysicsMotionType::Dynamic;
		request.allowSleeping = false;
		return backend.CreateBody(request, staging);
	}

	PhysicsConstraintCreateRequest MakeConstraint(
		PhysicsWorldHandle world,
		PhysicsSlotIndex slot,
		PhysicsConstraintKind kind,
		PhysicsBodyHandle bodyA,
		PhysicsBodyHandle bodyB,
		PhysicsVector3 anchorA,
		PhysicsVector3 anchorB) {
		PhysicsConstraintCreateRequest request;
		request.handle = {world.worldId, world.worldGeneration, slot, 1U};
		request.ownerObjectId = MakeStableId(2U, slot);
		request.ownerObjectGeneration = 1U;
		request.componentGeneration = 1U;
		request.recordId = MakeStableId(3U, slot);
		request.recordGeneration = 1U;
		request.kind = kind;
		request.bodyA = bodyA;
		request.bodyB = bodyB;
		request.localFrameA.position = anchorA;
		request.localFrameB.position = anchorB;
		request.axisA = {0.0F, 1.0F, 0.0F};
		request.axisB = request.axisA;
		request.normalAxisA = {1.0F, 0.0F, 0.0F};
		request.normalAxisB = request.normalAxisA;
		for(PhysicsConstraintAxisSettings& axis : request.axes) {
			// Apply the shared limit/spring/motor contract to every family. Individual
			// provider constraints consume the relevant axes and safely ignore others.
			axis.limitEnabled = true;
			axis.minimum = -0.20F;
			axis.maximum = 0.20F;
			axis.springEnabled = true;
			axis.springUsesFrequency = true;
			axis.springFrequency = 4.0F;
			axis.springDamping = 0.75F;
			axis.motorMode = PhysicsBackendMotorMode::PositionAndVelocity;
			axis.motorTargetPosition = 0.0F;
			axis.motorTargetVelocity = 0.0F;
			axis.motorMaximumForce = 250.0F;
		}
		return request;
	}

	bool PublishBodies(IPhysicsBackend& backend, PhysicsWorldHandle world, PhysicsBodyHandle& floor, std::vector<PhysicsBodyHandle>& chain) {
		const PhysicsShapeHandle floorShape{world.worldId, world.worldGeneration, 1U, 1U};
		const PhysicsShapeHandle bodyShape{world.worldId, world.worldGeneration, 2U, 1U};
		PhysicsBackendStagingToken staging;
		if(!CreateBoxShape(backend, world, floorShape, {30.0F, 1.0F, 30.0F}, staging).Succeeded()
			|| !CreateBoxShape(backend, world, bodyShape, {0.40F, 0.40F, 0.40F}, staging).Succeeded()) return false;

		floor = {world.worldId, world.worldGeneration, 1U, 1U};
		if(!CreateBody(backend, staging, world, floorShape, floor.slotIndex, PhysicsMotionType::Static, {0.0F, -0.50F, 0.0F}, 1U).Succeeded()) return false;

		// The chain is deliberately wide and elevated. Gravity, contacts, and 47
		// point constraints all execute without making the floor fixture unstable.
		chain.reserve(kChainBodyCount);
		for(std::uint32_t index = 0U; index < kChainBodyCount; ++index) {
			const PhysicsSlotIndex slot = index + 2U;
			const PhysicsVector3 position{-14.1F + static_cast<float>(index) * 0.60F, 6.0F, 0.0F};
			if(!CreateBody(backend, staging, world, bodyShape, slot, PhysicsMotionType::Dynamic, position, slot).Succeeded()) return false;
			chain.push_back({world.worldId, world.worldGeneration, slot, 1U});
		}
		return backend.CommitStaging(staging).Succeeded();
	}

	bool PublishConstraints(IPhysicsBackend& backend, PhysicsWorldHandle world, const std::vector<PhysicsBodyHandle>& chain) {
		if(chain.size() != kChainBodyCount) return false;
		PhysicsBackendStagingToken staging;
		const std::array<PhysicsConstraintKind, 8U> families{
			PhysicsConstraintKind::Fixed, PhysicsConstraintKind::Point, PhysicsConstraintKind::Distance,
			PhysicsConstraintKind::Hinge, PhysicsConstraintKind::Slider, PhysicsConstraintKind::Cone,
			PhysicsConstraintKind::SwingTwist, PhysicsConstraintKind::SixDegreesOfFreedom};

		PhysicsSlotIndex slot = 1U;
		for(std::size_t index = 0U; index < families.size(); ++index, ++slot) {
			const PhysicsVector3 worldPoint{-12.0F + static_cast<float>(index) * 3.0F, 7.5F, 0.0F};
			PhysicsConstraintCreateRequest request = MakeConstraint(world, slot, families[index], {}, chain[index], worldPoint, {});
			if(!backend.CreateConstraint(request, staging).Succeeded()) return false;
		}

		// Jolt's optional gear is exercised only after its two hinge records have
		// staged, mirroring the provider-neutral dependency expressed by their handles.
		const PhysicsConstraintHandle firstHinge{world.worldId, world.worldGeneration, slot, 1U};
		PhysicsConstraintCreateRequest first = MakeConstraint(world, slot++, PhysicsConstraintKind::Hinge, {}, chain[8U], {12.0F, 7.5F, 0.0F}, {});
		const PhysicsConstraintHandle secondHinge{world.worldId, world.worldGeneration, slot, 1U};
		PhysicsConstraintCreateRequest second = MakeConstraint(world, slot++, PhysicsConstraintKind::Hinge, {}, chain[9U], {15.0F, 7.5F, 0.0F}, {});
		if(!backend.CreateConstraint(first, staging).Succeeded() || !backend.CreateConstraint(second, staging).Succeeded()) return false;
		PhysicsConstraintCreateRequest gear = MakeConstraint(world, slot++, PhysicsConstraintKind::Gear, chain[8U], chain[9U], {}, {});
		gear.gearConstraintA = firstHinge;
		gear.gearConstraintB = secondHinge;
		gear.gearRatio = 2.0F;
		if(!backend.CreateConstraint(gear, staging).Succeeded()) return false;

		for(std::uint32_t index = 1U; index < kChainBodyCount; ++index, ++slot) {
			// Adjacent local anchors coincide in world space at creation, producing a
			// stable dense point-constraint chain rather than a contradictory lattice.
			PhysicsConstraintCreateRequest link = MakeConstraint(world, slot, PhysicsConstraintKind::Point,
				chain[index - 1U], chain[index], {0.30F, 0.0F, 0.0F}, {-0.30F, 0.0F, 0.0F});
			if(!backend.CreateConstraint(link, staging).Succeeded()) return false;
		}
		return backend.CommitStaging(staging).Succeeded();
	}

	bool PublishCharacters(IPhysicsBackend& backend, PhysicsWorldHandle world, std::array<CharacterIdentity, kCharacterCount>& characters) {
		PhysicsBackendStagingToken staging;
		for(std::uint32_t index = 0U; index < kCharacterCount; ++index) {
			CharacterIdentity identity;
			identity.handle = {world.worldId, world.worldGeneration, index, 1U};
			identity.objectId = MakeStableId(4U, index + 1U);
			PhysicsCharacterCreateRequest request;
			request.handle = identity.handle;
			request.objectId = identity.objectId;
			request.objectGeneration = identity.objectGeneration;
			request.componentGeneration = identity.componentGeneration;
			request.shapeKind = index % 2U == 0U ? PhysicsShapeKind::Capsule : PhysicsShapeKind::Cylinder;
			request.radius = 0.45F;
			request.height = 1.80F;
			request.pose.position = {-4.5F + static_cast<float>(index) * 3.0F, 1.0F, 2.0F};
			request.stepHeight = 0.45F;
			request.groundAdhesion = 0.20F;
			request.allowStairWalking = true;
			request.maximumStrength = 1500.0F;
			if(!backend.CreateCharacter(request, staging).Succeeded()) return false;
			characters[index] = identity;
		}
		return backend.CommitStaging(staging).Succeeded();
	}

	bool ExerciseCharacterCommands(
		IPhysicsBackend& backend,
		PhysicsWorldHandle world,
		const std::array<CharacterIdentity, kCharacterCount>& characters, FixtureCounts& counts) {
		bool movementObserved = false;
		bool jumpObserved = false;
		bool resizeSettled = false;
		bool teleportObserved = false;
		for(PhysicsTick tick = 1U; tick <= kFixedSteps; ++tick) {
			for(std::uint32_t index = 0U; index < kCharacterCount; ++index) {
				const CharacterIdentity& identity = characters[index];
				PhysicsCharacterUpdateRequest request;
				request.handle = identity.handle;
				request.objectId = identity.objectId;
				request.objectGeneration = identity.objectGeneration;
				request.componentGeneration = identity.componentGeneration;
				request.fixedDeltaTime = kFixedDeltaTime;
				request.desiredVelocity = {index % 2U == 0U ? 0.60F : -0.45F, 0.0F, 0.0F};
				// The sequence distributes movement, jumping, resizing, teleporting, and
				// ordinary support extraction across capsule and cylinder controllers.
				request.jumpRequested = index == 0U && tick == 30U;
				request.jumpSpeed = request.jumpRequested ? 5.0F : 0.0F;
				request.resizeRequested = index == 1U && tick == 60U;
				request.resizeHeight = request.resizeRequested ? 1.55F : 0.0F;
				request.teleportRequested = index == 2U && tick == 90U;
				if(request.teleportRequested) request.pose.position = {1.5F, 3.0F, 2.0F};
				if(!backend.UpdateCharacter(request).Succeeded()) return false;
			}
			// Updates store intent; this protected provider boundary consumes it once
			// before the rigid-body solver, matching the production fixed-step path.
			if(!backend.AdvanceCharacters(world, tick, kFixedDeltaTime).Succeeded()) return false;
			if(tick == 20U || tick == 30U || tick == 60U || tick == 90U) {
				PhysicsBackendExtraction observation;
				observation.world = world;
				if(!backend.Extract(observation).Succeeded()) return false;
				const auto findState = [&](std::uint32_t index) -> const PhysicsBackendCharacterState* {
					const auto found = std::find_if(observation.characters.begin(), observation.characters.end(),
						[&](const auto& state) { return state.handle == characters[index].handle; });
					return found == observation.characters.end() ? nullptr : &*found;
				};
				const std::uint32_t index = tick == 60U ? 1U : tick == 90U ? 2U : 0U;
				const auto* state = findState(index);
				if(state == nullptr || !IsFinite(*state)
					|| std::abs(state->completedStepDuration - kFixedDeltaTime) > 1.0e-6F) return false;
				if(tick == 20U) movementObserved = state->pose.position.x > -4.45F;
				if(tick == 30U) jumpObserved = state->linearVelocity.y > 1.0F;
				// Extraction does not expose shape dimensions. A successful advance
				// proves SetShape accepted the resize; finite completed state proves
				// settlement, rather than claiming an independent height measurement.
				if(tick == 60U) resizeSettled = !state->motionDiscontinuity;
				if(tick == 90U) teleportObserved = state->motionDiscontinuity
					&& std::abs(state->pose.position.x - 1.5F) < 0.05F
					&& std::abs(state->pose.position.y - 3.0F) < 0.05F
					&& std::abs(state->pose.position.z - 2.0F) < 0.05F;
			}
			if(!backend.Step({world, tick, kFixedDeltaTime, 1U}).Succeeded()) return false;
		}
		counts.movementJumpResizeTeleport = movementObserved && jumpObserved && resizeSettled && teleportObserved;
		return counts.movementJumpResizeTeleport;
	}

	bool ExerciseBreakAndResetContract() {
		// Break thresholds belong to PhysicsWorld policy, not to the provider. This
		// small isolated check proves a break is one-shot and a newer reset token is
		// required before the same record can break again.
		PhysicsWorld policy;
		if(!policy.Create(PhysicsSettings::Defaults())) return false;
		const PhysicsConstraintHandle handle = policy.AllocateConstraint();
		PhysicsConstraintRuntimeRegistration registration;
		registration.handle = handle;
		registration.ownerObjectId = MakeStableId(5U, 1U);
		registration.ownerObjectGeneration = 1U;
		registration.componentGeneration = 1U;
		registration.recordId = MakeStableId(5U, 2U);
		registration.recordGeneration = 1U;
		registration.breakForce = 10.0F;
		registration.breakTorque = std::numeric_limits<float>::infinity();
		registration.resetGeneration = 1U;
		if(!policy.RegisterConstraint(registration)) return false;
		PhysicsConstraintImpulseSample sample;
		sample.handle = handle;
		sample.ownerObjectId = registration.ownerObjectId;
		sample.ownerObjectGeneration = registration.ownerObjectGeneration;
		sample.componentGeneration = registration.componentGeneration;
		sample.recordId = registration.recordId;
		sample.recordGeneration = registration.recordGeneration;
		sample.resetGeneration = 1U;
		sample.linearImpulseMagnitude = 1.0F;
		std::vector<PhysicsEvent> events;
		if(!policy.EvaluateConstraintImpulses(1U, 0.05F, {&sample, 1U}, events)
			|| events.size() != 1U || events.front().type != PhysicsEventType::ConstraintBroken) return false;
		if(policy.EvaluateConstraintImpulses(1U, 0.05F, {&sample, 1U}, events)) return false;
		if(!policy.ResetConstraint(handle, 2U)) return false;
		sample.resetGeneration = 2U;
		events.clear();
		return policy.EvaluateConstraintImpulses(2U, 0.05F, {&sample, 1U}, events)
			&& events.size() == 1U && events.front().constraintResetGeneration == 2U;
	}

	bool InspectExtraction(IPhysicsBackend& backend, PhysicsWorldHandle world, FixtureCounts& counts) {
		PhysicsBackendExtraction extraction;
		extraction.world = world;
		if(!backend.Extract(extraction).Succeeded() || extraction.world != world) return false;
		counts.bodies = static_cast<std::uint32_t>(extraction.bodies.size());
		counts.constraints = static_cast<std::uint32_t>(extraction.constraints.size());
		counts.characters = static_cast<std::uint32_t>(extraction.characters.size());
		for(const PhysicsBackendBodyState& state : extraction.bodies) {
			if(!IsFinite(state)) return false;
			MixValue(counts.stateHash, state.handle.slotIndex);
			MixVector(counts.stateHash, state.pose.position);
			MixVector(counts.stateHash, state.linearVelocity);
		}
		for(const PhysicsBackendConstraintState& state : extraction.constraints) {
			if(!IsFinite(state)) return false;
			MixValue(counts.stateHash, state.handle.slotIndex);
			MixFloat(counts.stateHash, state.linearImpulseMagnitude);
			MixFloat(counts.stateHash, state.angularImpulseMagnitude);
		}
		for(const PhysicsBackendCharacterState& state : extraction.characters) {
			if(!IsFinite(state)) return false;
			counts.supportedCharacters += state.supported ? 1U : 0U;
			MixValue(counts.stateHash, state.handle.slotIndex);
			MixValue(counts.stateHash, static_cast<std::uint8_t>(state.groundState));
			MixVector(counts.stateHash, state.pose.position);
			MixVector(counts.stateHash, state.groundNormal);
			MixValue(counts.stateHash, state.supportBody.slotIndex);
		}
		const std::uint32_t expectedConstraints = 8U + 2U + 1U + (kChainBodyCount - 1U);
		return counts.bodies == kChainBodyCount + 1U && counts.constraints == expectedConstraints
			&& counts.characters == kCharacterCount && counts.supportedCharacters > 0U;
	}

	void PrintResult(bool passed, std::string_view stage, const FixtureCounts& counts, bool breakResetPassed, bool teardownPassed) {
		std::cout
			<< "{\"tool\":\"physics_constraint_character_stress\""
			<< ",\"passed\":" << (passed ? "true" : "false")
			<< ",\"stage\":\"" << stage << "\""
			<< ",\"provider\":{\"name\":\"Jolt\",\"version\":\"" << kProviderVersion
			<< "\",\"revision\":\"" << kProviderRevision << "\"}"
			<< ",\"fixedSteps\":" << kFixedSteps
			<< ",\"bodies\":" << counts.bodies
			<< ",\"constraints\":{\"families\":8,\"gear\":1,\"denseChainLinks\":" << kChainBodyCount - 1U
			<< ",\"total\":" << counts.constraints << ",\"limitsMotorsSprings\":true}"
			<< ",\"characters\":{\"total\":" << counts.characters << ",\"capsules\":2,\"cylinders\":2"
			<< ",\"supported\":" << counts.supportedCharacters << ",\"movementJumpResizeTeleport\":"
			<< (counts.movementJumpResizeTeleport ? "true" : "false") << "}"
			<< ",\"breakReset\":" << (breakResetPassed ? "true" : "false")
			<< ",\"stateHash\":\"" << counts.stateHash << "\""
			<< ",\"teardown\":" << (teardownPassed ? "true" : "false") << "}" << std::endl;
	}
}

int main() {
	bool passed = true;
	bool breakResetPassed = false;
	bool teardownPassed = false;
	FixtureCounts counts;
	std::string_view stage = "initialize";

	try {
		ThreadPool& pool = ThreadPool::instance();
		pool.shutdown();
		pool.initialize();
		{
			JoltPhysicsBackend concreteBackend(pool);
			IPhysicsBackend& backend = concreteBackend;
			const PhysicsWorldHandle world{{9902U}, 1U};
			PhysicsBackendWorldConfiguration configuration;
			configuration.world = world;
			configuration.maximumBodies = 256U;
			configuration.maximumBodyPairs = 4096U;
			configuration.maximumContactConstraints = 4096U;
			passed = backend.Initialize().Succeeded() && backend.CreateWorld(configuration).Succeeded();

			PhysicsBodyHandle floor;
			std::vector<PhysicsBodyHandle> chain;
			std::array<CharacterIdentity, kCharacterCount> characters{};
			if(passed) { stage = "publish-bodies"; passed = PublishBodies(backend, world, floor, chain); }
			if(passed) { stage = "publish-constraints"; passed = PublishConstraints(backend, world, chain); }
			if(passed) { stage = "publish-characters"; passed = PublishCharacters(backend, world, characters); }
			if(passed) { stage = "fixed-character-commands"; passed = ExerciseCharacterCommands(backend, world, characters, counts); }
			if(passed) { stage = "break-reset-contract"; breakResetPassed = ExerciseBreakAndResetContract(); }
			if(passed) { stage = "extract-invariants"; passed = breakResetPassed && InspectExtraction(backend, world, counts) && backend.IsHealthy(); }

			// Provider teardown must complete before stopping the shared task pool, since
			// the Jolt adapter may still own pool-backed work at its shutdown boundary.
			const bool destroyed = backend.DestroyWorld(world).Succeeded();
			const bool shutDown = backend.Shutdown().Succeeded();
			teardownPassed = destroyed && shutDown;
			passed = passed && teardownPassed;
			if(passed) stage = "complete";
		}
		pool.shutdown();
	} catch(const std::exception& exception) {
		std::cerr << "physics_constraint_character_stress exception: " << exception.what() << std::endl;
		passed = false;
		ThreadPool::instance().shutdown();
	} catch(...) {
		std::cerr << "physics_constraint_character_stress exception: unknown" << std::endl;
		passed = false;
		ThreadPool::instance().shutdown();
	}

	PrintResult(passed, stage, counts, breakResetPassed, teardownPassed);
	return passed ? 0 : 1;
}
