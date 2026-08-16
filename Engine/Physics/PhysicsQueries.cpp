// PhysicsQueries.cpp - deterministic query preparation and immutable results.

#include "PhysicsQueries.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <tuple>
#include <utility>

namespace TerraEngine::Physics {
	namespace {
		enum class QueryState : std::uint8_t {
			Free,
			Queued,
			Prepared,
			Completed,
			Cancelled,
			Expired
		};

		struct Validation {
			bool accepted = false;
			PhysicsQueryRejectionReason reason = PhysicsQueryRejectionReason::InvalidRequest;
			std::string diagnostic;
		};

		bool IsZeroUuid(const TerraEngine::UUID& value) noexcept {
			return value == TerraEngine::EMPTY_UUID;
		}

		float LengthSquared(const PhysicsVector3& value) noexcept {
			return value.x * value.x + value.y * value.y + value.z * value.z;
		}

		bool HasPositiveExtent(const PhysicsVector3& extent) noexcept {
			return extent.x > 0.0F && extent.y > 0.0F && extent.z > 0.0F;
		}

		bool WithinExtent(const PhysicsVector3& extent, float maximum) noexcept {
			return extent.x <= maximum && extent.y <= maximum && extent.z <= maximum;
		}

		PhysicsGeneration NextGeneration(PhysicsGeneration generation) noexcept {
			++generation;
			return generation == 0U ? 1U : generation;
		}

		bool IsSameWorld(PhysicsWorldHandle left, PhysicsWorldHandle right) noexcept {
			return left.IsValid() && right.IsValid()
				&& left.worldId == right.worldId
				&& left.worldGeneration == right.worldGeneration;
		}

		PhysicsQueryResult MakeResult(
			PhysicsQueryHandle handle,
			PhysicsWorldHandle world,
			PhysicsTick tick,
			PhysicsResultStatus status,
			PhysicsQueryRejectionReason reason,
			std::string correlationId,
			std::string diagnostic = {}) {
			PhysicsQueryResult result;
			result.handle = handle;
			result.world = world;
			result.tick = tick;
			result.status = status;
			result.rejectionReason = reason;
			result.correlationId = std::move(correlationId);
			result.diagnostic = std::move(diagnostic);
			result.hits = std::make_shared<const std::vector<PhysicsQueryHit>>();
			return result;
		}

		Validation ValidateRequest(const PhysicsQueryRequest& request, const PhysicsSettings& settings, PhysicsWorldHandle activeWorld, PhysicsTick currentTick) {
			if(!IsSameWorld(request.world, activeWorld)) {
				return {false, PhysicsQueryRejectionReason::StaleWorld,
					"Query world identity does not match the active physics world generation."};
			}
			if(!request.pose.IsFinite()) {
				return {false, PhysicsQueryRejectionReason::InvalidRequest,
					"Query pose must contain finite metre-space values."};
			}
			if(request.maximumHits == 0U || request.maximumHits > settings.maxHitsPerQuery) {
				return {false, PhysicsQueryRejectionReason::InvalidRequest,
					"Query maximumHits must stay within the calibrated settings limit."};
			}
			if(!PhysicsSettings::IsValidLayerMask(request.filter.collisionMask)
				|| request.filter.bodyTypes == PhysicsQueryBodyTypeMask::None) {
				return {false, PhysicsQueryRejectionReason::InvalidRequest,
					"Query filters require a valid 32-bit layer mask and at least one body type."};
			}
			if(request.filter.excludedBody.IsValid()
				&& (request.filter.excludedBody.worldId != activeWorld.worldId
					|| request.filter.excludedBody.worldGeneration != activeWorld.worldGeneration)) {
				return {false, PhysicsQueryRejectionReason::StaleWorld,
					"Excluded body handle belongs to a stale physics world generation."};
			}
			if(request.expiryTick != 0U && request.expiryTick <= currentTick) {
				return {false, PhysicsQueryRejectionReason::Expired,
					"Query expiry tick has already passed before submission."};
			}

			const bool isCast = request.kind == PhysicsQueryKind::RayCast || request.kind == PhysicsQueryKind::ShapeCast;
			const bool needsExtent = request.kind == PhysicsQueryKind::ShapeCast || request.kind == PhysicsQueryKind::Overlap;
			if(isCast && (!request.direction.IsFinite() || LengthSquared(request.direction) <= std::numeric_limits<float>::epsilon())) {
				return {false, PhysicsQueryRejectionReason::InvalidRequest,
					"Ray and shape-cast directions must be finite and non-zero."};
			}
			if(isCast && (!std::isfinite(request.distance) || request.distance <= 0.0F || request.distance > settings.maxCastDistance)) {
				return {false, PhysicsQueryRejectionReason::InvalidRequest,
					"Cast distance must be positive and within the calibrated settings limit."};
			}
			if(!isCast && (!std::isfinite(request.distance) || request.distance < 0.0F)) {
				return {false, PhysicsQueryRejectionReason::InvalidRequest,
					"Overlap and point query distance must be finite and non-negative."};
			}
			if(needsExtent && (!request.shapeExtent.IsFinite() || !HasPositiveExtent(request.shapeExtent) || !WithinExtent(request.shapeExtent, settings.maxShapeExtent))) {
				return {false, PhysicsQueryRejectionReason::InvalidRequest,
					"Shape-query extents must be finite, positive, and within calibrated bounds."};
			}
			return {true, PhysicsQueryRejectionReason::None, {}};
		}

