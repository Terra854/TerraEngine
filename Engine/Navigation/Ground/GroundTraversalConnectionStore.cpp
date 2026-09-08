// GroundTraversalConnectionStore.cpp - revision-qualified connection publication.

#include "GroundTraversalConnectionStore.h"

#include <algorithm>
#include <cmath>

namespace TerraEngine::Navigation::Ground
{
	namespace
	{
		bool DirectionAllows(NavigationConnectionDirection stored, NavigationConnectionDirection requested) noexcept
		{
			if(stored == NavigationConnectionDirection::Bidirectional) return true;
			return stored == requested;
		}

		bool IsValidOwner(NavigationOwnerHandle owner) noexcept
		{
			return owner.IsValid();
		}
	}

	GroundTraversalConnectionStore::GroundTraversalConnectionStore()
		: records_(std::make_shared<const std::vector<GroundTraversalConnectionRecord>>()) {}

	GroundTraversalConnectionUpsertResult GroundTraversalConnectionStore::Upsert(
		NavigationOwnerHandle owner,
		const NavigationConnectionComponent& authored,
		SupportCertificate support)
	{
		if(!IsValidOwner(owner)) return {false, 0U, "Connection owner lifetime is invalid."};
		if(!authored.Validate().accepted) return {false, 0U, "Connection authoring validation failed."};
		if(authored.connectionId == TerraEngine::EMPTY_UUID) return {false, 0U, "Connection identity is empty."};
		if(!support.endpointsSupported || !support.clearanceSupported || !std::isfinite(support.measuredClearanceMetres))
			return {false, 0U, "Connection support or clearance certificate is insufficient."};

		std::lock_guard lock(publicationMutex_);
		auto next = std::make_shared<std::vector<GroundTraversalConnectionRecord>>(*records_);
		const auto existing = std::find_if(next->begin(), next->end(), [&authored](const GroundTraversalConnectionRecord& record) {
			return record.authored.connectionId == authored.connectionId;
		});
		const std::uint64_t nextRevision = revision_ + 1U;
		GroundTraversalConnectionRecord record{owner, authored, support, nextRevision};
		if(existing == next->end()) next->push_back(std::move(record));
		else *existing = std::move(record);
		revision_ = nextRevision;
		records_ = std::const_pointer_cast<const std::vector<GroundTraversalConnectionRecord>>(next);
		return {true, revision_, {}};
	}

	bool GroundTraversalConnectionStore::Disable(const TerraEngine::UUID& connectionId)
	{
		std::lock_guard lock(publicationMutex_);
		auto next = std::make_shared<std::vector<GroundTraversalConnectionRecord>>(*records_);
		const auto existing = std::find_if(next->begin(), next->end(), [&connectionId](const GroundTraversalConnectionRecord& record) {
			return record.authored.connectionId == connectionId;
		});
		if(existing == next->end()) return false;
		existing->authored.enabled = false;
		existing->publishedRevision = ++revision_;
		records_ = std::const_pointer_cast<const std::vector<GroundTraversalConnectionRecord>>(next);
		return true;
	}

	bool GroundTraversalConnectionStore::Remove(const TerraEngine::UUID& connectionId)
	{
		std::lock_guard lock(publicationMutex_);
		auto next = std::make_shared<std::vector<GroundTraversalConnectionRecord>>(*records_);
		const auto oldSize = next->size();
		next->erase(std::remove_if(next->begin(), next->end(), [&connectionId](const GroundTraversalConnectionRecord& record) {
			return record.authored.connectionId == connectionId;
		}), next->end());
		if(next->size() == oldSize) return false;
		++revision_;
		records_ = std::const_pointer_cast<const std::vector<GroundTraversalConnectionRecord>>(next);
		return true;
	}

	GroundTraversalConnectionSnapshot GroundTraversalConnectionStore::AcquireSnapshot() const
	{
		std::lock_guard lock(publicationMutex_);
		return GroundTraversalConnectionSnapshot(records_, revision_);
	}

	const GroundTraversalConnectionRecord* GroundTraversalConnectionSnapshot::Find(const TerraEngine::UUID& connectionId) const noexcept
	{
		if(!records_) return nullptr;
		const auto iterator = std::find_if(records_->begin(), records_->end(), [&connectionId](const GroundTraversalConnectionRecord& record) {
			return record.authored.connectionId == connectionId;
		});
		return iterator == records_->end() ? nullptr : &*iterator;
	}

	bool GroundTraversalConnectionSnapshot::Contains(const TerraEngine::UUID& connectionId) const noexcept
	{
		return Find(connectionId) != nullptr;
	}

	bool GroundTraversalConnectionSnapshot::CanTraverse(const TerraEngine::UUID& connectionId,
		NavigationConnectionDirection requestedDirection, std::uint32_t capabilityMask) const noexcept
	{
		const GroundTraversalConnectionRecord* record = Find(connectionId);
		return record != nullptr && record->authored.enabled && record->authored.gameplayCondition
			&& (record->authored.capabilityMask & capabilityMask) != 0U
			&& DirectionAllows(record->authored.direction, requestedDirection)
			&& record->support.endpointsSupported && record->support.clearanceSupported;
	}

	GroundTraversalRouteResult GroundTraversalConnectionSnapshot::FindRouteUsing(const TerraEngine::UUID& connectionId,
		NavigationConnectionDirection requestedDirection, std::uint32_t capabilityMask,
		const NavigationProfile& profile) const noexcept
	{
		GroundTraversalRouteResult result{};
		result.connectionId = connectionId;
		result.sourceRevision = revision_;
		if(!profile.Validate().accepted) {
			result.outcome = NavigationOutcome::InvalidRequest;
			result.reason = NavigationReason::InvalidDestination;
			return result;
		}
		if(CanTraverse(connectionId, requestedDirection, capabilityMask)) {
			result.outcome = NavigationOutcome::Complete;
			result.reason = NavigationReason::None;
			return result;
		}
		result.outcome = Contains(connectionId) ? NavigationOutcome::UnavailableCoverage : NavigationOutcome::UnavailableCoverage;
		result.reason = NavigationReason::MissingSupport;
		return result;
	}

	GroundTraversalRouteValidation GroundTraversalRouteResult::ValidateAgainst(const GroundTraversalConnectionSnapshot& snapshot) const
	{
		if(outcome != NavigationOutcome::Complete || sourceRevision != snapshot.Revision()
			|| !snapshot.Contains(connectionId))
			return {false, NavigationReason::SceneReplaced};
		return {true, NavigationReason::None};
	}
}
