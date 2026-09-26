// LocomotionCommands.cpp - bounded command storage and atomic authority staging.

#include "LocomotionCommands.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace TerraEngine::Locomotion {
	namespace {
		std::atomic_uint32_t nextStoreIncarnation{1U};
		[[nodiscard]] std::size_t Mix(std::size_t seed, std::size_t value) noexcept {
			return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U));
		}

		[[nodiscard]] LocomotionReason ValidateShape(const LocomotionCommand& command) noexcept {
			if(!command.handle.IsValid() || command.producerId == 0U || command.sequence == 0U
				|| command.correlationId == 0U) return LocomotionReason::InvalidArgument;
			if(static_cast<unsigned>(command.kind) > static_cast<unsigned>(CommandKind::Release)
				|| static_cast<unsigned>(command.intentKind) > static_cast<unsigned>(IntentKind::Navigation)
				|| static_cast<unsigned>(command.directionSpace) > static_cast<unsigned>(DirectionSpace::ControllerLocal)
				|| static_cast<unsigned>(command.lifetime) > static_cast<unsigned>(IntentLifetime::UntilStopped)
				|| static_cast<unsigned>(command.mode) > static_cast<unsigned>(MotionMode::RootMotion))
				return LocomotionReason::InvalidArgument;
			if(!std::isfinite(command.direction.x) || !std::isfinite(command.direction.y)
				|| !std::isfinite(command.direction.z) || !std::isfinite(command.requestedSpeed)
				|| command.requestedSpeed < 0.0) return LocomotionReason::InvalidArgument;
			if(command.kind == CommandKind::Acquire && command.ownerToken != 0U)
				return LocomotionReason::InvalidArgument;
			if(command.kind != CommandKind::Acquire && command.ownerToken == 0U)
				return LocomotionReason::InvalidArgument;
			if(command.kind == CommandKind::Transfer && command.transferProducerId == 0U)
				return LocomotionReason::InvalidArgument;
			// Player input must be refreshed every movement update. Only Script
			// (and navigation's separate guidance path) may persist until stopped.
			if(command.kind == CommandKind::SetIntent
				&& command.intentKind == IntentKind::Player
				&& command.lifetime != IntentLifetime::OneUpdate)
				return LocomotionReason::InvalidArgument;
			return LocomotionReason::None;
		}

		[[nodiscard]] bool SameCommand(const LocomotionCommand& a, const LocomotionCommand& b) noexcept {
			return a.kind == b.kind && a.handle == b.handle
				&& a.expectedAuthorityEpoch == b.expectedAuthorityEpoch
				&& a.expectedConfigurationRevision == b.expectedConfigurationRevision
				&& a.ownerToken == b.ownerToken && a.producerId == b.producerId
				&& a.sequence == b.sequence && a.correlationId == b.correlationId
				&& a.intentKind == b.intentKind && a.directionSpace == b.directionSpace
				&& a.direction.x == b.direction.x && a.direction.y == b.direction.y
				&& a.direction.z == b.direction.z && a.requestedSpeed == b.requestedSpeed
				&& a.lifetime == b.lifetime && a.mode == b.mode
				&& a.transferProducerId == b.transferProducerId;
		}

		[[nodiscard]] CommandReceipt MakeReceipt(const LocomotionCommand& command,
			ReceiptState state, LocomotionReason reason, std::uint64_t epoch = 0U) noexcept {
			return {command.handle, command.producerId, command.sequence, command.correlationId,
				state, reason, epoch, 0U, 0U};
		}

		[[nodiscard]] bool SameController(const Physics::PhysicsCharacterCommandTarget& a,
			const Physics::PhysicsCharacterCommandTarget& b) noexcept {
			return a.world == b.world && a.character == b.character
				&& a.objectId == b.objectId && a.objectGeneration == b.objectGeneration
				&& a.componentGeneration == b.componentGeneration;
		}
	}

	std::size_t LocomotionCommandStore::ReceiptKeyHash::operator()(const ReceiptKey& key) const noexcept {
		std::size_t hash = std::hash<std::uint64_t>{}(key.handle.sceneLifetime);
		for(const auto byte : key.handle.objectId) hash = Mix(hash, byte);
		hash = Mix(hash, key.handle.objectGeneration);
		hash = Mix(hash, key.handle.attachmentGeneration);
		hash = Mix(hash, key.handle.slotIndex);
		hash = Mix(hash, key.handle.slotGeneration);
		hash = Mix(hash, key.producerId);
		return Mix(hash, key.sequence);
	}

	std::size_t LocomotionCommandStore::ControllerKeyHash::operator()(const ControllerKey& key) const noexcept {
		std::size_t hash = std::hash<std::uint64_t>{}(key.worldId);
		hash = Mix(hash, key.worldGeneration);
		hash = Mix(hash, key.slotIndex);
		return Mix(hash, key.slotGeneration);
	}

	LocomotionCommandStore::LocomotionCommandStore()
		: authorityRoot_(std::make_shared<LocomotionAuthorityRoot>()),
		observationRoot_(std::make_shared<LocomotionObservationRoot>()) {
		const auto incarnation = nextStoreIncarnation.fetch_add(1U, std::memory_order_relaxed);
		storeGenerationPrefix_ = incarnation == 0U || incarnation > 0xFFFFU ? 0U : incarnation << 16U;
		slots_.reserve(kMaximumLiveSlots);
		freeSlots_.reserve(kMaximumLiveSlots);
		controllerSlots_.reserve(kMaximumLiveSlots);
		receiptIndex_.reserve(kMaximumRetainedReceipts);
	}

	bool LocomotionCommandStore::Matches(LocomotionHandle handle) const noexcept {
		return handle.IsValid() && handle.slotIndex < slots_.size()
			&& slots_[handle.slotIndex].alive
			&& slots_[handle.slotIndex].generation == handle.slotGeneration
			&& slots_[handle.slotIndex].record.handle == handle;
	}

	BindResult LocomotionCommandStore::Bind(LocomotionHandle identity, ResolvedLocomotionBindings bindings) {
		if(storeGenerationPrefix_ == 0U || identity.sceneLifetime == 0U || identity.objectId == EMPTY_UUID
			|| identity.objectGeneration == 0U || identity.attachmentGeneration == 0U
			|| bindings.configurationRevision == 0U || !std::isfinite(bindings.maximumSpeed)
			|| bindings.maximumSpeed < 0.0 || bindings.maximumSpeed > 100.0
			|| (bindings.initialMotionMode != MotionMode::Velocity
				&& bindings.initialMotionMode != MotionMode::RootMotion)
			|| !bindings.controller.world.IsValid()
			|| !bindings.controller.character.IsValid() || !bindings.controllerSupported
			|| bindings.controller.objectId != identity.objectId
			|| bindings.controller.objectGeneration != identity.objectGeneration
			|| bindings.controller.world.worldId != bindings.controller.character.worldId
			|| bindings.controller.world.worldGeneration != bindings.controller.character.worldGeneration)
			return {{}, LocomotionReason::MissingBinding};
		std::lock_guard lock(mutex_);
		const ControllerKey key{bindings.controller.character.worldId.value,
			bindings.controller.character.worldGeneration, bindings.controller.character.slotIndex,
			bindings.controller.character.slotGeneration};
		if(controllerSlots_.contains(key)) return {{}, LocomotionReason::OwnershipConflict};
		std::uint32_t index = kInvalidLocomotionSlot;
		while(!freeSlots_.empty()) {
			const auto candidate = freeSlots_.back();
			freeSlots_.pop_back();
			if((slots_[candidate].generation & 0xFFFFU) != 0xFFFFU) {
				index = candidate;
				break;
			}
		}
		if(index == kInvalidLocomotionSlot) {
			if(slots_.size() >= kMaximumLiveSlots) return {{}, LocomotionReason::CapacityExceeded};
			index = static_cast<std::uint32_t>(slots_.size());
			slots_.push_back({});
		}
		Slot& slot = slots_[index];
		if(slot.generation == 0U) slot.generation = storeGenerationPrefix_;
		++slot.generation;
		slot.alive = true;
		identity.slotIndex = index;
		identity.slotGeneration = slot.generation;
		slot.record = {};
		slot.record.handle = identity;
		slot.record.bindings = bindings;
		slot.record.authority.handle = identity;
		slot.record.authority.controller = bindings.controller;
		slot.record.authority.mode = bindings.initialMotionMode;
		slot.record.authority.configurationRevision = bindings.configurationRevision;
		slot.producers.clear();
		slot.safetyStop.reset();
		slot.pendingCount = 0U;
		controllerSlots_.emplace(key, index);
		authorityRoot_ = BuildAuthorityRoot({});
		++revision_;
		return {identity, LocomotionReason::None};
	}

	bool LocomotionCommandStore::UpdateBinding(LocomotionHandle handle, ResolvedLocomotionBindings bindings) {
		std::lock_guard lock(mutex_);
		if(!Matches(handle) || bindings.configurationRevision == 0U
			|| !std::isfinite(bindings.maximumSpeed) || bindings.maximumSpeed < 0.0
			|| bindings.maximumSpeed > 100.0
			|| (bindings.initialMotionMode != MotionMode::Velocity
				&& bindings.initialMotionMode != MotionMode::RootMotion)
			|| !SameController(slots_[handle.slotIndex].record.bindings.controller, bindings.controller)) return false;
		Slot& slot = slots_[handle.slotIndex];
		if(slot.record.bindings.configurationRevision != bindings.configurationRevision
			|| !bindings.controllerSupported) {
			// The parent publishes the matching physical revocation before updating
			// this copied binding at the same protected boundary.
			slot.record.authority.ownerToken = 0U;
			slot.record.authority.state = AuthorityState::Idle;
			slot.record.intent.reset();
			if(slot.record.authority.authorityEpoch != std::numeric_limits<std::uint64_t>::max())
				++slot.record.authority.authorityEpoch;
		}
		slot.record.bindings = bindings;
		if(slot.record.authority.state == AuthorityState::Idle)
			slot.record.authority.mode = bindings.initialMotionMode;
		slot.record.authority.configurationRevision = bindings.configurationRevision;
		authorityRoot_ = BuildAuthorityRoot({});
		++revision_;
		return true;
	}

	std::optional<LocomotionRuntimeRecord> LocomotionCommandStore::Read(LocomotionHandle handle) const {
		std::lock_guard lock(mutex_);
		if(!Matches(handle)) return {};
		return slots_[handle.slotIndex].record;
	}

	std::vector<LocomotionRuntimeRecord> LocomotionCommandStore::SnapshotLive() const {
		std::lock_guard lock(mutex_);
		std::vector<LocomotionRuntimeRecord> result;
		result.reserve(slots_.size());
		for(const Slot& slot : slots_) if(slot.alive) result.push_back(slot.record);
		return result;
	}

	std::vector<LocomotionCommand> LocomotionCommandStore::SnapshotEligibleCommands(
		std::uint64_t updateEpoch) const {
		std::lock_guard lock(mutex_);
		std::vector<LocomotionCommand> result;
		result.reserve(std::min<std::size_t>(pendingCount_ + slots_.size(),
			kMaximumQueuedCommands + kMaximumLiveSlots));
		// Safety Stop has admission priority even when an older ordinary batch
		// fills the queue; copy it first for shard preparation and diagnostics.
		for(const Slot& slot : slots_)
			if(slot.alive && slot.safetyStop && slot.safetyEarliestUpdateEpoch <= updateEpoch)
				result.push_back(*slot.safetyStop);
		for(const PendingBatch& batch : pending_)
			if(batch.earliestUpdateEpoch <= updateEpoch)
				result.insert(result.end(), batch.commands.begin(), batch.commands.end());
		return result;
	}

	void LocomotionCommandStore::RetainReceipt(const LocomotionCommand& command, CommandReceipt receipt) {
		if(receiptRing_.size() == kMaximumRetainedReceipts) {
			// Pending receipts cannot be evicted: an eventual Acquire result might
			// carry the only owner token the caller can use. The queue/safety bounds
			// leave room for these while terminal history rotates.
			const auto old = *firstTerminalReceipt_;
			receiptIndex_.erase({old->command.handle, old->command.producerId, old->command.sequence});
			const auto next = receiptRing_.erase(old);
			--terminalReceiptCount_;
			if(terminalReceiptCount_ == 0U) firstTerminalReceipt_.reset();
			else firstTerminalReceipt_ = next;
		}
		std::list<RetainedReceipt>::iterator it;
		if(receipt.state == ReceiptState::Queued) {
			receiptRing_.push_front({command, receipt});
			it = receiptRing_.begin();
		} else {
			receiptRing_.push_back({command, receipt});
			it = std::prev(receiptRing_.end());
			if(!firstTerminalReceipt_) firstTerminalReceipt_ = it;
			++terminalReceiptCount_;
		}
		receiptIndex_.insert_or_assign({command.handle, command.producerId, command.sequence}, it);
	}

	void LocomotionCommandStore::CompleteReceipt(const CommandReceipt& receipt) noexcept {
		const auto found = receiptIndex_.find({receipt.handle, receipt.producerId, receipt.sequence});
		if(found != receiptIndex_.end() && found->second->receipt.correlationId == receipt.correlationId) {
			const bool wasQueued = found->second->receipt.state == ReceiptState::Queued;
			found->second->receipt = receipt;
			if(wasQueued && receipt.state != ReceiptState::Queued) {
				// Splice preserves the indexed iterator and allocates nothing after a
				// physical transition was accepted.
				receiptRing_.splice(receiptRing_.end(), receiptRing_, found->second);
				if(!firstTerminalReceipt_) firstTerminalReceipt_ = found->second;
				++terminalReceiptCount_;
			}
		}
	}

	CommandBatchAdmission LocomotionCommandStore::SubmitBatch(
		std::span<const LocomotionCommand> commands, std::uint64_t earliestUpdateEpoch) {
		CommandBatchAdmission admission{};
		if(commands.empty() || commands.size() > kMaximumCommandBatch || earliestUpdateEpoch == 0U) {
			admission.reason = LocomotionReason::InvalidArgument;
			return admission;
		}
		std::lock_guard lock(mutex_);
		admission.receipts.reserve(commands.size());
		LocomotionReason rejection = LocomotionReason::None;
		bool anyDuplicate = false;
		bool anyFresh = false;
		std::unordered_map<std::uint32_t, std::size_t> actorIncrements;
		std::unordered_map<std::uint32_t, std::unordered_map<std::uint64_t, std::uint64_t>> proposedHighWater;
		for(const auto& command : commands) {
			LocomotionReason reason = ValidateShape(command);
			if(reason == LocomotionReason::None && !Matches(command.handle)) reason = LocomotionReason::StaleHandle;
			if(reason != LocomotionReason::None) { rejection = reason; break; }
			const auto key = ReceiptKey{command.handle, command.producerId, command.sequence};
			if(const auto existing = receiptIndex_.find(key); existing != receiptIndex_.end()) {
				if(!SameCommand(existing->second->command, command)) { rejection = LocomotionReason::SequenceConflict; break; }
				anyDuplicate = true;
				admission.receipts.push_back(existing->second->receipt);
				continue;
			}
			anyFresh = true;
			Slot& slot = slots_[command.handle.slotIndex];
			const auto producer = slot.producers.find(command.producerId);
			auto& proposedForActor = proposedHighWater[command.handle.slotIndex];
			if(producer == slot.producers.end() && !proposedForActor.contains(command.producerId)) {
				std::size_t newProducers = 0U;
				for(const auto& [proposedId, sequence] : proposedForActor) {
					(void)sequence;
					if(!slot.producers.contains(proposedId)) ++newProducers;
				}
				if(slot.producers.size() + newProducers >= kMaximumProducersPerActor) {
					rejection = LocomotionReason::CapacityExceeded; break;
				}
			}
			const std::uint64_t prior = proposedForActor.contains(command.producerId)
				? proposedForActor[command.producerId]
				: producer == slot.producers.end() ? 0U : producer->second.sequence;
			if(command.sequence <= prior) { rejection = LocomotionReason::ExpiredReceipt; break; }
			proposedForActor[command.producerId] = command.sequence;
			++actorIncrements[command.handle.slotIndex];
			admission.receipts.push_back(MakeReceipt(command, ReceiptState::Queued,
				LocomotionReason::None, earliestUpdateEpoch));
		}
		if(rejection == LocomotionReason::None && anyDuplicate && anyFresh)
			rejection = LocomotionReason::SequenceConflict;
		const bool reservedSafetyStop = commands.size() == 1U && commands.front().kind == CommandKind::Stop;
		if(rejection == LocomotionReason::None && anyFresh) {
			if(reservedSafetyStop) {
				const Slot& slot = slots_[commands.front().handle.slotIndex];
				const auto& current = slot.record.authority;
				const auto& stop = commands.front();
				if(current.ownerToken == 0U || current.ownerToken != stop.ownerToken
					|| current.ownerProducerId != stop.producerId
					|| current.authorityEpoch != stop.expectedAuthorityEpoch
					|| slot.record.bindings.configurationRevision != stop.expectedConfigurationRevision
					|| current.state == AuthorityState::Suspended)
					rejection = LocomotionReason::OwnershipConflict;
			}
			const std::size_t availableReceiptSlots = kMaximumRetainedReceipts - receiptRing_.size()
				+ terminalReceiptCount_ + (reservedSafetyStop
					&& slots_[commands.front().handle.slotIndex].safetyStop ? 1U : 0U);
			if(availableReceiptSlots < commands.size()) rejection = LocomotionReason::CapacityExceeded;
			if(!reservedSafetyStop && pendingCount_ + commands.size() > kMaximumQueuedCommands)
				rejection = LocomotionReason::CapacityExceeded;
			if(!reservedSafetyStop)
				for(const auto& [index, increment] : actorIncrements)
					if(slots_[index].pendingCount + increment > kMaximumCommandsPerActor)
						rejection = LocomotionReason::CapacityExceeded;
		}
		if(rejection != LocomotionReason::None) {
			admission.reason = rejection;
			admission.receipts.clear();
			for(const auto& command : commands) {
				const auto receipt = MakeReceipt(command, ReceiptState::Rejected, rejection);
				admission.receipts.push_back(receipt);
				// A well-formed live producer operation has a durable terminal
				// answer even when this whole batch is rejected at admission.
				// Burn its sequence so an evicted rejection can never execute later.
				if(ValidateShape(command) != LocomotionReason::None || !Matches(command.handle)
					|| receiptIndex_.contains({command.handle, command.producerId, command.sequence})) continue;
				Slot& slot = slots_[command.handle.slotIndex];
				auto producer = slot.producers.find(command.producerId);
				if(producer == slot.producers.end()) {
					if(slot.producers.size() >= kMaximumProducersPerActor) continue;
					producer = slot.producers.emplace(command.producerId, ProducerHighWater{}).first;
				}
				if(command.sequence <= producer->second.sequence
					|| (receiptRing_.size() == kMaximumRetainedReceipts && terminalReceiptCount_ == 0U)) continue;
				producer->second.sequence = command.sequence;
				RetainReceipt(command, receipt);
			}
			return admission;
		}
		if(!anyFresh) {
			admission.queued = true;
			return admission;
		}
		for(const auto& [index, producers] : proposedHighWater)
			for(const auto& [producerId, sequence] : producers)
				slots_[index].producers[producerId].sequence = sequence;
		if(reservedSafetyStop) {
			Slot& slot = slots_[commands.front().handle.slotIndex];
			if(slot.safetyStop) {
				CommandReceipt superseded = MakeReceipt(*slot.safetyStop, ReceiptState::Rejected,
					LocomotionReason::Stopped);
				CompleteReceipt(superseded);
			}
			slot.safetyStop = commands.front();
			slot.safetyEarliestUpdateEpoch = earliestUpdateEpoch;
		} else {
			PendingBatch pending{};
			pending.id = nextBatchId_++;
			pending.earliestUpdateEpoch = earliestUpdateEpoch;
			pending.commands.assign(commands.begin(), commands.end());
			pending_.push_back(std::move(pending));
			pendingCount_ += commands.size();
			for(const auto& [index, increment] : actorIncrements) slots_[index].pendingCount += increment;
		}
		for(std::size_t index = 0U; index < commands.size(); ++index)
			RetainReceipt(commands[index], admission.receipts[index]);
		admission.queued = true;
		return admission;
	}

	std::shared_ptr<const LocomotionAuthorityRoot> LocomotionCommandStore::BuildAuthorityRoot(
		std::span<const PreparedCommandBatch::SlotUpdate> updates) const {
		auto next = std::make_shared<LocomotionAuthorityRoot>();
		next->publicationEpoch = authorityRoot_ ? authorityRoot_->publicationEpoch + 1U : 1U;
		next->entries.reserve(slots_.size());
		for(std::uint32_t index = 0U; index < slots_.size(); ++index) {
			if(!slots_[index].alive) continue;
			const auto updated = std::find_if(updates.begin(), updates.end(),
				[index](const auto& item) { return item.slotIndex == index; });
			next->entries.push_back(updated == updates.end()
				? slots_[index].record.authority : updated->next.authority);
			next->byController.emplace(next->entries.back().controller.character, next->entries.size() - 1U);
		}
		return next;
	}

	std::optional<PreparedCommandBatch> LocomotionCommandStore::PrepareNextBatch(
		std::uint64_t updateEpoch, const LocomotionExternalPreflight& preflight) const {
		PreparedCommandBatch prepared{};
		prepared.reservation = std::unique_lock(mutex_);
		const PendingBatch* selected = nullptr;
		PendingBatch safetyBatch{};
		for(const Slot& slot : slots_) {
			if(!slot.alive || !slot.safetyStop || slot.safetyEarliestUpdateEpoch > updateEpoch) continue;
			safetyBatch.commands.push_back(*slot.safetyStop);
			selected = &safetyBatch;
			break;
		}
		if(!selected && !pending_.empty() && pending_.front().earliestUpdateEpoch <= updateEpoch)
			selected = &pending_.front();
		if(!selected) return {};
		prepared.storeRevision = revision_;
		prepared.batchId = selected->id;
		prepared.nextOwnerToken = nextOwnerToken_;
		prepared.receipts.reserve(selected->commands.size());
		std::vector<LocomotionCommand> ordered = selected->commands;
		std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
			return a.producerId == b.producerId ? a.sequence < b.sequence : a.producerId < b.producerId;
		});
		LocomotionReason rejection = LocomotionReason::None;
		for(const auto& command : ordered) {
			bool adjusted = false;
			if(!Matches(command.handle)) { rejection = LocomotionReason::StaleHandle; break; }
			const auto slotIndex = command.handle.slotIndex;
			auto update = std::find_if(prepared.updates.begin(), prepared.updates.end(),
				[slotIndex](const auto& item) { return item.slotIndex == slotIndex; });
			if(update == prepared.updates.end()) {
				prepared.updates.push_back({slotIndex, slots_[slotIndex].record});
				update = std::prev(prepared.updates.end());
			}
			auto& record = update->next;
			auto& authority = record.authority;
			if(command.expectedConfigurationRevision != record.bindings.configurationRevision) {
				rejection = LocomotionReason::StaleConfiguration; break;
			}
			if(command.expectedAuthorityEpoch != authority.authorityEpoch) {
				rejection = LocomotionReason::OwnershipConflict; break;
			}
			if(authority.authorityEpoch == std::numeric_limits<std::uint64_t>::max()) {
				rejection = LocomotionReason::CapacityExceeded; break;
			}
			if(preflight) {
				rejection = preflight(command, record);
				if(rejection != LocomotionReason::None) break;
			}
			if(command.kind == CommandKind::Acquire) {
				if(authority.state != AuthorityState::Idle || authority.ownerToken != 0U) {
					rejection = LocomotionReason::OwnershipConflict; break;
				}
				if(!record.bindings.controllerSupported) {
					rejection = LocomotionReason::UnsupportedController; break;
				}
				if(command.mode == MotionMode::RootMotion && !record.bindings.animationRootReady) {
					rejection = LocomotionReason::AnimationUnavailable; break;
				}
				if(prepared.nextOwnerToken == std::numeric_limits<std::uint64_t>::max()) {
					rejection = LocomotionReason::CapacityExceeded; break;
				}
				authority.ownerToken = prepared.nextOwnerToken++;
				authority.ownerProducerId = command.producerId;
				authority.mode = command.mode;
				authority.kind = command.intentKind;
				authority.state = AuthorityState::Controlled;
				++authority.authorityEpoch;
				record.intent.reset();
			} else {
				if(authority.ownerToken == 0U || authority.ownerToken != command.ownerToken
					|| authority.ownerProducerId != command.producerId
					|| authority.state == AuthorityState::Suspended) {
					rejection = LocomotionReason::OwnershipConflict; break;
				}
				switch(command.kind) {
				case CommandKind::SetIntent:
				case CommandKind::FollowNavigation:
					if(command.kind == CommandKind::FollowNavigation
						&& record.bindings.navigationObjectId == EMPTY_UUID) {
						rejection = LocomotionReason::MissingBinding; break;
					}
					adjusted = command.requestedSpeed > record.bindings.maximumSpeed;
					record.intent = MovementIntent{command.ownerToken,
						command.kind == CommandKind::FollowNavigation ? IntentKind::Navigation : command.intentKind,
						command.sequence, command.directionSpace, command.direction,
						std::min(command.requestedSpeed, record.bindings.maximumSpeed),
						command.lifetime, updateEpoch};
					authority.kind = record.intent->kind;
					authority.state = AuthorityState::Controlled;
					break;
				case CommandKind::Stop:
					record.intent.reset();
					authority.state = AuthorityState::Stopped;
					++authority.authorityEpoch;
					break;
				case CommandKind::SetMode:
					if(command.mode == MotionMode::RootMotion && !record.bindings.animationRootReady) {
						rejection = LocomotionReason::AnimationUnavailable; break;
					}
					authority.mode = command.mode;
					authority.state = AuthorityState::Controlled;
					record.intent.reset();
					++authority.authorityEpoch;
					break;
				case CommandKind::Transfer:
					if(prepared.nextOwnerToken == std::numeric_limits<std::uint64_t>::max()) {
						rejection = LocomotionReason::CapacityExceeded; break;
					}
					authority.ownerToken = prepared.nextOwnerToken++;
					authority.ownerProducerId = command.transferProducerId;
					authority.kind = command.intentKind;
					authority.state = AuthorityState::Controlled;
					record.intent.reset();
					++authority.authorityEpoch;
					break;
				case CommandKind::Release:
					authority.ownerToken = 0U;
					authority.ownerProducerId = 0U;
					authority.state = AuthorityState::Idle;
					record.intent.reset();
					++authority.authorityEpoch;
					break;
				case CommandKind::Acquire:
					break;
				}
			}
			if(rejection != LocomotionReason::None) break;
			authority.acceptedSequence = command.sequence;
			prepared.receipts.push_back(MakeReceipt(command,
				adjusted ? ReceiptState::Adjusted : ReceiptState::Applied,
				LocomotionReason::None, updateEpoch));
			prepared.receipts.back().authorityEpoch = authority.authorityEpoch;
			prepared.receipts.back().ownerToken = authority.ownerToken;
		}
		if(rejection != LocomotionReason::None) {
			prepared.accepted = false;
			prepared.reason = rejection;
			prepared.updates.clear();
			prepared.receipts.clear();
			for(const auto& command : selected->commands)
				prepared.receipts.push_back(MakeReceipt(command, ReceiptState::Rejected,
					rejection, updateEpoch));
			return prepared;
		}
		prepared.accepted = true;
		for(const auto& update : prepared.updates) {
			const auto& previous = slots_[update.slotIndex].record.authority;
			if(previous.ownerToken != update.next.authority.ownerToken
				|| previous.authorityEpoch != update.next.authority.authorityEpoch
				|| previous.mode != update.next.authority.mode
				|| previous.state != update.next.authority.state)
				prepared.transitions.push_back({previous, update.next.authority});
		}
		prepared.nextRoot = BuildAuthorityRoot(prepared.updates);
		return prepared;
	}

	bool LocomotionCommandStore::CommitPreparedBatch(PreparedCommandBatch&& prepared) noexcept {
		if(!prepared.reservation.owns_lock()) return false;
		if(prepared.storeRevision != revision_ || !prepared.accepted
			|| (prepared.batchId != 0U && (pending_.empty() || pending_.front().id != prepared.batchId))) {
			prepared.reservation.unlock();
			return false;
		}
		for(auto& update : prepared.updates) slots_[update.slotIndex].record = std::move(update.next);
		if(prepared.batchId == 0U) {
			const auto index = prepared.receipts.front().handle.slotIndex;
			slots_[index].safetyStop.reset();
		} else {
			for(const auto& command : pending_.front().commands) {
				--slots_[command.handle.slotIndex].pendingCount;
				--pendingCount_;
			}
			pending_.pop_front();
		}
		for(const auto& receipt : prepared.receipts) CompleteReceipt(receipt);
		nextOwnerToken_ = prepared.nextOwnerToken;
		authorityRoot_.swap(prepared.nextRoot);
		++revision_;
		prepared.reservation.unlock();
		return true;
	}

	bool LocomotionCommandStore::RejectPreparedBatch(PreparedCommandBatch&& prepared,
		LocomotionReason reason) noexcept {
		if(!prepared.reservation.owns_lock()) return false;
		if(prepared.storeRevision != revision_
			|| (prepared.batchId != 0U && (pending_.empty() || pending_.front().id != prepared.batchId))) {
			prepared.reservation.unlock();
			return false;
		}
		if(prepared.batchId == 0U) {
			const auto index = prepared.receipts.front().handle.slotIndex;
			slots_[index].safetyStop.reset();
		} else {
			for(const auto& command : pending_.front().commands) {
				--slots_[command.handle.slotIndex].pendingCount;
				--pendingCount_;
			}
			pending_.pop_front();
		}
		for(auto& receipt : prepared.receipts) {
			receipt.state = ReceiptState::Rejected;
			receipt.reason = reason;
			receipt.ownerToken = 0U;
			CompleteReceipt(receipt);
		}
		++revision_;
		prepared.reservation.unlock();
		return true;
	}

	CommandReceipt LocomotionCommandStore::ReadReceipt(LocomotionHandle handle,
		std::uint64_t producerId, std::uint64_t sequence, std::uint64_t correlationId) const {
		std::lock_guard lock(mutex_);
		if(!handle.IsValid() || producerId == 0U || sequence == 0U || correlationId == 0U)
			return {handle, producerId, sequence, correlationId, ReceiptState::Rejected,
				LocomotionReason::InvalidArgument};
		const auto found = receiptIndex_.find({handle, producerId, sequence});
		if(found != receiptIndex_.end() && found->second->receipt.correlationId == correlationId)
			return found->second->receipt;
		return {handle, producerId, sequence, correlationId, ReceiptState::Expired,
			LocomotionReason::ExpiredReceipt};
	}

	std::shared_ptr<const LocomotionAuthorityRoot> LocomotionCommandStore::CopyAuthorityRoot() const {
		std::lock_guard lock(mutex_);
		return authorityRoot_;
	}

	std::shared_ptr<const LocomotionObservationRoot> LocomotionCommandStore::CopyObservationRoot() const {
		std::lock_guard lock(mutex_);
		return observationRoot_;
	}

	bool LocomotionCommandStore::PublishObservations(std::span<const LocomotionObservation> observations) {
		if(observations.size() > kMaximumLiveSlots) return false;
		std::lock_guard lock(mutex_);
		auto next = std::make_shared<LocomotionObservationRoot>();
		next->publicationEpoch = observationRoot_->publicationEpoch + 1U;
		next->entries.reserve(observations.size());
		for(const auto& observation : observations) {
			if(!Matches(observation.handle)) return false;
			LocomotionObservation copied = observation;
			copied.publicationEpoch = next->publicationEpoch;
			next->entries.push_back(std::move(copied));
		}
		observationRoot_ = std::move(next);
		return true;
	}

	void LocomotionCommandStore::RemovePendingFor(LocomotionHandle handle) {
		for(auto it = pending_.begin(); it != pending_.end();) {
			const auto containsHandle = std::any_of(it->commands.begin(), it->commands.end(),
				[handle](const auto& command) { return command.handle == handle; });
			if(!containsHandle) { ++it; continue; }
			for(const auto& command : it->commands) {
				if(Matches(command.handle)) --slots_[command.handle.slotIndex].pendingCount;
				--pendingCount_;
				CompleteReceipt(MakeReceipt(command, ReceiptState::Rejected, LocomotionReason::Revoked));
			}
			it = pending_.erase(it);
		}
	}

	bool LocomotionCommandStore::Revoke(LocomotionHandle handle, LocomotionReason reason) {
		std::lock_guard lock(mutex_);
		if(!Matches(handle)) return false;
		Slot& slot = slots_[handle.slotIndex];
		if(slot.safetyStop) {
			CompleteReceipt(MakeReceipt(*slot.safetyStop, ReceiptState::Rejected, reason));
			slot.safetyStop.reset();
		}
		RemovePendingFor(handle);
		slot.record.intent.reset();
		slot.record.authority.ownerToken = 0U;
		slot.record.authority.ownerProducerId = 0U;
		slot.record.authority.state = reason == LocomotionReason::ExternalExclusive
			? AuthorityState::Suspended : AuthorityState::Idle;
		slot.record.authority.suspendedReason = reason;
		if(slot.record.authority.authorityEpoch != std::numeric_limits<std::uint64_t>::max())
			++slot.record.authority.authorityEpoch;
		authorityRoot_ = BuildAuthorityRoot({});
		++revision_;
		return true;
	}

	bool LocomotionCommandStore::ReleaseSlot(LocomotionHandle handle) {
		std::lock_guard lock(mutex_);
		if(!Matches(handle)) return false;
		Slot& slot = slots_[handle.slotIndex];
		RemovePendingFor(handle);
		if(slot.safetyStop) {
			CompleteReceipt(MakeReceipt(*slot.safetyStop, ReceiptState::Rejected, LocomotionReason::Revoked));
			slot.safetyStop.reset();
		}
		const auto& character = slot.record.bindings.controller.character;
		controllerSlots_.erase({character.worldId.value, character.worldGeneration,
			character.slotIndex, character.slotGeneration});
		slot.record = {};
		slot.producers.clear();
		slot.alive = false;
		if((slot.generation & 0xFFFFU) != 0xFFFFU) freeSlots_.push_back(handle.slotIndex);
		authorityRoot_ = BuildAuthorityRoot({});
		++revision_;
		return true;
	}

	std::size_t LocomotionCommandStore::RevokeScene(std::uint64_t sceneLifetime, LocomotionReason reason) {
		std::vector<LocomotionHandle> handles;
		{
			std::lock_guard lock(mutex_);
			for(const Slot& slot : slots_)
				if(slot.alive && slot.record.handle.sceneLifetime == sceneLifetime)
					handles.push_back(slot.record.handle);
		}
		std::size_t count = 0U;
		for(const auto handle : handles) count += Revoke(handle, reason) ? 1U : 0U;
		return count;
	}

	void LocomotionCommandStore::ExpireOneUpdateIntents(std::uint64_t updateEpoch) {
		std::lock_guard lock(mutex_);
		for(Slot& slot : slots_) {
			if(!slot.alive || !slot.record.intent
				|| slot.record.intent->lifetime != IntentLifetime::OneUpdate
				|| slot.record.intent->acceptedUpdateEpoch >= updateEpoch) continue;
			slot.record.intent.reset();
		}
	}

	std::size_t LocomotionCommandStore::LiveCount() const {
		std::lock_guard lock(mutex_);
		return static_cast<std::size_t>(std::count_if(slots_.begin(), slots_.end(),
			[](const Slot& slot) { return slot.alive; }));
	}

	std::size_t LocomotionCommandStore::PendingCount() const {
		std::lock_guard lock(mutex_);
		std::size_t safetyCount = 0U;
		for(const Slot& slot : slots_) safetyCount += slot.alive && slot.safetyStop ? 1U : 0U;
		return pendingCount_ + safetyCount;
	}
}