		bool MatchesBodyType(PhysicsMotionType type, PhysicsQueryBodyTypeMask mask) noexcept {
			switch(type) {
			case PhysicsMotionType::Static: return HasFlag(mask, PhysicsQueryBodyTypeMask::Static);
			case PhysicsMotionType::Dynamic: return HasFlag(mask, PhysicsQueryBodyTypeMask::Dynamic);
			case PhysicsMotionType::Kinematic: return HasFlag(mask, PhysicsQueryBodyTypeMask::Kinematic);
			}
			return false;
		}

		bool MatchesFilter(const PhysicsQueryHit& hit, const PhysicsQueryFilter& filter, PhysicsWorldHandle world) noexcept {
			if(!hit.body.IsValid() || !hit.position.IsFinite() || !hit.normal.IsFinite()
				|| !std::isfinite(hit.distance) || !std::isfinite(hit.fraction)
				|| hit.distance < 0.0F || hit.fraction < 0.0F
				|| hit.body.worldId != world.worldId || hit.body.worldGeneration != world.worldGeneration) {
				return false;
			}
			if((filter.collisionMask & (1U << hit.collisionLayer)) == 0U
				|| (!filter.includeSensors && hit.sensor)
				|| !MatchesBodyType(hit.motionType, filter.bodyTypes)) {
				return false;
			}
			if(filter.excludedBody.IsValid() && hit.body == filter.excludedBody) {
				return false;
			}
			if(!IsZeroUuid(filter.excludedObjectId) && hit.objectId == filter.excludedObjectId
				&& (filter.excludedObjectGeneration == 0U || hit.objectGeneration == filter.excludedObjectGeneration)) {
				return false;
			}
			return true;
		}

		std::vector<PhysicsQueryHit> Canonicalize(
			std::span<const PhysicsQueryHit> candidates,
			const PhysicsQueryRequest& request,
			bool& truncated) {
			// Cap the local vector before copying any provider candidates. This makes
			// manager-owned memory proportional to maxHits (at most 64), not to a
			// malformed provider response containing an arbitrary number of hits.
			std::vector<PhysicsQueryHit> canonical;
			canonical.reserve(request.maximumHits);
			for(const PhysicsQueryHit& hit : candidates) {
				if(!MatchesFilter(hit, request.filter, request.world)) {
					continue;
				}

				const PhysicsQueryHitDedupKey key = DedupKey(hit);
				const auto duplicate = std::find_if(canonical.begin(), canonical.end(), [&key](const PhysicsQueryHit& retained) {
					return DedupKey(retained) == key;
				});
				if(duplicate != canonical.end()) {
					if(IsStableHitOrder(hit, *duplicate)) {
						*duplicate = hit;
					}
					continue;
				}

				canonical.push_back(hit);
				std::stable_sort(canonical.begin(), canonical.end(), IsStableHitOrder);
				if(canonical.size() > request.maximumHits) {
					canonical.pop_back();
					truncated = true;
				}
			}
			std::stable_sort(canonical.begin(), canonical.end(), IsStableHitOrder);
			return canonical;
		}
	}

