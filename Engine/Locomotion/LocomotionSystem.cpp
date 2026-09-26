// LocomotionSystem.cpp - lifecycle reconciliation at protected frame boundaries.

#include "LocomotionSystem.h"
#include "LocomotionPreparation.h"
#include "LocomotionNavigationBindings.h"
#include "LocomotionAnimationBindings.h"
#include "Animation/AnimationSystem.h"
#include "Animation/Runtime/AnimationConsumerRegistry.h"

#include "Components/CharacterLocomotionComponent.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Navigation/Agents/NavigationAgentSystem.h"
#include "Core/Object/ObjectManager.h"
#include "Physics/PhysicsSystem.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace TerraEngine::Locomotion {
	namespace {
		class MovementExclusionGuard final : public Physics::CharacterMotionValidity {
		public:
			Animation::AnimatorInstanceHandle animator;
			std::shared_ptr<const Physics::CharacterMotionValidity> navigation;
			[[nodiscard]] Physics::CharacterMotionValidityReason Validate() const noexcept override {
				const auto exclusions = Animation::AnimationSystem::Instance().ReadRagdollExclusions();
				if(exclusions && std::any_of(exclusions->entries.begin(), exclusions->entries.end(),
					[this](const auto& entry) { return entry.animator == animator; }))
					return Physics::CharacterMotionValidityReason::StaleEvidence;
				return navigation ? navigation->Validate() : Physics::CharacterMotionValidityReason::Valid;
			}
		};
		[[nodiscard]] Physics::CharacterMotionOwner PhysicalOwner(LocomotionHandle handle) {
			return {handle.sceneLifetime, handle.objectId, handle.objectGeneration,
				handle.attachmentGeneration, handle.slotIndex, handle.slotGeneration};
		}

		// Compare typed authoring values without serializing JSON in the frame loop.
		[[nodiscard]] bool SameConfiguration(const CharacterLocomotionComponent& left,
			const CharacterLocomotionComponent& right) {
			return left.active == right.active && left.controllerBinding == right.controllerBinding
				&& left.navigationBinding == right.navigationBinding && left.animatorBinding == right.animatorBinding
				&& left.animatorObjectId == right.animatorObjectId && left.maximumSpeed == right.maximumSpeed
				&& left.acceleration == right.acceleration && left.braking == right.braking
				&& left.turnRate == right.turnRate && left.facing == right.facing
				&& left.initialMotionMode == right.initialMotionMode && left.speedParameter == right.speedParameter
				&& left.localDirectionXParameter == right.localDirectionXParameter
				&& left.localDirectionZParameter == right.localDirectionZParameter
				&& left.groundedParameter == right.groundedParameter && left.movingParameter == right.movingParameter;
		}
	}

	struct LocomotionSystem::Impl {
		static constexpr std::size_t kShardCount = 8U;
		struct CapturedCandidate {
			LocomotionPreparationInput input;
			std::shared_ptr<const Physics::CharacterMotionValidity> navigationValidity;
			std::uint64_t producer = 0U;
			std::uint64_t sequence = 0U;
			bool pendingIntent = false;
		};
		struct Binding {
			PublishedLocomotionBinding published;
			CharacterLocomotionComponent configuration;
			ResolvedLocomotionBindings resolved;
			CapturedLocomotionAnimator animator;
			std::uint64_t animatorBindingGeneration = 1U;
			PreparedMotionParameters lastParameters;
			bool parametersSubmitted = false;
			PreparedMotionParameters pendingParameters;
			std::array<Animation::AnimationCommand, kMotionParameterCount> pendingParameterCommands;
			std::size_t pendingParameterCount = 0U;
		};
		// This store survives Initialize/Shutdown so slot generations cannot reset
		// and accidentally authorize a wrapper retained across a runtime restart.
		LocomotionCommandStore store;
		mutable std::mutex mutex;
		std::unordered_map<UUID, Binding> bindings;
		bool initialized = false;
		std::uint64_t updateEpoch = 0U;
		std::uint64_t parameterSequence = 1U;
		std::uint64_t animatorPublication = 1U;
		LocomotionDiagnostics diagnostics;
		std::vector<LocomotionParameterTraceRecord> parameterTrace;
		std::size_t parameterTraceCount = 0U;
		std::size_t parameterTraceNext = 0U;
		std::uint64_t parameterTraceSequence = 0U;
		// Capture owns vector allocation. Each worker writes a disjoint range of
		// prepared values; Publish reads only after all eight dependency edges.
		std::vector<CapturedCandidate> captured;
		std::vector<PreparedLocomotionMotion> prepared;
		std::array<std::size_t, kMaximumLiveSlots> baseInputs{};
		std::array<std::vector<std::size_t>, kMaximumLiveSlots> intentInputs;

		[[nodiscard]] bool Excluded(Animation::AnimatorInstanceHandle animator) const {
			const auto exclusions = Animation::AnimationSystem::Instance().ReadRagdollExclusions();
			return exclusions && std::any_of(exclusions->entries.begin(), exclusions->entries.end(),
				[animator](const auto& entry) { return entry.animator == animator; });
		}

		// Build all storage before physical ownership is changed. A prepared
		// logical authority root is safe to inspect while the store is reserved.
		[[nodiscard]] std::shared_ptr<Animation::AnimationRootControllerBindingSnapshot> BuildAnimatorTable(
			const LocomotionAuthorityRoot& authority,
			std::span<const Physics::CharacterMotionLeaseChange> proposed = {}) {
			auto table = std::make_shared<Animation::AnimationRootControllerBindingSnapshot>();
			table->publicationGeneration = animatorPublication;
			table->active.reserve(bindings.size());
			for(const auto& entry : authority.entries) {
				const auto found = bindings.find(entry.handle.objectId);
				if(found == bindings.end() || found->second.published.handle != entry.handle) continue;
				const auto& binding = found->second;
				if(!binding.animator.animator.IsValid()) continue;
				auto lease = Physics::PhysicsSystem::Instance().ReadCharacterMotionLease(entry.controller);
				for(const auto& change : proposed) if(change.next.controller == entry.controller) lease = change.next;
				if(!lease) continue;
				Animation::AnimationRootControllerBinding row;
				row.animator = binding.animator.animator;
				row.animatorObjectId = binding.animator.objectId;
				row.animatorObjectGeneration = binding.animator.objectGeneration;
				row.animatorComponentGeneration = binding.animator.componentGeneration;
				row.controller = binding.resolved.controller;
				row.owner = PhysicalOwner(entry.handle);
				row.authorityEpoch = lease->authorityEpoch;
				row.motionEpoch = lease->motionEpoch;
				row.bindingGeneration = binding.animatorBindingGeneration;
				row.frame = binding.resolved.frame;
				row.mode = entry.mode == MotionMode::RootMotion ? Physics::CharacterMotionMode::RootMotion : Physics::CharacterMotionMode::Velocity;
				row.locomotionOwned = entry.ownerToken != 0U && lease->state == Physics::CharacterMotionLeaseState::LocomotionOwned
					&& lease->owner == row.owner;
				row.rootMotionEnabled = row.locomotionOwned && entry.state == AuthorityState::Controlled
					&& entry.mode == MotionMode::RootMotion && binding.animator.rootReady && !Excluded(row.animator);
				row.capturedRelativeBasisRotation = binding.animator.relativeRotation;
				row.capturedRelativeBasisScale = binding.animator.relativeScale;
				row.capturedRelativeTranslation = binding.animator.relativeTranslation;
				row.maximumRootSpeed = static_cast<float>(binding.configuration.maximumSpeed);
				row.maximumYawRate = static_cast<float>(binding.configuration.turnRate);
				row.validityGuard = binding.animator.validity;
				row.bindingValidityGuard = binding.animator.bindingValidity;
				const auto object = ObjectManager::instance().getObject(entry.handle.objectId);
				const auto* controller = object ? ObjectManager::instance().GetComponent<CharacterControllerComponent>(*object) : nullptr;
				row.facingLocked = controller && controller->rotationPolicy == Physics::PhysicsCharacterRotationPolicy::Locked;
				table->active.push_back(std::move(row));
			}
			// Owned entries precede idle aliases; protected acquisition rejects two
			// owners of one animator. Ordering makes lookup stable across hash rehash.
			std::sort(table->active.begin(), table->active.end(), [](const auto& a, const auto& b) {
				if(a.animator.slotIndex != b.animator.slotIndex) return a.animator.slotIndex < b.animator.slotIndex;
				if(a.animator.generation != b.animator.generation) return a.animator.generation < b.animator.generation;
				if(a.locomotionOwned != b.locomotionOwned) return a.locomotionOwned;
				return a.owner.slotIndex < b.owner.slotIndex;
			});
			return table;
		}

		[[nodiscard]] Animation::AnimationConsumerRegistry::PreparedRootMotionConsumers PrepareConsumers(
			const Animation::AnimationRootControllerBindingSnapshot& table) {
			std::vector<Animation::AnimatorInstanceHandle> active;
			for(const auto& row : table.active) if(row.rootMotionEnabled) active.push_back(row.animator);
			return Animation::AnimationConsumerRegistry::Instance().PrepareRootMotionConsumers(active,
				static_cast<Animation::AnimationGeneration>(table.publicationGeneration), 0);
		}

		void CommitAnimatorTable(std::shared_ptr<const Animation::AnimationRootControllerBindingSnapshot> table,
			Animation::AnimationConsumerRegistry::PreparedRootMotionConsumers&& consumers) {
			Animation::AnimationConsumerRegistry::Instance().CommitRootMotionConsumers(std::move(consumers));
			Animation::AnimationSystem::Instance().PublishRootControllerBindings(std::move(table));
			if(animatorPublication < std::numeric_limits<Animation::AnimationGeneration>::max()) ++animatorPublication;
		}

		void RefreshAnimatorTable() {
			const auto root = store.CopyAuthorityRoot();
			try {
				auto table = BuildAnimatorTable(*root);
				auto consumers = PrepareConsumers(*table);
				if(consumers.Status() == Animation::AnimationStatus::Ok) CommitAnimatorTable(std::move(table), std::move(consumers));
			} catch(const std::bad_alloc&) {
				// Retain the guarded table: removing it would reopen legacy Transform
				// root fallback for an already leased animator. Current lease/asset
				// guards reject obsolete entries, and no new owner is admitted here.
			}
		}

		void SuspendExcluded() {
			auto& animation = Animation::AnimationSystem::Instance();
			auto& physics = Physics::PhysicsSystem::Instance();
			const auto table = animation.ReadRootControllerBindings();
			const auto exclusions = animation.ReadRagdollExclusions();
			if(table && exclusions) (void)Animation::ApplyRagdollControllerExclusions(*table, *exclusions, physics);
			for(const auto& [id, binding] : bindings) {
				(void)id;
				const auto lease = physics.ReadCharacterMotionLease(binding.resolved.controller);
				if(!lease) continue;
				const auto record = store.Read(binding.published.handle);
				if(lease->state == Physics::CharacterMotionLeaseState::ExternalExclusive
					&& record && (record->authority.ownerToken != 0U || record->authority.state != AuthorityState::Suspended))
					(void)store.Revoke(binding.published.handle, LocomotionReason::ExternalExclusive);
				else if(lease->state == Physics::CharacterMotionLeaseState::Unleased && record
					&& record->authority.state == AuthorityState::Suspended
					&& record->authority.suspendedReason == LocomotionReason::ExternalExclusive
					&& !Excluded(binding.animator.animator)) {
					// Only Physics' explicit recovery/retirement grant ends suspension.
					// Publish a fresh Idle epoch, never resurrect the previous token or
					// queued intent. Callers must observe it and explicitly Acquire again.
					(void)store.Revoke(binding.published.handle, LocomotionReason::None);
				}
			}
		}

		[[nodiscard]] bool ReleasePhysical(const Binding& binding) {
			auto& physics = Physics::PhysicsSystem::Instance();
			const auto lease = physics.ReadCharacterMotionLease(binding.resolved.controller);
			if(!lease || lease->state != Physics::CharacterMotionLeaseState::LocomotionOwned
				|| lease->owner != PhysicalOwner(binding.published.handle)) return true;
			// ExternalExclusive is deliberately untouched: removing a locomotion
			// component cannot release an active ragdoll's physical exclusion.
			Physics::CharacterMotionLeaseChange change{*lease, *lease};
			change.next.state = Physics::CharacterMotionLeaseState::Unleased;
			change.next.owner = {};
			if(change.next.motionEpoch == std::numeric_limits<std::uint64_t>::max()
				|| change.next.authorityEpoch == std::numeric_limits<std::uint64_t>::max()
				|| change.next.publicationEpoch == std::numeric_limits<std::uint64_t>::max()) {
				Physics::CharacterMotionTarget stop;
				stop.controller = lease->controller;
				stop.owner = lease->owner;
				stop.authorityEpoch = lease->authorityEpoch;
				stop.motionEpoch = lease->motionEpoch;
				stop.mode = lease->mode;
				stop.frame = binding.resolved.frame;
				// Exhausted epochs cannot authorize a new owner. Seal this last
				// lifetime against every older prepared target while clearing it.
				stop.locomotionUpdateEpoch = std::numeric_limits<std::uint64_t>::max();
				stop.clearImmediately = true;
				return physics.PublishCharacterMotionTarget(stop).Succeeded();
			}
			++change.next.authorityEpoch;
			++change.next.motionEpoch;
			++change.next.publicationEpoch;
			return physics.PublishCharacterMotionLeases(std::span(&change, 1U)).accepted;
		}

		void Clear(LocomotionReason reason) {
			for(auto iterator = bindings.begin(); iterator != bindings.end();) {
				const auto& binding = iterator->second;
				// Never discard the only retry identity while a physical clear failed.
				// Protected boundaries retry retained entries before further use.
				if(!ReleasePhysical(binding)) { ++iterator; continue; }
				(void)store.Revoke(binding.published.handle, reason);
				(void)store.ReleaseSlot(binding.published.handle);
				iterator = bindings.erase(iterator);
			}
			(void)store.PublishObservations({});
			// Clear only this feature's overlay, never unrelated Animation consumers.
			if(bindings.empty()) {
				Animation::AnimationConsumerRegistry::Instance().ClearRootMotionConsumers();
				Animation::AnimationSystem::Instance().PublishRootControllerBindings({});
			} else RefreshAnimatorTable();
			diagnostics.liveSlots = store.LiveCount();
			diagnostics.pendingCommands = store.PendingCount();
			++diagnostics.publicationEpoch;
		}

		// Reconcile on the coordinator only. Physics packets are immutable, but ECS
		// membership and authored values require this declared protected boundary.
		void Reconcile() {
			auto& physics = Physics::PhysicsSystem::Instance();
			const auto world = physics.WorldHandle();
			if(!world.IsValid()) { if(!bindings.empty()) Clear(LocomotionReason::NotPlaying); return; }
			auto& objects = ObjectManager::instance();
			const auto packet = physics.LatestCharacterStatePacket();
			std::unordered_map<UUID, const Physics::PhysicsCharacterState*> characters;
			if(packet && packet->world == world && packet->states) {
				characters.reserve(packet->states->size());
				for(const auto& state : *packet->states) characters.emplace(state.objectId, &state);
			}
			// Character packets belong to the primary backend. FrameDescriptors()
			// enumerates additional islands only, not this implicit primary frame.
			const Physics::PhysicsFrameHandle frame{Physics::kPrimaryPhysicsFrameId, world.worldGeneration};
			for(auto iterator = bindings.begin(); iterator != bindings.end();) {
				const auto object = objects.getObject(iterator->first);
				const auto& handle = iterator->second.published.handle;
				const auto* component = object ? objects.getRegistry().try_get<CharacterLocomotionComponent>(object->EntityId()) : nullptr;
				const auto* controller = object ? objects.getRegistry().try_get<CharacterControllerComponent>(object->EntityId()) : nullptr;
				const auto currentCharacter = characters.find(iterator->first);
				// A surviving ECS component is not proof that its old provider handle
				// still exists. Replacement/omitted physical state invalidates the slot
				// itself, so an old wrapper cannot bind a newly created controller.
				const bool currentPhysicalIdentity = currentCharacter != characters.end()
					&& currentCharacter->second->handle == iterator->second.resolved.controller.character
					&& currentCharacter->second->objectGeneration == handle.objectGeneration
					&& currentCharacter->second->componentGeneration == handle.attachmentGeneration;
				const bool alive = object && handle.sceneLifetime == objects.GetSceneGeneration()
					&& handle.objectGeneration == objects.getObjectLifetimeGeneration(*object)
					&& handle.attachmentGeneration == objects.getObjectComponentGeneration(*object)
					&& component && component->active && component->ValidateAuthoredValues()
					&& controller && controller->active && controller->enabled && controller->Validate().accepted
					&& objects.isObjectActiveInHierarchy(*object)
					&& iterator->second.resolved.controller.world == world && currentPhysicalIdentity;
				if(!alive) {
					if(!ReleasePhysical(iterator->second)) { ++iterator; continue; }
					(void)store.Revoke(handle, LocomotionReason::StaleHandle);
					(void)store.ReleaseSlot(handle);
					iterator = bindings.erase(iterator);
				} else ++iterator;
			}
			for(const auto entity : objects.getRegistry().view<CharacterLocomotionComponent, CharacterControllerComponent>()) {
				const auto object = objects.getObject(entity);
				if(!object || !objects.isObjectActiveInHierarchy(*object)) continue;
				const auto& component = objects.getRegistry().get<CharacterLocomotionComponent>(entity);
				const auto& controller = objects.getRegistry().get<CharacterControllerComponent>(entity);
				if(!component.active || !component.ValidateAuthoredValues() || !controller.active
					|| !controller.enabled || !controller.Validate().accepted) continue;
				const UUID id = objects.getUUID(*object);
				const auto foundCharacter = characters.find(id);
				if(foundCharacter == characters.end()) continue;
				const auto& state = *foundCharacter->second;
				if(state.objectGeneration != objects.getObjectLifetimeGeneration(*object)
					|| state.componentGeneration != objects.getObjectComponentGeneration(*object)) continue;
				const auto existing = bindings.find(id);
				ResolvedLocomotionBindings resolved;
				resolved.configurationRevision = existing == bindings.end() ? 1U : existing->second.resolved.configurationRevision;
				resolved.controller = {state.world, state.handle, state.objectId, state.objectGeneration, state.componentGeneration};
				resolved.frame = frame;
				resolved.controllerComponentGeneration = state.componentGeneration;
				resolved.controllerSupported = true;
				resolved.maximumSpeed = component.maximumSpeed;
				LocomotionHandle animatorOwner;
				animatorOwner.sceneLifetime = objects.GetSceneGeneration();
				animatorOwner.objectId = id;
				animatorOwner.objectGeneration = state.objectGeneration;
				animatorOwner.attachmentGeneration = state.componentGeneration;
				if(existing != bindings.end()) animatorOwner = existing->second.published.handle;
				auto animator = CaptureLocomotionAnimator(animatorOwner, component,
					existing == bindings.end() ? nullptr : &existing->second.animator);
				resolved.animatorObjectId = animator.objectId;
				resolved.animatorAttachmentGeneration = animator.componentGeneration;
				resolved.animatorControllerGeneration = animator.controller.generation;
				resolved.rootSourceGeneration = animator.clip.generation;
				resolved.animationRootReady = animator.rootReady;
				if(component.navigationBinding == CharacterLocomotionComponent::kSelfBinding) {
					const auto agent = Navigation::NavigationAgentSystem::Instance().ResolvePublishedAgent(id);
					if(agent) {
						resolved.navigationObjectId = id;
						resolved.navigationAttachmentGeneration = agent->attachmentGeneration;
						resolved.navigationConfigurationRevision = Navigation::NavigationAgentSystem::Instance().ReadGuidance(*agent).configurationRevision;
					}
				}
				resolved.initialMotionMode = component.initialMotionMode == CharacterLocomotionComponent::kRootMotionMode
					? MotionMode::RootMotion : MotionMode::Velocity;
				if(existing != bindings.end()) {
					const auto& priorAnimator = existing->second.animator;
					const bool animatorChanged = priorAnimator.animator != animator.animator
						|| priorAnimator.controller != animator.controller || priorAnimator.clip != animator.clip
						|| priorAnimator.componentGeneration != animator.componentGeneration
						|| priorAnimator.rootReady != animator.rootReady;
					if(animatorChanged) {
						const auto record = store.Read(existing->second.published.handle);
						if(record && record->authority.ownerToken != 0U && record->authority.mode == MotionMode::RootMotion
							&& (!animator.rootReady || priorAnimator.animator != animator.animator
								|| priorAnimator.controller != animator.controller)) {
							if(!ReleasePhysical(existing->second)) continue;
							(void)store.Revoke(existing->second.published.handle, LocomotionReason::AnimationUnavailable);
						}
						if(existing->second.animatorBindingGeneration == std::numeric_limits<std::uint64_t>::max()) continue;
						++existing->second.animatorBindingGeneration;
						existing->second.parametersSubmitted = false;
						existing->second.pendingParameterCount = 0U;
					}
					if(!SameConfiguration(existing->second.configuration, component)) {
						if(!ReleasePhysical(existing->second)) continue;
						(void)store.Revoke(existing->second.published.handle, LocomotionReason::StaleConfiguration);
						if(resolved.configurationRevision == std::numeric_limits<std::uint64_t>::max()) continue;
						++resolved.configurationRevision;
						if(store.UpdateBinding(existing->second.published.handle, resolved)) {
							existing->second.configuration = component;
							existing->second.resolved = resolved;
						}
					}
					else if((existing->second.resolved.navigationObjectId != resolved.navigationObjectId
						|| existing->second.resolved.navigationAttachmentGeneration != resolved.navigationAttachmentGeneration
						|| existing->second.resolved.navigationConfigurationRevision != resolved.navigationConfigurationRevision || animatorChanged)
						&& store.UpdateBinding(existing->second.published.handle, resolved)) {
						// Refresh optional copied evidence without revoking manual authority.
						existing->second.resolved = resolved;
					}
					existing->second.animator = std::move(animator);
					continue;
				}
				LocomotionHandle identity;
				identity.sceneLifetime = objects.GetSceneGeneration();
				identity.objectId = id;
				identity.objectGeneration = state.objectGeneration;
				identity.attachmentGeneration = state.componentGeneration;
				const auto bound = store.Bind(identity, resolved);
				if(bound.Succeeded()) bindings.emplace(id, Binding{{static_cast<std::uint32_t>(entity), bound.handle}, component, resolved, std::move(animator)});
			}
			diagnostics.liveSlots = store.LiveCount();
			diagnostics.pendingCommands = store.PendingCount();
		}

		void Capture() {
			std::lock_guard lock(mutex);
			captured.clear();
			prepared.clear();
			baseInputs.fill(std::numeric_limits<std::size_t>::max());
			for(auto& indexes : intentInputs) indexes.clear();
			if(!initialized || updateEpoch == std::numeric_limits<std::uint64_t>::max()) return;
			++updateEpoch;
			Reconcile();
			store.ExpireOneUpdateIntents(updateEpoch);
			const auto records = store.SnapshotLive();
			const auto commands = store.SnapshotEligibleCommands(updateEpoch);
			auto& physics = Physics::PhysicsSystem::Instance();
			const auto packet = physics.LatestCharacterStatePacket();
			if(!packet || !packet->states) return;
			std::unordered_map<UUID, const Physics::PhysicsCharacterState*> states;
			for(const auto& state : *packet->states) states.emplace(state.objectId, &state);
			captured.reserve(records.size() + commands.size());
			for(const auto& record : records) {
				const auto binding = bindings.find(record.handle.objectId);
				const auto state = states.find(record.handle.objectId);
				if(binding == bindings.end() || state == states.end()) continue;
				CapturedCandidate candidate;
				candidate.input.runtime = record;
				candidate.input.physical = *state->second;
				candidate.input.frame = binding->second.resolved.frame;
				candidate.input.acceleration = binding->second.configuration.acceleration;
				candidate.input.braking = binding->second.configuration.braking;
				candidate.input.turnRate = binding->second.configuration.turnRate;
				candidate.input.facing = binding->second.configuration.facing;
				candidate.input.locomotionUpdateEpoch = updateEpoch;
				const auto object = ObjectManager::instance().getObject(record.handle.objectId);
				const auto* controller = object ? ObjectManager::instance().GetComponent<CharacterControllerComponent>(*object) : nullptr;
				if(!controller) continue;
				candidate.input.controllerUp = {controller->upDirection.x, controller->upDirection.y, controller->upDirection.z};
				if(controller->rotationPolicy == Physics::PhysicsCharacterRotationPolicy::Locked)
					candidate.input.facing = CharacterLocomotionComponent::kFacingHold;
				const auto lease = physics.ReadCharacterMotionLease(record.bindings.controller);
				candidate.input.physicalMotionEpoch = lease ? lease->motionEpoch : 0U;
				candidate.navigationValidity = CaptureNavigationBinding(candidate.input);
				candidate.producer = record.authority.ownerProducerId;
				candidate.sequence = record.intent ? record.intent->sequence : 0U;
				baseInputs[record.handle.slotIndex] = captured.size();
				captured.push_back(std::move(candidate));
			}
			// Direction preparation is independent of command acceptance. Rejected
			// candidates are never selected, and later producers miss this cutoff.
			for(const auto& command : commands) {
				if(command.kind != CommandKind::SetIntent && command.kind != CommandKind::FollowNavigation) continue;
				if(command.handle.slotIndex >= baseInputs.size()) continue;
				const auto base = baseInputs[command.handle.slotIndex];
				if(base == std::numeric_limits<std::size_t>::max()
					|| captured[base].input.runtime.handle != command.handle) continue;
				auto candidate = captured[base];
				candidate.input.candidateIntent = MovementIntent{command.ownerToken,
					command.kind == CommandKind::FollowNavigation ? IntentKind::Navigation : command.intentKind,
					command.sequence, command.directionSpace, command.direction, command.requestedSpeed,
					command.lifetime, updateEpoch};
				candidate.pendingIntent = true;
				candidate.producer = command.producerId;
				candidate.sequence = command.sequence;
				intentInputs[command.handle.slotIndex].push_back(captured.size());
				captured.push_back(std::move(candidate));
			}
			prepared.resize(captured.size());
			diagnostics.preparedCharacters = records.size();
			diagnostics.activeShards = std::min(kShardCount, captured.size());
		}

		void PrepareShard(std::size_t shard) {
			const auto begin = captured.size() * shard / kShardCount;
			const auto end = captured.size() * (shard + 1U) / kShardCount;
			for(auto index = begin; index < end; ++index)
				prepared[index] = PrepareLocomotionMotion(captured[index].input);
		}

		void AdmitCommands() {
			auto& physics = Physics::PhysicsSystem::Instance();
			while(auto batch = store.PrepareNextBatch(updateEpoch, [this, &physics](const auto& command, const auto& record) {
				const auto binding = bindings.find(record.handle.objectId);
				if(binding == bindings.end() || binding->second.published.handle != record.handle)
					return LocomotionReason::StaleHandle;
				const auto lease = physics.ReadCharacterMotionLease(record.bindings.controller);
				if(!lease) return LocomotionReason::StaleHandle;
				if(lease->state == Physics::CharacterMotionLeaseState::ExternalExclusive)
					return LocomotionReason::ExternalExclusive;
				if(binding->second.animator.animator.IsValid() && Excluded(binding->second.animator.animator))
					return LocomotionReason::ExternalExclusive;
				// Reject unsupported procedural facing before ownership changes, so
				// a failed acquisition cannot leave a controller leased but unusable.
				if(command.kind == CommandKind::Acquire || command.kind == CommandKind::SetMode
					|| command.kind == CommandKind::SetIntent) {
					const auto object = ObjectManager::instance().getObject(record.handle.objectId);
					const auto* controller = object ? ObjectManager::instance().GetComponent<CharacterControllerComponent>(*object) : nullptr;
					if(!controller) return LocomotionReason::MissingBinding;
					if((command.kind == CommandKind::Acquire || command.kind == CommandKind::SetMode)
						&& command.mode == MotionMode::RootMotion
						&& (controller->rotationPolicy == Physics::PhysicsCharacterRotationPolicy::Locked
							|| !controller->ValidateLocomotionFacing().accepted)) return LocomotionReason::UnsupportedController;
					if(binding->second.configuration.facing == CharacterLocomotionComponent::kFacingMovement
						&& controller->rotationPolicy != Physics::PhysicsCharacterRotationPolicy::Locked
						&& !controller->ValidateLocomotionFacing().accepted) return LocomotionReason::UnsupportedController;
					if(binding->second.configuration.facing == CharacterLocomotionComponent::kFacingMovement
						&& controller->rotationPolicy != Physics::PhysicsCharacterRotationPolicy::Locked) {
						const auto base = baseInputs[record.handle.slotIndex];
						if(base >= captured.size()) return LocomotionReason::MissingBinding;
						const auto q = captured[base].input.physical.pose.rotation;
						// Rotated local +Y must remain +Y. Normalization makes this check
						// insensitive to harmless copied quaternion roundoff.
						const double length = double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z + double(q.w) * q.w;
						if(!std::isfinite(length) || length <= 1.0e-12
							|| std::abs(2.0 * (double(q.x) * q.y - double(q.w) * q.z) / length) > 0.001
							|| std::abs(2.0 * (double(q.y) * q.z + double(q.w) * q.x) / length) > 0.001
							|| 2.0 * (double(q.x) * q.x + double(q.z) * q.z) / length > 0.001)
							return LocomotionReason::UnsupportedController;
					}
				}
				return LocomotionReason::None;
			})) {
				if(!batch->accepted) {
					(void)store.RejectPreparedBatch(std::move(*batch), batch->reason);
					continue;
				}
				std::vector<Physics::CharacterMotionLeaseChange> changes;
				LocomotionReason rejected = LocomotionReason::None;
				for(const auto& transition : batch->transitions) {
					// Resuming a stopped owner changes state but not its physical epoch.
					if(transition.previous.ownerToken == transition.next.ownerToken
						&& transition.previous.authorityEpoch == transition.next.authorityEpoch
						&& transition.previous.mode == transition.next.mode) continue;
					const auto current = physics.ReadCharacterMotionLease(transition.next.controller);
					if(!current) { rejected = LocomotionReason::StaleHandle; break; }
					if(current->motionEpoch == std::numeric_limits<std::uint64_t>::max()
						|| current->publicationEpoch == std::numeric_limits<std::uint64_t>::max()) {
						rejected = LocomotionReason::CapacityExceeded; break;
					}
					if(current->state == Physics::CharacterMotionLeaseState::LocomotionOwned
						&& current->owner != PhysicalOwner(transition.previous.handle)) {
						rejected = LocomotionReason::OwnershipConflict; break;
					}
					Physics::CharacterMotionLeaseChange change{*current, *current};
					change.next.authorityEpoch = transition.next.authorityEpoch;
					++change.next.motionEpoch;
					++change.next.publicationEpoch;
					change.next.owner = transition.next.ownerToken ? PhysicalOwner(transition.next.handle) : Physics::CharacterMotionOwner{};
					change.next.state = transition.next.ownerToken ? Physics::CharacterMotionLeaseState::LocomotionOwned
						: Physics::CharacterMotionLeaseState::Unleased;
					change.next.mode = transition.next.mode == MotionMode::Velocity
						? Physics::CharacterMotionMode::Velocity : Physics::CharacterMotionMode::RootMotion;
					changes.push_back(change);
				}
				std::shared_ptr<Animation::AnimationRootControllerBindingSnapshot> animatorTable;
				Animation::AnimationConsumerRegistry::PreparedRootMotionConsumers consumers;
				if(rejected == LocomotionReason::None) {
					try {
						animatorTable = BuildAnimatorTable(*batch->nextRoot, changes);
						for(std::size_t index = 1U; index < animatorTable->active.size(); ++index) {
							const auto& a = animatorTable->active[index - 1U];
							const auto& b = animatorTable->active[index];
							if(a.animator == b.animator && a.locomotionOwned && b.locomotionOwned) {
								rejected = LocomotionReason::OwnershipConflict;
								break;
							}
						}
						if(rejected == LocomotionReason::None) {
							consumers = PrepareConsumers(*animatorTable);
							if(consumers.Status() != Animation::AnimationStatus::Ok) rejected = LocomotionReason::CapacityExceeded;
						}
					} catch(const std::bad_alloc&) { rejected = LocomotionReason::CapacityExceeded; }
				}
				if(rejected == LocomotionReason::None && !changes.empty()) {
					const auto result = physics.PublishCharacterMotionLeases(changes);
					if(!result.accepted) rejected = result.error == Physics::CharacterMotionLeaseError::StaleCharacter
						? LocomotionReason::StaleHandle : LocomotionReason::OwnershipConflict;
				}
				// The store reservation spans physical preflight/publication and this
				// allocation-free logical swap: no producer can interleave a handoff.
				if(rejected != LocomotionReason::None) (void)store.RejectPreparedBatch(std::move(*batch), rejected);
				else {
					CommitAnimatorTable(std::move(animatorTable), std::move(consumers));
					(void)store.CommitPreparedBatch(std::move(*batch));
				}
			}
		}

		void PublishParameters(Binding& binding, LocomotionObservation& observation) {
			observation.animationReason = binding.animator.reason;
			observation.animationParameterError = binding.animator.mapping.error;
			observation.animationParameterField = binding.animator.mapping.failedField;
			if(!binding.animator.animator.IsValid()) return;
			// Admission is not publication. Keep one batch in flight per actor and
			// promote its value cache only after every exact receipt is successful.
			// A rejected/expired batch retries from the last confirmed values.
			if(binding.pendingParameterCount != 0U) {
				bool complete = true;
				bool succeeded = true;
				for(std::size_t index = 0U; index < binding.pendingParameterCount; ++index) {
					const auto& command = binding.pendingParameterCommands[index];
					const auto receipt = Animation::AnimationSystem::Instance().ReadCommandReceipt(
						command.producerId, command.producerSequence, command.correlationId);
					complete = complete && receipt.status != Animation::AnimationStatus::Queued;
					succeeded = succeeded && receipt.status == Animation::AnimationStatus::Ok;
				}
				if(!complete) return;
				if(succeeded) {
					binding.lastParameters = std::move(binding.pendingParameters);
					binding.parametersSubmitted = true;
				} else binding.parametersSubmitted = false;
				binding.pendingParameterCount = 0U;
			}
			if(parameterSequence > std::numeric_limits<std::uint64_t>::max() - kMotionParameterCount) {
				observation.animationReason = LocomotionReason::CapacityExceeded;
				return;
			}
			// One process-wide producer sequence range covers every actor, because
			// Animation's replay ledger is keyed by producer rather than animator.
			auto parameters = PrepareMotionParameters(binding.animator.mapping, observation, 0x4C4F434FU, parameterSequence);
			if(parameters.error != MotionParameterError::None) {
				observation.animationReason = LocomotionReason::AnimationUnavailable;
				observation.animationParameterError = parameters.error;
				observation.animationParameterField = parameters.failedField;
				return;
			}
			std::array<Animation::AnimationCommand, kMotionParameterCount> changed;
			std::size_t count = 0U;
			for(std::size_t index = 0U; index < parameters.count; ++index) {
				auto& command = parameters.commands[index];
				// Parameter IDs belong to this exact controller publication, not just
				// the animator slot which may survive a controller asset reload.
				command.expectedController = binding.animator.controller;
				const bool unchanged = binding.parametersSubmitted
					&& std::any_of(binding.lastParameters.commands.begin(), binding.lastParameters.commands.begin() + binding.lastParameters.count,
						[&command](const auto& old) { return old.target == command.target && old.parameterId == command.parameterId
							&& old.parameterType == command.parameterType && old.value == command.value; });
				if(!unchanged) changed[count++] = command;
			}
			if(count == 0U) return;
			parameterSequence += kMotionParameterCount;
			const auto receipt = Animation::AnimationSystem::Instance().SubmitCommandBatch(std::span(changed.data(), count));
			if(receipt.status == Animation::AnimationStatus::Queued) {
				if(!parameterTrace.empty() && parameterTraceSequence != std::numeric_limits<std::uint64_t>::max()) {
					// Preallocated diagnostic storage captures the exact admitted batch,
					// not a later snapshot which could merge or omit parameter changes.
					auto& trace = parameterTrace[parameterTraceNext];
					trace = {++parameterTraceSequence, updateEpoch,
						static_cast<Physics::PhysicsTick>(Physics::PhysicsSystem::Instance().CurrentFixedTick()),
						binding.animator.objectId, static_cast<std::uint32_t>(count), changed};
					parameterTraceNext = (parameterTraceNext + 1U) % parameterTrace.size();
					parameterTraceCount = std::min(parameterTraceCount + 1U, parameterTrace.size());
				}
				binding.pendingParameters = std::move(parameters);
				binding.pendingParameterCommands = std::move(changed);
				binding.pendingParameterCount = count;
			} else observation.animationReason = receipt.status == Animation::AnimationStatus::StaleHandle
				? LocomotionReason::AnimationUnavailable : LocomotionReason::CapacityExceeded;
		}

		void Publish() {
			const auto started = std::chrono::steady_clock::now();
			std::lock_guard lock(mutex);
			if(!initialized || updateEpoch == 0U) return;
			Reconcile();
			RefreshAnimatorTable();
			SuspendExcluded();
			AdmitCommands();
			auto& physics = Physics::PhysicsSystem::Instance();
			std::vector<LocomotionObservation> observations;
			for(const auto& record : store.SnapshotLive()) {
				const auto base = baseInputs[record.handle.slotIndex];
				if(base >= captured.size() || captured[base].input.runtime.handle != record.handle
					|| captured[base].input.runtime.bindings.configurationRevision != record.bindings.configurationRevision) continue;
				auto selected = base;
				if(record.intent) {
					for(const auto index : intentInputs[record.handle.slotIndex])
						if(captured[index].producer == record.authority.ownerProducerId
							&& captured[index].sequence == record.intent->sequence) selected = index;
				}
				auto output = prepared[selected];
				if(record.intent && record.intent->kind == IntentKind::Navigation) {
					output.target.validityGuard = captured[selected].navigationValidity;
					if(output.target.validityGuard
						&& output.target.validityGuard->Validate() != Physics::CharacterMotionValidityReason::Valid) {
						output.reason = LocomotionReason::StaleConfiguration;
						output.observation.navigationReason = output.reason;
						output.observation.navigationState = NavigationMotionState::Waiting;
						output.target.validityGuard.reset();
					}
				}
				output.observation.authority = record.authority;
				output.observation.handle = record.handle;
				if(output.reason != LocomotionReason::None) output.observation.bindingReason = output.reason;
				const auto liveBinding = bindings.find(record.handle.objectId);
				if(liveBinding != bindings.end() && liveBinding->second.animator.animator.IsValid()) {
					auto guard = std::make_shared<MovementExclusionGuard>();
					guard->animator = liveBinding->second.animator.animator;
					guard->navigation = output.target.validityGuard;
					output.target.validityGuard = std::move(guard);
					if(Excluded(liveBinding->second.animator.animator)) {
						output.reason = LocomotionReason::ExternalExclusive;
						output.observation.bindingReason = output.reason;
					}
				}
				const auto lease = physics.ReadCharacterMotionLease(record.bindings.controller);
				if(lease && record.authority.ownerToken != 0U && lease->owner == PhysicalOwner(record.handle)
					&& lease->state == Physics::CharacterMotionLeaseState::LocomotionOwned) {
					auto& target = output.target;
					target.controller = record.bindings.controller;
					target.owner = lease->owner;
					target.authorityEpoch = lease->authorityEpoch;
					target.motionEpoch = lease->motionEpoch;
					target.mode = lease->mode;
					target.frame = record.bindings.frame;
					target.locomotionUpdateEpoch = updateEpoch;
					if(!record.intent || record.authority.state != AuthorityState::Controlled
						|| output.reason != LocomotionReason::None) {
						target.planarVelocity = {};
						target.hasFacing = false;
					}
					target.clearImmediately = record.authority.state == AuthorityState::Stopped
						|| output.reason != LocomotionReason::None;
					// Root intervals alone move/turn a root-owned character. Ordinary
					// targets are used in this mode only for an explicit safety clear.
					const auto publication = target.mode == Physics::CharacterMotionMode::Velocity || target.clearImmediately
						? physics.PublishCharacterMotionTarget(target) : Physics::Backend::PhysicsBackendResult{};
					if(!publication.Succeeded()) {
						++diagnostics.rejectedTargets;
						diagnostics.lastTargetFailure = publication.message.substr(0U, 256U);
						output.observation.bindingReason = publication.error == Physics::Backend::PhysicsBackendErrorCode::UnsupportedFrame
							? LocomotionReason::UnsupportedFrame : publication.error == Physics::Backend::PhysicsBackendErrorCode::OwnershipConflict
							? LocomotionReason::OwnershipConflict : publication.error == Physics::Backend::PhysicsBackendErrorCode::StaleHandle
							? LocomotionReason::StaleHandle : LocomotionReason::UnsupportedController;
						// A rejected new target must never leave yesterday's contribution.
						target.hasFacing = false;
						target.planarVelocity = {};
						target.clearImmediately = true;
						target.validityGuard.reset();
						(void)physics.PublishCharacterMotionTarget(target);
					}
					output.observation.desiredVelocity = {target.planarVelocity.x, target.planarVelocity.y, target.planarVelocity.z};
				} else output.observation.desiredVelocity = {};
				const auto binding = bindings.find(record.handle.objectId);
				if(binding != bindings.end()) PublishParameters(binding->second, output.observation);
				observations.push_back(std::move(output.observation));
			}
			(void)store.PublishObservations(observations);
			++diagnostics.publicationEpoch;
			diagnostics.pendingCommands = store.PendingCount();
			diagnostics.lastPublishMicroseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - started).count());
		}
	};

	LocomotionSystem::LocomotionSystem() : impl_(std::make_unique<Impl>()) {}
	LocomotionSystem::~LocomotionSystem() = default;
	LocomotionSystem& LocomotionSystem::Instance() { static LocomotionSystem system; return system; }
	void LocomotionSystem::Initialize() { std::lock_guard lock(impl_->mutex); impl_->initialized = true; }
	void LocomotionSystem::Shutdown() {
		std::lock_guard lock(impl_->mutex);
		impl_->Clear(LocomotionReason::Revoked);
		impl_->initialized = false;
	}
	void LocomotionSystem::RevokeAll(LocomotionReason reason) { std::lock_guard lock(impl_->mutex); impl_->Clear(reason); }
	void LocomotionSystem::ObserveProtectedBoundary() {
		std::lock_guard lock(impl_->mutex);
		if(impl_->initialized) impl_->Reconcile();
	}
	void LocomotionSystem::Update(float) {
		// Direct test/runtime callers follow the same capture/prepare/publish order.
		// Scheduled frames run these preparation ranges on independent worker jobs.
		impl_->Capture();
		for(std::size_t shard = 0U; shard < Impl::kShardCount; ++shard) impl_->PrepareShard(shard);
		impl_->Publish();
	}
	std::string LocomotionSystem::SystemName() { return "LocomotionSystem"; }
	Scheduling::SystemDeclaration LocomotionSystem::SchedulerDeclaration() {
		using namespace Scheduling;
		SystemDeclaration declaration;
		declaration.name = SystemName();
		declaration.phase = "Locomotion";
		declaration.after = {"ScriptingSystem"};
		declaration.before = {"AnimationSystem", "PhysicsSystem"};
		declaration.reads = {kResourceSceneChangePublication, kResourceLocomotionComponents,
			"Resources", "SceneGraph", "ObjectManager", kResourcePhysicsCharacterMotionLeases,
			kResourceLocomotionCommands, kResourcePhysicsCharacterStatePackets,
			kResourceNavigationAgentState, kResourceNavigationAgentGuidance, kResourceNavigationRouteValidity,
			kResourceAnimationAuthorityFrame, kResourceAnimationRuntimeState, kResourceAnimationRagdollMappings};
		declaration.writes = {kResourceLocomotionAuthority, kResourceLocomotionObservations,
			kResourceLocomotionReceipts, kResourceCharacterMotionTargets, kResourcePhysicsCharacterMotionLeases,
			kResourceLocomotionAnimatorBindings, kResourceAnimationConsumerMasks, kResourceAnimationCommandQueues,
			"PhysicsWorld", kResourceRootMotionIntervals, kResourceRootMotionResults};
		declaration.mainThread = true;
		declaration.phaseKind = WorkerSystemPhaseKind::ProtectedCommit;
		declaration.hardBoundary = true;
		declaration.boundaryClassification = HardBoundaryClassification::Protected;
		declaration.safeSubwork = "Eight independent copied-input direction and completed-tick feedback preparation shards.";
		declaration.protectedCommit = "Capture live bindings; preflight and publish atomic leases, receipts, typed targets and observations.";
		declaration.protectedReason = "Only the coordinator may inspect live ECS and change physical ownership.";
		declaration.requiresFlushedSceneMutations = true;
		declaration.omitWhenNoFrameWorkloads = true;
		declaration.profilingLabel = "Locomotion";
		declaration.usesDefaultMetadata = false;
		return declaration;
	}
	std::vector<Scheduling::FrameWorkloadDeclaration> LocomotionSystem::DeclareFrameWorkloads(float) {
		using namespace Scheduling;
		std::lock_guard lock(impl_->mutex);
		if(!impl_->initialized || (!Physics::PhysicsSystem::Instance().WorldHandle().IsValid() && impl_->bindings.empty())) return {};
		// With no authored or retained attachment there is nothing to capture.
		// This read is coordinator-only, before any worker callback is dispatched.
		const auto authored = ObjectManager::instance().getRegistry().view<CharacterLocomotionComponent>();
		if(impl_->bindings.empty() && authored.begin() == authored.end()) return {};
		auto capture = MakeProtectedCommitPhase("LocomotionCapture", SystemName(),
			{kResourceSceneChangePublication, kResourceLocomotionComponents, kResourceLocomotionCommands, kResourcePhysicsCharacterStatePackets,
				"Resources", "SceneGraph", "ObjectManager", kResourcePhysicsCharacterMotionLeases,
				kResourceNavigationAgentState, kResourceNavigationAgentGuidance, kResourceNavigationRouteValidity,
				kResourceAnimationAuthorityFrame, kResourceAnimationRuntimeState, kResourceAnimationRagdollMappings},
			{kResourceLocomotionCaptureInputs, kResourceLocomotionAuthority, kResourceLocomotionReceipts,
				kResourceCharacterMotionTargets, kResourcePhysicsCharacterMotionLeases, kResourceLocomotionAnimatorBindings,
				kResourceAnimationConsumerMasks, "PhysicsWorld", kResourceRootMotionIntervals, kResourceRootMotionResults},
			"Live component binding and physical lease revocation require protected ownership.", "LocomotionCapture");
		capture.phase = "Locomotion";
		capture.stableDescriptorId = "LocomotionSystem/Locomotion/Capture";
		capture.work = [this](JobGraphContext&) { impl_->Capture(); };
		std::vector<FrameWorkloadDeclaration> workloads;
		workloads.push_back(std::move(capture));
		const std::array<const char*, Impl::kShardCount> shards{kResourceLocomotionPrepared0,
			kResourceLocomotionPrepared1, kResourceLocomotionPrepared2, kResourceLocomotionPrepared3,
			kResourceLocomotionPrepared4, kResourceLocomotionPrepared5, kResourceLocomotionPrepared6, kResourceLocomotionPrepared7};
		std::vector<std::string> reads{kResourceLocomotionCaptureInputs, kResourceLocomotionCommands,
			"Resources", "SceneGraph", "ObjectManager", kResourcePhysicsCharacterMotionLeases,
			kResourceLocomotionComponents, kResourcePhysicsCharacterStatePackets,
			kResourceNavigationAgentState, kResourceNavigationAgentGuidance, kResourceNavigationRouteValidity,
			kResourceAnimationAuthorityFrame, kResourceAnimationRuntimeState, kResourceAnimationRagdollMappings};
		std::vector<std::string> dependencies;
		for(std::size_t shard = 0U; shard < Impl::kShardCount; ++shard) {
			auto prepare = MakeWorkerPreparePhase("LocomotionPrepare", SystemName(),
				{kResourceLocomotionCaptureInputs}, {shards[shard]}, "LocomotionPrepare");
			prepare.phase = "Locomotion";
			prepare.stableDescriptorId = "LocomotionSystem/Locomotion/Prepare/" + std::to_string(shard);
			prepare.explicitDependencies = {"LocomotionSystem/Locomotion/Capture"};
			prepare.estimatedWorkItemCount = std::max<std::size_t>(1U, impl_->bindings.size() / Impl::kShardCount);
			prepare.work = [this, shard](JobGraphContext&) { impl_->PrepareShard(shard); };
			dependencies.push_back(prepare.stableDescriptorId);
			reads.emplace_back(shards[shard]);
			workloads.push_back(std::move(prepare));
		}
		auto publish = MakeProtectedCommitPhase("LocomotionPublish", SystemName(), std::move(reads),
			{kResourceLocomotionAuthority, kResourceLocomotionObservations, kResourceLocomotionReceipts,
				kResourceCharacterMotionTargets, kResourcePhysicsCharacterMotionLeases, kResourceLocomotionAnimatorBindings,
				kResourceAnimationConsumerMasks, kResourceAnimationCommandQueues,
				"PhysicsWorld", kResourceRootMotionIntervals, kResourceRootMotionResults},
			"Current attachment and physical lease checks precede atomic command publication.", "LocomotionPublish");
		publish.phase = "Locomotion";
		publish.hardBoundary = true;
		publish.stableDescriptorId = "LocomotionSystem/Locomotion/Publish";
		publish.explicitDependencies = std::move(dependencies);
		publish.work = [this](JobGraphContext&) { impl_->Publish(); };
		workloads.push_back(std::move(publish));
		return workloads;
	}
	std::optional<LocomotionHandle> LocomotionSystem::ResolvePublished(std::uint32_t entity) const {
		std::lock_guard lock(impl_->mutex);
		for(const auto& [id, binding] : impl_->bindings) {
			(void)id;
			if(binding.published.entity == entity) return binding.published.handle;
		}
		return std::nullopt;
	}
	std::optional<LocomotionHandle> LocomotionSystem::ResolvePublished(UUID id) const {
		std::lock_guard lock(impl_->mutex);
		const auto found = impl_->bindings.find(id);
		return found == impl_->bindings.end() ? std::nullopt : std::optional(found->second.published.handle);
	}
	std::optional<PublishedLocomotionResolution> LocomotionSystem::ResolvePublishedSnapshot(UUID id) const {
		// Keep binding lookup and authority preconditions within the same protected
		// read. An observation packet may intentionally lag this authority root.
		std::lock_guard lock(impl_->mutex);
		const auto found = impl_->bindings.find(id);
		if(found == impl_->bindings.end()) return std::nullopt;
		const auto record = impl_->store.Read(found->second.published.handle);
		if(!record) return std::nullopt;
		return PublishedLocomotionResolution{record->handle, record->bindingReason,
			record->authority.configurationRevision, record->authority.authorityEpoch};
	}
	CommandBatchAdmission LocomotionSystem::SubmitBatch(std::span<const LocomotionCommand> commands) {
		std::lock_guard lock(impl_->mutex);
		if(!impl_->initialized || !Physics::PhysicsSystem::Instance().WorldHandle().IsValid())
			return {false, LocomotionReason::NotPlaying, {}};
		if(impl_->updateEpoch == std::numeric_limits<std::uint64_t>::max())
			return {false, LocomotionReason::CapacityExceeded, {}};
		return impl_->store.SubmitBatch(commands, impl_->updateEpoch + 1U);
	}
	CommandReceipt LocomotionSystem::ReadReceipt(LocomotionHandle handle, std::uint64_t producer,
		std::uint64_t sequence, std::uint64_t correlation) const {
		return impl_->store.ReadReceipt(handle, producer, sequence, correlation);
	}
	std::shared_ptr<const LocomotionObservationRoot> LocomotionSystem::CopyObservations() const { return impl_->store.CopyObservationRoot(); }
	std::optional<CommandReceipt> LocomotionSystem::ReadCurrentReceipt(LocomotionHandle handle, std::uint64_t producer,
		std::uint64_t sequence, std::uint64_t correlation) const {
		std::lock_guard lock(impl_->mutex);
		const auto found = impl_->bindings.find(handle.objectId);
		if(found == impl_->bindings.end() || found->second.published.handle != handle || !impl_->store.Read(handle))
			return std::nullopt;
		return impl_->store.ReadReceipt(handle, producer, sequence, correlation);
	}
	PublishedLocomotionObservationSnapshot LocomotionSystem::CopyObservationsForHandle(LocomotionHandle handle) const {
		std::lock_guard lock(impl_->mutex);
		const auto found = impl_->bindings.find(handle.objectId);
		if(found == impl_->bindings.end() || found->second.published.handle != handle || !impl_->store.Read(handle))
			return {};
		return {true, impl_->store.CopyObservationRoot()};
	}
	LocomotionDiagnostics LocomotionSystem::ReadDiagnostics() const { std::lock_guard lock(impl_->mutex); return impl_->diagnostics; }
	bool LocomotionSystem::ConfigureParameterTrace(bool enabled, std::size_t capacity) {
		if(enabled && (capacity == 0U || capacity > 16384U)) return false;
		std::lock_guard lock(impl_->mutex);
		try {
			std::vector<LocomotionParameterTraceRecord> next(enabled ? capacity : 0U);
			impl_->parameterTrace.swap(next);
		} catch(const std::bad_alloc&) { return false; }
		impl_->parameterTraceCount = 0U;
		impl_->parameterTraceNext = 0U;
		return true;
	}
	LocomotionParameterTraceSnapshot LocomotionSystem::CopyParameterTrace(std::uint64_t afterSequence) const {
		std::lock_guard lock(impl_->mutex);
		LocomotionParameterTraceSnapshot result;
		result.enabled = !impl_->parameterTrace.empty();
		result.latestSequence = impl_->parameterTraceSequence;
		result.reservedBytes = impl_->parameterTrace.capacity() * sizeof(LocomotionParameterTraceRecord);
		const auto sequence = impl_->parameterTraceSequence;
		const auto count = impl_->parameterTraceCount;
		const auto earliest = count == 0U ? sequence + (sequence != std::numeric_limits<std::uint64_t>::max()) : sequence - count + 1U;
		result.lostRecords = afterSequence > sequence || (afterSequence < earliest && earliest - afterSequence > 1U)
			|| sequence == std::numeric_limits<std::uint64_t>::max();
		result.records.reserve(count);
		for(std::size_t index = 0U; index < count; ++index) {
			const auto& record = impl_->parameterTrace[(impl_->parameterTraceNext + impl_->parameterTrace.size() - count + index) % impl_->parameterTrace.size()];
			if(record.sequence > afterSequence) result.records.push_back(record);
		}
		return result;
	}
}
