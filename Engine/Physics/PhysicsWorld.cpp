// PhysicsWorld.cpp - state machine and deterministic fixed-step bookkeeping.

#include "PhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace TerraEngine::Physics {
	namespace {
		std::uint64_t ComputeCookedPayloadChecksum(const std::vector<std::byte>& payload) noexcept {
			std::uint64_t hash = 14695981039346656037ULL;
			for(const std::byte value : payload) {
				hash ^= std::to_integer<std::uint8_t>(value);
				hash *= 1099511628211ULL;
			}
			return hash;
		}

		std::uint64_t RuntimeHandleKey(PhysicsConstraintHandle handle) noexcept {
			return (static_cast<std::uint64_t>(handle.slotIndex) << 32U)
				| static_cast<std::uint64_t>(handle.slotGeneration);
		}

		std::uint64_t RuntimeHandleKey(PhysicsCharacterHandle handle) noexcept {
			return (static_cast<std::uint64_t>(handle.slotIndex) << 32U)
				| static_cast<std::uint64_t>(handle.slotGeneration);
		}

		bool IsBreakThresholdValid(float value) noexcept {
			// Infinity has a useful authoring meaning: that axis is unbreakable.
			return !std::isnan(value) && value >= 0.0F;
		}

		bool IsFiniteCharacterState(const PhysicsCharacterState& state) noexcept {
			return state.handle.IsValid()
				&& state.world.IsValid()
				&& state.tick != 0U
				&& state.objectId != TerraEngine::EMPTY_UUID
				&& state.objectGeneration != 0U
				&& state.componentGeneration != 0U
				&& state.pose.IsFinite()
				&& state.linearVelocity.IsFinite()
				&& state.groundPosition.IsFinite()
				&& state.groundNormal.IsFinite()
				&& state.supportVelocity.IsFinite();
		}
	}

	bool PhysicsCharacterStatePacket::IsValidFor(
		PhysicsWorldHandle activeWorld,
		PhysicsTick currentTick) const noexcept {
		return world == activeWorld && world.IsValid() && tick != 0U
			&& expiryTick >= tick && currentTick <= expiryTick && states != nullptr;
	}

	PhysicsWorld::PhysicsWorld(PhysicsWorldId requestedWorldId, PhysicsCommandQueueLimits commandLimits)
		: worldId(requestedWorldId.IsValid() ? requestedWorldId : kPrimaryPhysicsWorldId)
		, commandQueue(commandLimits) {
		// A default-constructed queue rejects work until Create assigns a concrete
		// world handle. This prevents commands targeting generation zero.
		commandQueue.StopAccepting();
	}

	bool PhysicsWorld::Create(const PhysicsSettings& requestedSettings) {
		if(!requestedSettings.Validate().accepted) {
			return false;
		}

		std::lock_guard<std::mutex> lock(mutex);
		if(state != PhysicsWorldState::Absent && state != PhysicsWorldState::Destroyed) {
			return false;
		}

		// The first lifetime advances from zero. Teardown already advances the
		// cursor before a recreated world becomes visible, so reuse that reserved
		// value instead of skipping a generation on every stop/start cycle.
		const bool firstLifetime = state == PhysicsWorldState::Absent;
		state = PhysicsWorldState::Creating;
		activeGeneration = firstLifetime
			? NextNonzeroGeneration(generationCursor)
			: generationCursor;
		generationCursor = activeGeneration;
		settings = requestedSettings;
		ResetTimingLocked();
		commandQueue.ResetForWorld(ActiveHandleLocked());
		queryManager = std::make_unique<PhysicsQueryManager>(settings);
		eventSubscriptions = std::make_unique<PhysicsEventSubscriptionRegistry>();
		eventHistory = std::make_unique<PhysicsEventHistory>(PhysicsEventPacketLimits{
			4096U, 1024U, static_cast<std::size_t>(settings.retainedResultTicks) + 1U,
			settings.retainedResultTicks});
		// Automation must be able to inspect canonical contacts even when gameplay
		// has no subscriber. Keep its bounded immutable history distinct so this
		// observational path cannot broaden managed event delivery.
		diagnosticEventHistory = std::make_unique<PhysicsEventHistory>(PhysicsEventPacketLimits{
			4096U, 1024U, static_cast<std::size_t>(settings.retainedResultTicks) + 1U,
			settings.retainedResultTicks});
		constraintRuntime.clear();
		characterStateHistory.clear();
		state = PhysicsWorldState::Active;
		return true;
	}

	bool PhysicsWorld::SetPaused(bool paused) {
		std::lock_guard<std::mutex> lock(mutex);
		if(paused && state == PhysicsWorldState::Active) {
			state = PhysicsWorldState::Paused;
			return true;
		}
		if(!paused && state == PhysicsWorldState::Paused) {
			// Resuming leaves any residual accumulator intact; it is consumed under
			// the normal capped policy on the next scheduling opportunity.
			state = PhysicsWorldState::Active;
			return true;
		}
		return false;
	}

	bool PhysicsWorld::IsPaused() const {
		std::lock_guard<std::mutex> lock(mutex);
		return state == PhysicsWorldState::Paused;
	}

	bool PhysicsWorld::UpdateSettings(const PhysicsSettings& requestedSettings) {
		if(!requestedSettings.Validate().accepted) {
			return false;
		}

		std::lock_guard<std::mutex> lock(mutex);
		if(!CanAccessRegistriesLocked()) {
			return false;
		}

		settings = requestedSettings;
		if(timing.accumulator > settings.maxAccumulatedTime) {
			const float discarded = timing.accumulator - settings.maxAccumulatedTime;
			timing.accumulator = settings.maxAccumulatedTime;
			timing.clampedTime += discarded;
			timing.droppedTime += discarded;
		}
		return true;
	}

	PhysicsWorldHandle PhysicsWorld::Handle() const {
		std::lock_guard<std::mutex> lock(mutex);
		return ActiveHandleLocked();
	}

	PhysicsWorldState PhysicsWorld::State() const {
		std::lock_guard<std::mutex> lock(mutex);
		return state;
	}

	PhysicsSettings PhysicsWorld::Settings() const {
		std::lock_guard<std::mutex> lock(mutex);
		return settings;
	}

	PhysicsWorldTimingState PhysicsWorld::Timing() const {
		std::lock_guard<std::mutex> lock(mutex);
		PhysicsWorldTimingState result = timing;
		result.pendingExplicitStepCount = static_cast<std::uint32_t>(explicitPausedStepDurations.size());
		return result;
	}

	PhysicsWorldRegistryStatistics PhysicsWorld::RegistryStatistics() const {
		std::lock_guard<std::mutex> lock(mutex);
		return PhysicsWorldRegistryStatistics{
			bodyRegistry.Capacity(), bodyRegistry.AliveCount(),
			shapeRegistry.Capacity(), shapeRegistry.AliveCount(),
			constraintRegistry.Capacity(), constraintRegistry.AliveCount(),
			characterRegistry.Capacity(), characterRegistry.AliveCount()};
	}

	PhysicsQueryReceipt PhysicsWorld::SubmitQuery(PhysicsQueryRequest request) {
		std::lock_guard<std::mutex> lock(mutex);
		if(!CanAccessRegistriesLocked() || queryManager == nullptr) {
			return {false, {}, PhysicsQueryRejectionReason::StaleWorld,
				"The physics world is not accepting query work."};
		}
		return queryManager->Submit(std::move(request), ActiveHandleLocked(), timing.tick);
	}

	PhysicsQueryBatchSubmission PhysicsWorld::SubmitQueryBatch(std::span<const PhysicsQueryRequest> requests) {
		std::lock_guard<std::mutex> lock(mutex);
		if(!CanAccessRegistriesLocked() || queryManager == nullptr) {
			return {false, PhysicsQueryRejectionReason::StaleWorld, {}};
		}
		return queryManager->SubmitBatch(requests, ActiveHandleLocked(), timing.tick);
	}

	bool PhysicsWorld::CancelQuery(PhysicsQueryHandle handle) {
		std::lock_guard<std::mutex> lock(mutex);
		return CanAccessRegistriesLocked() && queryManager != nullptr
			&& queryManager->Cancel(handle, ActiveHandleLocked());
	}

	PhysicsPreparedQueryBatch PhysicsWorld::PrepareQueries(PhysicsTick executionTick) {
		std::lock_guard<std::mutex> lock(mutex);
		if(!CanAccessRegistriesLocked() || queryManager == nullptr) return {};
		queryManager->Expire(ActiveHandleLocked(), executionTick);
		return queryManager->Prepare(ActiveHandleLocked(), executionTick);
	}

	bool PhysicsWorld::CompleteQuery(
		const PhysicsPreparedQuery& prepared,
		PhysicsTick completionTick,
		PhysicsResultStatus status,
		std::span<const PhysicsQueryHit> candidates,
		bool providerTruncated,
		std::string diagnostic) {
		std::lock_guard<std::mutex> lock(mutex);
		return CanAccessRegistriesLocked() && queryManager != nullptr
			&& queryManager->Complete(prepared, ActiveHandleLocked(), completionTick,
				status, candidates, providerTruncated, std::move(diagnostic));
	}

	PhysicsQueryResult PhysicsWorld::ReadQuery(PhysicsQueryHandle handle) const {
		std::lock_guard<std::mutex> lock(mutex);
		const PhysicsWorldHandle active = ActiveHandleLocked();
		if(!CanAccessRegistriesLocked() || queryManager == nullptr
			|| handle.worldId != active.worldId || handle.worldGeneration != active.worldGeneration) {
			PhysicsQueryResult result;
			result.handle = handle;
			result.world = active;
			result.tick = timing.tick;
			result.status = PhysicsResultStatus::StaleTarget;
			result.rejectionReason = PhysicsQueryRejectionReason::StaleWorld;
			result.diagnostic = "The query result belongs to an absent or stale world.";
			result.hits = std::make_shared<const std::vector<PhysicsQueryHit>>();
			return result;
		}
		return queryManager->Read(handle, active, timing.tick);
	}

	PhysicsQueryDiagnostics PhysicsWorld::QueryDiagnostics() const {
		std::lock_guard<std::mutex> lock(mutex);
		return queryManager != nullptr ? queryManager->GetDiagnostics() : PhysicsQueryDiagnostics{};
	}

	PhysicsEventSubscription PhysicsWorld::SubscribeEvents(PhysicsEventSubscriptionPolicy policy) {
		std::lock_guard<std::mutex> lock(mutex);
		return CanAccessRegistriesLocked() && eventSubscriptions != nullptr
			? eventSubscriptions->Subscribe(std::move(policy)) : PhysicsEventSubscription{};
	}

	bool PhysicsWorld::UnsubscribeEvents(std::uint64_t id, PhysicsGeneration generation) {
		std::lock_guard<std::mutex> lock(mutex);
		return CanAccessRegistriesLocked() && eventSubscriptions != nullptr
			&& eventSubscriptions->Unsubscribe(id, generation);
	}

	std::shared_ptr<const PhysicsEventPacket> PhysicsWorld::PublishEvents(
		PhysicsTick tick,
		std::span<const PhysicsEvent> events,
		std::uint64_t providerDroppedCount) {
		std::lock_guard<std::mutex> lock(mutex);
		if(!CanAccessRegistriesLocked() || eventSubscriptions == nullptr || eventHistory == nullptr || diagnosticEventHistory == nullptr
			|| tick == 0U) return nullptr;
		PhysicsEventThreadLocalCollector collector(std::max<std::size_t>(events.size(), 1U));
		for(const PhysicsEvent& event : events) collector.Record(event);
		const PhysicsEventThreadLocalCollector* collectors[] = {&collector};
		const PhysicsEventPacketLimits limits{4096U, 4096U,
			static_cast<std::size_t>(settings.retainedResultTicks) + 1U,
			settings.retainedResultTicks};
		const PhysicsEventSubscriptionSnapshot subscriptions = eventSubscriptions->Snapshot();
		PhysicsEventPacket packet = PhysicsEventPacketBuilder::MergeCollectors(
			ActiveHandleLocked(), tick, collectors, subscriptions,
			limits);
		PhysicsEventPacket diagnosticPacket = PhysicsEventPacketBuilder::MergeCollectors(
			ActiveHandleLocked(), tick, collectors, subscriptions, limits, false);
		packet.droppedCount += providerDroppedCount;
		packet.overflowed = packet.overflowed || providerDroppedCount != 0U;
		diagnosticPacket.droppedCount += providerDroppedCount;
		diagnosticPacket.overflowed = diagnosticPacket.overflowed || providerDroppedCount != 0U;
		// Both histories contain immutable packets built from the same validated
		// collector. No synthetic subscription is added to the gameplay registry.
		if(!eventHistory->Publish(std::move(packet)) || !diagnosticEventHistory->Publish(std::move(diagnosticPacket))) return nullptr;
		return eventHistory->Acquire(ActiveHandleLocked(), tick, tick);
	}

	std::shared_ptr<const PhysicsEventPacket> PhysicsWorld::AcquireEventPacket(
		PhysicsTick requestedTick,
		PhysicsTick currentTick) const {
		std::lock_guard<std::mutex> lock(mutex);
		return CanAccessRegistriesLocked() && eventHistory != nullptr
			? eventHistory->Acquire(ActiveHandleLocked(), requestedTick, currentTick) : nullptr;
	}

	std::shared_ptr<const PhysicsEventPacket> PhysicsWorld::AcquireDiagnosticEventPacket(
		PhysicsTick requestedTick,
		PhysicsTick currentTick) const {
		std::lock_guard<std::mutex> lock(mutex);
		return CanAccessRegistriesLocked() && diagnosticEventHistory != nullptr
			? diagnosticEventHistory->Acquire(ActiveHandleLocked(), requestedTick, currentTick) : nullptr;
	}

	bool PhysicsWorld::RegisterConstraint(PhysicsConstraintRuntimeRegistration registration) {
		std::lock_guard<std::mutex> lock(mutex);
		const PhysicsWorldHandle active = ActiveHandleLocked();
		if(!CanAccessRegistriesLocked()
			|| constraintRegistry.Status(registration.handle, active) != PhysicsHandleStatus::Alive
			|| registration.ownerObjectId == TerraEngine::EMPTY_UUID
			|| registration.ownerObjectGeneration == 0U
			|| registration.componentGeneration == 0U
			|| registration.recordId == TerraEngine::EMPTY_UUID
			|| registration.recordGeneration == 0U
			|| registration.resetGeneration == 0U
			|| !IsBreakThresholdValid(registration.breakForce)
			|| !IsBreakThresholdValid(registration.breakTorque)) {
			return false;
		}
		const std::uint64_t key = RuntimeHandleKey(registration.handle);
		if(constraintRuntime.contains(key)) return false;
		constraintRuntime.emplace(key, PhysicsConstraintRuntimeState{std::move(registration)});
		return true;
	}

	bool PhysicsWorld::RemoveConstraint(PhysicsConstraintHandle handle) {
		std::lock_guard<std::mutex> lock(mutex);
		const PhysicsWorldHandle active = ActiveHandleLocked();
		if(!CanAccessRegistriesLocked()
			|| constraintRegistry.Status(handle, active) != PhysicsHandleStatus::Alive) return false;
		return constraintRuntime.erase(RuntimeHandleKey(handle)) == 1U;
	}

	bool PhysicsWorld::ResetConstraint(PhysicsConstraintHandle handle, PhysicsGeneration resetGeneration) {
		std::lock_guard<std::mutex> lock(mutex);
		const PhysicsWorldHandle active = ActiveHandleLocked();
		if(!CanAccessRegistriesLocked()
			|| resetGeneration == 0U
			|| constraintRegistry.Status(handle, active) != PhysicsHandleStatus::Alive) return false;
		const auto found = constraintRuntime.find(RuntimeHandleKey(handle));
		if(found == constraintRuntime.end() || found->second.registration.handle != handle
			|| resetGeneration <= found->second.registration.resetGeneration) return false;
		// A strictly newer authoring token is the only operation that can revive a
		// broken record. Equal/older commands are duplicates or stale and are atomic
		// failures, preserving exactly-once break publication.
		found->second.registration.resetGeneration = resetGeneration;
		found->second.broken = false;
		found->second.lastForce = 0.0F;
		found->second.lastTorque = 0.0F;
		return true;
	}

	bool PhysicsWorld::EvaluateConstraintImpulses(
		PhysicsTick tick,
		float fixedDeltaTime,
		std::span<const PhysicsConstraintImpulseSample> samples,
		std::vector<PhysicsEvent>& breakEvents) {
		if(tick == 0U || !std::isfinite(fixedDeltaTime) || fixedDeltaTime <= 0.0F) return false;
		std::lock_guard<std::mutex> lock(mutex);
		if(!CanAccessRegistriesLocked()) return false;

		struct PreparedSample {
			std::uint64_t key = 0U;
			float force = 0.0F;
			float torque = 0.0F;
			bool breaks = false;
		};
		std::vector<PreparedSample> prepared;
		prepared.reserve(samples.size());
		std::unordered_set<std::uint64_t> seen;
		seen.reserve(samples.size());
		const PhysicsWorldHandle active = ActiveHandleLocked();
		for(const PhysicsConstraintImpulseSample& sample : samples) {
			const std::uint64_t key = RuntimeHandleKey(sample.handle);
			const auto found = constraintRuntime.find(key);
			if(!sample.handle.IsValid() || !seen.insert(key).second
				|| constraintRegistry.Status(sample.handle, active) != PhysicsHandleStatus::Alive
				|| found == constraintRuntime.end() || found->second.registration.handle != sample.handle
				|| sample.ownerObjectId != found->second.registration.ownerObjectId
				|| sample.ownerObjectGeneration != found->second.registration.ownerObjectGeneration
				|| sample.componentGeneration != found->second.registration.componentGeneration
				|| sample.recordId != found->second.registration.recordId
				|| sample.recordGeneration != found->second.registration.recordGeneration
				|| sample.resetGeneration != found->second.registration.resetGeneration
				|| tick <= found->second.lastSampleTick
				|| !std::isfinite(sample.linearImpulseMagnitude) || sample.linearImpulseMagnitude < 0.0F
				|| !std::isfinite(sample.angularImpulseMagnitude) || sample.angularImpulseMagnitude < 0.0F) {
				return false;
			}
			const float force = sample.linearImpulseMagnitude / fixedDeltaTime;
			const float torque = sample.angularImpulseMagnitude / fixedDeltaTime;
			if(!std::isfinite(force) || !std::isfinite(torque)) return false;
			prepared.push_back({key, force, torque,
				!found->second.broken && (force > found->second.registration.breakForce
					|| torque > found->second.registration.breakTorque)});
		}

		std::vector<PhysicsEvent> emitted;
		emitted.reserve(prepared.size());
		for(const PreparedSample& sample : prepared) {
			PhysicsConstraintRuntimeState& state = constraintRuntime.at(sample.key);
			state.lastSampleTick = tick;
			state.lastForce = sample.force;
			state.lastTorque = sample.torque;
			if(!sample.breaks) continue;
			state.broken = true;
			PhysicsEvent event;
			event.type = PhysicsEventType::ConstraintBroken;
			event.world = active;
			event.tick = tick;
			event.hasSecondParticipant = false;
			// Populate first's object fields as well so generic subscription filters
			// can include/exclude an owning object without special callback code.
			event.first.objectId = state.registration.ownerObjectId;
			event.first.objectGeneration = state.registration.ownerObjectGeneration;
			event.first.componentGeneration = state.registration.componentGeneration;
			event.first.colliderRecordId = state.registration.recordId;
			event.first.recordGeneration = state.registration.recordGeneration;
			event.constraint = state.registration.handle;
			event.constraintOwnerObjectId = state.registration.ownerObjectId;
			event.constraintOwnerObjectGeneration = state.registration.ownerObjectGeneration;
			event.constraintComponentGeneration = state.registration.componentGeneration;
			event.constraintRecordId = state.registration.recordId;
			event.constraintRecordGeneration = state.registration.recordGeneration;
			event.constraintResetGeneration = state.registration.resetGeneration;
			event.constraintBreakForce = sample.force;
			event.constraintBreakTorque = sample.torque;
			event.constraintForceThreshold = state.registration.breakForce;
			event.constraintTorqueThreshold = state.registration.breakTorque;
			emitted.push_back(std::move(event));
		}
		std::sort(emitted.begin(), emitted.end(), PhysicsEventLess);
		breakEvents.insert(breakEvents.end(),
			std::make_move_iterator(emitted.begin()), std::make_move_iterator(emitted.end()));
		return true;
	}

	std::shared_ptr<const PhysicsConstraintRuntimeState> PhysicsWorld::AcquireConstraintState(
		PhysicsConstraintHandle handle) const {
		std::lock_guard<std::mutex> lock(mutex);
		const PhysicsWorldHandle active = ActiveHandleLocked();
		if(!CanAccessRegistriesLocked()
			|| constraintRegistry.Status(handle, active) != PhysicsHandleStatus::Alive) return {};
		const auto found = constraintRuntime.find(RuntimeHandleKey(handle));
		return found == constraintRuntime.end() || found->second.registration.handle != handle
			? std::shared_ptr<const PhysicsConstraintRuntimeState>{}
			: std::make_shared<const PhysicsConstraintRuntimeState>(found->second);
	}

	bool PhysicsWorld::PublishCharacterStates(
		PhysicsTick tick,
		std::span<const PhysicsCharacterState> states) {
		std::lock_guard<std::mutex> lock(mutex);
		const PhysicsWorldHandle active = ActiveHandleLocked();
		if(!CanAccessRegistriesLocked() || tick == 0U) return false;
		if(std::any_of(characterStateHistory.begin(), characterStateHistory.end(), [tick, active](const auto& packet) {
			return packet != nullptr && packet->world == active && packet->tick == tick;
		})) return false;
		std::unordered_set<std::uint64_t> seen;
		seen.reserve(states.size());
		std::vector<PhysicsCharacterState> copied;
		copied.reserve(states.size());
		for(const PhysicsCharacterState& stateValue : states) {
			if(!IsFiniteCharacterState(stateValue) || stateValue.world != active || stateValue.tick != tick
				|| !seen.insert(RuntimeHandleKey(stateValue.handle)).second
				|| characterRegistry.Status(stateValue.handle, active) != PhysicsHandleStatus::Alive
				|| (stateValue.supportBody.IsValid()
					&& bodyRegistry.Status(stateValue.supportBody, active) != PhysicsHandleStatus::Alive)) {
				return false;
			}
			copied.push_back(stateValue);
		}
		std::sort(copied.begin(), copied.end(), [](const PhysicsCharacterState& left, const PhysicsCharacterState& right) {
			return left.handle < right.handle;
		});
		PhysicsCharacterStatePacket packet;
		packet.world = active;
		packet.tick = tick;
		packet.expiryTick = tick > std::numeric_limits<PhysicsTick>::max() - settings.retainedResultTicks
			? std::numeric_limits<PhysicsTick>::max() : tick + settings.retainedResultTicks;
		packet.states = std::make_shared<const std::vector<PhysicsCharacterState>>(std::move(copied));
		characterStateHistory.push_back(std::make_shared<const PhysicsCharacterStatePacket>(std::move(packet)));
		const std::size_t maximumPackets = static_cast<std::size_t>(settings.retainedResultTicks) + 1U;
		while(characterStateHistory.size() > maximumPackets) characterStateHistory.erase(characterStateHistory.begin());
		return true;
	}

	std::shared_ptr<const PhysicsCharacterStatePacket> PhysicsWorld::AcquireCharacterStatePacket(
		PhysicsTick requestedTick,
		PhysicsTick currentTick) const {
		std::lock_guard<std::mutex> lock(mutex);
		const PhysicsWorldHandle active = ActiveHandleLocked();
		if(!CanAccessRegistriesLocked()) return {};
		const auto found = std::find_if(characterStateHistory.begin(), characterStateHistory.end(),
			[active, requestedTick, currentTick](const auto& packet) {
				return packet != nullptr && packet->tick == requestedTick
					&& packet->IsValidFor(active, currentTick);
			});
		return found == characterStateHistory.end() ? std::shared_ptr<const PhysicsCharacterStatePacket>{} : *found;
	}

	bool PhysicsWorld::PublishCookedShape(PhysicsCookedShapePublication publication) {
		constexpr std::size_t kMaximumRuntimeCookedPayloadBytes = 64U * 1024U * 1024U;
		if(publication.assetId == TerraEngine::EMPTY_UUID
			|| publication.assetRevision == 0U
			|| publication.data.recipeHash == 0U
			|| publication.data.formatFamily.empty()
			|| publication.data.provider.empty()
			|| publication.data.providerVersion.empty()
			|| publication.data.providerRevision.empty()
			|| publication.data.formatVersion == 0U
			|| publication.data.payloadChecksum == 0U
			|| publication.data.payload.empty()
			|| publication.data.payload.size() > kMaximumRuntimeCookedPayloadBytes
			|| ComputeCookedPayloadChecksum(publication.data.payload) != publication.data.payloadChecksum) {
			return false;
		}

		std::lock_guard<std::mutex> lock(mutex);
		if(!CanAccessRegistriesLocked()) return false;
		const auto previous = cookedShapeAssets.find(publication.assetId);
		if(previous != cookedShapeAssets.end()
			&& publication.assetRevision <= previous->second->assetRevision) {
			// A late worker result cannot overwrite a newer or already published
			// asset revision at this protected boundary.
			return false;
		}

		std::shared_ptr<const PhysicsCookedShapeData> sharedData;
		const auto shared = cookedShapes.find(publication.data.recipeHash);
		if(shared != cookedShapes.end()) {
			const bool compatible = shared->second->formatFamily == publication.data.formatFamily
				&& shared->second->provider == publication.data.provider
				&& shared->second->providerVersion == publication.data.providerVersion
				&& shared->second->providerRevision == publication.data.providerRevision
				&& shared->second->formatVersion == publication.data.formatVersion
				&& shared->second->payloadChecksum == publication.data.payloadChecksum
				&& shared->second->payload.size() == publication.data.payload.size();
			if(!compatible) {
				// One normalized recipe hash must identify exactly one cook under a
				// provider contract. Reject collisions instead of silently redirecting
				// other assets that already share the immutable entry.
				return false;
			}
			sharedData = shared->second;
			++cookedShapeStatistics.reuseCount;
		} else {
			sharedData = std::make_shared<const PhysicsCookedShapeData>(std::move(publication.data));
			cookedShapes[sharedData->recipeHash] = sharedData;
		}

		std::uint64_t publicationGeneration = 1U;
		std::uint64_t previousRecipeHash = 0U;
		if(previous != cookedShapeAssets.end()) {
			publicationGeneration = previous->second->publicationGeneration + 1U;
			if(publicationGeneration == 0U) publicationGeneration = 1U;
			previousRecipeHash = previous->second->data->recipeHash;
			++cookedShapeStatistics.replacementCount;
		}
		auto state = std::make_shared<PhysicsCookedShapeAssetState>();
		state->assetId = publication.assetId;
		state->assetRevision = publication.assetRevision;
		state->publicationGeneration = publicationGeneration;
		state->data = std::move(sharedData);
		cookedShapeAssets[publication.assetId] = std::move(state);
		++cookedShapeStatistics.publicationCount;

		if(previousRecipeHash != 0U && previousRecipeHash != cookedShapeAssets[publication.assetId]->data->recipeHash) {
			bool stillReferenced = false;
			for(const auto& [assetId, assetState] : cookedShapeAssets) {
				(void)assetId;
				if(assetState->data->recipeHash == previousRecipeHash) {
					stillReferenced = true;
					break;
				}
			}
			if(!stillReferenced) {
				// Erasing the cache owner is reader-safe: leases held by prepared jobs
				// and published snapshots keep the immutable data alive.
				cookedShapes.erase(previousRecipeHash);
			}
		}
		return true;
	}

	bool PhysicsWorld::MarkCookedShapeRecookFailed(
		const TerraEngine::UUID& assetId,
		std::string diagnostic) {
		if(assetId == TerraEngine::EMPTY_UUID || diagnostic.empty()) return false;
		constexpr std::size_t kMaximumStaleDiagnosticLength = 1024U;
		if(diagnostic.size() > kMaximumStaleDiagnosticLength) {
			diagnostic.resize(kMaximumStaleDiagnosticLength);
		}

		std::lock_guard<std::mutex> lock(mutex);
		if(!CanAccessRegistriesLocked()) return false;
		const auto found = cookedShapeAssets.find(assetId);
		if(found == cookedShapeAssets.end() || !found->second->data) return false;
		auto stale = std::make_shared<PhysicsCookedShapeAssetState>(*found->second);
		stale->staleLastKnownGood = true;
		stale->diagnostic = std::move(diagnostic);
		++stale->publicationGeneration;
		if(stale->publicationGeneration == 0U) stale->publicationGeneration = 1U;
		found->second = std::move(stale);
		++cookedShapeStatistics.staleRetentionCount;
		return true;
	}

	std::shared_ptr<const PhysicsCookedShapeAssetState> PhysicsWorld::AcquireCookedShape(
		const TerraEngine::UUID& assetId) const {
		std::lock_guard<std::mutex> lock(mutex);
		const auto found = cookedShapeAssets.find(assetId);
		if(!CanAccessRegistriesLocked() || found == cookedShapeAssets.end()) {
			++cookedShapeStatistics.missCount;
			return {};
		}
		++cookedShapeStatistics.hitCount;
		return found->second;
	}

	bool PhysicsWorld::RetireCookedShape(const TerraEngine::UUID& assetId) {
		std::lock_guard<std::mutex> lock(mutex);
		if(!CanAccessRegistriesLocked()) return false;
		const auto found = cookedShapeAssets.find(assetId);
		if(found == cookedShapeAssets.end()) return false;
		const std::uint64_t recipeHash = found->second->data->recipeHash;
		cookedShapeAssets.erase(found);
		bool stillReferenced = false;
		for(const auto& [remainingId, remaining] : cookedShapeAssets) {
			(void)remainingId;
			if(remaining->data->recipeHash == recipeHash) {
				stillReferenced = true;
				break;
			}
		}
		if(!stillReferenced) cookedShapes.erase(recipeHash);
		++cookedShapeStatistics.retirementCount;
		return true;
	}

	PhysicsCookedShapeCacheStatistics PhysicsWorld::CookedShapeCacheStatistics() const {
		std::lock_guard<std::mutex> lock(mutex);
		PhysicsCookedShapeCacheStatistics result = cookedShapeStatistics;
		result.sharedShapeCount = cookedShapes.size();
		result.assetCount = cookedShapeAssets.size();
		result.payloadBytes = 0U;
		for(const auto& [recipeHash, data] : cookedShapes) {
			(void)recipeHash;
			result.payloadBytes += data->payload.size();
		}
		return result;
	}

	PhysicsCommandReceipt PhysicsWorld::EnqueueCommand(PhysicsCommand command) {
		// Queue synchronization is internal, so producers do not need the world
		// lifecycle mutex while a protected teardown stops admission atomically.
		return commandQueue.Enqueue(std::move(command));
	}

	bool PhysicsWorld::CancelCommand(PhysicsCommandId commandId) {
		return commandQueue.Cancel(commandId);
	}

	PhysicsCommandBatch PhysicsWorld::MergeCommands(PhysicsTick inclusiveTick) {
		std::lock_guard<std::mutex> lock(mutex);
		return commandQueue.MergeForProtectedBoundary(inclusiveTick, activeGeneration);
	}

	PhysicsCommandQueueDiagnostics PhysicsWorld::CommandQueueDiagnostics() const {
		return commandQueue.GetDiagnostics();
	}

	bool PhysicsWorld::RequestExplicitPausedStep() {
		std::lock_guard<std::mutex> lock(mutex);
		if(state != PhysicsWorldState::Paused || !explicitPausedStepDurations.empty()) {
			return false;
		}

		// Capturing the duration at request time makes this token coherent even if
		// an editor changes project settings before the scheduler consumes it.
		explicitPausedStepDurations.push_back(settings.fixedTimeStep);
		return true;
	}

	PhysicsFixedStepPlan PhysicsWorld::PlanFixedSteps(float elapsedTime) {
		PhysicsFixedStepPlan plan;
		std::lock_guard<std::mutex> lock(mutex);
		if(!CanAccessRegistriesLocked()) {
			return plan;
		}

		if(state == PhysicsWorldState::Paused) {
			// Paused Next bypasses the accumulator, so old residual frame time cannot
			// turn one user action into zero or several physics ticks.
			if(!explicitPausedStepDurations.empty()) {
				const float duration = explicitPausedStepDurations.front();
				explicitPausedStepDurations.pop_front();
				plan.requests.push_back(IssueStepLocked(duration, 0U, true));
				plan.consumedExplicitPausedStep = true;
			}
			plan.remainingAccumulator = timing.accumulator;
			return plan;
		}

		if(std::isfinite(elapsedTime) && elapsedTime > 0.0F) {
			const float accumulated = timing.accumulator + elapsedTime;
			if(accumulated > settings.maxAccumulatedTime) {
				plan.clampedTime = accumulated - settings.maxAccumulatedTime;
				timing.clampedTime += plan.clampedTime;
				timing.droppedTime += plan.clampedTime;
				timing.accumulator = settings.maxAccumulatedTime;
			} else {
				timing.accumulator = accumulated;
			}
		}

		plan.requests.reserve(settings.maxSubsteps);
		while(plan.normalSubstepCount < settings.maxSubsteps
			&& timing.accumulator >= settings.fixedTimeStep) {
			timing.accumulator -= settings.fixedTimeStep;
			plan.requests.push_back(IssueStepLocked(
				settings.fixedTimeStep, plan.normalSubstepCount, false));
			++plan.normalSubstepCount;
		}

		// The cap bounds one frame's catch-up work. Whole excess ticks are dropped
		// rather than repeatedly delaying all future frames; a fractional remainder
		// is retained so ordinary frame-rate jitter still integrates correctly.
		if(plan.normalSubstepCount == settings.maxSubsteps
			&& timing.accumulator >= settings.fixedTimeStep) {
			const float wholeOverflow = std::floor(timing.accumulator / settings.fixedTimeStep)
				* settings.fixedTimeStep;
			timing.accumulator -= wholeOverflow;
			plan.droppedTime = wholeOverflow;
			timing.droppedTime += wholeOverflow;
		}

		plan.remainingAccumulator = timing.accumulator;
		return plan;
	}

	bool PhysicsWorld::MarkStepCompleted(const PhysicsFixedStepRequest& request) {
		std::lock_guard<std::mutex> lock(mutex);
		if(!request.IsValid() || !CanAccessRegistriesLocked()
			|| request.world != ActiveHandleLocked()
			|| request.tick != timing.completedTick + 1U
			|| request.tick > timing.tick) {
			return false;
		}

		timing.completedTick = request.tick;
		timing.lastCompletedStep = request;
		return true;
	}

	bool PhysicsWorld::MarkFailed() {
		std::lock_guard<std::mutex> lock(mutex);
		if(state != PhysicsWorldState::Creating && state != PhysicsWorldState::Active
			&& state != PhysicsWorldState::Paused) {
			return false;
		}

		// Failure also stops new producers immediately; normal teardown remains
		// responsible for cancellation, job joins, and generation invalidation.
		state = PhysicsWorldState::Failed;
		commandQueue.StopAccepting();
		return true;
	}

	bool PhysicsWorld::BeginStopping() {
		std::lock_guard<std::mutex> lock(mutex);
		if(state != PhysicsWorldState::Creating && state != PhysicsWorldState::Active
			&& state != PhysicsWorldState::Paused && state != PhysicsWorldState::Failed) {
			return false;
		}

		state = PhysicsWorldState::Stopping;
		commandQueue.StopAccepting();
		return true;
	}

	std::size_t PhysicsWorld::BeginDraining() {
		std::lock_guard<std::mutex> lock(mutex);
		if(state != PhysicsWorldState::Stopping) {
			return 0U;
		}

		// Queue cancellation precedes provider/job joins. No command can enter
		// after BeginStopping, so this drain establishes a finite teardown set.
		const std::size_t cancelled = commandQueue.CancelAndDrain();
		state = PhysicsWorldState::Draining;
		return cancelled;
	}

	bool PhysicsWorld::CompleteTeardown() {
		std::lock_guard<std::mutex> lock(mutex);
		if(state != PhysicsWorldState::Draining) {
			return false;
		}

		// The owner must join all jobs and destroy backend records before this point.
		// Clearing every allocator leaves existing public handles tombstoned, and
		// advancing the cursor makes them stale even if a slot is later reused.
		bodyRegistry.Clear();
		shapeRegistry.Clear();
		constraintRegistry.Clear();
		characterRegistry.Clear();
		constraintRuntime.clear();
		characterStateHistory.clear();
		// Cache owners retire at world teardown. Any worker or immutable published
		// snapshot still holding a lease keeps its bytes alive until it finishes.
		cookedShapeAssets.clear();
		cookedShapes.clear();
		cookedShapeStatistics = {};
		if(queryManager != nullptr) queryManager->ResetForWorld({});
		queryManager.reset();
		eventHistory.reset();
		diagnosticEventHistory.reset();
		eventSubscriptions.reset();
		ResetTimingLocked();
		activeGeneration = 0U;
		generationCursor = NextNonzeroGeneration(generationCursor);
		state = PhysicsWorldState::Destroyed;
		return true;
	}

	PhysicsBodyHandle PhysicsWorld::AllocateBody() {
		std::lock_guard<std::mutex> lock(mutex);
		return CanAccessRegistriesLocked() ? bodyRegistry.Allocate(ActiveHandleLocked()) : PhysicsBodyHandle{};
	}

	PhysicsShapeHandle PhysicsWorld::AllocateShape() {
		std::lock_guard<std::mutex> lock(mutex);
		return CanAccessRegistriesLocked() ? shapeRegistry.Allocate(ActiveHandleLocked()) : PhysicsShapeHandle{};
	}

	PhysicsConstraintHandle PhysicsWorld::AllocateConstraint() {
		std::lock_guard<std::mutex> lock(mutex);
		return CanAccessRegistriesLocked() ? constraintRegistry.Allocate(ActiveHandleLocked()) : PhysicsConstraintHandle{};
	}

	PhysicsCharacterHandle PhysicsWorld::AllocateCharacter() {
		std::lock_guard<std::mutex> lock(mutex);
		return CanAccessRegistriesLocked() ? characterRegistry.Allocate(ActiveHandleLocked()) : PhysicsCharacterHandle{};
	}

	bool PhysicsWorld::Release(PhysicsBodyHandle handle) {
		std::lock_guard<std::mutex> lock(mutex);
		return CanAccessRegistriesLocked() && bodyRegistry.Status(handle, ActiveHandleLocked()) == PhysicsHandleStatus::Alive
			&& bodyRegistry.Release(handle);
	}

	bool PhysicsWorld::Release(PhysicsShapeHandle handle) {
		std::lock_guard<std::mutex> lock(mutex);
		return CanAccessRegistriesLocked() && shapeRegistry.Status(handle, ActiveHandleLocked()) == PhysicsHandleStatus::Alive
			&& shapeRegistry.Release(handle);
	}

	bool PhysicsWorld::Release(PhysicsConstraintHandle handle) {
		std::lock_guard<std::mutex> lock(mutex);
		if(!CanAccessRegistriesLocked() || constraintRegistry.Status(handle, ActiveHandleLocked()) != PhysicsHandleStatus::Alive) {
			return false;
		}
		constraintRuntime.erase(RuntimeHandleKey(handle));
		return constraintRegistry.Release(handle);
	}

	bool PhysicsWorld::Release(PhysicsCharacterHandle handle) {
		std::lock_guard<std::mutex> lock(mutex);
		if(!CanAccessRegistriesLocked() || characterRegistry.Status(handle, ActiveHandleLocked()) != PhysicsHandleStatus::Alive) {
			return false;
		}
		const bool released = characterRegistry.Release(handle);
		if(released) {
			// Packets are immutable, so remove only packets that mention the released
			// handle; unrelated character observations retain their normal lifetime.
			characterStateHistory.erase(std::remove_if(characterStateHistory.begin(), characterStateHistory.end(),
				[handle](const auto& packet) {
					return packet == nullptr || std::any_of(packet->states->begin(), packet->states->end(),
						[handle](const PhysicsCharacterState& stateValue) { return stateValue.handle == handle; });
				}), characterStateHistory.end());
		}
		return released;
	}

	PhysicsHandleStatus PhysicsWorld::Status(PhysicsBodyHandle handle) const {
		std::lock_guard<std::mutex> lock(mutex);
		return bodyRegistry.Status(handle, ActiveHandleLocked());
	}

	PhysicsHandleStatus PhysicsWorld::Status(PhysicsShapeHandle handle) const {
		std::lock_guard<std::mutex> lock(mutex);
		return shapeRegistry.Status(handle, ActiveHandleLocked());
	}

	PhysicsHandleStatus PhysicsWorld::Status(PhysicsConstraintHandle handle) const {
		std::lock_guard<std::mutex> lock(mutex);
		return constraintRegistry.Status(handle, ActiveHandleLocked());
	}

	PhysicsHandleStatus PhysicsWorld::Status(PhysicsCharacterHandle handle) const {
		std::lock_guard<std::mutex> lock(mutex);
		return characterRegistry.Status(handle, ActiveHandleLocked());
	}

	PhysicsGeneration PhysicsWorld::NextNonzeroGeneration(PhysicsGeneration generation) noexcept {
		++generation;
		return generation == 0U ? 1U : generation;
	}

	bool PhysicsWorld::CanAccessRegistriesLocked() const noexcept {
		return state == PhysicsWorldState::Active || state == PhysicsWorldState::Paused;
	}

	PhysicsWorldHandle PhysicsWorld::ActiveHandleLocked() const noexcept {
		return activeGeneration != 0U ? PhysicsWorldHandle{worldId, activeGeneration} : PhysicsWorldHandle{};
	}

	PhysicsFixedStepRequest PhysicsWorld::IssueStepLocked(
		float duration,
		std::uint32_t substepIndex,
		bool explicitPausedStep) {
		++timing.tick;
		if(timing.tick == 0U) {
			// Tick zero is reserved as invalid. Wrap is practically unreachable, but
			// retaining the invariant avoids silently issuing an invalid request.
			++timing.tick;
		}
		return PhysicsFixedStepRequest{ActiveHandleLocked(), timing.tick, duration, substepIndex, explicitPausedStep};
	}

	void PhysicsWorld::ResetTimingLocked() noexcept {
		timing = {};
		explicitPausedStepDurations.clear();
	}
}
