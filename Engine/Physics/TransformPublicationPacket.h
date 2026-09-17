// TransformPublicationPacket.h - worker-prepared changed-pose publication.
//
// Provider state is converted into these engine-owned values before a protected
// scene boundary. Chunks bound worker scratch use, while publicationGroupId keeps
// every chunk from one simulation tick under one atomic commit decision.

#pragma once

#include "Core/Export.h"
#include "PhysicsTypes.h"
#include "Scene/SpatialBounds.h"
#include "Scene/TransactionalMutation.h"
#include "Utilities/UUID.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Physics {
	enum class TransformPublicationVisibilityBoundary : std::uint8_t {
		AfterPhysics,
		FrameEnd
	};

	enum class TransformPublicationPacketState : std::uint8_t {
		Prepared,
		PreflightAccepted,
		Committed,
		Acknowledged,
		Rejected,
		RolledBack
	};

	// This identity proves that complete query state and changed publication came
	// from the same immutable state-store view, even though their sizes differ.
	struct CompleteBodySnapshotIdentity {
		PhysicsWorldHandle world;
		PhysicsFrameHandle frame;
		PhysicsTick tick = 0;
		std::uint64_t storeGeneration = 0;
		std::uint64_t snapshotGeneration = 0;
	};

	struct TransformPublicationBoundsChange {
		Scene::SpatialAABB previousBounds;
		Scene::SpatialAABB changedBounds;
		std::uint64_t expectedSpatialRevision = 0;
	};

	// A Transform may be authoritative from either a simulated rigid body or a
	// standalone virtual character. Keeping the authority explicit prevents a
	// controller-only object from borrowing a fake body handle at validation.
	enum class TransformPublicationAuthority : std::uint8_t {
		Body,
		Character
	};

	// Worker conversion receives no Object or component pointer. Every generation
	// needed for protected validation travels with the compact record instead.
	struct TransformPublicationSourceRecord {
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		TransformPublicationAuthority authority = TransformPublicationAuthority::Body;
		PhysicsBodyHandle body;
		PhysicsCharacterHandle character;
		PhysicsFrameHandle frame;
		std::uint64_t objectLifetimeGeneration = 0;
		PhysicsGeneration bodyGeneration = 0;
		std::uint64_t componentGeneration = 0;
		std::uint64_t expectedTransformRevision = 0;
		std::uint64_t expectedHierarchyGeneration = 0;
		TerraEngine::UUID parentObjectId = TerraEngine::EMPTY_UUID;
		std::uint64_t expectedParentLifetimeGeneration = 0;
		std::uint64_t expectedParentTransformRevision = 0;
		// The physics provider pose is meaningful only relative to this exact
		// canonical frame origin. It is never treated as a global float position.
		Scene::WorldPosition canonicalFrameOrigin{};
		PhysicsPose localPose;
		PhysicsPose worldPoseReceipt;
		std::optional<TransformPublicationBoundsChange> boundsChange;
		bool poseChanged = false;
		bool explicitlyRequired = false;
	};

	struct TransformPublicationEntry {
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		TransformPublicationAuthority authority = TransformPublicationAuthority::Body;
		PhysicsBodyHandle body;
		PhysicsCharacterHandle character;
		PhysicsFrameHandle frame;
		std::uint64_t objectLifetimeGeneration = 0;
		PhysicsGeneration bodyGeneration = 0;
		std::uint64_t componentGeneration = 0;
		std::uint64_t expectedTransformRevision = 0;
		std::uint64_t expectedHierarchyGeneration = 0;
		TerraEngine::UUID parentObjectId = TerraEngine::EMPTY_UUID;
		std::uint64_t expectedParentLifetimeGeneration = 0;
		std::uint64_t expectedParentTransformRevision = 0;
		Scene::WorldPosition canonicalFrameOrigin{};
		PhysicsPose localPose;
		PhysicsPose worldPoseReceipt;
		std::optional<TransformPublicationBoundsChange> boundsChange;
	};

	struct TransformPublicationChunk {
		std::size_t chunkIndex = 0;
		std::size_t firstEntry = 0;
		std::size_t entryCount = 0;
	};

	struct TransformPublicationPacket {
		CompleteBodySnapshotIdentity snapshot;
		std::uint64_t sceneGeneration = 0;
		std::uint64_t hierarchyGeneration = 0;
		std::uint64_t publicationGroupId = 0;
		std::uint64_t packetGeneration = 0;
		TransformPublicationVisibilityBoundary targetBoundary = TransformPublicationVisibilityBoundary::FrameEnd;
		TransformPublicationPacketState state = TransformPublicationPacketState::Prepared;
		std::vector<TransformPublicationEntry> entries;
		std::vector<TransformPublicationChunk> chunks;
	};

	enum class TransformPublicationValidationReason : std::uint8_t {
		None,
		InvalidIdentity,
		InvalidGeneration,
		InvalidPose,
		InvalidBounds,
		DuplicateTarget,
		InvalidChunkLayout,
		StaleScene,
		StaleHierarchy,
		StaleObject,
		StaleBody,
		StaleTransform,
		StaleParent
	};

	struct TransformPublicationValidationResult {
		bool accepted = false;
		TransformPublicationValidationReason reason = TransformPublicationValidationReason::None;
		std::size_t entryIndex = 0;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		std::string message;
	};

	struct TransformPublicationEntryReceipt {
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		TransformPublicationAuthority authority = TransformPublicationAuthority::Body;
		PhysicsBodyHandle body;
		PhysicsCharacterHandle character;
		PhysicsPose worldPose;
	};

	struct TransformPublicationReceipt {
		std::uint64_t publicationGroupId = 0;
		std::uint64_t packetGeneration = 0;
		PhysicsTick tick = 0;
		TransformPublicationPacketState state = TransformPublicationPacketState::Rejected;
		std::vector<Scene::TransactionPublicationChunkReceipt> chunkReceipts;
		std::vector<TransformPublicationEntryReceipt> acknowledgedEntries;

		bool Accepted() const { return state == TransformPublicationPacketState::Acknowledged; }
	};

	TERRAENGINE_API TransformPublicationPacket PrepareTransformPublicationPacket(
		CompleteBodySnapshotIdentity snapshot,
		std::uint64_t sceneGeneration,
		std::uint64_t hierarchyGeneration,
		std::uint64_t publicationGroupId,
		std::uint64_t packetGeneration,
		std::span<const TransformPublicationSourceRecord> source,
		std::size_t chunkSize,
		TransformPublicationVisibilityBoundary targetBoundary);
	TERRAENGINE_API TransformPublicationValidationResult ValidateTransformPublicationPacket(const TransformPublicationPacket& packet);
	TERRAENGINE_API Scene::TransactionPublicationGroup BuildTransformPublicationGroup(const TransformPublicationPacket& packet);
	TERRAENGINE_API TransformPublicationReceipt MakeTransformPublicationReceipt(
		const TransformPublicationPacket& packet,
		const Scene::TransactionPublicationGroupResult& groupResult);
	TERRAENGINE_API std::string_view ToString(TransformPublicationValidationReason reason);
}
