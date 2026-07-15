#include "PhysicsAuthoringGeneration.h"

#include <algorithm>

namespace TerraEngine::Physics {
	std::optional<PhysicsAuthoringGeneration> PhysicsAuthoringGeneration::Create(
		PhysicsAuthoringGenerationData value) {
		if(!value.world.IsValid() || value.generation == 0U
			|| value.snapshotGeneration == 0U || value.settingsHash == 0U) {
			return std::nullopt;
		}

		std::sort(value.changes.begin(), value.changes.end(),
			[](const PhysicsAuthoringChange& left, const PhysicsAuthoringChange& right) {
				return left.objectId < right.objectId;
			});
		for(std::size_t index = 0U; index < value.changes.size(); ++index) {
			const PhysicsAuthoringChange& change = value.changes[index];
			if(change.objectId == TerraEngine::EMPTY_UUID
				|| change.reasons == PhysicsAuthoringChangeReason::None
				|| (index > 0U && value.changes[index - 1U].objectId == change.objectId)) {
				return std::nullopt;
			}
			const bool removed = HasAnyReason(change.reasons, PhysicsAuthoringChangeReason::Removed);
			if(removed != (change.descriptorIndex == kInvalidPhysicsDescriptorIndex)
				|| (!removed && change.objectGeneration == 0U)) {
				return std::nullopt;
			}
		}

		std::sort(value.assetObservations.begin(), value.assetObservations.end(),
			[](const PhysicsAuthoringAssetObservation& left,
				const PhysicsAuthoringAssetObservation& right) {
				if(left.assetKind != right.assetKind) return left.assetKind < right.assetKind;
				return left.assetId < right.assetId;
			});
		for(std::size_t index = 0U; index < value.assetObservations.size(); ++index) {
			const PhysicsAuthoringAssetObservation& observation = value.assetObservations[index];
			if(observation.assetId == TerraEngine::EMPTY_UUID || observation.generation == 0U
				|| observation.generation > value.assetJournalGeneration
				|| (index > 0U
					&& value.assetObservations[index - 1U].assetKind == observation.assetKind
					&& value.assetObservations[index - 1U].assetId == observation.assetId)) {
				return std::nullopt;
			}
		}

		return PhysicsAuthoringGeneration(
			std::make_shared<const PhysicsAuthoringGenerationData>(std::move(value)));
	}

	const PhysicsAuthoringChange* PhysicsAuthoringGeneration::Find(
		const TerraEngine::UUID& objectId) const noexcept {
		if(data == nullptr) return nullptr;
		const auto found = std::lower_bound(data->changes.begin(), data->changes.end(), objectId,
			[](const PhysicsAuthoringChange& change, const TerraEngine::UUID& id) {
				return change.objectId < id;
			});
		return found != data->changes.end() && found->objectId == objectId ? &*found : nullptr;
	}

	std::vector<PhysicsAuthoringChange> PhysicsAuthoringGeneration::ChangesWithReason(
		PhysicsAuthoringChangeReason mask) const {
		std::vector<PhysicsAuthoringChange> matches;
		if(data == nullptr || mask == PhysicsAuthoringChangeReason::None) return matches;
		for(const PhysicsAuthoringChange& change : data->changes) {
			if(HasAnyReason(change.reasons, mask)) matches.push_back(change);
		}
		return matches;
	}
}
