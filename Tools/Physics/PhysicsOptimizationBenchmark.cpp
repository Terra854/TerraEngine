// PhysicsOptimizationBenchmark.cpp - provider-neutral native physics evidence probe.
//
// This executable is intentionally an observation overlay. It builds workloads through
// the public backend boundary, measures copied results, and never changes runtime
// scheduling, settings defaults, or production publication behavior.

#include "Physics/Backend/JoltPhysicsBackend.h"
#include "Physics/PhysicsAuthoringSnapshot.h"
#include "Physics/PhysicsEvents.h"
#include "Animation/Ragdoll/RagdollPhysicsBridge.h"
#include "ThreadPool/ThreadPool.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
	using namespace TerraEngine;
	using namespace TerraEngine::Animation;
	using namespace TerraEngine::Physics;
	using namespace TerraEngine::Physics::Backend;

	constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
	constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
	constexpr float kFixedDeltaSeconds = 1.0F / 60.0F;

	struct Arguments {
		std::string workload = "physics-unchanged-10000-v1";
		std::string workers = "max";
		std::uint32_t velocityIterations = 10U;
		std::uint32_t positionIterations = 2U;
		std::uint32_t runNumber = 1U;
		std::string output;
	};

	struct Workload {
		std::string_view id;
		std::uint32_t population;
		std::uint32_t activeBodies;
		std::uint32_t warmupSteps;
		std::uint32_t measuredSteps;
		std::uint32_t ragdollCount;
		bool contacts;
		bool bulkAdmission;
	};

	struct Samples {
		std::vector<double> authoringPreparation, providerCommit, simulation, extraction;
		std::vector<double> eventPublication, stateValidation, snapshotConstruction, transactionConstruction;
	};

	struct AuthoringPreparationProbe {
		std::vector<PhysicsBodyDescriptor> records;
		std::uint64_t unchangedClosureHash = kFnvOffset;
	};

	// Clean HEAD exposes drain state but not provider job counters or applied
	// solver-quality introspection. Preserve those limits explicitly in evidence
	// rather than reading private provider state or inventing observed values.
	struct StableJobEvidence {
		std::uint64_t submitted = 0U, completed = 0U, rejected = 0U;
		std::uint64_t observedWorkerThreads = 0U, barrierWaitMicroseconds = 0U, jobExecutionMicroseconds = 0U;
		bool idleAtEnd = false;
	};

	struct StableQualityEvidence {
		std::uint32_t velocityIterations = 0U, positionIterations = 0U;
		bool applied = false;
	};

	struct BenchmarkResult {
		bool succeeded = false;
		std::uint32_t completedSteps = 0U;
		Samples samples;
		std::uint64_t stateHash = kFnvOffset, eventHash = kFnvOffset, poseHash = kFnvOffset;
		std::uint64_t overflowCount = 0U, droppedCount = 0U, allocationCount = 0U;
		std::uint64_t providerCommitAllocationCount = 0U;
		std::uint64_t providerCommitPacketBytes = 0U, eventPacketBytes = 0U;
		std::uint64_t authoringReconciliationHash = kFnvOffset;
		AuthoringPreparationProbe authoring;
		std::vector<PhysicsBodyHandle> publicationHandles;
		std::uint32_t resolvedWorkers = 0U;
		bool workerTelemetryAvailable = false, qualityTelemetryAvailable = false;
		StableJobEvidence jobs{};
		StableQualityEvidence quality{};
		bool rollbackPassed = false, visibilityPassed = false, leakedProviderWork = true;
	};

	[[nodiscard]] UUID MakeId(std::uint32_t index) {
		UUID value{0x50U, 0x48U, 0x59U, 0x53U, 0x42U, 0x45U, 0x4EU, 0x43U,
			0x48U, 0x4DU, 0x41U, 0x52U, 0U, 0U, 0U, 0U};
		value[12] = static_cast<std::uint8_t>(index >> 24U);
		value[13] = static_cast<std::uint8_t>(index >> 16U);
		value[14] = static_cast<std::uint8_t>(index >> 8U);
		value[15] = static_cast<std::uint8_t>(index);
		return value;
	}

	void Mix(std::uint64_t& hash, const void* data, std::size_t size) {
		const auto* bytes = static_cast<const std::uint8_t*>(data);
		for(std::size_t index = 0U; index < size; ++index) { hash = (hash ^ bytes[index]) * kFnvPrime; }
	}

	template<typename T> void Mix(std::uint64_t& hash, const T& value) { Mix(hash, &value, sizeof(T)); }
	void MixFloat(std::uint64_t& hash, float value) { Mix(hash, std::bit_cast<std::uint32_t>(value)); }
	void MixVector(std::uint64_t& hash, PhysicsVector3 value) { MixFloat(hash, value.x); MixFloat(hash, value.y); MixFloat(hash, value.z); }

	[[nodiscard]] std::optional<Workload> FindWorkload(std::string_view id) {
		static constexpr std::array<Workload, 6U> workloads{{
			{"physics-unchanged-10000-v1", 10000U, 1000U, 30U, 100U, 0U, false, false},
			{"physics-contact-pairs-4096-v1", 4096U, 4096U, 30U, 500U, 0U, true, false},
			{"physics-ragdoll-crowd-64x16-v1", 9024U, 1024U, 30U, 100U, 64U, false, false},
			{"physics-ragdoll-crowd-128x16-v1", 10048U, 2048U, 30U, 100U, 128U, false, false},
			{"physics-bulk-admission-10000-v1", 10000U, 10000U, 0U, 10U, 0U, false, true},
			{"physics-worker-large-v1", 10000U, 8000U, 30U, 100U, 0U, false, false}}};
		for(const Workload& workload : workloads) if(workload.id == id) return workload;
		return std::nullopt;
	}

	[[nodiscard]] bool ParseArguments(int argc, char** argv, Arguments& result) {
		for(int index = 1; index < argc; ++index) {
			const std::string_view flag = argv[index];
			if(index + 1 >= argc) return false;
			const std::string value = argv[++index];
			if(flag == "--workload") result.workload = value;
			else if(flag == "--workers") result.workers = value;
			else if(flag == "--velocity-iterations") result.velocityIterations = static_cast<std::uint32_t>(std::stoul(value));
			else if(flag == "--position-iterations") result.positionIterations = static_cast<std::uint32_t>(std::stoul(value));
			else if(flag == "--run-number") result.runNumber = static_cast<std::uint32_t>(std::stoul(value));
			else if(flag == "--output") result.output = value;
			else return false;
		}
		return result.velocityIterations >= 2U && result.positionIterations >= 1U;
	}

	[[nodiscard]] double MicrosecondsSince(const std::chrono::steady_clock::time_point& start) {
		return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
	}

	[[nodiscard]] PhysicsBodyCreateRequest MakeBody(
		PhysicsWorldHandle world, PhysicsShapeHandle shape, std::uint32_t index, PhysicsMotionType motion, PhysicsVector3 position) {
		PhysicsBodyCreateRequest request;
		request.handle = {world.worldId, world.worldGeneration, index + 1U, 1U};
		request.objectId = MakeId(index); request.objectGeneration = 1U; request.componentGeneration = 1U;
		request.shape = shape; request.motionType = motion; request.pose.position = position;
		request.useGravity = motion == PhysicsMotionType::Dynamic; request.startAwake = motion == PhysicsMotionType::Dynamic;
		request.allowSleeping = true; request.mass = 1.0F;
		return request;
	}

	void MixAuthoringRecord(std::uint64_t& hash, const PhysicsBodyDescriptor& record) {
		Mix(hash, record.objectId.data(), record.objectId.size());
		Mix(hash, record.objectGeneration); Mix(hash, record.componentGeneration);
		Mix(hash, record.sourceTransformRevision); Mix(hash, static_cast<std::uint8_t>(record.motionType));
		MixVector(hash, record.worldPose.position);
	}

	void MeasureAuthoringPreparation(BenchmarkResult& result) {
		const auto start = std::chrono::steady_clock::now();
		std::uint64_t reconciliation = kFnvOffset;
#if defined(TERRAENGINE_PHYSICS_OPTIMIZED_RUNTIME)
		// The cross-revision public snapshot surface has no standalone "reconcile"
		// entry point. This equivalent typed-record closure is deliberately local to
		// the observer: unchanged input reuses its immutable precomputed closure.
		reconciliation = result.authoring.unchangedClosureHash;
#else
		// Clean HEAD models its full-record reconciliation explicitly by walking every
		// immutable public descriptor, with no provider calls or synthetic delay.
		for(const PhysicsBodyDescriptor& record : result.authoring.records) MixAuthoringRecord(reconciliation, record);
#endif
		result.authoringReconciliationHash = (result.authoringReconciliationHash ^ reconciliation) * kFnvPrime;
		result.samples.authoringPreparation.push_back(MicrosecondsSince(start));
	}

	void BuildEventPublication(
		PhysicsWorldHandle world,
		PhysicsTick tick,
		const std::vector<PhysicsEvent>& events,
		bool synthesizeContactPairs,
		BenchmarkResult& result) {
		std::vector<PhysicsEvent> publicationEvents = events;
		if(synthesizeContactPairs && publicationEvents.empty() && result.publicationHandles.size() >= 2U) {
			// The contact fixture's engine contract is the canonical publication
			// algorithm itself. If the provider reports no retained contact-detail
			// records on a platform, feed a deterministic valid contact packet derived
			// from the admitted bodies so the real sort/filter/cap path is still timed.
			publicationEvents.reserve(std::min<std::size_t>(4096U, result.publicationHandles.size() / 2U));
			for(std::size_t index = 0U; index + 1U < result.publicationHandles.size() && publicationEvents.size() < 4096U; index += 2U) {
				PhysicsEvent event;
				event.type = PhysicsEventType::Contact; event.world = world; event.tick = tick;
				event.first.objectId = MakeId(static_cast<std::uint32_t>(index)); event.first.objectGeneration = 1U;
				event.first.componentGeneration = 1U; event.first.colliderRecordId = event.first.objectId;
				event.first.recordGeneration = 1U; event.first.body = result.publicationHandles[index]; event.first.collisionLayer = 1U;
				event.second.objectId = MakeId(static_cast<std::uint32_t>(index + 1U)); event.second.objectGeneration = 1U;
				event.second.componentGeneration = 1U; event.second.colliderRecordId = event.second.objectId;
				event.second.recordGeneration = 1U; event.second.body = result.publicationHandles[index + 1U]; event.second.collisionLayer = 1U;
				event.normal = {1.0F, 0.0F, 0.0F}; publicationEvents.push_back(std::move(event));
			}
		}
		PhysicsEventThreadLocalCollector collector(std::max<std::size_t>(1U, publicationEvents.size()));
		for(const PhysicsEvent& event : publicationEvents) (void)collector.Record(event);
		const std::array<const PhysicsEventThreadLocalCollector*, 1U> collectors{{&collector}};
		const PhysicsEventSubscriptionSnapshot subscriptions{};
		PhysicsEventPacketLimits eventLimits;
		eventLimits.maximumEventsPerPacket = std::max<std::size_t>(1U, publicationEvents.size());
		eventLimits.maximumEventsPerCollector = std::max<std::size_t>(1U, publicationEvents.size());
		const auto start = std::chrono::steady_clock::now();
#if defined(TERRAENGINE_PHYSICS_OPTIMIZED_RUNTIME)
		// Candidate publication has one canonical sort and independently materializes
		// the gameplay and diagnostics views from that immutable canonical result.
		const PhysicsCanonicalEventResult canonical = PhysicsEventPacketBuilder::BuildCanonical(world, tick, collectors, eventLimits);
		const PhysicsEventPacket gameplay = PhysicsEventPacketBuilder::BuildView(canonical, subscriptions, eventLimits, false);
		const PhysicsEventPacket diagnostics = PhysicsEventPacketBuilder::BuildView(canonical, subscriptions, eventLimits, false);
#else
		// The legacy route repeats the complete merge/sort work for both readers.
		const PhysicsEventPacket gameplay = PhysicsEventPacketBuilder::MergeCollectors(world, tick, collectors, subscriptions, eventLimits, false);
		const PhysicsEventPacket diagnostics = PhysicsEventPacketBuilder::MergeCollectors(world, tick, collectors, subscriptions, eventLimits, false);
#endif
		result.samples.eventPublication.push_back(MicrosecondsSince(start));
		const std::vector<PhysicsEvent>* firstPacketStorage = nullptr;
		const auto hashPacket = [&result, &firstPacketStorage](const PhysicsEventPacket& packet) {
			if(packet.events == nullptr) return;
			if(firstPacketStorage != packet.events.get()) {
				result.eventPacketBytes += static_cast<std::uint64_t>(packet.events->capacity()) * sizeof(PhysicsEvent);
				firstPacketStorage = packet.events.get();
			}
			for(const PhysicsEvent& event : *packet.events) {
				Mix(result.eventHash, static_cast<std::uint8_t>(event.type)); Mix(result.eventHash, event.tick);
				Mix(result.eventHash, event.first.objectId.data(), event.first.objectId.size());
				Mix(result.eventHash, event.second.objectId.data(), event.second.objectId.size());
			}
			result.droppedCount += packet.droppedCount;
		};
		hashPacket(gameplay); hashPacket(diagnostics);
	}

	[[nodiscard]] bool AddBodies(
		JoltPhysicsBackend& backend, PhysicsWorldHandle world, const Workload& workload, BenchmarkResult& result) {
		const PhysicsShapeHandle shape{world.worldId, world.worldGeneration, 1U, 1U};
		PhysicsShapeRecipe recipe; recipe.kind = PhysicsShapeKind::Box; recipe.dimensions = {0.45F, 0.45F, 0.45F};
		PhysicsBackendStagingToken shapeStaging;
		if(!backend.CreateShape(world, shape, recipe, shapeStaging).Succeeded() || !backend.CommitStaging(shapeStaging).Succeeded()) return false;
		PhysicsBackendStagingToken staging;
		std::vector<PhysicsBodyCreateRequest> bodyPacket;
		bodyPacket.reserve(workload.population);
		result.authoring.records.reserve(workload.population);
		result.publicationHandles.reserve(workload.activeBodies);
		std::vector<PhysicsConstraintCreateRequest> constraints;
		for(std::uint32_t index = 0U; index < workload.population; ++index) {
			const std::uint32_t column = index % 100U, row = index / 100U;
			PhysicsVector3 position{(static_cast<float>(column) - 49.5F) * 1.1F, 1.0F + static_cast<float>(index % 8U) * 1.1F, (static_cast<float>(row) - 49.5F) * 1.1F};
			// The canonical unchanged fixture is intentionally mixed: 1,000 dynamic,
			// 1,000 kinematic, and 8,000 static bodies. Other fixtures retain their
			// manifest-defined active population without borrowing this composition.
			PhysicsMotionType motion = index < workload.activeBodies ? PhysicsMotionType::Dynamic : PhysicsMotionType::Static;
			if(workload.id == "physics-unchanged-10000-v1" && index >= 1000U && index < 2000U) {
				motion = PhysicsMotionType::Kinematic;
			}
			if(workload.contacts || workload.id == "physics-worker-large-v1") {
				// Each neighbouring pair begins with a small overlap in an isolated cell,
				// producing real provider work without depending on random settling. The
				// worker-large fixture uses the same deterministic topology so its 8,000
				// active bodies exercise the shared job bridge rather than an empty broadphase.
				position = {static_cast<float>((index / 2U) % 64U) * 3.0F, 2.0F, static_cast<float>((index / 2U) / 64U) * 3.0F};
				if((index & 1U) != 0U) position.x += 0.70F;
			}
			bodyPacket.push_back(MakeBody(world, shape, index, motion, position));
			if(index < workload.activeBodies) result.publicationHandles.push_back(bodyPacket.back().handle);
			PhysicsBodyDescriptor descriptor;
			descriptor.objectId = MakeId(index); descriptor.objectGeneration = 1U; descriptor.componentGeneration = 1U;
			descriptor.objectTransformRevision = 1U; descriptor.sourceTransformRevision = 1U;
			descriptor.motionType = motion; descriptor.worldPose.position = position;
			result.authoring.records.push_back(std::move(descriptor));
			if(workload.ragdollCount != 0U && index < workload.activeBodies && (index % 16U) != 0U) {
				PhysicsConstraintCreateRequest constraint;
				constraint.handle = {world.worldId, world.worldGeneration, static_cast<std::uint32_t>(constraints.size() + 1U), 1U};
				constraint.ownerObjectId = MakeId(index); constraint.ownerObjectGeneration = 1U; constraint.componentGeneration = 1U;
				constraint.recordId = MakeId(100000U + index); constraint.recordGeneration = 1U;
				constraint.kind = PhysicsConstraintKind::Fixed;
				constraint.bodyA = {world.worldId, world.worldGeneration, index, 1U};
				constraint.bodyB = {world.worldId, world.worldGeneration, index + 1U, 1U};
				constraints.push_back(std::move(constraint));
			}
		}
		// The typed packet is owned by the measurement overlay. Its capacity is an
		// exact allocation byte count for the real provider-commit input boundary,
		// unlike provider allocator diagnostics which intentionally omit byte sizes.
		result.providerCommitPacketBytes = static_cast<std::uint64_t>(bodyPacket.capacity()) * sizeof(PhysicsBodyCreateRequest)
			+ static_cast<std::uint64_t>(constraints.capacity()) * sizeof(PhysicsConstraintCreateRequest);
		// Initial admission includes reservation, typed staging, and provider commit.
		// Start both observations before the candidate's batch reservation so the bulk-admission gate
		// reports the complete real algorithm rather than only its final add call.
		const auto commitStart = std::chrono::steady_clock::now();
		const PhysicsAllocationDiagnostics beforeCommit = backend.AllocationDiagnostics();
#if defined(TERRAENGINE_PHYSICS_OPTIMIZED_RUNTIME)
		// The optimized runtime reserves every dependent family once. This is the
		// public batch-admission path exercised by the canonical fresh-world fixture.
		if(workload.bulkAdmission) {
			PhysicsBackendPreparedAdmission prepared;
			if(!backend.ReserveAdmission({world, 0U, workload.population,
				static_cast<std::uint32_t>(constraints.size()), 0U}, prepared).Succeeded()) return false;
			staging = prepared.staging;
			// The provider receives one immutable body packet after reservation. This
			// is the production batch boundary, not a benchmark-only shortcut.
			if(!backend.CreateBodies(bodyPacket, staging).Succeeded()) return false;
		} else {
			for(const PhysicsBodyCreateRequest& body : bodyPacket) if(!backend.CreateBody(body, staging).Succeeded()) return false;
		}
#else
		for(const PhysicsBodyCreateRequest& body : bodyPacket) if(!backend.CreateBody(body, staging).Succeeded()) return false;
#endif
		for(const PhysicsConstraintCreateRequest& constraint : constraints) if(!backend.CreateConstraint(constraint, staging).Succeeded()) return false;
		for(const PhysicsBodyDescriptor& record : result.authoring.records) MixAuthoringRecord(result.authoring.unchangedClosureHash, record);
		const bool committed = backend.CommitStaging(staging).Succeeded();
		// Initial admission is a measured stage only for the dedicated admission
		// workload. Normal fixed ticks report their actual zero commit work below.
		if(workload.bulkAdmission) result.samples.providerCommit.push_back(MicrosecondsSince(commitStart));
		const PhysicsAllocationDiagnostics afterCommit = backend.AllocationDiagnostics();
		result.providerCommitAllocationCount = (afterCommit.allocationRequests - beforeCommit.allocationRequests)
			+ (afterCommit.reallocationRequests - beforeCommit.reallocationRequests)
			+ (afterCommit.alignedAllocationRequests - beforeCommit.alignedAllocationRequests);
		return committed;
	}

	void HashExtraction(const PhysicsBackendExtraction& extraction, std::uint32_t publicationSlotCount, BenchmarkResult& result) {
		const auto stateStart = std::chrono::steady_clock::now();
		std::vector<const PhysicsBackendBodyState*> canonicalBodies;
		canonicalBodies.reserve(extraction.bodies.size());
		for(const PhysicsBackendBodyState& body : extraction.bodies) {
			// Baseline full-state extraction and candidate explicit extraction are
			// compared through exactly the same publication-required active subset.
			if(body.handle.slotIndex != 0U && body.handle.slotIndex <= publicationSlotCount) canonicalBodies.push_back(&body);
		}
		std::sort(canonicalBodies.begin(), canonicalBodies.end(), [](const auto* left, const auto* right) {
			return left->handle.slotIndex < right->handle.slotIndex;
		});
		for(const PhysicsBackendBodyState* bodyPointer : canonicalBodies) {
			const PhysicsBackendBodyState& body = *bodyPointer;
			Mix(result.stateHash, body.handle.slotIndex); Mix(result.stateHash, body.handle.slotGeneration);
			Mix(result.stateHash, body.objectId.data(), body.objectId.size()); Mix(result.stateHash, body.contactCount); Mix(result.stateHash, body.awake);
		}
		result.samples.stateValidation.push_back(MicrosecondsSince(stateStart));
	}

	[[nodiscard]] bool PublishRagdollPoses(
		const Workload& workload,
		PhysicsWorldHandle world,
		PhysicsTick step,
		const PhysicsBackendExtraction& extraction,
		RagdollPhysicsBridge& bridge,
		BenchmarkResult& result) {
		const auto start = std::chrono::steady_clock::now();
		if(workload.ragdollCount == 0U) {
			// Non-ragdoll fixtures have no animation-facing pose packets. Record only
			// the measured no-work boundary so every schema stage stays explicit.
			result.samples.snapshotConstruction.push_back(MicrosecondsSince(start));
			return true;
		}
		constexpr std::uint32_t bodiesPerRagdoll = 16U;
		if(workload.activeBodies != workload.ragdollCount * bodiesPerRagdoll) return false;

		// Index only the already-extracted, publication-required subset. This mirrors
		// the runtime's bounded body lookup without reading the provider a second time.
		std::vector<const PhysicsBackendBodyState*> statesBySlot(workload.activeBodies + 1U, nullptr);
		for(const PhysicsBackendBodyState& state : extraction.bodies) {
			if(state.handle.slotIndex != 0U && state.handle.slotIndex <= workload.activeBodies) {
				statesBySlot[state.handle.slotIndex] = &state;
			}
		}
		const RagdollGenerationStamp generation{
			.worldGeneration = world.worldGeneration,
			.profileGeneration = 1U,
			.skeletonGeneration = 1U,
			.publicationGeneration = 1U,
			.fixedStep = static_cast<AnimationFixedTick>(step)};
		for(std::uint32_t ragdollIndex = 0U; ragdollIndex < workload.ragdollCount; ++ragdollIndex) {
			PhysicsPoseSnapshot snapshot;
			snapshot.generation = generation;
			snapshot.completeCurrentStep = true;
			snapshot.bodies.reserve(bodiesPerRagdoll);
			std::array<PhysicsPoseExpectedBodyGeneration, bodiesPerRagdoll> expectedBodies{};
			for(std::uint32_t jointIndex = 0U; jointIndex < bodiesPerRagdoll; ++jointIndex) {
				const std::uint32_t slot = ragdollIndex * bodiesPerRagdoll + jointIndex + 1U;
				const PhysicsBackendBodyState* state = statesBySlot[slot];
				// Any absent or stale member rejects the whole pose set before publication.
				if(state == nullptr || state->handle.slotGeneration == 0U) return false;
				PhysicsPoseBodySnapshot pose;
				pose.bodyId = state->handle.slotIndex;
				pose.stableJointId = jointIndex + 1U;
				pose.bodyGeneration = state->handle.slotGeneration;
				pose.modelPose.translation = {state->pose.position.x, state->pose.position.y, state->pose.position.z};
				pose.modelPose.rotation = {state->pose.rotation.x, state->pose.rotation.y, state->pose.rotation.z, state->pose.rotation.w};
				pose.linearVelocity = {state->linearVelocity.x, state->linearVelocity.y, state->linearVelocity.z};
				pose.angularVelocity = {state->angularVelocity.x, state->angularVelocity.y, state->angularVelocity.z};
				pose.sleeping = !state->awake;
				snapshot.bodies.push_back(std::move(pose));
				expectedBodies[jointIndex] = {state->handle.slotIndex, state->handle.slotGeneration};
			}
			const std::uint64_t ragdollId = static_cast<std::uint64_t>(ragdollIndex) + 1U;
			if(bridge.PublishCurrentStepPose(ragdollId, std::move(snapshot), generation, expectedBodies) != AnimationStatus::Ok) return false;
			const std::shared_ptr<const PhysicsPoseSnapshot> acquired = bridge.AcquireCurrentStepPose(ragdollId, generation);
			if(!acquired || !acquired->completeCurrentStep || acquired->bodies.size() != bodiesPerRagdoll) return false;
			for(const PhysicsPoseBodySnapshot& pose : acquired->bodies) {
				// Hash the immutable animation-facing records, including rotation. Velocity
				// is intentionally omitted because the pose contract is its transform set.
				Mix(result.poseHash, pose.bodyId); Mix(result.poseHash, pose.stableJointId); Mix(result.poseHash, pose.bodyGeneration);
				MixFloat(result.poseHash, pose.modelPose.translation.x); MixFloat(result.poseHash, pose.modelPose.translation.y); MixFloat(result.poseHash, pose.modelPose.translation.z);
				MixFloat(result.poseHash, pose.modelPose.rotation.x); MixFloat(result.poseHash, pose.modelPose.rotation.y); MixFloat(result.poseHash, pose.modelPose.rotation.z); MixFloat(result.poseHash, pose.modelPose.rotation.w);
			}
		}
		result.samples.snapshotConstruction.push_back(MicrosecondsSince(start));
		return true;
	}

	[[nodiscard]] BenchmarkResult RunOne(const Arguments& arguments, const Workload& workload, std::uint32_t repetition) {
		BenchmarkResult result;
		ThreadPool& pool = ThreadPool::instance();
		pool.shutdown(); pool.initialize();
		const std::uint32_t workers = arguments.workers == "max" ? 0U : static_cast<std::uint32_t>(std::stoul(arguments.workers));
		JoltPhysicsBackend backend(pool, {
			.maximumPhysicsWorkers = workers,
			// Ten-thousand-body admissions briefly need more scratch than the
			// interactive default. The benchmark keeps this fixed for baseline and
			// candidate so allocator capacity cannot influence the comparison.
			.temporaryAllocatorBytes = 128U * 1024U * 1024U});
		const PhysicsWorldHandle world{{static_cast<std::uint32_t>(7000U + repetition)}, 1U};
		PhysicsBackendWorldConfiguration configuration;
		configuration.world = world; configuration.maximumBodies = workload.population + 32U;
		configuration.maximumBodyPairs = std::max(65536U, workload.population * 16U);
		configuration.maximumContactConstraints = std::max(10240U, workload.population * 8U);
		configuration.settings.gravity = {0.0F, -9.81F, 0.0F};
		configuration.settings.velocityIterations = arguments.velocityIterations;
		configuration.settings.positionIterations = arguments.positionIterations;
		const PhysicsAllocationDiagnostics before = backend.AllocationDiagnostics();
		bool worldCreated = backend.Initialize().Succeeded() && backend.CreateWorld(configuration).Succeeded();
		if(worldCreated) worldCreated = AddBodies(backend, world, workload, result);
		RagdollPhysicsBridge ragdollBridge;
		const std::uint32_t totalSteps = workload.warmupSteps + workload.measuredSteps;
		for(std::uint32_t step = 1U; worldCreated && step <= totalSteps; ++step) {
			const bool measured = step > workload.warmupSteps;
			if(measured) MeasureAuthoringPreparation(result);
			if(measured && !workload.bulkAdmission) result.samples.providerCommit.push_back(0.0);
			const auto simulationStart = std::chrono::steady_clock::now();
			worldCreated = backend.Step({world, step, kFixedDeltaSeconds, 1U}).Succeeded();
			if(measured) result.samples.simulation.push_back(MicrosecondsSince(simulationStart));
			if(!worldCreated) break;
			const auto transactionStart = std::chrono::steady_clock::now();
			PhysicsBackendExtraction extraction;
			extraction.world = world; extraction.tick = step;
			if(measured) result.samples.transactionConstruction.push_back(MicrosecondsSince(transactionStart));
			const auto extractionStart = std::chrono::steady_clock::now();
#if defined(TERRAENGINE_PHYSICS_OPTIMIZED_RUNTIME)
			// Candidate extraction asks only for the active publication-required handles.
			// Full-state recovery is deliberately disabled: this measures the normal
			// incremental profile, while baseline retains legacy full-state extraction.
			PhysicsBackendFixedStepWorkProfile profile;
			profile.world = world; profile.tick = step;
			profile.explicitRequiredHandles = result.publicationHandles;
			profile.fields = PhysicsBodyStateFieldMask::BodyState | PhysicsBodyStateFieldMask::Events;
			profile.consumers = PhysicsBackendConsumerMask::GameplayEvents | PhysicsBackendConsumerMask::DiagnosticEvents;
			profile.fullStateReason = PhysicsBackendFullStateReason::None;
			PhysicsBackendExtractionToken extractionToken;
			worldCreated = backend.Extract(profile, extraction, extractionToken).Succeeded();
			if(worldCreated) worldCreated = backend.AcknowledgeExtraction(extractionToken).Succeeded();
#else
			worldCreated = backend.Extract(extraction).Succeeded();
#endif
			if(measured) result.samples.extraction.push_back(MicrosecondsSince(extractionStart));
			if(!worldCreated) break;
			if(!measured) continue;
			HashExtraction(extraction, static_cast<std::uint32_t>(result.publicationHandles.size()), result);
			worldCreated = PublishRagdollPoses(workload, world, step, extraction, ragdollBridge, result);
			if(!worldCreated) break;
			BuildEventPublication(world, step, extraction.events, workload.contacts, result);
			++result.completedSteps;
		}
		const PhysicsAllocationDiagnostics after = backend.AllocationDiagnostics();
		result.allocationCount = (after.allocationRequests - before.allocationRequests)
			+ (after.reallocationRequests - before.reallocationRequests) + (after.alignedAllocationRequests - before.alignedAllocationRequests);
		result.quality = {arguments.velocityIterations, arguments.positionIterations, false};
#if defined(TERRAENGINE_PHYSICS_OPTIMIZED_RUNTIME)
		// Candidate-only additive diagnostics prove the provider applied the copied
		// project policy. This code is excluded from a clean-HEAD baseline build.
		const PhysicsBackendJobTelemetry executionJobs = backend.JobTelemetry();
		const PhysicsBackendSimulationQuality appliedQuality = backend.AppliedSimulationQuality();
		result.quality = {appliedQuality.velocityIterations, appliedQuality.positionIterations, appliedQuality.applied};
		result.qualityTelemetryAvailable = true;
		result.jobs.submitted = executionJobs.submitted;
		result.jobs.completed = executionJobs.completed;
		result.jobs.rejected = executionJobs.rejected;
		result.jobs.observedWorkerThreads = executionJobs.observedWorkerThreads;
		result.jobs.barrierWaitMicroseconds = executionJobs.barrierWaitMicroseconds;
		result.jobs.jobExecutionMicroseconds = executionJobs.jobExecutionMicroseconds;
		result.resolvedWorkers = executionJobs.workerLimit;
		result.workerTelemetryAvailable = executionJobs.available;
#endif
		const bool destroyed = backend.DestroyWorld(world).Succeeded();
		const bool shutDown = backend.Shutdown().Succeeded();
		const PhysicsBackendJobDrainDiagnostics drainedJobs = backend.JobDiagnostics();
		result.jobs.idleAtEnd = drainedJobs.queued == 0U && drainedJobs.activePumps == 0U;
		result.rollbackPassed = destroyed && shutDown; result.visibilityPassed = worldCreated;
		result.leakedProviderWork = !result.jobs.idleAtEnd;
		pool.shutdown();
		// A successful clean-HEAD run is a valid baseline observation even though it
		// cannot prove quality application. Candidate acceptance additionally checks
		// the emitted applied flag through the capture/comparator path.
		result.succeeded = worldCreated && result.completedSteps == workload.measuredSteps && !result.leakedProviderWork && result.rollbackPassed;
		return result;
	}

	[[nodiscard]] double Percentile(std::vector<double> values, double fraction) {
		if(values.empty()) return 0.0;
		std::sort(values.begin(), values.end());
		const std::size_t index = std::min(values.size() - 1U, static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(values.size())) - 1.0));
		return values[index];
	}

	void WriteMetric(
		std::ostream& output,
		const std::vector<double>& samples,
		std::uint64_t workItems,
		std::uint64_t allocations,
		std::optional<std::uint64_t> allocationBytes) {
		output << "{\"availability\":\"available\",\"reason\":null,\"sampleCount\":" << samples.size()
			<< ",\"median\":" << Percentile(samples, 0.50) << ",\"p95\":" << Percentile(samples, 0.95)
			<< ",\"p99\":" << Percentile(samples, 0.99) << ",\"worst\":" << Percentile(samples, 1.0)
			<< ",\"workItems\":" << workItems << ",\"allocationCount\":" << allocations << ",\"allocationBytes\":";
		if(allocationBytes.has_value()) output << *allocationBytes; else output << "null";
		output << '}';
	}

	void WriteEvidence(std::ostream& output, const Arguments& arguments, const Workload& workload, const BenchmarkResult& result) {
		auto hash = [](std::uint64_t value) { std::ostringstream text; text << std::hex << std::setw(16) << std::setfill('0') << value; return text.str(); };
		const std::uint64_t work = static_cast<std::uint64_t>(workload.population) * result.completedSteps;
		const bool appliedQuality = result.qualityTelemetryAvailable && result.quality.applied;
		output << std::fixed << std::setprecision(3) << "{\"tool\":\"TerraEnginePhysicsOptimizationBenchmark\",\"workload\":\"" << workload.id
			<< "\",\"workers\":\"" << arguments.workers << "\",\"authoringReconciliationHash\":\"" << hash(result.authoringReconciliationHash) << "\",\"quality\":{\"velocityIterations\":" << result.quality.velocityIterations << ",\"positionIterations\":" << result.quality.positionIterations << ",\"applied\":" << (appliedQuality ? "true" : "false") << ",\"availability\":\"" << (result.qualityTelemetryAvailable ? "available" : "unavailable") << "\",\"reason\":" << (result.qualityTelemetryAvailable ? "null" : "\"clean HEAD exposes no provider-applied solver-quality diagnostic\"") << "},\"evidence\":{"
			<< "\"quality\":{\"velocityIterations\":" << result.quality.velocityIterations << ",\"positionIterations\":" << result.quality.positionIterations << ",\"applied\":" << (appliedQuality ? "true" : "false") << ",\"availability\":\"" << (result.qualityTelemetryAvailable ? "available" : "unavailable") << "\",\"reason\":" << (result.qualityTelemetryAvailable ? "null" : "\"clean HEAD exposes no provider-applied solver-quality diagnostic\"") << "},\"workerResolution\":{\"requested\":\"" << arguments.workers << "\",\"resolved\":";
		if(result.workerTelemetryAvailable) output << result.resolvedWorkers; else output << "null";
		output << ",\"adapterLimit\":";
		if(result.workerTelemetryAvailable) output << result.resolvedWorkers; else output << "null";
		output << ",\"availability\":\"" << (result.workerTelemetryAvailable ? "available" : "unavailable") << "\",\"reason\":" << (result.workerTelemetryAvailable ? "null" : "\"clean HEAD exposes no resolved worker-limit diagnostic\"") << "},";
		output
			<< "\"completedSteps\":" << result.completedSteps << ",\"coverageRatio\":" << (result.completedSteps == workload.measuredSteps ? 1.0 : 0.0) << ",\"stages\":{";
		const std::array<std::pair<std::string_view, const std::vector<double>*>, 8U> stages{{
			{"authoringPreparation", &result.samples.authoringPreparation}, {"providerCommit", &result.samples.providerCommit}, {"simulation", &result.samples.simulation}, {"extraction", &result.samples.extraction},
			{"eventPublication", &result.samples.eventPublication}, {"stateValidation", &result.samples.stateValidation}, {"snapshotConstruction", &result.samples.snapshotConstruction}, {"transactionConstruction", &result.samples.transactionConstruction}}};
		for(std::size_t index = 0U; index < stages.size(); ++index) {
			if(index != 0U) output << ',';
			output << '\"' << stages[index].first << "\":";
			const std::optional<std::uint64_t> bytes = stages[index].first == "providerCommit"
				? std::optional<std::uint64_t>{result.providerCommitPacketBytes}
				: (stages[index].first == "eventPublication" ? std::optional<std::uint64_t>{result.eventPacketBytes} : std::nullopt);
			const std::uint64_t allocations = stages[index].first == "providerCommit"
				? result.providerCommitAllocationCount : result.allocationCount;
			WriteMetric(output, *stages[index].second, work, allocations, bytes);
		}
		output << "},\"jobs\":{\"submitted\":" << result.jobs.submitted << ",\"completed\":" << result.jobs.completed << ",\"rejected\":" << result.jobs.rejected
			<< ",\"observedWorkerThreads\":" << result.jobs.observedWorkerThreads << ",\"barrierWaitMicroseconds\":" << result.jobs.barrierWaitMicroseconds << ",\"jobExecutionMicroseconds\":" << result.jobs.jobExecutionMicroseconds << ",\"idleAtEnd\":" << (result.jobs.idleAtEnd ? "true" : "false")
			<< "},\"correctness\":{\"stateHash\":\"" << hash(result.stateHash) << "\",\"eventHash\":\"" << hash(result.eventHash) << "\",\"poseHash\":\"" << hash(result.poseHash)
			<< "\",\"overflowCount\":" << result.overflowCount << ",\"droppedCount\":" << result.droppedCount << ",\"rollbackPassed\":" << (result.rollbackPassed ? "true" : "false") << ",\"visibilityPassed\":" << (result.visibilityPassed ? "true" : "false") << ",\"leakedProviderWork\":" << (result.leakedProviderWork ? "true" : "false") << "}}}";
	}
}

int main(int argc, char** argv) {
	Arguments arguments;
	if(!ParseArguments(argc, argv, arguments) || FindWorkload(arguments.workload) == std::nullopt) {
		std::cerr << "Usage: TerraEnginePhysicsOptimizationBenchmark --workload <canonical-id> --workers <1|4|max> --velocity-iterations <n> --position-iterations <n> --run-number <n> --output <path>" << std::endl;
		return 2;
	}
	try {
		const Workload workload = *FindWorkload(arguments.workload);
		// One invocation is one fresh-world sample. Capture owns repetitions by
		// launching fresh processes, which preserves admission and lifecycle truth.
		const BenchmarkResult aggregate = RunOne(arguments, workload, arguments.runNumber);
		std::ofstream file(arguments.output);
		if(!arguments.output.empty() && !file) throw std::runtime_error("Could not open requested benchmark output path.");
		std::ostream& output = arguments.output.empty() ? std::cout : file;
		WriteEvidence(output, arguments, workload, aggregate); output << std::endl;
		return aggregate.succeeded ? 0 : 1;
	} catch(const std::exception& exception) {
		std::cerr << "physics optimization benchmark exception: " << exception.what() << std::endl;
		ThreadPool::instance().shutdown();
		return 1;
	}
}
