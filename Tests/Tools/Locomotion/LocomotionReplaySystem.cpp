// LocomotionReplaySystem.cpp - no alternate physics or direct transform writes.
#include "LocomotionReplaySystem.h"
#include "Physics/PhysicsSystem.h"
#include "Animation/AnimationSystem.h"

#include <mutex>
#include <vector>

namespace TerraEngine::Locomotion::Validation {
	struct LocomotionReplaySystem::Impl {
		mutable std::mutex mutex;
		bool initialized = false;
		std::uint64_t queuedFrame = 0U;
		std::vector<Physics::CharacterMotionTraceRecord> pending;
		std::vector<Physics::CharacterMotionTraceRecord> roots;
		std::vector<LocomotionParameterTraceRecord> parameters;
		std::vector<Physics::CharacterMotionLeaseChange> leaseScratch;
		ReplayPublicationResult result;
	};
	LocomotionReplaySystem::LocomotionReplaySystem() : impl_(std::make_unique<Impl>()) {}
	LocomotionReplaySystem::~LocomotionReplaySystem() = default;
	LocomotionReplaySystem& LocomotionReplaySystem::Instance() {
		static LocomotionReplaySystem value;
		return value;
	}
	void LocomotionReplaySystem::Initialize() {
		std::lock_guard lock(impl_->mutex);
		impl_->initialized = true;
		impl_->pending.clear();
		impl_->roots.clear();
		impl_->parameters.clear();
		impl_->queuedFrame = 0U;
		impl_->result = {};
	}
	void LocomotionReplaySystem::Shutdown() {
		std::lock_guard lock(impl_->mutex);
		impl_->pending.clear();
		impl_->roots.clear();
		impl_->parameters.clear();
		impl_->queuedFrame = 0U;
		impl_->initialized = false;
	}
	std::string LocomotionReplaySystem::SystemName() { return "LocomotionReplayValidationSystem"; }
	Scheduling::SystemDeclaration LocomotionReplaySystem::SchedulerDeclaration() {
		using namespace Scheduling;
		SystemDeclaration result;
		result.name = SystemName();
		result.phase = "Locomotion";
		result.after = {"ScriptingSystem"};
		result.before = {"AnimationSystem", "PhysicsSystem"};
		result.reads = {kResourcePhysicsCharacterStatePackets, kResourcePhysicsCharacterMotionLeases};
		result.writes = {kResourceCharacterMotionTargets, kResourcePhysicsCharacterMotionLeases,
			kResourceRootMotionIntervals, kResourceRootMotionResults, kResourceAnimationCommandQueues, "PhysicsWorld"};
		result.mainThread = true;
		result.phaseKind = WorkerSystemPhaseKind::ProtectedCommit;
		result.boundaryClassification = HardBoundaryClassification::Protected;
		result.hardBoundary = true;
		result.requiresFlushedSceneMutations = true;
		result.safeSubwork = "Recorded immutable typed packets are prepared by the validation harness before this frame.";
		result.protectedCommit = "Replay exact current-world leases, velocity targets and root intervals through Physics ingress.";
		result.protectedReason = "Physical ownership and ingress are protected; no worker may commit them.";
		result.profilingLabel = "LocomotionReplayValidation";
		result.usesDefaultMetadata = false;
		return result;
	}
	bool LocomotionReplaySystem::QueueFrame(std::uint64_t frame,
		std::span<const Physics::CharacterMotionTraceRecord> records,
		std::span<const LocomotionParameterTraceRecord> parameters) {
		std::lock_guard lock(impl_->mutex);
		if(!impl_->initialized || frame == 0U || frame <= impl_->result.frame
			|| impl_->queuedFrame != 0U || !impl_->roots.empty() || records.size() > 65536U || parameters.size() > 4096U) return false;
		for(const auto& batch : parameters)
			if(batch.count == 0U || batch.count > batch.commands.size()) return false;
		// Check whole-batch framing before any future live state can be touched.
		for(std::size_t index = 0U; index < records.size();) {
			const auto& first = records[index];
			if(first.batchIndex != 0U || first.batchCount == 0U || first.batchCount > records.size() - index) return false;
			for(std::size_t member = 0U; member < first.batchCount; ++member) {
				const auto& record = records[index + member];
				if(record.batch != first.batch || record.batchIndex != member || record.batchCount != first.batchCount
					|| record.payload.index() != first.payload.index()) return false;
			}
			if(first.batchCount != 1U && !std::holds_alternative<Physics::CharacterMotionLeaseChange>(first.payload)) return false;
			index += first.batchCount;
		}
		try {
			// Allocation belongs to the outside-frame input preparation, not the
			// measured protected replay boundary. Publish only a complete candidate.
			std::vector<Physics::CharacterMotionTraceRecord> next(records.begin(), records.end());
			std::vector<LocomotionParameterTraceRecord> nextParameters(parameters.begin(), parameters.end());
			impl_->roots.reserve(records.size());
			impl_->leaseScratch.reserve(records.size());
			impl_->pending.swap(next);
			impl_->parameters.swap(nextParameters);
		}
		catch(const std::bad_alloc&) { return false; }
		impl_->queuedFrame = frame;
		return true;
	}
	ReplayPublicationResult LocomotionReplaySystem::ReadResult() const {
		std::lock_guard lock(impl_->mutex);
		return impl_->result;
	}
	void LocomotionReplaySystem::Update(float) {
		std::lock_guard lock(impl_->mutex);
		if(!impl_->initialized || impl_->queuedFrame == 0U) return;
		impl_->result = {impl_->queuedFrame, 0U, 0U, {}};
		auto& physics = Physics::PhysicsSystem::Instance();
		for(std::size_t index = 0U; index < impl_->pending.size();) {
			const auto& record = impl_->pending[index];
			if(std::holds_alternative<Physics::RootMotionInterval>(record.payload)) {
				impl_->roots.push_back(record);
				++index;
				continue;
			}
			bool accepted = false;
			if(std::holds_alternative<Physics::CharacterMotionLeaseChange>(record.payload)) {
				// Scratch capacity was reserved with the complete frame outside the
				// measured update. Keep lease groups atomic without per-frame allocation.
				auto& changes = impl_->leaseScratch;
				changes.clear();
				for(std::size_t member = 0U; member < record.batchCount; ++member)
					changes.push_back(std::get<Physics::CharacterMotionLeaseChange>(impl_->pending[index + member].payload));
				accepted = physics.PublishCharacterMotionLeases(changes).accepted;
			} else if(const auto* target = std::get_if<Physics::CharacterMotionTarget>(&record.payload)) {
				accepted = physics.PublishCharacterMotionTarget(*target).Succeeded();
			}
			if(!accepted) {
				impl_->result.rejected = impl_->pending.size() - index;
				impl_->result.failure = "Recorded ingress rejected at sequence " + std::to_string(record.sequence)
					+ "; this control run cannot qualify a performance comparison.";
				break;
			}
			impl_->result.accepted += record.batchCount;
			index += record.batchCount;
		}
		impl_->pending.clear();
		for(const auto& batch : impl_->parameters) {
			if(impl_->result.rejected != 0U) break;
			const auto receipt = Animation::AnimationSystem::Instance().SubmitCommandBatch(
				std::span(batch.commands.data(), batch.count));
			if(receipt.status != Animation::AnimationStatus::Queued) {
				++impl_->result.rejected;
				impl_->result.failure = "Recorded animation parameter batch rejected; the paired run is invalid.";
			}
		}
		impl_->parameters.clear();
		impl_->queuedFrame = 0U;
	}
	void LocomotionReplaySystem::PublishRootIntervals() {
		std::lock_guard lock(impl_->mutex);
		if(!impl_->initialized) return;
		auto& physics = Physics::PhysicsSystem::Instance();
		for(const auto& record : impl_->roots) {
			if(impl_->result.rejected != 0U) break;
			const auto reserved = physics.ReserveRootMotionInterval(std::get<Physics::RootMotionInterval>(record.payload));
			const bool accepted = reserved.accepted && physics.CommitRootMotionInterval(reserved.reservation).accepted;
			if(!accepted) {
				if(reserved.accepted) (void)physics.CancelRootMotionIntervalReservation(reserved.reservation);
				++impl_->result.rejected;
				impl_->result.failure = "Recorded root ingress rejected; the paired run is invalid.";
			} else ++impl_->result.accepted;
		}
		impl_->roots.clear();
	}
	LocomotionReplayRootSystem& LocomotionReplayRootSystem::Instance() {
		static LocomotionReplayRootSystem value;
		return value;
	}
	Scheduling::SystemDeclaration LocomotionReplayRootSystem::SchedulerDeclaration() {
		auto result = LocomotionReplaySystem::Instance().SchedulerDeclaration();
		result.name = SystemName();
		result.phase = "Animation";
		result.after = {"AnimationSystem", "LocomotionReplayValidationSystem"};
		result.before = {"PhysicsSystem"};
		result.safeSubwork = "Root intervals retain the immutable recorded payload prepared outside this frame.";
		result.protectedCommit = "Publish recorded root intervals after Animation and before fixed Physics.";
		result.profilingLabel = "LocomotionReplayRootValidation";
		return result;
	}
}
