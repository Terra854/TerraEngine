// PhysicsGameplayStress.cpp - installed-runtime stress probe for physics gameplay APIs.
//
// This executable is intentionally a small direct client of the public engine physics
// boundary. It does not include Jolt headers or IDs: the backend remains the only
// provider-aware implementation, while this probe demonstrates the same contracts a
// gameplay runtime uses under a dense query and command workload.

#include "Physics/Backend/JoltPhysicsBackend.h"
#include "Physics/PhysicsCommands.h"
#include "ThreadPool/ThreadPool.h"
#include "Utilities/UUID.h"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
	using namespace TerraEngine;
	using namespace TerraEngine::Physics;
	using namespace TerraEngine::Physics::Backend;

	// Keep the workload practical for an installed editor while still exercising the
	// exact project batch limit. These values deliberately mirror PhysicsSettings.
	constexpr std::uint32_t kGridWidth = 32U;
	constexpr std::uint32_t kBodyCount = kGridWidth * kGridWidth;
	constexpr std::uint32_t kQueryCount = 10000U;
	constexpr std::uint32_t kProducerCount = 4U;
	constexpr std::uint32_t kCommandsPerProducer = 64U;
	constexpr std::string_view kProviderVersion = "5.6.1";
	constexpr std::string_view kProviderRevision = "2e28006e4a367624bb512bf2c37ca99a93d9d1ea";
	constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
	constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

	struct BodyIdentity {
		PhysicsBodyHandle handle;
		UUID objectId{};
	};

	struct QueryPassResult {
		bool passed = true;
		std::uint32_t successfulQueries = 0U;
		std::uint32_t queriesWithHits = 0U;
		std::uint64_t hitCount = 0U;
		std::uint64_t orderHash = kFnvOffset;
		double milliseconds = 0.0;
	};

	UUID MakeBodyId(std::uint32_t index) {
		// Stable object identities make the query hash reproducible across separate
		// installed runs, not merely across two passes inside one process.
		UUID id{0x54U, 0x45U, 0x50U, 0x48U, 0x59U, 0x53U, 0x49U, 0x43U,
			0x53U, 0x54U, 0x52U, 0x45U, 0x53U, 0x53U, 0x00U, 0x00U};
		const std::uint32_t ordinal = index + 1U;
		id[12] = static_cast<std::uint8_t>(ordinal >> 24U);
		id[13] = static_cast<std::uint8_t>(ordinal >> 16U);
		id[14] = static_cast<std::uint8_t>(ordinal >> 8U);
		id[15] = static_cast<std::uint8_t>(ordinal);
		return id;
	}

	void MixBytes(std::uint64_t& hash, const void* bytes, std::size_t byteCount) {
		const auto* value = static_cast<const std::uint8_t*>(bytes);
		for(std::size_t index = 0U; index < byteCount; ++index) {
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

	void MixHit(std::uint64_t& hash, const PhysicsBackendQueryHit& hit) {
		// Hash the engine-owned hit fields in their documented stable order. Provider
		// pointers and Jolt IDs never participate in this reproducibility check.
		MixValue(hash, hit.body.slotIndex);
		MixValue(hash, hit.body.slotGeneration);
		MixBytes(hash, hit.objectId.data(), hit.objectId.size());
		MixValue(hash, hit.objectGeneration);
		MixValue(hash, hit.componentGeneration);
		MixVector(hash, hit.position);
		MixVector(hash, hit.normal);
		MixFloat(hash, hit.distance);
		MixFloat(hash, hit.fraction);
		MixValue(hash, hit.subShapeKey);
		MixValue(hash, hit.collisionLayer);
		MixValue(hash, static_cast<std::uint8_t>(hit.motionType));
		MixValue(hash, static_cast<std::uint8_t>(hit.sensor));
	}

	PhysicsBackendQueryRequest MakeStressQuery(PhysicsWorldHandle world, std::uint32_t index) {
		const std::uint32_t cell = index % kBodyCount;
		const std::uint32_t row = cell / kGridWidth;
		const std::uint32_t column = cell % kGridWidth;
		const float x = (static_cast<float>(column) - 15.5F) * 2.0F;
		const float z = (static_cast<float>(row) - 15.5F) * 2.0F;

		PhysicsBackendQueryRequest request;
		request.world = world;
		request.hitMode = PhysicsQueryHitMode::Closest;
		request.maximumHits = 1U;
		request.filter.bodyTypes = PhysicsQueryBodyTypeMask::All;
		request.pose.position = {x, 0.0F, z};

		// Cycle every supported query family so the same exact 10,000-request batch
		// covers ray, sweep, overlap, and point code paths through the real backend.
		switch(index % 4U) {
		case 0U:
			request.kind = PhysicsQueryKind::RayCast;
			request.pose.position.y = 4.0F;
			request.direction = {0.0F, -1.0F, 0.0F};
			request.distance = 8.0F;
			break;
		case 1U:
			request.kind = PhysicsQueryKind::ShapeCast;
			request.pose.position.y = 3.0F;
			request.direction = {0.0F, -1.0F, 0.0F};
			request.distance = 6.0F;
			request.shapeExtent = {0.20F, 0.20F, 0.20F};
			break;
		case 2U:
			request.kind = PhysicsQueryKind::Overlap;
			request.shapeExtent = {0.20F, 0.20F, 0.20F};
			break;
		default:
			request.kind = PhysicsQueryKind::Point;
			break;
		}
		return request;
	}

	QueryPassResult RunQueryPass(JoltPhysicsBackend& backend, PhysicsWorldHandle world) {
		struct QuerySlot {
			bool succeeded = false;
			bool hadHit = false;
			std::uint32_t hitCount = 0U;
			std::uint64_t hash = kFnvOffset;
		};

		std::vector<QuerySlot> slots(kQueryCount);
		std::vector<std::future<void>> completions;
		completions.reserve(kQueryCount);
		ThreadPool& pool = ThreadPool::instance();
		const auto start = std::chrono::steady_clock::now();

		for(std::uint32_t index = 0U; index < kQueryCount; ++index) {
			// Each task owns its copied request and writes one exclusive result slot.
			// This avoids locks in the measurement path while the backend's shared lock
			// protects concurrent read-only provider queries.
			auto task = pool.createTask([&backend, &slots, request = MakeStressQuery(world, index), index]() {
				const PhysicsBackendQueryResult result = backend.Query(request);
				QuerySlot& slot = slots[index];
				slot.succeeded = result.result.Succeeded();
				slot.hitCount = static_cast<std::uint32_t>(result.hits.size());
				slot.hadHit = !result.hits.empty();
				MixValue(slot.hash, index);
				MixValue(slot.hash, static_cast<std::uint8_t>(result.result.status));
				MixValue(slot.hash, static_cast<std::uint8_t>(result.result.error));
				MixValue(slot.hash, slot.hitCount);
				MixValue(slot.hash, static_cast<std::uint8_t>(result.truncated));
				for(const PhysicsBackendQueryHit& hit : result.hits) MixHit(slot.hash, hit);
			});
			completions.push_back(task->getFuture());
			pool.enqueue(task);
		}

		for(std::future<void>& completion : completions) completion.get();
		const auto stop = std::chrono::steady_clock::now();

		QueryPassResult pass;
		pass.milliseconds = std::chrono::duration<double, std::milli>(stop - start).count();
		for(std::uint32_t index = 0U; index < kQueryCount; ++index) {
			const QuerySlot& slot = slots[index];
			pass.passed = pass.passed && slot.succeeded;
			pass.successfulQueries += slot.succeeded ? 1U : 0U;
			pass.queriesWithHits += slot.hadHit ? 1U : 0U;
			pass.hitCount += slot.hitCount;
			MixValue(pass.orderHash, index);
			MixValue(pass.orderHash, slot.hash);
		}
		return pass;
	}

	bool ApplyConcurrentCommandProbe(
		JoltPhysicsBackend& backend,
		PhysicsWorldHandle world,
		const std::vector<BodyIdentity>& dynamicBodies,
		std::uint32_t& acceptedCommands,
		std::uint32_t& appliedCommands) {
		if(dynamicBodies.empty()) return false;

		PhysicsCommandQueue queue({
			.maximumCommands = kProducerCount * kCommandsPerProducer,
			.maximumCommandsPerProducer = kCommandsPerProducer});
		queue.ResetForWorld(world);
		std::atomic<std::uint32_t> accepted{0U};
		std::vector<std::thread> producers;
		producers.reserve(kProducerCount);

		for(PhysicsSequence producer = 1U; producer <= kProducerCount; ++producer) {
			producers.emplace_back([&queue, &dynamicBodies, &accepted, producer, world]() {
				for(PhysicsSequence sequence = 1U; sequence <= kCommandsPerProducer; ++sequence) {
					const BodyIdentity& body = dynamicBodies[(producer * kCommandsPerProducer + sequence) % dynamicBodies.size()];
					PhysicsCommandMetadata metadata;
					metadata.effectiveTick = 1U;
					metadata.producerId = producer;
					metadata.producerSequence = sequence;
					metadata.correlationId = "stress-command";
					const PhysicsBodyCommandTarget target{
						.world = world,
						.objectId = body.objectId,
						.objectGeneration = 1U,
						.componentGeneration = 1U};
					if(queue.Enqueue(PhysicsCommand::MakeContinuousForce(
						target, {0.0F, 0.01F, 0.0F}, std::move(metadata))).accepted) {
						++accepted;
					}
				}
			});
		}
		for(std::thread& producer : producers) producer.join();

		const PhysicsCommandBatch batch = queue.MergeForProtectedBoundary(1U, world.worldGeneration);
		acceptedCommands = accepted.load();
		if(acceptedCommands != kProducerCount * kCommandsPerProducer
			|| batch.commands.size() != acceptedCommands
			|| batch.staleWorldGenerationCount != 0U) return false;
		for(std::size_t index = 0U; index < batch.commands.size(); ++index) {
			const PhysicsCommand& command = batch.commands[index];
			if(command.protectedBoundarySequence != index + 1U) return false;
			if(index != 0U) {
				const PhysicsCommand& previous = batch.commands[index - 1U];
				if(previous.producerId > command.producerId
					|| (previous.producerId == command.producerId
						&& previous.producerSequence >= command.producerSequence)) return false;
			}
			if(!backend.ApplyCommand(command).Succeeded()) return false;
			++appliedCommands;
		}
		return appliedCommands == acceptedCommands;
	}

	bool PublishGrid(
		JoltPhysicsBackend& backend,
		PhysicsWorldHandle world,
		std::vector<BodyIdentity>& dynamicBodies) {
		const PhysicsShapeHandle sharedShape{world.worldId, world.worldGeneration, 1U, 1U};
		PhysicsShapeRecipe box;
		box.kind = PhysicsShapeKind::Box;
		box.dimensions = {0.45F, 0.45F, 0.45F};
		PhysicsBackendStagingToken shapeStaging;
		if(!backend.CreateShape(world, sharedShape, box, shapeStaging).Succeeded()
			|| !backend.CommitStaging(shapeStaging).Succeeded()) return false;

		PhysicsBackendStagingToken bodyStaging;
		for(std::uint32_t index = 0U; index < kBodyCount; ++index) {
			const std::uint32_t row = index / kGridWidth;
			const std::uint32_t column = index % kGridWidth;
			PhysicsBodyCreateRequest body;
			body.handle = {world.worldId, world.worldGeneration, index + 1U, 1U};
			body.objectId = MakeBodyId(index);
			body.objectGeneration = 1U;
			body.componentGeneration = 1U;
			body.shape = sharedShape;
			body.motionType = index % 4U == 0U ? PhysicsMotionType::Dynamic : PhysicsMotionType::Static;
			body.useGravity = false;
			body.pose.position = {
				(static_cast<float>(column) - 15.5F) * 2.0F,
				0.0F,
				(static_cast<float>(row) - 15.5F) * 2.0F};
			if(!backend.CreateBody(body, bodyStaging).Succeeded()) return false;
			if(body.motionType == PhysicsMotionType::Dynamic) {
				dynamicBodies.push_back({body.handle, body.objectId});
			}
		}
		return backend.CommitStaging(bodyStaging).Succeeded();
	}

	void PrintResult(
		bool passed,
		std::uint32_t acceptedCommands,
		std::uint32_t appliedCommands,
		const QueryPassResult& first,
		const QueryPassResult& second,
		bool teardownPassed) {
		std::cout
			<< "{\"tool\":\"physics_gameplay_stress\""
			<< ",\"passed\":" << (passed ? "true" : "false")
			<< ",\"provider\":{\"name\":\"Jolt\",\"version\":\"" << kProviderVersion
			<< "\",\"revision\":\"" << kProviderRevision << "\"}"
			<< ",\"bodies\":{\"total\":" << kBodyCount << ",\"dynamic\":" << kBodyCount / 4U
			<< ",\"static\":" << kBodyCount - kBodyCount / 4U << ",\"gravityEnabled\":false}"
			<< ",\"commands\":{\"accepted\":" << acceptedCommands << ",\"applied\":" << appliedCommands << "}"
			<< ",\"queries\":{\"perPass\":" << kQueryCount
			<< ",\"passOne\":{\"successful\":" << first.successfulQueries << ",\"withHits\":" << first.queriesWithHits
			<< ",\"hits\":" << first.hitCount << ",\"orderHash\":\"" << first.orderHash
			<< "\",\"milliseconds\":" << first.milliseconds << "}"
			<< ",\"passTwo\":{\"successful\":" << second.successfulQueries << ",\"withHits\":" << second.queriesWithHits
			<< ",\"hits\":" << second.hitCount << ",\"orderHash\":\"" << second.orderHash
			<< "\",\"milliseconds\":" << second.milliseconds << "}"
			<< ",\"stableOrder\":" << (first.orderHash == second.orderHash ? "true" : "false") << "}"
			<< ",\"bounds\":{\"maxRequestsPerBatch\":" << kPhysicsMaximumRequestsPerBatch
			<< ",\"maxHitsPerQuery\":" << kPhysicsMaximumHitsPerQuery
			<< ",\"maxCastDistance\":" << kPhysicsMaximumCastDistance
			<< ",\"maxShapeExtent\":" << kPhysicsMaximumShapeExtent << "}"
			<< ",\"teardown\":" << (teardownPassed ? "true" : "false") << "}" << std::endl;
	}
}

int main() {
	bool passed = true;
	bool teardownPassed = false;
	std::uint32_t acceptedCommands = 0U;
	std::uint32_t appliedCommands = 0U;
	QueryPassResult firstPass;
	QueryPassResult secondPass;

	try {
		ThreadPool& pool = ThreadPool::instance();
		// The singleton is restarted explicitly so installed runs do not inherit work
		// from a host process that previously used the shared scheduler pool.
		pool.shutdown();
		pool.initialize();

		{
			JoltPhysicsBackend backend(pool);
			const PhysicsWorldHandle world{{9901U}, 1U};
			PhysicsBackendWorldConfiguration configuration;
			configuration.world = world;
			configuration.maximumBodies = kBodyCount + 64U;
			configuration.maximumBodyPairs = kBodyCount * 8U;
			configuration.maximumContactConstraints = kBodyCount * 4U;
			configuration.settings.gravity = {0.0F, 0.0F, 0.0F};

			passed = backend.Initialize().Succeeded() && backend.CreateWorld(configuration).Succeeded();
			std::vector<BodyIdentity> dynamicBodies;
			if(passed) passed = PublishGrid(backend, world, dynamicBodies) && dynamicBodies.size() == kBodyCount / 4U;
			if(passed) passed = ApplyConcurrentCommandProbe(backend, world, dynamicBodies, acceptedCommands, appliedCommands);
			if(passed) passed = backend.Step({world, 1U, 1.0F / 60.0F, 1U}).Succeeded();
			if(passed) firstPass = RunQueryPass(backend, world);
			if(passed) secondPass = RunQueryPass(backend, world);
			if(passed) {
				passed = firstPass.passed && secondPass.passed
					&& firstPass.successfulQueries == kQueryCount && secondPass.successfulQueries == kQueryCount
					&& firstPass.queriesWithHits == kQueryCount && secondPass.queriesWithHits == kQueryCount
					&& firstPass.orderHash == secondPass.orderHash && backend.IsHealthy();
			}

			// World destruction occurs before backend shutdown, and backend shutdown occurs
			// before the shared ThreadPool stops. That ordering is required because Jolt's
			// adapter may still own pool-backed jobs until its provider teardown finishes.
			const bool destroyed = backend.DestroyWorld(world).Succeeded();
			const bool shutDown = backend.Shutdown().Succeeded();
			teardownPassed = destroyed && shutDown;
			passed = passed && teardownPassed;
		}
		pool.shutdown();
	} catch(const std::exception& exception) {
		std::cerr << "physics_gameplay_stress exception: " << exception.what() << std::endl;
		passed = false;
		ThreadPool::instance().shutdown();
	} catch(...) {
		std::cerr << "physics_gameplay_stress exception: unknown" << std::endl;
		passed = false;
		ThreadPool::instance().shutdown();
	}

	PrintResult(passed, acceptedCommands, appliedCommands, firstPass, secondPass, teardownPassed);
	return passed ? 0 : 1;
}
