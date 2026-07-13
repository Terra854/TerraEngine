// PhysicsWorld.h - provider-neutral lifetime, identity, and fixed-step policy.
//
// This class deliberately owns only engine-facing state. A backend may use the
// handles issued here, but its provider objects, pointers, and job machinery do
// not cross this public boundary.

#pragma once

#include "Core/Export.h"
#include "PhysicsCommands.h"
#include "PhysicsEvents.h"
#include "PhysicsQueries.h"
#include "PhysicsSettings.h"
#include "Utilities/UUID.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace TerraEngine::Physics {
	inline constexpr PhysicsWorldId kPrimaryPhysicsWorldId{1U};

	// One request describes exactly one fixed physics update. Consumers must use
	// deltaTime for every callback belonging to this tick, including paused Next.
	struct PhysicsFixedStepRequest {
		PhysicsWorldHandle world;
		PhysicsTick tick = 0;
		float deltaTime = 0.0F;
		std::uint32_t substepIndex = 0;
		bool explicitPausedStep = false;

		constexpr bool IsValid() const noexcept {
			return world.IsValid() && tick != 0U && deltaTime > 0.0F;
		}
	};

	// The planning result makes dropped time and paused-token consumption visible
	// to scheduler diagnostics without exposing mutable world internals.
	struct PhysicsFixedStepPlan {
		std::vector<PhysicsFixedStepRequest> requests;
		float clampedTime = 0.0F;
		float droppedTime = 0.0F;
		float remainingAccumulator = 0.0F;
		std::uint32_t normalSubstepCount = 0;
		bool consumedExplicitPausedStep = false;
	};

	// These values are a consistent snapshot of the fixed-step state. Tick is
	// the most recently issued request; completedTick advances only after the
	// caller reports a successful provider step and publication preparation.
	struct PhysicsWorldTimingState {
		PhysicsTick tick = 0;
		PhysicsTick completedTick = 0;
		float accumulator = 0.0F;
		float clampedTime = 0.0F;
		float droppedTime = 0.0F;
		std::uint32_t pendingExplicitStepCount = 0;
		PhysicsFixedStepRequest lastCompletedStep;
	};

	// Registry statistics are intentionally handle-oriented. Provider IDs remain
	// private to backend record implementations added by later runtime slices.
	struct PhysicsWorldRegistryStatistics {
		std::size_t bodyCapacity = 0;
		std::size_t bodyCount = 0;
		std::size_t shapeCapacity = 0;
		std::size_t shapeCount = 0;
		std::size_t constraintCapacity = 0;
		std::size_t constraintCount = 0;
		std::size_t characterCapacity = 0;
		std::size_t characterCount = 0;
	};

	// Provider bytes are opaque and immutable after publication. Multiple asset
	// GUIDs with the same normalized recipe hash share this one allocation.
	struct PhysicsCookedShapeData {
		std::uint64_t recipeHash = 0U;
		std::string formatFamily;
		std::string provider;
		std::string providerVersion;
		std::string providerRevision;
		std::uint32_t formatVersion = 0U;
		std::uint64_t payloadChecksum = 0U;
		std::vector<std::byte> payload;
	};

	// Staleness belongs to an asset publication, not to shared shape bytes: one
	// asset may retain an old cook while another asset safely shares those bytes.
	struct PhysicsCookedShapeAssetState {
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		std::uint64_t assetRevision = 0U;
		std::uint64_t publicationGeneration = 0U;
		std::shared_ptr<const PhysicsCookedShapeData> data;
		std::string diagnostic;
		bool staleLastKnownGood = false;
	};

	struct PhysicsCookedShapePublication {
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		std::uint64_t assetRevision = 0U;
		PhysicsCookedShapeData data;
	};

	struct PhysicsCookedShapeCacheStatistics {
		std::size_t sharedShapeCount = 0U;
		std::size_t assetCount = 0U;
		std::size_t payloadBytes = 0U;
		std::uint64_t hitCount = 0U;
		std::uint64_t missCount = 0U;
		std::uint64_t publicationCount = 0U;
		std::uint64_t replacementCount = 0U;
		std::uint64_t reuseCount = 0U;
		std::uint64_t staleRetentionCount = 0U;
		std::uint64_t retirementCount = 0U;
	};

	// A runtime constraint registration deliberately mirrors only stable engine
	// identity and break policy. Provider-specific joint objects remain behind the
	// backend, while PhysicsWorld owns exactly-once break publication semantics.
	struct PhysicsConstraintRuntimeRegistration {
		PhysicsConstraintHandle handle;
		TerraEngine::UUID ownerObjectId = TerraEngine::EMPTY_UUID;
		PhysicsGeneration ownerObjectGeneration = 0U;
		PhysicsGeneration componentGeneration = 0U;
		TerraEngine::UUID recordId = TerraEngine::EMPTY_UUID;
		PhysicsGeneration recordGeneration = 0U;
		float breakForce = std::numeric_limits<float>::infinity();
		float breakTorque = std::numeric_limits<float>::infinity();
		PhysicsGeneration resetGeneration = 1U;
	};

	// The backend reports solver impulses for one completed fixed tick. World
	// converts them to force/torque before evaluating the durable break policy.
	struct PhysicsConstraintImpulseSample {
		PhysicsConstraintHandle handle;
		TerraEngine::UUID ownerObjectId = TerraEngine::EMPTY_UUID;
		PhysicsGeneration ownerObjectGeneration = 0U;
		PhysicsGeneration componentGeneration = 0U;
		TerraEngine::UUID recordId = TerraEngine::EMPTY_UUID;
		PhysicsGeneration recordGeneration = 0U;
		PhysicsGeneration resetGeneration = 0U;
		float linearImpulseMagnitude = 0.0F;
		float angularImpulseMagnitude = 0.0F;
	};

	struct PhysicsConstraintRuntimeState {
		PhysicsConstraintRuntimeRegistration registration;
		PhysicsTick lastSampleTick = 0U;
		float lastForce = 0.0F;
		float lastTorque = 0.0F;
		bool broken = false;
	};

	// Character state is an immutable observation packet. Desired movement and
	// jump/resize requests remain command payloads, never serializable state.
	enum class PhysicsCharacterGroundState : std::uint8_t {
		OnGround,
		OnSteepGround,
		NotSupported,
		InAir
	};

	struct PhysicsCharacterState {
		PhysicsCharacterHandle handle;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		PhysicsGeneration objectGeneration = 0U;
		PhysicsGeneration componentGeneration = 0U;
		PhysicsWorldHandle world;
		PhysicsTick tick = 0U;
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

	struct PhysicsCharacterStatePacket {
		PhysicsWorldHandle world;
		PhysicsTick tick = 0U;
		PhysicsTick expiryTick = 0U;
		std::shared_ptr<const std::vector<PhysicsCharacterState>> states;

		[[nodiscard]] bool IsValidFor(PhysicsWorldHandle activeWorld, PhysicsTick currentTick) const noexcept;
	};

	// PhysicsWorld represents the one initial gameplay world. It can be safely
	// observed from worker preparation code, but callers may allocate/release
	// handles only while Active or Paused. It owns no private worker pool.
	class TERRAENGINE_API PhysicsWorld final {
	public:
		explicit PhysicsWorld(
			PhysicsWorldId worldId = kPrimaryPhysicsWorldId,
			PhysicsCommandQueueLimits commandLimits = {});
		~PhysicsWorld() = default;

		PhysicsWorld(const PhysicsWorld&) = delete;
		PhysicsWorld& operator=(const PhysicsWorld&) = delete;
		PhysicsWorld(PhysicsWorld&&) = delete;
		PhysicsWorld& operator=(PhysicsWorld&&) = delete;

		// Creation validates settings before changing state, then assigns a fresh,
		// nonzero generation and enables this world's bounded command queue.
		bool Create(const PhysicsSettings& settings);
		bool SetPaused(bool paused);
		bool IsPaused() const;
		bool UpdateSettings(const PhysicsSettings& settings);

		PhysicsWorldHandle Handle() const;
		PhysicsWorldState State() const;
		PhysicsSettings Settings() const;
		PhysicsWorldTimingState Timing() const;
		PhysicsWorldRegistryStatistics RegistryStatistics() const;
		// Publishing replaces only the addressed asset state. Existing readers keep
		// their shared immutable lease until their worker job or snapshot retires.
		bool PublishCookedShape(PhysicsCookedShapePublication publication);
		bool MarkCookedShapeRecookFailed(const TerraEngine::UUID& assetId, std::string diagnostic);
		std::shared_ptr<const PhysicsCookedShapeAssetState> AcquireCookedShape(
			const TerraEngine::UUID& assetId) const;
		bool RetireCookedShape(const TerraEngine::UUID& assetId);
		PhysicsCookedShapeCacheStatistics CookedShapeCacheStatistics() const;
		PhysicsCommandReceipt EnqueueCommand(PhysicsCommand command);
		bool CancelCommand(PhysicsCommandId commandId);
		PhysicsCommandBatch MergeCommands(PhysicsTick inclusiveTick);
		PhysicsCommandQueueDiagnostics CommandQueueDiagnostics() const;

		// Query submissions are generation-validated and bounded at this world
		// boundary. Preparation yields immutable packets for worker/backend execution;
		// completion and publication occur only at the protected physics boundary.
		PhysicsQueryReceipt SubmitQuery(PhysicsQueryRequest request);
		PhysicsQueryBatchSubmission SubmitQueryBatch(std::span<const PhysicsQueryRequest> requests);
		bool CancelQuery(PhysicsQueryHandle handle);
		PhysicsPreparedQueryBatch PrepareQueries(PhysicsTick executionTick);
		bool CompleteQuery(
			const PhysicsPreparedQuery& prepared,
			PhysicsTick completionTick,
			PhysicsResultStatus status,
			std::span<const PhysicsQueryHit> candidates,
			bool providerTruncated = false,
			std::string diagnostic = {});
		PhysicsQueryResult ReadQuery(PhysicsQueryHandle handle) const;
		PhysicsQueryDiagnostics QueryDiagnostics() const;

		PhysicsEventSubscription SubscribeEvents(PhysicsEventSubscriptionPolicy policy);
		bool UnsubscribeEvents(std::uint64_t id, PhysicsGeneration generation);
		std::shared_ptr<const PhysicsEventPacket> PublishEvents(
			PhysicsTick tick,
			std::span<const PhysicsEvent> events,
			std::uint64_t providerDroppedCount = 0U);
		std::shared_ptr<const PhysicsEventPacket> AcquireEventPacket(
			PhysicsTick requestedTick,
			PhysicsTick currentTick) const;

		// Constraint lifetime and break policy are evaluated only at a protected
		// completed-tick boundary. A rejected input batch changes no record.
		bool RegisterConstraint(PhysicsConstraintRuntimeRegistration registration);
		bool RemoveConstraint(PhysicsConstraintHandle handle);
		bool ResetConstraint(PhysicsConstraintHandle handle, PhysicsGeneration resetGeneration);
		bool EvaluateConstraintImpulses(
			PhysicsTick tick,
			float fixedDeltaTime,
			std::span<const PhysicsConstraintImpulseSample> samples,
			std::vector<PhysicsEvent>& breakEvents);
		std::shared_ptr<const PhysicsConstraintRuntimeState> AcquireConstraintState(PhysicsConstraintHandle handle) const;

		// Character publications are bounded and immutable so gameplay/editor/C# can
		// observe ground/support state without reading provider objects mid-step.
		bool PublishCharacterStates(PhysicsTick tick, std::span<const PhysicsCharacterState> states);
		std::shared_ptr<const PhysicsCharacterStatePacket> AcquireCharacterStatePacket(
			PhysicsTick requestedTick,
			PhysicsTick currentTick) const;

		// Next is accepted only while paused and only when no earlier Next token is
		// outstanding. Capturing duration here prevents a later settings edit from
		// changing the requested step's duration.
		bool RequestExplicitPausedStep();
		PhysicsFixedStepPlan PlanFixedSteps(float elapsedTime);
		bool MarkStepCompleted(const PhysicsFixedStepRequest& request);

		// A provider or orchestration failure may enter Failed. It still requires
		// the normal stopping and draining path before a new world can be created.
		bool MarkFailed();

		// Ordered teardown is split so the scheduler/backend can join jobs and
		// destroy provider objects between draining commands and clearing handles.
		// BeginStopping: stop submission. BeginDraining: cancel queued commands.
		// CompleteTeardown: clear registries and advance the generation.
		bool BeginStopping();
		std::size_t BeginDraining();
		bool CompleteTeardown();

		PhysicsCommandQueue& Commands() noexcept { return commandQueue; }
		const PhysicsCommandQueue& Commands() const noexcept { return commandQueue; }

		PhysicsBodyHandle AllocateBody();
		PhysicsShapeHandle AllocateShape();
		PhysicsConstraintHandle AllocateConstraint();
		PhysicsCharacterHandle AllocateCharacter();
		bool Release(PhysicsBodyHandle handle);
		bool Release(PhysicsShapeHandle handle);
		bool Release(PhysicsConstraintHandle handle);
		bool Release(PhysicsCharacterHandle handle);
		PhysicsHandleStatus Status(PhysicsBodyHandle handle) const;
		PhysicsHandleStatus Status(PhysicsShapeHandle handle) const;
		PhysicsHandleStatus Status(PhysicsConstraintHandle handle) const;
		PhysicsHandleStatus Status(PhysicsCharacterHandle handle) const;

	private:
		static PhysicsGeneration NextNonzeroGeneration(PhysicsGeneration generation) noexcept;
		bool CanAccessRegistriesLocked() const noexcept;
		PhysicsWorldHandle ActiveHandleLocked() const noexcept;
		PhysicsFixedStepRequest IssueStepLocked(float duration, std::uint32_t substepIndex, bool explicitPausedStep);
		void ResetTimingLocked() noexcept;

		mutable std::mutex mutex;
		const PhysicsWorldId worldId;
		PhysicsGeneration generationCursor = 0;
		PhysicsGeneration activeGeneration = 0;
		PhysicsWorldState state = PhysicsWorldState::Absent;
		PhysicsSettings settings = PhysicsSettings::Defaults();
		PhysicsWorldTimingState timing;
		std::deque<float> explicitPausedStepDurations;
		PhysicsCommandQueue commandQueue;
		std::unique_ptr<PhysicsQueryManager> queryManager;
		std::unique_ptr<PhysicsEventSubscriptionRegistry> eventSubscriptions;
		std::unique_ptr<PhysicsEventHistory> eventHistory;
		PhysicsHandleAllocator<PhysicsBodyHandle> bodyRegistry;
		PhysicsHandleAllocator<PhysicsShapeHandle> shapeRegistry;
		PhysicsHandleAllocator<PhysicsConstraintHandle> constraintRegistry;
		PhysicsHandleAllocator<PhysicsCharacterHandle> characterRegistry;
		std::unordered_map<std::uint64_t, PhysicsConstraintRuntimeState> constraintRuntime;
		std::vector<std::shared_ptr<const PhysicsCharacterStatePacket>> characterStateHistory;
		std::unordered_map<std::uint64_t, std::shared_ptr<const PhysicsCookedShapeData>> cookedShapes;
		std::unordered_map<TerraEngine::UUID, std::shared_ptr<const PhysicsCookedShapeAssetState>> cookedShapeAssets;
		mutable PhysicsCookedShapeCacheStatistics cookedShapeStatistics;
	};
}
