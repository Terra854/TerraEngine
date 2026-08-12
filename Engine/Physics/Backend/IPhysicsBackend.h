// IPhysicsBackend.h - coarse provider-neutral simulation boundary.
//
// The engine owns every value in this interface. Jolt headers, IDs, allocators,
// callbacks, and object pointers remain inside the concrete backend so gameplay,
// serialization, managed code, and export manifests never depend on provider ABI.

#pragma once

#include "Physics/PhysicsSettings.h"
#include "Physics/PhysicsEvents.h"
#include "Physics/PhysicsQueries.h"
#include "Physics/PhysicsTypes.h"
#include "Utilities/UUID.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Physics {
	struct PhysicsCommand;

	namespace Backend {
		enum class PhysicsBackendErrorCode : std::uint8_t {
			None,
			InvalidArgument,
			InvalidState,
			StaleWorld,
			StaleHandle,
			CapacityExceeded,
			Unsupported,
			Cancelled,
			ProviderFailure,
			FatalProviderFailure
		};

		struct PhysicsBackendResult {
			PhysicsResultStatus status = PhysicsResultStatus::Success;
			PhysicsBackendErrorCode error = PhysicsBackendErrorCode::None;
			std::string message;

			bool Succeeded() const noexcept {
				return status == PhysicsResultStatus::Success
					|| status == PhysicsResultStatus::Accepted;
			}
		};

		struct PhysicsBackendWorldConfiguration {
			PhysicsWorldHandle world;
			PhysicsSettings settings = PhysicsSettings::Defaults();
			std::uint32_t maximumBodies = 65536U;
			std::uint32_t maximumBodyPairs = 65536U;
			std::uint32_t maximumContactConstraints = 10240U;
		};

		struct PhysicsShapeRecipe {
			PhysicsShapeKind kind = PhysicsShapeKind::Box;
			PhysicsVector3 dimensions{1.0F, 1.0F, 1.0F};
			PhysicsVector3 localCenter;
			PhysicsQuaternion localRotation;
			TerraEngine::UUID sourceAssetId{};
			std::uint64_t stableRecipeHash = 0;
			std::uint32_t subShapeKey = 0;
			bool sensor = false;
			std::vector<PhysicsVector3> points;
			std::vector<std::uint32_t> indices;
			// A compound is an ordered tree of provider-neutral child recipes. The
			// authoring snapshot rejects cycles before this packet reaches a worker.
			std::vector<PhysicsShapeRecipe> children;
			// Cooked bytes remain opaque at this boundary. The family, provider, and
			// exact revision let a backend reject incompatible data before asking the
			// provider to deserialize it; no provider type or layout crosses the API.
			std::string cookedFormatFamily;
			std::string cookedProvider;
			std::string cookedProviderVersion;
			std::string cookedProviderRevision;
			std::uint32_t cookedFormatVersion = 0U;
			std::uint64_t cookedPayloadChecksum = 0U;
			std::shared_ptr<const std::vector<std::byte>> cookedPayload;
		};

		struct PhysicsCookRequest {
			PhysicsWorldHandle world;
			PhysicsShapeRecipe recipe;
			std::string correlationId;
		};

		struct PhysicsCookResult {
			PhysicsBackendResult result;
			std::vector<std::byte> payload;
			std::uint64_t recipeHash = 0;
			std::string formatFamily;
			std::string provider;
			std::string providerVersion;
			std::string providerRevision;
			std::uint32_t formatVersion = 0U;
			std::uint64_t payloadChecksum = 0U;
		};

		struct PhysicsBodyCreateRequest {
			PhysicsBodyHandle handle;
			TerraEngine::UUID objectId{};
			PhysicsGeneration objectGeneration = 0;
			PhysicsGeneration componentGeneration = 0;
			PhysicsShapeHandle shape;
			PhysicsMotionType motionType = PhysicsMotionType::Static;
			PhysicsMotionQuality motionQuality = PhysicsMotionQuality::Discrete;
			PhysicsPose pose;
			PhysicsVector3 linearVelocity;
			PhysicsVector3 angularVelocity;
			float mass = 1.0F;
			float friction = 0.5F;
			float restitution = 0.0F;
			float linearDamping = 0.05F;
			float angularDamping = 0.05F;
			float maximumLinearVelocity = 500.0F;
			float maximumAngularVelocity = 94.24778F;
			// Ragdoll profiles may intentionally tune gravity per body. Ordinary rigid
			// bodies retain the previous full-gravity default, while zero remains the
			// provider-neutral representation for gravity-disabled bodies.
			float gravityFactor = 1.0F;
			std::uint32_t allowedDegreesOfFreedom = 0x3FU;
			std::uint8_t collisionLayer = 0;
			std::uint32_t collisionMask = 0xFFFFFFFFU;
			PhysicsCombineMode frictionCombine = PhysicsCombineMode::Average;
			PhysicsCombineMode restitutionCombine = PhysicsCombineMode::Maximum;
			bool useGravity = true;
			bool sensor = false;
			bool allowSleeping = true;
			bool startAwake = true;
			bool enabled = true;
		};

		struct PhysicsBodyUpdateRequest {
			PhysicsBodyHandle handle;
			PhysicsPose pose;
			PhysicsVector3 linearVelocity;
			PhysicsVector3 angularVelocity;
			std::uint8_t collisionLayer = 0;
			std::uint32_t collisionMask = 0xFFFFFFFFU;
			bool enabled = true;
			bool wake = false;
		};

		enum class PhysicsBackendMotorMode : std::uint8_t {
			Off,
			Velocity,
			Position,
			PositionAndVelocity
		};

		// One provider-neutral axis packet covers linear and angular axes. Simple
		// joints use the first relevant axis; SixDOF consumes all six in XYZ order.
		struct PhysicsConstraintAxisSettings {
			bool limitEnabled = false;
			float minimum = 0.0F;
			float maximum = 0.0F;
			bool springEnabled = false;
			// False interprets springFrequency as stiffness for backward-compatible
			// authoring; true selects provider frequency-and-damping mode.
			bool springUsesFrequency = false;
			float springFrequency = 0.0F;
			float springDamping = 0.0F;
			PhysicsBackendMotorMode motorMode = PhysicsBackendMotorMode::Off;
			float motorTargetPosition = 0.0F;
			float motorTargetVelocity = 0.0F;
			float motorMaximumForce = 0.0F;
		};

		struct PhysicsConstraintCreateRequest {
			PhysicsConstraintHandle handle;
			TerraEngine::UUID ownerObjectId{};
			PhysicsGeneration ownerObjectGeneration = 0U;
			PhysicsGeneration componentGeneration = 0U;
			TerraEngine::UUID recordId{};
			PhysicsGeneration recordGeneration = 0U;
			PhysicsConstraintKind kind = PhysicsConstraintKind::Fixed;
			PhysicsBodyHandle bodyA;
			PhysicsBodyHandle bodyB;
			PhysicsPose localFrameA;
			PhysicsPose localFrameB;
			PhysicsVector3 axisA{0.0F, 1.0F, 0.0F};
			PhysicsVector3 axisB{0.0F, 1.0F, 0.0F};
			PhysicsVector3 normalAxisA{1.0F, 0.0F, 0.0F};
			PhysicsVector3 normalAxisB{1.0F, 0.0F, 0.0F};
			std::array<PhysicsConstraintAxisSettings, 6U> axes{};
			PhysicsConstraintHandle gearConstraintA;
			PhysicsConstraintHandle gearConstraintB;
			float gearRatio = 1.0F;
			float breakForce = std::numeric_limits<float>::infinity();
			float breakTorque = std::numeric_limits<float>::infinity();
			bool collideConnected = false;
			bool enabled = true;
			PhysicsGeneration resetGeneration = 1U;
		};

		struct PhysicsCharacterCreateRequest {
			PhysicsCharacterHandle handle;
			TerraEngine::UUID objectId{};
			PhysicsGeneration objectGeneration = 0U;
			PhysicsGeneration componentGeneration = 0U;
			PhysicsPose pose;
			PhysicsShapeKind shapeKind = PhysicsShapeKind::Capsule;
			float radius = 0.5F;
			float height = 2.0F;
			PhysicsVector3 localCenter;
			PhysicsVector3 upDirection{0.0F, 1.0F, 0.0F};
			float stepHeight = 0.4F;
			float supportVolumeRadius = 0.5F;
			float supportVolumeHeight = 0.2F;
			float maximumSlopeAngleRadians = 0.87266463F;
			float contactPadding = 0.02F;
			float mass = 80.0F;
			float maximumStrength = 1000.0F;
			std::uint8_t collisionLayer = 0;
			std::uint32_t collisionMask = 0xFFFFFFFFU;
			float groundAdhesion = 0.0F;
			float gravityScale = 1.0F;
			float jumpSpeed = 5.0F;
			float penetrationRecoverySpeed = 1.0F;
			bool allowStairWalking = true;
			bool useGravity = true;
			bool enabled = true;
		};

		struct PhysicsCharacterUpdateRequest {
			PhysicsCharacterHandle handle;
			TerraEngine::UUID objectId{};
			PhysicsGeneration objectGeneration = 0U;
			PhysicsGeneration componentGeneration = 0U;
			PhysicsPose pose;
			PhysicsVector3 desiredVelocity;
			float fixedDeltaTime = 1.0F / 60.0F;
			float jumpSpeed = 0.0F;
			float resizeHeight = 0.0F;
			bool jumpRequested = false;
			bool resizeRequested = false;
			bool teleportRequested = false;
			bool recoverPenetration = true;
			bool enabled = true;
		};

		struct PhysicsBackendQueryRequest {
			PhysicsWorldHandle world;
			PhysicsQueryKind kind = PhysicsQueryKind::RayCast;
			PhysicsQueryHitMode hitMode = PhysicsQueryHitMode::Closest;
			PhysicsPose pose;
			PhysicsVector3 direction;
			PhysicsVector3 shapeExtent;
			float distance = 0.0F;
			PhysicsQueryFilter filter;
			std::uint32_t maximumHits = 1U;
		};

		struct PhysicsBackendQueryHit {
			PhysicsBodyHandle body;
			TerraEngine::UUID objectId{};
			PhysicsGeneration objectGeneration = 0U;
			PhysicsGeneration componentGeneration = 0U;
			TerraEngine::UUID materialId{};
			std::string surfaceTag;
			PhysicsVector3 position;
			PhysicsVector3 normal;
			float distance = 0.0F;
			float fraction = 0.0F;
			std::uint32_t subShapeKey = 0;
			std::uint8_t collisionLayer = 0U;
			PhysicsMotionType motionType = PhysicsMotionType::Static;
			bool sensor = false;
		};

		struct PhysicsBackendQueryResult {
			PhysicsBackendResult result;
			std::vector<PhysicsBackendQueryHit> hits;
			bool truncated = false;
		};

		// Stable ranges address the provider's retained deterministic order rather
		// than transient provider IDs. Half-open ranges make batching unambiguous.
		struct PhysicsBackendStableRange {
			std::uint32_t firstIndex = 0U;
			std::uint32_t count = 0U;

			constexpr std::uint64_t EndIndex() const noexcept {
				return static_cast<std::uint64_t>(firstIndex) + count;
			}

			auto operator<=>(const PhysicsBackendStableRange&) const = default;
		};

		enum class PhysicsBackendExtractionSelection : std::uint8_t {
			// PublicationRelevant includes newly created/updated bodies and movable
			// bodies that are active after the completed fixed step.
			PublicationRelevant,
			ActiveBodies,
			ExplicitHandles,
			StableRanges,
			FullState
		};

		struct PhysicsBackendExtractionRequest {
			PhysicsWorldHandle world;
			PhysicsTick tick = 0U;
			PhysicsBackendExtractionSelection selection = PhysicsBackendExtractionSelection::PublicationRelevant;
			PhysicsBodyStateFieldMask fields = PhysicsBodyStateFieldMask::BodyState;
			std::vector<PhysicsBackendStableRange> stableRanges;
			std::vector<PhysicsBodyHandle> explicitHandles;
			std::uint32_t batchSize = 1024U;
			std::uint32_t parallelThreshold = 512U;
			bool fullState = false;
			bool allowParallelReads = true;
			bool drainEvents = true;

			bool IsValid() const noexcept {
				if(!world.IsValid() || batchSize == 0U || batchSize > 8192U
					|| parallelThreshold == 0U || fields == PhysicsBodyStateFieldMask::None) {
					return false;
				}
				std::uint64_t previousEnd = 0U;
				for(std::size_t index = 0U; index < stableRanges.size(); ++index) {
					const PhysicsBackendStableRange& range = stableRanges[index];
					if(range.count == 0U || range.EndIndex() > std::numeric_limits<std::uint32_t>::max()
						|| (index != 0U && range.firstIndex < previousEnd)) {
						return false;
					}
					previousEnd = range.EndIndex();
				}
				if(selection == PhysicsBackendExtractionSelection::StableRanges && stableRanges.empty()) return false;
				if(selection == PhysicsBackendExtractionSelection::ExplicitHandles && explicitHandles.empty()) return false;
				return !fullState || selection == PhysicsBackendExtractionSelection::FullState;
			}
		};

		struct PhysicsBackendExtractionCapabilities {
			bool stableProviderOrder = false;
			bool fieldMaskedBodyReads = false;
			bool independentBodyReadsSafe = false;
			bool exclusiveEventDrain = false;
			std::uint32_t maximumBatchSize = 1U;
		};

		// Backend job telemetry contains copied scalar facts only. Providers that do
		// not use the shared job bridge return the default unavailable/idle snapshot.
		struct PhysicsBackendJobTelemetry {
			std::uint64_t submitted = 0U, completed = 0U, rejected = 0U;
			std::uint64_t batchCount = 0U, lockAcquisitionCount = 0U, pumpSubmissionCount = 0U;
			std::uint64_t barrierWaitCount = 0U, barrierWaitMicroseconds = 0U;
			std::uint64_t maximumBarrierWaitMicroseconds = 0U, jobExecutionMicroseconds = 0U;
			std::uint32_t queued = 0U, activePumps = 0U, maximumActivePumps = 0U;
			std::uint32_t workerLimit = 0U, observedWorkerThreads = 0U;
			bool available = false;
			bool idle = true;
		};

		struct PhysicsBackendSimulationQuality {
			PhysicsWorldHandle world;
			std::uint32_t velocityIterations = 0U;
			std::uint32_t positionIterations = 0U;
			bool applied = false;
		};

		enum class PhysicsBackendFullStateReason : std::uint8_t {
			None,
			InitialPublication,
			Recovery,
			IntegrityVerification,
			DeclaredConsumer
		};

		enum class PhysicsBackendConsumerMask : std::uint32_t {
			None = 0U,
			GameplayEvents = 1U << 0U,
			DiagnosticEvents = 1U << 1U,
			DetailedContacts = 1U << 2U,
			RagdollPose = 1U << 3U,
			DebugVisualization = 1U << 4U,
			FullProviderState = 1U << 5U
		};

		constexpr PhysicsBackendConsumerMask operator|(
			PhysicsBackendConsumerMask left,
			PhysicsBackendConsumerMask right) noexcept {
			return static_cast<PhysicsBackendConsumerMask>(
				static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
		}

		struct PhysicsBackendFixedStepWorkProfile {
			PhysicsWorldHandle world;
			PhysicsTick tick = 0U;
			PhysicsGeneration authoringGeneration = 0U;
			std::uint64_t providerOrderGeneration = 0U;
			std::uint64_t stateStoreGeneration = 0U;
			std::vector<std::uint32_t> activeStableIndices;
			std::vector<std::uint32_t> dirtyStableIndices;
			std::vector<PhysicsBodyHandle> removedHandles;
			std::vector<PhysicsBodyHandle> explicitRequiredHandles;
			PhysicsBackendConsumerMask consumers = PhysicsBackendConsumerMask::None;
			PhysicsBackendFullStateReason fullStateReason = PhysicsBackendFullStateReason::None;
			PhysicsBodyStateFieldMask fields = PhysicsBodyStateFieldMask::BodyState;
			std::uint32_t rangeSize = 1024U;
			std::uint32_t availableWorkers = 1U;
		};

		struct PhysicsBackendExtractionToken {
			PhysicsWorldHandle world;
			PhysicsTick tick = 0U;
			std::uint64_t value = 0U;

			bool IsValid() const noexcept {
				return world.IsValid() && tick != 0U && value != 0U;
			}
		};

		struct PhysicsBackendBodyState {
			PhysicsBodyHandle handle;
			PhysicsPose pose;
			PhysicsVector3 linearVelocity;
			PhysicsVector3 angularVelocity;
			float mass = 0.0F;
			std::uint32_t contactCount = 0;
			bool awake = false;
			// The stable index and ownership generations are engine-owned metadata.
			// Consumers can therefore resolve current ownership without hashing a
			// provider record or interpreting a provider-specific body identifier.
			std::uint32_t stableIndex = 0U;
			TerraEngine::UUID objectId{};
			PhysicsGeneration objectGeneration = 0U;
			PhysicsGeneration componentGeneration = 0U;
			PhysicsBodyStateFieldMask fields = PhysicsBodyStateFieldMask::None;
			PhysicsBodyStateChangeMask change = PhysicsBodyStateChangeMask::None;
			PhysicsMotionType motionType = PhysicsMotionType::Static;
		};

		struct PhysicsBackendConstraintState {
			PhysicsConstraintHandle handle;
			TerraEngine::UUID ownerObjectId{};
			PhysicsGeneration ownerObjectGeneration = 0U;
			PhysicsGeneration componentGeneration = 0U;
			TerraEngine::UUID recordId{};
			PhysicsGeneration recordGeneration = 0U;
			// Jolt exposes solver lambdas, which are impulses. PhysicsWorld converts
			// them to force/torque with the fixed step before evaluating break policy.
			float linearImpulseMagnitude = 0.0F;
			float angularImpulseMagnitude = 0.0F;
			bool enabled = true;
		};

		enum class PhysicsCharacterGroundState : std::uint8_t {
			OnGround,
			OnSteepGround,
			NotSupported,
			InAir
		};

		struct PhysicsBackendCharacterState {
			PhysicsCharacterHandle handle;
			TerraEngine::UUID objectId{};
			PhysicsGeneration objectGeneration = 0U;
			PhysicsGeneration componentGeneration = 0U;
			PhysicsPose pose;
			PhysicsVector3 linearVelocity;
			PhysicsCharacterGroundState groundState = PhysicsCharacterGroundState::InAir;
			PhysicsVector3 groundPosition;
			PhysicsVector3 groundNormal{0.0F, 1.0F, 0.0F};
			PhysicsVector3 supportVelocity;
			PhysicsBodyHandle supportBody;
			std::uint32_t contactCount = 0U;
			bool supported = false;
		};

		struct PhysicsBackendExtraction {
			PhysicsWorldHandle world;
			PhysicsTick tick = 0;
			std::vector<PhysicsBackendBodyState> bodies;
			std::vector<PhysicsBodyHandle> removedBodies;
			std::vector<PhysicsBackendConstraintState> constraints;
			std::vector<PhysicsBackendCharacterState> characters;
			std::uint64_t activeBodyCount = 0;
			std::uint64_t contactCount = 0;
			std::uint64_t constraintCount = 0;
			// Provider callbacks are converted after their worker barrier. These
			// engine-owned values can therefore be sorted and published safely.
			std::vector<PhysicsEvent> events;
			std::uint64_t droppedEventCount = 0U;
			std::uint64_t providerOrderGeneration = 0U;
			std::uint64_t totalBodyCount = 0U;
			std::uint64_t examinedBodyCount = 0U;
			std::uint64_t extractedBodyCount = 0U;
			std::uint32_t extractionRangeCount = 0U;
			PhysicsBodyStateFieldMask extractedFields = PhysicsBodyStateFieldMask::None;
			bool fullState = false;
			bool usedParallelReads = false;
			bool usedSerialFallback = false;
			bool eventDrainPerformed = false;
		};

		struct PhysicsBackendStepRequest {
			PhysicsWorldHandle world;
			PhysicsTick tick = 0;
			float deltaTime = 0.0F;
			std::uint32_t collisionSteps = 1U;
		};

		struct PhysicsBackendStagingToken {
			PhysicsWorldHandle world;
			std::uint64_t value = 0;

			bool IsValid() const noexcept {
				return world.IsValid() && value != 0;
			}
		};

		// Admission reservations are copied sizing facts. They let a provider grow
		// staging containers once before any shape/body dependency becomes visible.
		struct PhysicsBackendAdmissionReservation {
			PhysicsWorldHandle world;
			std::uint32_t shapeCount = 0U;
			std::uint32_t bodyCount = 0U;
			std::uint32_t constraintCount = 0U;
			std::uint32_t characterCount = 0U;

			bool IsValid() const noexcept {
				return world.IsValid()
					&& (shapeCount != 0U || bodyCount != 0U || constraintCount != 0U || characterCount != 0U);
			}
		};

		struct PhysicsBackendPreparedAdmission {
			PhysicsBackendStagingToken staging;
			std::uint32_t reservedShapes = 0U;
			std::uint32_t reservedBodies = 0U;
			std::uint32_t reservedConstraints = 0U;
			std::uint32_t reservedCharacters = 0U;
			std::uint64_t stagingAllocationCount = 0U;
		};

		// This interface deliberately exposes broad operations rather than mirroring
		// provider classes. Each call executes at an orchestrator-declared boundary.
		class TERRAENGINE_API IPhysicsBackend {
		public:
			virtual ~IPhysicsBackend() = default;

			virtual PhysicsBackendResult Initialize() = 0;
			virtual PhysicsBackendResult Shutdown() = 0;
			virtual PhysicsBackendResult CreateWorld(const PhysicsBackendWorldConfiguration& configuration) = 0;
			virtual PhysicsBackendResult DestroyWorld(PhysicsWorldHandle world) = 0;
			virtual PhysicsBackendResult ReplaceWorld(
				const PhysicsBackendWorldConfiguration& configuration,
				PhysicsBackendStagingToken& staging) = 0;
			virtual PhysicsBackendResult ReserveAdmission(
				const PhysicsBackendAdmissionReservation& reservation,
				PhysicsBackendPreparedAdmission& prepared) = 0;

			virtual PhysicsCookResult CookShape(const PhysicsCookRequest& request) = 0;
			virtual PhysicsBackendResult CreateShape(
				PhysicsWorldHandle world,
				PhysicsShapeHandle handle,
				const PhysicsShapeRecipe& recipe,
				PhysicsBackendStagingToken& staging) = 0;
			virtual PhysicsBackendResult DestroyShape(PhysicsShapeHandle handle) = 0;

			virtual PhysicsBackendResult CreateBody(
				const PhysicsBodyCreateRequest& request,
				PhysicsBackendStagingToken& staging) = 0;
			// The default preserves compatibility for providers that have only the
			// established single-record boundary. Optimized providers may override it
			// to retain one protected admission lock for an immutable request packet;
			// callers still own RollbackStaging when any member is rejected.
			virtual PhysicsBackendResult CreateBodies(
				std::span<const PhysicsBodyCreateRequest> requests,
				PhysicsBackendStagingToken& staging) {
				for(const PhysicsBodyCreateRequest& request : requests) {
					const PhysicsBackendResult result = CreateBody(request, staging);
					if(!result.Succeeded()) return result;
				}
				return {};
			}
			virtual PhysicsBackendResult UpdateBody(const PhysicsBodyUpdateRequest& request) = 0;
			virtual PhysicsBackendResult DestroyBody(PhysicsBodyHandle handle) = 0;

			virtual PhysicsBackendResult CreateConstraint(
				const PhysicsConstraintCreateRequest& request,
				PhysicsBackendStagingToken& staging) = 0;
			virtual PhysicsBackendResult UpdateConstraint(const PhysicsConstraintCreateRequest& request) = 0;
			virtual PhysicsBackendResult DestroyConstraint(PhysicsConstraintHandle handle) = 0;

			virtual PhysicsBackendResult CreateCharacter(
				const PhysicsCharacterCreateRequest& request,
				PhysicsBackendStagingToken& staging) = 0;
			virtual PhysicsBackendResult UpdateCharacter(const PhysicsCharacterUpdateRequest& request) = 0;
			virtual PhysicsBackendResult DestroyCharacter(PhysicsCharacterHandle handle) = 0;

			virtual PhysicsBackendResult ApplyCommand(const PhysicsCommand& command) = 0;
			virtual PhysicsBackendResult Step(const PhysicsBackendStepRequest& request) = 0;
			virtual PhysicsBackendQueryResult Query(const PhysicsBackendQueryRequest& request) const = 0;
			// A batch is one provider read boundary while every request keeps its own
			// result. Providers that cannot coalesce locking retain compatible behavior
			// through this default implementation.
			virtual PhysicsBackendResult QueryBatch(
				std::span<const PhysicsBackendQueryRequest> requests,
				std::span<PhysicsBackendQueryResult> results) const {
				if(requests.size() != results.size()) {
					return {PhysicsResultStatus::Rejected, PhysicsBackendErrorCode::InvalidArgument,
						"The provider query batch has mismatched request and result spans."};
				}
				for(std::size_t index = 0U; index < requests.size(); ++index) {
					results[index] = Query(requests[index]);
				}
				return {};
			}
			virtual PhysicsBackendResult Extract(PhysicsBackendExtraction& extraction) const = 0;
			// The request overload is additive so existing provider implementations and
			// compatibility tests keep the established full-state call. Providers may
			// override it to expose bounded incremental collection.
			virtual PhysicsBackendResult Extract(
				const PhysicsBackendExtractionRequest& request,
				PhysicsBackendExtraction& extraction) const {
				if(!request.IsValid()) {
					return {PhysicsResultStatus::Rejected, PhysicsBackendErrorCode::InvalidArgument,
						"The provider-neutral extraction request is invalid."};
				}
				extraction.world = request.world;
				extraction.tick = request.tick;
				return Extract(extraction);
			}
			// Profiles and leases are additive: older providers can retain the
			// established request path, while incremental providers keep pending state
			// until the protected publisher explicitly acknowledges this token.
			virtual PhysicsBackendResult Extract(
				const PhysicsBackendFixedStepWorkProfile& profile,
				PhysicsBackendExtraction& extraction,
				PhysicsBackendExtractionToken& token) const {
				PhysicsBackendExtractionRequest request;
				request.world = profile.world;
				request.tick = profile.tick;
				request.fields = profile.fields;
				request.batchSize = profile.rangeSize;
				request.parallelThreshold = profile.rangeSize;
				request.fullState = profile.fullStateReason != PhysicsBackendFullStateReason::None;
				request.selection = request.fullState
					? PhysicsBackendExtractionSelection::FullState
					: PhysicsBackendExtractionSelection::PublicationRelevant;
				const PhysicsBackendResult result = Extract(request, extraction);
				if(result.Succeeded()) token = {profile.world, profile.tick, 1U};
				return result;
			}
			virtual PhysicsBackendResult AcknowledgeExtraction(PhysicsBackendExtractionToken token) {
				return token.IsValid() ? PhysicsBackendResult{}
					: PhysicsBackendResult{PhysicsResultStatus::Rejected, PhysicsBackendErrorCode::InvalidArgument,
						"The extraction acknowledgement token is invalid."};
			}
			virtual PhysicsBackendResult AbortExtraction(PhysicsBackendExtractionToken token) {
				return token.IsValid() ? PhysicsBackendResult{}
					: PhysicsBackendResult{PhysicsResultStatus::Rejected, PhysicsBackendErrorCode::InvalidArgument,
						"The extraction abort token is invalid."};
			}
			virtual PhysicsBackendExtractionCapabilities ExtractionCapabilities() const noexcept {
				return {};
			}
			virtual PhysicsBackendJobTelemetry JobTelemetry() const noexcept {
				return {};
			}
			virtual PhysicsBackendSimulationQuality AppliedSimulationQuality() const noexcept {
				return {};
			}

			virtual PhysicsBackendResult CommitStaging(PhysicsBackendStagingToken staging) = 0;
			virtual PhysicsBackendResult RollbackStaging(PhysicsBackendStagingToken staging) = 0;
			virtual bool IsHealthy() const noexcept = 0;
		};
	}
}