	struct PhysicsQueryManager::Impl {
		struct Slot {
			PhysicsGeneration generation = 1U;
			QueryState state = QueryState::Free;
			PhysicsQueryRequest request;
			std::optional<PhysicsQueryResult> result;
		};

		explicit Impl(PhysicsSettings configuredSettings)
			: settings(std::move(configuredSettings)) {
			const std::size_t historyWindows = static_cast<std::size_t>(settings.retainedResultTicks) + 1U;
			maximumLiveQueryCount = static_cast<std::size_t>(settings.maxRequestsPerBatch) * historyWindows;
			slots.reserve(maximumLiveQueryCount);
			freeSlots.reserve(maximumLiveQueryCount);
		}

		PhysicsSettings settings;
		mutable std::mutex mutex;
		std::vector<Slot> slots;
		std::vector<PhysicsSlotIndex> freeSlots;
		PhysicsQueryDiagnostics diagnostics;
		std::size_t maximumLiveQueryCount = 0U;
	};

	PhysicsQueryManager::PhysicsQueryManager(PhysicsSettings settings)
		: impl(std::make_unique<Impl>(std::move(settings))) {
		const PhysicsSettingsValidationResult validation = impl->settings.Validate();
		if(!validation.accepted) {
			// Invalid settings are never allowed to weaken the calibrated defaults.
			impl = std::make_unique<Impl>(PhysicsSettings::Defaults());
		}
		impl->diagnostics.maximumLiveQueryCount = impl->maximumLiveQueryCount;
	}

	PhysicsQueryManager::~PhysicsQueryManager() = default;

	PhysicsQueryReceipt PhysicsQueryManager::Submit(PhysicsQueryRequest request, PhysicsWorldHandle activeWorld, PhysicsTick currentTick) {
		const PhysicsQueryBatchSubmission batch = SubmitBatch(std::span<const PhysicsQueryRequest>(&request, 1U), activeWorld, currentTick);
		if(batch.accepted && !batch.receipts.empty()) {
			return batch.receipts.front();
		}
		return PhysicsQueryReceipt{false, {}, batch.rejectionReason, "Query submission was rejected before allocation."};
	}

