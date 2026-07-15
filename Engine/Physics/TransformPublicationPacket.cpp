// TransformPublicationPacket.cpp - changed-only packet conversion and receipts.

#include "TransformPublicationPacket.h"

#include <algorithm>
#include <unordered_set>

namespace TerraEngine::Physics {
	namespace {
		glm::vec3 ToGlm(const PhysicsVector3& value)
		{
			return {value.x, value.y, value.z};
		}

		glm::quat ToGlm(const PhysicsQuaternion& value)
		{
			return {value.w, value.x, value.y, value.z};
		}

		bool HasValidBounds(const TransformPublicationBoundsChange& change)
		{
			return change.expectedSpatialRevision != 0U
				&& change.previousBounds.IsUsable()
				&& change.changedBounds.IsUsable()
				&& Scene::IsFinite(change.previousBounds.min)
				&& Scene::IsFinite(change.previousBounds.max)
				&& Scene::IsFinite(change.changedBounds.min)
				&& Scene::IsFinite(change.changedBounds.max);
		}
	}

	TransformPublicationPacket PrepareTransformPublicationPacket(
		CompleteBodySnapshotIdentity snapshot,
		std::uint64_t sceneGeneration,
		std::uint64_t hierarchyGeneration,
		std::uint64_t publicationGroupId,
		std::uint64_t packetGeneration,
		std::span<const TransformPublicationSourceRecord> source,
		std::size_t chunkSize,
		TransformPublicationVisibilityBoundary targetBoundary)
	{
		TransformPublicationPacket packet;
		packet.snapshot = snapshot;
		packet.sceneGeneration = sceneGeneration;
		packet.hierarchyGeneration = hierarchyGeneration;
		packet.publicationGroupId = publicationGroupId;
		packet.packetGeneration = packetGeneration;
		packet.targetBoundary = targetBoundary;
		packet.entries.reserve(source.size());
		for (const TransformPublicationSourceRecord& record : source)
		{
			// Complete snapshots can contain thousands of retained sleeping/static
			// records. Only actual changes or explicit consumer requests pay mutation cost.
			if (!record.poseChanged && !record.explicitlyRequired)
			{
				continue;
			}
			packet.entries.push_back(TransformPublicationEntry{
				.objectId = record.objectId,
				.body = record.body,
				.objectLifetimeGeneration = record.objectLifetimeGeneration,
				.bodyGeneration = record.bodyGeneration,
				.componentGeneration = record.componentGeneration,
				.expectedTransformRevision = record.expectedTransformRevision,
				.expectedHierarchyGeneration = record.expectedHierarchyGeneration,
				.parentObjectId = record.parentObjectId,
				.expectedParentLifetimeGeneration = record.expectedParentLifetimeGeneration,
				.expectedParentTransformRevision = record.expectedParentTransformRevision,
				.localPose = record.localPose,
				.worldPoseReceipt = record.worldPoseReceipt,
				.boundsChange = record.boundsChange});
		}

		if (chunkSize == 0U) chunkSize = 1U;
		for (std::size_t first = 0U, chunkIndex = 0U; first < packet.entries.size(); first += chunkSize, ++chunkIndex)
		{
			packet.chunks.push_back(TransformPublicationChunk{
				.chunkIndex = chunkIndex,
				.firstEntry = first,
				.entryCount = std::min(chunkSize, packet.entries.size() - first)});
		}
		return packet;
	}

