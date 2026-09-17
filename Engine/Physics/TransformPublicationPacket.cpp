// TransformPublicationPacket.cpp - changed-only packet conversion and receipts.

#include "TransformPublicationPacket.h"

#include <algorithm>
#include <unordered_set>

namespace TerraEngine::Physics {
	namespace {
		Scene::WorldPose ToCanonicalPose(
			const Scene::WorldPosition& frameOrigin,
			const PhysicsPose& localPose)
		{
			// Physics providers emit bounded frame-local poses. At this boundary they
			// become a typed canonical pose value; frame migration supplies the cell
			// origin before a distant world can publish this packet.
			Scene::WorldDelta delta;
			delta.offsetMetres = {
				static_cast<double>(localPose.position.x),
				static_cast<double>(localPose.position.y),
				static_cast<double>(localPose.position.z)};
			Scene::WorldPose result;
			const Scene::WorldCoordinateResult translated = Scene::ApplyWorldDelta(frameOrigin, delta);
			if (translated.Succeeded())
			{
				result.position = translated.position;
			}
			result.rotation = {localPose.rotation.w, localPose.rotation.x,
				localPose.rotation.y, localPose.rotation.z};
			return result;
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
				.authority = record.authority,
				.body = record.body,
				.character = record.character,
				.frame = record.frame,
				.objectLifetimeGeneration = record.objectLifetimeGeneration,
				.bodyGeneration = record.bodyGeneration,
				.componentGeneration = record.componentGeneration,
				.expectedTransformRevision = record.expectedTransformRevision,
				.expectedHierarchyGeneration = record.expectedHierarchyGeneration,
				.parentObjectId = record.parentObjectId,
				.expectedParentLifetimeGeneration = record.expectedParentLifetimeGeneration,
				.expectedParentTransformRevision = record.expectedParentTransformRevision,
				.canonicalFrameOrigin = record.canonicalFrameOrigin,
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
			const bool validAuthority = (entry.authority == TransformPublicationAuthority::Body
				&& entry.body.IsValid() && !entry.character.IsValid())
				|| (entry.authority == TransformPublicationAuthority::Character
					&& entry.character.IsValid() && !entry.body.IsValid());
			if (entry.objectId == TerraEngine::EMPTY_UUID || !validAuthority)
			{
				result.reason = TransformPublicationValidationReason::InvalidIdentity;
				result.message = "A publication entry has an invalid or mixed provider authority identity.";
				return result;
			}
			if (!objectIds.insert(entry.objectId).second)
			{
				result.reason = TransformPublicationValidationReason::DuplicateTarget;
				result.message = "An object appears more than once in one publication tick.";
				return result;
			}
			if (entry.frame.IsValid() && entry.frame != packet.snapshot.frame)
			{
				result.reason = TransformPublicationValidationReason::InvalidGeneration;
				result.message = "A publication entry belongs to a stale physics frame generation.";
				return result;
			}
			const PhysicsWorldId authorityWorldId = entry.authority == TransformPublicationAuthority::Body
				? entry.body.worldId : entry.character.worldId;
			const PhysicsGeneration authorityWorldGeneration = entry.authority == TransformPublicationAuthority::Body
				? entry.body.worldGeneration : entry.character.worldGeneration;
			const PhysicsGeneration authoritySlotGeneration = entry.authority == TransformPublicationAuthority::Body
				? entry.body.slotGeneration : entry.character.slotGeneration;
			if (authorityWorldId != packet.snapshot.world.worldId
				|| authorityWorldGeneration != packet.snapshot.world.worldGeneration
				|| entry.objectLifetimeGeneration == 0U || entry.bodyGeneration == 0U
				|| entry.componentGeneration == 0U || entry.expectedTransformRevision == 0U
				|| authoritySlotGeneration != entry.bodyGeneration
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
			if (!entry.localPose.IsFinite() || !entry.worldPoseReceipt.IsFinite()
				|| !Scene::NormalizeWorldPosition(entry.canonicalFrameOrigin).Succeeded())
			{
				result.reason = TransformPublicationValidationReason::InvalidPose;
				result.message = "A publication entry contains a nonfinite pose.";
				return result;
			}
			Scene::WorldDelta localDelta;
			localDelta.offsetMetres = {static_cast<double>(entry.localPose.position.x),
				static_cast<double>(entry.localPose.position.y), static_cast<double>(entry.localPose.position.z)};
			if (!Scene::ApplyWorldDelta(entry.canonicalFrameOrigin, localDelta).Succeeded())
			{
				result.reason = TransformPublicationValidationReason::InvalidPose;
				result.message = "A frame-local physics pose cannot be localized to its canonical origin.";
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
					.pose = ToCanonicalPose(entry.canonicalFrameOrigin, entry.localPose),
					// Body and character simulation publish rigid poses only; the mesh's
					// authored scale must survive initialization and every later step.
					.preserveScale = true});
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
			receipt.acknowledgedEntries.push_back({entry.objectId, entry.authority,
				entry.body, entry.character, entry.worldPoseReceipt});
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