	PhysicsQueryBatchSubmission PhysicsQueryManager::SubmitBatch(
		std::span<const PhysicsQueryRequest> requests,
		PhysicsWorldHandle activeWorld,
		PhysicsTick currentTick) {
		PhysicsQueryBatchSubmission submission;
		if(requests.empty()) {
			submission.rejectionReason = PhysicsQueryRejectionReason::InvalidRequest;
			return submission;
		}
		if(requests.size() > impl->settings.maxRequestsPerBatch) {
			submission.rejectionReason = PhysicsQueryRejectionReason::BatchLimit;
			return submission;
		}

		for(const PhysicsQueryRequest& request : requests) {
			const Validation validation = ValidateRequest(request, impl->settings, activeWorld, currentTick);
			if(!validation.accepted) {
				submission.rejectionReason = validation.reason;
				return submission;
			}
		}

		std::lock_guard<std::mutex> lock(impl->mutex);
		const std::size_t allocatableSlots = impl->freeSlots.size() + (impl->maximumLiveQueryCount - impl->slots.size());
		if(requests.size() > allocatableSlots) {
			++impl->diagnostics.rejectedLimitCount;
			submission.rejectionReason = PhysicsQueryRejectionReason::BatchLimit;
			return submission;
		}
		std::size_t queuedOrPrepared = 0U;
		for(const Impl::Slot& slot : impl->slots) {
			queuedOrPrepared += slot.state == QueryState::Queued || slot.state == QueryState::Prepared ? 1U : 0U;
		}
		if(queuedOrPrepared + requests.size() > impl->settings.maxRequestsPerBatch) {
			++impl->diagnostics.rejectedLimitCount;
			submission.rejectionReason = PhysicsQueryRejectionReason::BatchLimit;
			return submission;
		}

		submission.accepted = true;
		submission.rejectionReason = PhysicsQueryRejectionReason::None;
		submission.receipts.reserve(requests.size());
		for(PhysicsQueryRequest request : requests) {
			PhysicsSlotIndex index = kInvalidPhysicsSlot;
			if(!impl->freeSlots.empty()) {
				index = impl->freeSlots.back();
				impl->freeSlots.pop_back();
				Impl::Slot& slot = impl->slots[index];
				slot.generation = NextGeneration(slot.generation);
				slot.state = QueryState::Queued;
				slot.result.reset();
				slot.request = std::move(request);
			} else {
				if(impl->slots.size() >= impl->maximumLiveQueryCount) {
					// The preflight above keeps this defensive branch unreachable unless
					// a caller changes settings concurrently in a future integration.
					submission.accepted = false;
					submission.rejectionReason = PhysicsQueryRejectionReason::BatchLimit;
					++impl->diagnostics.rejectedLimitCount;
					return submission;
				}
				index = static_cast<PhysicsSlotIndex>(impl->slots.size());
				impl->slots.push_back(Impl::Slot{1U, QueryState::Queued, std::move(request), std::nullopt});
			}

			Impl::Slot& slot = impl->slots[index];
			const PhysicsTick defaultExpiry = currentTick + impl->settings.retainedResultTicks;
			if(slot.request.expiryTick == 0U) {
				slot.request.expiryTick = defaultExpiry;
			}
			slot.request.submittedTick = currentTick;
			const PhysicsQueryHandle handle{activeWorld.worldId, activeWorld.worldGeneration, index, slot.generation,
				currentTick, slot.request.expiryTick};
			submission.receipts.push_back(PhysicsQueryReceipt{true, handle, PhysicsQueryRejectionReason::None, {}});
			++impl->diagnostics.acceptedCount;
		}

		std::size_t live = impl->slots.size() - impl->freeSlots.size();
		impl->diagnostics.peakLiveQueryCount = std::max(impl->diagnostics.peakLiveQueryCount, live);
		impl->diagnostics.queuedDepth = queuedOrPrepared + requests.size();
		return submission;
	}