	TransformPublicationValidationResult ValidateTransformPublicationPacket(const TransformPublicationPacket& packet)
	{
		TransformPublicationValidationResult result;
		if (!packet.snapshot.world.IsValid() || packet.publicationGroupId == 0U || packet.packetGeneration == 0U)
		{
			result.reason = TransformPublicationValidationReason::InvalidIdentity;
			result.message = "Publication packet identity is incomplete.";
			return result;
		}
		if (packet.snapshot.storeGeneration == 0U || packet.snapshot.snapshotGeneration == 0U
			|| packet.sceneGeneration == 0U || packet.hierarchyGeneration == 0U)
		{
			result.reason = TransformPublicationValidationReason::InvalidGeneration;
			result.message = "Publication packet generations must be nonzero.";
			return result;
		}
		if (packet.entries.empty() || packet.chunks.empty())
		{
			result.reason = TransformPublicationValidationReason::InvalidChunkLayout;
			result.message = "Changed-pose publication requires entries and chunks.";
			return result;
		}

		std::unordered_set<TerraEngine::UUID> objectIds;
		objectIds.reserve(packet.entries.size());
		for (std::size_t index = 0U; index < packet.entries.size(); ++index)
		{
			const TransformPublicationEntry& entry = packet.entries[index];
			result.entryIndex = index;
			result.objectId = entry.objectId;
			if (entry.objectId == TerraEngine::EMPTY_UUID || !entry.body.IsValid())
			{
				result.reason = TransformPublicationValidationReason::InvalidIdentity;
				result.message = "A publication entry has an invalid object or body identity.";
				return result;
			}
			if (!objectIds.insert(entry.objectId).second)
			{
				result.reason = TransformPublicationValidationReason::DuplicateTarget;
				result.message = "An object appears more than once in one publication tick.";
				return result;
			}
			if (entry.body.worldId != packet.snapshot.world.worldId
				|| entry.body.worldGeneration != packet.snapshot.world.worldGeneration
				|| entry.objectLifetimeGeneration == 0U || entry.bodyGeneration == 0U
				|| entry.componentGeneration == 0U || entry.expectedTransformRevision == 0U
				|| entry.body.slotGeneration != entry.bodyGeneration
				|| entry.expectedHierarchyGeneration != packet.hierarchyGeneration)
			{
				result.reason = TransformPublicationValidationReason::InvalidGeneration;
				result.message = "A publication entry generation does not match its complete snapshot.";
				return result;
			}
			if (entry.parentObjectId == TerraEngine::EMPTY_UUID
				&& (entry.expectedParentLifetimeGeneration != 0U || entry.expectedParentTransformRevision != 0U))
			{
				result.reason = TransformPublicationValidationReason::InvalidGeneration;
				result.message = "A root entry cannot carry parent generations.";
				return result;
			}
			if (entry.parentObjectId != TerraEngine::EMPTY_UUID
				&& (entry.expectedParentLifetimeGeneration == 0U || entry.expectedParentTransformRevision == 0U))
			{
				result.reason = TransformPublicationValidationReason::InvalidGeneration;
				result.message = "A parent-relative entry needs complete parent generations.";
				return result;
			}
			if (!entry.localPose.IsFinite() || !entry.worldPoseReceipt.IsFinite())
			{
				result.reason = TransformPublicationValidationReason::InvalidPose;
				result.message = "A publication entry contains a nonfinite pose.";
				return result;
			}
			if (entry.boundsChange && !HasValidBounds(*entry.boundsChange))
			{
				result.reason = TransformPublicationValidationReason::InvalidBounds;
				result.message = "A changed-bounds receipt is incomplete or nonfinite.";
				return result;
			}
		}

		std::size_t expectedFirst = 0U;
		for (std::size_t index = 0U; index < packet.chunks.size(); ++index)
		{
			const TransformPublicationChunk& chunk = packet.chunks[index];
			if (chunk.chunkIndex != index || chunk.firstEntry != expectedFirst || chunk.entryCount == 0U
				|| chunk.entryCount > packet.entries.size() - chunk.firstEntry)
			{
				result.reason = TransformPublicationValidationReason::InvalidChunkLayout;
				result.message = "Publication chunks must cover the entry array once in deterministic order.";
				return result;
			}
			expectedFirst += chunk.entryCount;
		}
		if (expectedFirst != packet.entries.size())
		{
			result.reason = TransformPublicationValidationReason::InvalidChunkLayout;
			result.message = "Publication chunks do not cover every changed entry.";
			return result;
		}
		result.accepted = true;
		result.reason = TransformPublicationValidationReason::None;
		result.message = "Publication packet is structurally valid.";
		return result;
	}

