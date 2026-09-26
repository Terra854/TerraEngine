// RagdollPhysicsBridge.cpp - Bounded deterministic command staging.
#include "Animation/Ragdoll/RagdollPhysicsBridge.h"
#include "Physics/PhysicsCommands.h"
#include <algorithm>
#include <cmath>
#include <new>
#include <tuple>
namespace TerraEngine::Animation {
	namespace {
		AnimationStatus ValidateRequest(const RagdollPhysicsRequest& request) noexcept {
			const bool noProviderRetirement = request.kind == RagdollPhysicsRequestKind::Teardown
				&& request.retirementIntent != nullptr;
			if(request.ragdollId == 0U
				|| (!request.generation.IsValid() && !noProviderRetirement)
				|| (request.retirementIntent && request.kind != RagdollPhysicsRequestKind::Teardown)) {
				return AnimationStatus::InvalidArgument;
			}
			const bool needsActivationPose = request.kind == RagdollPhysicsRequestKind::Create
				|| request.kind == RagdollPhysicsRequestKind::Rebuild
				|| request.kind == RagdollPhysicsRequestKind::Activate;
			if(needsActivationPose
				&& ValidatePhysicsPoseSnapshot(request.activationPose, request.generation)
					!= AnimationStatus::Ok) {
				return AnimationStatus::Rejected;
			}
			if(request.kind != RagdollPhysicsRequestKind::UpdateMotors) {
				return request.motorDrives.empty()
					? AnimationStatus::Ok : AnimationStatus::InvalidArgument;
			}
			if(request.motorDrives.empty()
				|| request.motorDrives.size() > kRagdollSnapshotMaximumBodies) {
				return AnimationStatus::InvalidArgument;
			}
			for(std::size_t index = 0U; index < request.motorDrives.size(); ++index) {
				const RagdollMotorDrive& drive = request.motorDrives[index];
				if(drive.constraintId == 0U
					|| !std::isfinite(drive.strengthScale)
					|| drive.strengthScale < 0.0F) {
					return AnimationStatus::InvalidArgument;
				}
				for(std::size_t previous = 0U; previous < index; ++previous) {
					if(request.motorDrives[previous].constraintId == drive.constraintId) {
						return AnimationStatus::InvalidArgument;
					}
				}
			}
			return AnimationStatus::Ok;
		}
	}