	bool PhysicsQueryManager::Cancel(PhysicsQueryHandle handle, PhysicsWorldHandle activeWorld) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		if(!IsSameWorld({handle.worldId, handle.worldGeneration}, activeWorld)
			|| handle.resultSlot >= impl->slots.size()) {
			return false;
		}
		Impl::Slot& slot = impl->slots[handle.resultSlot];
		if(slot.generation != handle.resultGeneration || (slot.state != QueryState::Queued && slot.state != QueryState::Prepared)) {
			return false;
		}
		slot.state = QueryState::Cancelled;
		slot.result = MakeResult(handle, activeWorld, handle.submittedTick, PhysicsResultStatus::Cancelled,
			PhysicsQueryRejectionReason::Cancelled, slot.request.correlationId, "Query was cancelled before publication.");
		++impl->diagnostics.cancelledCount;
		return true;
	}

	PhysicsPreparedQueryBatch PhysicsQueryManager::Prepare(PhysicsWorldHandle activeWorld, PhysicsTick currentTick) {
		PhysicsPreparedQueryBatch batch;
		batch.world = activeWorld;
		batch.tick = currentTick;
		batch.queries.reserve(impl->settings.maxRequestsPerBatch);
		std::lock_guard<std::mutex> lock(impl->mutex);
		for(PhysicsSlotIndex index = 0U; index < impl->slots.size(); ++index) {
			Impl::Slot& slot = impl->slots[index];
			if(slot.state != QueryState::Queued) {
				continue;
			}
			const PhysicsQueryHandle handle{activeWorld.worldId, activeWorld.worldGeneration, index, slot.generation,
				slot.request.submittedTick, slot.request.expiryTick};
			if(!batch.frame.IsValid()) batch.frame = slot.request.frame;
			if(!IsSameWorld(slot.request.world, activeWorld)) {
				slot.state = QueryState::Completed;
				slot.result = MakeResult(handle, activeWorld, currentTick, PhysicsResultStatus::StaleTarget,
					PhysicsQueryRejectionReason::StaleWorld, slot.request.correlationId, "Queued query belongs to a stale world generation.");
				++impl->diagnostics.staleWorldCount;
				batch.rejected.push_back(*slot.result);
				continue;
			}
			if(currentTick >= slot.request.expiryTick) {
				slot.state = QueryState::Expired;
				slot.result = MakeResult(handle, activeWorld, currentTick, PhysicsResultStatus::Expired,
					PhysicsQueryRejectionReason::Expired, slot.request.correlationId, "Query expired before execution.");
				++impl->diagnostics.expiredCount;
				batch.rejected.push_back(*slot.result);
				continue;
			}
			slot.state = QueryState::Prepared;
			batch.reservedResultHitCapacity += slot.request.maximumHits;
			batch.queries.push_back(PhysicsPreparedQuery{handle, slot.request});
		}
		impl->diagnostics.queuedDepth = 0U;
		impl->diagnostics.preparedDepth = batch.queries.size();
		return batch;
	}

	bool PhysicsQueryManager::Complete(
		const PhysicsPreparedQuery& prepared,
		PhysicsWorldHandle activeWorld,
		PhysicsTick completionTick,
		PhysicsResultStatus status,
		std::span<const PhysicsQueryHit> candidates,
		bool providerTruncated,
		std::string diagnostic) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		const PhysicsQueryHandle handle = prepared.handle;
		if(!IsSameWorld({handle.worldId, handle.worldGeneration}, activeWorld)
			|| handle.resultSlot >= impl->slots.size()) {
			return false;
		}
		Impl::Slot& slot = impl->slots[handle.resultSlot];
		if(slot.generation != handle.resultGeneration || slot.state != QueryState::Prepared) {
			return false;
		}
		if(completionTick >= slot.request.expiryTick) {
			slot.state = QueryState::Expired;
			slot.result = MakeResult(handle, activeWorld, completionTick, PhysicsResultStatus::Expired,
				PhysicsQueryRejectionReason::Expired, slot.request.correlationId, "Query expired before result publication.");
			++impl->diagnostics.expiredCount;
			return false;
		}

		bool truncated = providerTruncated;
		std::vector<PhysicsQueryHit> canonical;
		if(status == PhysicsResultStatus::Success) {
			canonical = Canonicalize(candidates, slot.request, truncated);
		}
		PhysicsQueryResult result = MakeResult(handle, activeWorld, completionTick, status,
			status == PhysicsResultStatus::Cancelled ? PhysicsQueryRejectionReason::Cancelled : PhysicsQueryRejectionReason::None,
			slot.request.correlationId, std::move(diagnostic));
		result.truncated = truncated;
		result.hits = std::make_shared<const std::vector<PhysicsQueryHit>>(std::move(canonical));
		slot.state = QueryState::Completed;
		slot.result = std::move(result);
		++impl->diagnostics.completedCount;
		impl->diagnostics.preparedDepth = impl->diagnostics.preparedDepth > 0U ? impl->diagnostics.preparedDepth - 1U : 0U;
		impl->diagnostics.retainedResultCount = 0U;
		std::size_t retainedHits = 0U;
		for(const Impl::Slot& retained : impl->slots) {
			if(retained.state == QueryState::Completed && retained.result.has_value()) {
				++impl->diagnostics.retainedResultCount;
				retainedHits += retained.result->hits->size();
			}
		}
		impl->diagnostics.peakRetainedHitCount = std::max(impl->diagnostics.peakRetainedHitCount, retainedHits);
		return true;
	}

	PhysicsQueryResult PhysicsQueryManager::Read(PhysicsQueryHandle handle, PhysicsWorldHandle activeWorld, PhysicsTick currentTick) const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		if(!IsSameWorld({handle.worldId, handle.worldGeneration}, activeWorld)
			|| handle.resultSlot >= impl->slots.size()) {
			return MakeResult(handle, activeWorld, currentTick, PhysicsResultStatus::StaleTarget,
				PhysicsQueryRejectionReason::StaleHandle, {}, "Query handle does not belong to the active world generation.");
		}
		const Impl::Slot& slot = impl->slots[handle.resultSlot];
		if(slot.generation != handle.resultGeneration || slot.state == QueryState::Free) {
			return MakeResult(handle, activeWorld, currentTick, PhysicsResultStatus::StaleTarget,
				PhysicsQueryRejectionReason::StaleHandle, {}, "Query handle generation has been retired or reused.");
		}
		if(currentTick >= handle.expiryTick || slot.state == QueryState::Expired) {
			return MakeResult(handle, activeWorld, currentTick, PhysicsResultStatus::Expired,
				PhysicsQueryRejectionReason::Expired, slot.request.correlationId, "Query result has expired.");
		}
		if(slot.result.has_value()) {
			return *slot.result;
		}
		return MakeResult(handle, activeWorld, currentTick, PhysicsResultStatus::Accepted,
			PhysicsQueryRejectionReason::ResultUnavailable, slot.request.correlationId, "Query result is not published yet.");
	}

	void PhysicsQueryManager::Expire(PhysicsWorldHandle activeWorld, PhysicsTick currentTick) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		for(PhysicsSlotIndex index = 0U; index < impl->slots.size(); ++index) {
			Impl::Slot& slot = impl->slots[index];
			if(slot.state == QueryState::Free) {
				continue;
			}
			if(!IsSameWorld(slot.request.world, activeWorld) || currentTick >= slot.request.expiryTick) {
				if(slot.state != QueryState::Expired) {
					++impl->diagnostics.expiredCount;
				}
				slot.state = QueryState::Free;
				slot.result.reset();
				slot.request = {};
				impl->freeSlots.push_back(index);
			}
		}
		impl->diagnostics.queuedDepth = 0U;
		impl->diagnostics.preparedDepth = 0U;
		impl->diagnostics.retainedResultCount = 0U;
		for(const Impl::Slot& slot : impl->slots) {
			impl->diagnostics.queuedDepth += slot.state == QueryState::Queued ? 1U : 0U;
			impl->diagnostics.preparedDepth += slot.state == QueryState::Prepared ? 1U : 0U;
			impl->diagnostics.retainedResultCount += slot.state == QueryState::Completed ? 1U : 0U;
		}
	}

	void PhysicsQueryManager::ResetForWorld(PhysicsWorldHandle activeWorld) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		impl->slots.clear();
		impl->freeSlots.clear();
		impl->diagnostics.queuedDepth = 0U;
		impl->diagnostics.preparedDepth = 0U;
		impl->diagnostics.retainedResultCount = 0U;
		(void)activeWorld; // The caller owns world lifecycle; this resets all stale generations atomically.
	}

	PhysicsQueryDiagnostics PhysicsQueryManager::GetDiagnostics() const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->diagnostics;
	}

	PhysicsQueryHitDedupKey DedupKey(const PhysicsQueryHit& hit) noexcept {
		return PhysicsQueryHitDedupKey{hit.body, hit.objectId, hit.objectGeneration, hit.subShapeKey};
	}

	bool IsStableHitOrder(const PhysicsQueryHit& left, const PhysicsQueryHit& right) noexcept {
		if(left.distance != right.distance) return left.distance < right.distance;
		if(left.fraction != right.fraction) return left.fraction < right.fraction;
		// Public ordering follows stable authored identity before compact runtime
		// slots. Slot reuse therefore cannot reorder otherwise equal hits.
		return std::tie(left.objectId, left.objectGeneration, left.subShapeKey,
			left.body.worldId, left.body.worldGeneration, left.body.slotIndex, left.body.slotGeneration)
			< std::tie(right.objectId, right.objectGeneration, right.subShapeKey,
				right.body.worldId, right.body.worldGeneration, right.body.slotIndex, right.body.slotGeneration);
	}

	std::string_view ToString(PhysicsQueryRejectionReason reason) {
		switch(reason) {
		case PhysicsQueryRejectionReason::None: return "None";
		case PhysicsQueryRejectionReason::InvalidRequest: return "InvalidRequest";
		case PhysicsQueryRejectionReason::BatchLimit: return "BatchLimit";
		case PhysicsQueryRejectionReason::StaleWorld: return "StaleWorld";
		case PhysicsQueryRejectionReason::Expired: return "Expired";
		case PhysicsQueryRejectionReason::Cancelled: return "Cancelled";
		case PhysicsQueryRejectionReason::StaleHandle: return "StaleHandle";
		case PhysicsQueryRejectionReason::ResultUnavailable: return "ResultUnavailable";
		}
		return "Unknown";
	}
}