	Scene::TransactionPublicationGroup BuildTransformPublicationGroup(const TransformPublicationPacket& packet)
	{
		Scene::TransactionPublicationGroup group;
		group.groupId = "physics-transform-group-" + std::to_string(packet.publicationGroupId);
		if (!ValidateTransformPublicationPacket(packet).accepted)
		{
			return group;
		}
		group.chunks.reserve(packet.chunks.size());
		for (const TransformPublicationChunk& chunk : packet.chunks)
		{
			Scene::Transaction transaction;
			transaction.id = group.groupId + "-chunk-" + std::to_string(chunk.chunkIndex);
			transaction.origin = Scene::TransactionOrigin::System;
			transaction.originName = "PhysicsTransformPublication";
			transaction.mode = Scene::TransactionMode::Commit;
			transaction.readSet.sceneGeneration = packet.sceneGeneration;
			transaction.readSet.sceneRevision = packet.hierarchyGeneration;
			transaction.budget.maxWriteCount = chunk.entryCount;
			transaction.readSet.objects.reserve(chunk.entryCount);
			Scene::TransactionWriteOperation operation;
			operation.kind = Scene::TransactionWriteKind::UpdateTransformBatch;
			operation.transformUpdates.reserve(chunk.entryCount);
			for (std::size_t offset = 0U; offset < chunk.entryCount; ++offset)
			{
				const TransformPublicationEntry& entry = packet.entries[chunk.firstEntry + offset];
				transaction.readSet.objects.push_back(Scene::TransactionObjectObservation{
					.objectId = entry.objectId,
					.lifetimeGeneration = entry.objectLifetimeGeneration,
					.componentGeneration = entry.componentGeneration,
					.hierarchyRevision = entry.expectedHierarchyGeneration,
					.transformRevision = entry.expectedTransformRevision,
					.parentObjectId = entry.parentObjectId,
					.parentLifetimeGeneration = entry.expectedParentLifetimeGeneration,
					.parentTransformRevision = entry.expectedParentTransformRevision});
				operation.transformUpdates.push_back(Scene::TransactionTransformUpdate{
					.objectId = entry.objectId,
					.position = ToGlm(entry.localPose.position),
					.rotation = ToGlm(entry.localPose.rotation)});
			}
			transaction.writeSet.operations.push_back(std::move(operation));
			group.chunks.push_back(std::move(transaction));
		}
		return group;
	}

	TransformPublicationReceipt MakeTransformPublicationReceipt(
		const TransformPublicationPacket& packet,
		const Scene::TransactionPublicationGroupResult& groupResult)
	{
		TransformPublicationReceipt receipt;
		receipt.publicationGroupId = packet.publicationGroupId;
		receipt.packetGeneration = packet.packetGeneration;
		receipt.tick = packet.snapshot.tick;
		receipt.chunkReceipts = groupResult.chunkReceipts;
		if (!groupResult.Committed())
		{
			receipt.state = groupResult.preflightAccepted
				? TransformPublicationPacketState::RolledBack
				: TransformPublicationPacketState::Rejected;
			return receipt;
		}

		// Acknowledgement is created only from the protected flush receipt. Until
		// this point PhysicsWorld must retain its previous last-published poses.
		receipt.state = TransformPublicationPacketState::Acknowledged;
		receipt.acknowledgedEntries.reserve(packet.entries.size());
		for (const TransformPublicationEntry& entry : packet.entries)
		{
			receipt.acknowledgedEntries.push_back({entry.objectId, entry.body, entry.worldPoseReceipt});
		}
		return receipt;
	}

	std::string_view ToString(TransformPublicationValidationReason reason)
	{
		switch (reason)
		{
		case TransformPublicationValidationReason::None: return "none";
		case TransformPublicationValidationReason::InvalidIdentity: return "invalid-identity";
		case TransformPublicationValidationReason::InvalidGeneration: return "invalid-generation";
		case TransformPublicationValidationReason::InvalidPose: return "invalid-pose";
		case TransformPublicationValidationReason::InvalidBounds: return "invalid-bounds";
		case TransformPublicationValidationReason::DuplicateTarget: return "duplicate-target";
		case TransformPublicationValidationReason::InvalidChunkLayout: return "invalid-chunk-layout";
		case TransformPublicationValidationReason::StaleScene: return "stale-scene";
		case TransformPublicationValidationReason::StaleHierarchy: return "stale-hierarchy";
		case TransformPublicationValidationReason::StaleObject: return "stale-object";
		case TransformPublicationValidationReason::StaleBody: return "stale-body";
		case TransformPublicationValidationReason::StaleTransform: return "stale-transform";
		case TransformPublicationValidationReason::StaleParent: return "stale-parent";
		}
		return "unknown";
	}
}