	AnimationStatus RagdollPhysicsBridge::Enqueue(RagdollPhysicsRequest request) noexcept {
		if(const AnimationStatus status = ValidateRequest(request);
			status != AnimationStatus::Ok) return status;
		if(request.retirementIntent) {
			// The same all-or-none normalization used by batch retirement must also
			// protect a single trusted Teardown producer.
			return EnqueueBatch(std::span(&request, 1U));
		}
		try {
			std::lock_guard lock(mutex_);
			if(pending_.size() >= kRagdollSnapshotMaximumBodies) return AnimationStatus::HardLimitExceeded;
			pending_.push_back(std::move(request));
			return AnimationStatus::Queued;
		} catch(const std::bad_alloc&) { return AnimationStatus::HardLimitExceeded; }
	}
	AnimationStatus RagdollPhysicsBridge::EnqueueBatch(
		std::span<const RagdollPhysicsRequest> requests) noexcept {
		if(requests.empty() || requests.size() > kRagdollSnapshotMaximumBodies) {
			return AnimationStatus::InvalidArgument;
		}
		for(const RagdollPhysicsRequest& request : requests) {
			if(const AnimationStatus status = ValidateRequest(request);
				status != AnimationStatus::Ok) return status;
		}
		try {
			std::lock_guard lock(mutex_);
			const bool taggedRetirement = std::any_of(requests.begin(), requests.end(),
				[](const RagdollPhysicsRequest& request) {
					return request.retirementIntent != nullptr;
				});
			if(taggedRetirement) {
				// Stage all three roots first. A rejected retirement cannot erase an
				// accepted Create/Activate or its future terminal receipt.
				std::vector<RagdollPhysicsRequest> candidatePending = pending_;
				std::vector<RagdollPhysicsRequest> candidateUndelivered = undelivered_;
				std::vector<RagdollPhysicsResult> candidateCancelled =
					pendingRetirementCancellations_;
				for(const RagdollPhysicsRequest& retirement : requests) {
					if(!retirement.retirementIntent) continue;
					if(std::count_if(requests.begin(), requests.end(),
						[&retirement](const RagdollPhysicsRequest& request) {
							return request.ragdollId == retirement.ragdollId;
						}) != 1U) return AnimationStatus::InvalidArgument;
					const auto cancelOlder = [&](std::vector<RagdollPhysicsRequest>& queue) {
						for(auto entry = queue.begin(); entry != queue.end();) {
							if(entry->ragdollId != retirement.ragdollId) {
								++entry;
								continue;
							}
							if(entry->retirementIntent) return false;
							if(candidateCancelled.size() >= kRagdollSnapshotMaximumBodies)
								return false;
							candidateCancelled.push_back({entry->ragdollId, entry->kind,
								entry->generation, entry->correlationId,
								AnimationStatus::Cancelled, 0,
								"Superseded by exact animator retirement."});
							entry = queue.erase(entry);
						}
						return true;
					};
					if(!cancelOlder(candidatePending)
						|| !cancelOlder(candidateUndelivered))
						return AnimationStatus::HardLimitExceeded;
				}
				if(requests.size() > kRagdollSnapshotMaximumBodies - candidatePending.size())
					return AnimationStatus::HardLimitExceeded;
				candidatePending.reserve(candidatePending.size() + requests.size());
				candidatePending.insert(candidatePending.end(), requests.begin(), requests.end());
				pending_.swap(candidatePending);
				undelivered_.swap(candidateUndelivered);
				pendingRetirementCancellations_.swap(candidateCancelled);
				return AnimationStatus::Queued;
			}
			if(requests.size() > kRagdollSnapshotMaximumBodies - pending_.size()) {
				return AnimationStatus::HardLimitExceeded;
			}
			std::vector<RagdollPhysicsRequest> candidate = pending_;
			candidate.reserve(candidate.size() + requests.size());
			candidate.insert(candidate.end(), requests.begin(), requests.end());
			pending_ = std::move(candidate);
			return AnimationStatus::Queued;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}
	AnimationStatus RagdollPhysicsBridge::Drain(std::vector<RagdollPhysicsRequest>& output) noexcept {
		try {
			std::lock_guard lock(mutex_);
			// Swap avoids allocation after ownership has crossed the protected boundary.
			// The ordering keys make mixed producers deterministic before validation.
			output.clear();
			// An acknowledgement allocation can fail after a previous drain. Retry
			// that complete batch before accepting newer producer work, preserving
			// deterministic ordering without merging vectors under allocation pressure.
			if(!undelivered_.empty()) output.swap(undelivered_);
			else output.swap(pending_);
			std::sort(output.begin(), output.end(), [](const auto& left, const auto& right) {
				return std::tie(left.generation.fixedStep, left.ragdollId, left.producerOrder, left.correlationId)
					< std::tie(right.generation.fixedStep, right.ragdollId, right.producerOrder, right.correlationId);
			});
			return AnimationStatus::Ok;
		} catch(...) { return AnimationStatus::InternalError; }
	}

	void RagdollPhysicsBridge::RestoreUndeliveredDrain(
		std::vector<RagdollPhysicsRequest>&& requests) noexcept {
		std::lock_guard lock(mutex_);
		// New producers may enqueue while PhysicsSystem creates result records. Keep
		// their queue intact and retain this earlier batch in its own retry slot.
		// The protected commit is serial, so undelivered_ is empty at this point.
		undelivered_.swap(requests);
	}
	AnimationStatus RagdollPhysicsBridge::RejectBatchAtProtectedBoundary(
		std::span<const RagdollPhysicsRequest> requests,
		AnimationFixedTick committedStep,
		std::string diagnostic) noexcept {
		std::shared_ptr<const RagdollPhysicsResultSnapshot> prepared;
		const AnimationStatus status = PrepareResultSnapshot(
			requests, committedStep, AnimationStatus::Rejected,
			std::move(diagnostic), prepared);
		if(status != AnimationStatus::Ok) return status;
		PublishPreparedResultsAtProtectedBoundary(std::move(prepared));
		return AnimationStatus::Rejected;
	}

	AnimationStatus RagdollPhysicsBridge::PrepareResultSnapshot(
		std::span<const RagdollPhysicsRequest> requests,
		AnimationFixedTick committedStep,
		AnimationStatus status,
		std::string diagnostic,
		std::shared_ptr<const RagdollPhysicsResultSnapshot>& output) const noexcept {
		if(requests.empty() || committedStep <= 0) return AnimationStatus::InvalidArgument;
		try {
			auto results = std::make_shared<std::vector<RagdollPhysicsResult>>();
			std::size_t cancellationCount = 0U;
			{
				std::lock_guard lock(mutex_);
				cancellationCount = pendingRetirementCancellations_.size();
				results->reserve(cancellationCount + requests.size());
				for(const RagdollPhysicsResult& cancelled : pendingRetirementCancellations_) {
					RagdollPhysicsResult terminal = cancelled;
					terminal.committedStep = committedStep;
					results->push_back(std::move(terminal));
				}
			}
			for(const RagdollPhysicsRequest& request : requests) {
				results->push_back({request.ragdollId, request.kind, request.generation,
					request.correlationId, status, committedStep, diagnostic});
			}
			auto snapshot = std::make_shared<RagdollPhysicsResultSnapshot>();
			snapshot->committedStep = committedStep;
			snapshot->results = std::move(results);
			snapshot->includedRetirementCancellationCount = cancellationCount;
			output = std::move(snapshot);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	void RagdollPhysicsBridge::PublishPreparedResultsAtProtectedBoundary(
		std::shared_ptr<const RagdollPhysicsResultSnapshot> results,
		std::span<const std::uint64_t> retiredPoseIds) noexcept {
		std::lock_guard lock(mutex_);
		for(const std::uint64_t ragdollId : retiredPoseIds) currentStepPoses_.erase(ragdollId);
		if(results && results->includedRetirementCancellationCount != 0U) {
			const std::size_t consumed = std::min(
				results->includedRetirementCancellationCount,
				pendingRetirementCancellations_.size());
			pendingRetirementCancellations_.erase(
				pendingRetirementCancellations_.begin(),
				pendingRetirementCancellations_.begin() + consumed);
		}
		latestResults_ = std::move(results);
	}

	AnimationStatus RagdollPhysicsBridge::PublishCurrentStepPose(
		std::uint64_t ragdollId,
		PhysicsPoseSnapshot snapshot,
		const RagdollGenerationStamp& expected,
		std::span<const PhysicsPoseExpectedBodyGeneration> expectedBodies) noexcept {
		if(ragdollId == 0U) return AnimationStatus::InvalidArgument;
		// Provider range completion can differ between worker counts. Seal the
		// immutable animation-facing order before validation and publication.
		std::sort(snapshot.bodies.begin(), snapshot.bodies.end(), [](const auto& left, const auto& right) {
			return std::tie(left.bodyId, left.stableJointId) < std::tie(right.bodyId, right.stableJointId);
		});
		if(const AnimationStatus status = ValidatePhysicsPoseSnapshot(snapshot, expected, expectedBodies);
			status != AnimationStatus::Ok) return status;
		try {
			auto immutable = std::make_shared<const PhysicsPoseSnapshot>(std::move(snapshot));
			std::lock_guard lock(mutex_);
			currentStepPoses_.insert_or_assign(ragdollId, std::move(immutable));
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) { return AnimationStatus::HardLimitExceeded; }
	}

	std::shared_ptr<const PhysicsPoseSnapshot> RagdollPhysicsBridge::AcquireCurrentStepPose(
		std::uint64_t ragdollId,
		const RagdollGenerationStamp& expected) const noexcept {
		std::lock_guard lock(mutex_);
		const auto found = currentStepPoses_.find(ragdollId);
		if(found == currentStepPoses_.end() || !found->second
			|| ValidatePhysicsPoseSnapshot(*found->second, expected) != AnimationStatus::Ok) return {};
		return found->second;
	}

	std::shared_ptr<const PhysicsPoseSnapshot> RagdollPhysicsBridge::AcquireLatestCurrentStepPose(
		std::uint64_t ragdollId,
		const RagdollGenerationStamp& expected) const noexcept {
		std::lock_guard lock(mutex_);
		const auto found = currentStepPoses_.find(ragdollId);
		if(found == currentStepPoses_.end() || !found->second || !expected.IsValid()) return {};
		const RagdollGenerationStamp& actual = found->second->generation;
		if(actual.worldGeneration != expected.worldGeneration
			|| actual.profileGeneration != expected.profileGeneration
			|| actual.skeletonGeneration != expected.skeletonGeneration
			|| actual.publicationGeneration != expected.publicationGeneration
			|| actual.fixedStep < expected.fixedStep
			|| ValidatePhysicsPoseSnapshot(*found->second, actual) != AnimationStatus::Ok) {
			return {};
		}
		return found->second;
	}

	std::shared_ptr<const RagdollPhysicsResultSnapshot> RagdollPhysicsBridge::LatestResults() const noexcept {
		std::lock_guard lock(mutex_);
		return latestResults_;
	}

	std::size_t RagdollPhysicsBridge::PendingCount() const noexcept {
		std::lock_guard lock(mutex_);
		return pending_.size() + undelivered_.size();
	}

	bool RagdollPhysicsBridge::HasPendingRagdoll(std::uint64_t ragdollId) const noexcept
	{
		if(ragdollId == 0U) return false;
		std::lock_guard lock(mutex_);
		const auto matches = [ragdollId](const RagdollPhysicsRequest& request) {
			return request.ragdollId == ragdollId;
		};
		return std::any_of(pending_.begin(), pending_.end(), matches)
			|| std::any_of(undelivered_.begin(), undelivered_.end(), matches);
	}

	bool RagdollPhysicsBridge::HasPendingExternalRetirementOwner(
		std::uint64_t externalOwnerId) const noexcept
	{
		if(externalOwnerId == 0U) return false;
		std::lock_guard lock(mutex_);
		const auto matches = [externalOwnerId](const RagdollPhysicsRequest& request) {
			return request.retirementIntent
				&& request.retirementIntent->externalOwnerId == externalOwnerId;
		};
		return std::any_of(pending_.begin(), pending_.end(), matches)
			|| std::any_of(undelivered_.begin(), undelivered_.end(), matches);
	}

	void RagdollPhysicsBridge::Clear() noexcept {
		std::lock_guard lock(mutex_);
		pending_.clear();
		undelivered_.clear();
		pendingRetirementCancellations_.clear();
		currentStepPoses_.clear();
		latestResults_.reset();
	}
}
