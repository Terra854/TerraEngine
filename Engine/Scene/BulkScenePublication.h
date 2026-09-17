// BulkScenePublication.h - one-boundary prepared scene replacement contract.
//
// Worker code constructs BulkScenePublicationPacket from immutable JSON records.
// Only PublishBulkScenePacket touches ObjectManager, and callers must invoke it
// from their declared protected scheduler boundary.

#pragma once

#include "Core/Export.h"
#include "Scene/Serializer.h"
#include "Scene/WorldRegion.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class ObjectManager;

namespace TerraEngine::Scene {
	struct StableBulkObjectIdentity {
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		WorldRegionKey homeRegion{};
	};

	struct BulkScenePublicationPacket {
		std::uint64_t expectedSceneGeneration = 0U;
		std::uint64_t preparedGeneration = 0U;
		std::uint64_t reservedObjectCount = 0U;
		std::uint64_t reservedComponentCount = 0U;
		std::uint64_t reservedHierarchyCount = 0U;
		bool cancelled = false;
		// Rejected detached decodes carry only their generic schema reason. They
		// never carry partial objects into the protected publication boundary.
		std::string rejectionReason;
		std::vector<PreparedSceneObjectRecord> objects;
		// Stable identity is validated before the protected materialization loop.
		// This prevents a worker packet from replacing a live UUID with a new one.
		std::vector<StableBulkObjectIdentity> stableIdentities;
	};

	struct BulkScenePublicationPreflight {
		bool accepted = false;
		std::string reason;
	};

	struct BulkScenePublicationResult {
		bool published = false;
		bool rolledBack = false;
		bool cancelled = false;
		std::uint64_t publishedSceneGeneration = 0U;
		std::string reason;
	};

	TERRAENGINE_API BulkScenePublicationPacket BuildBulkScenePublicationPacket(
		const PreparedSceneDecode& prepared,
		std::uint64_t expectedSceneGeneration,
		std::uint64_t preparedGeneration);
	TERRAENGINE_API BulkScenePublicationPreflight PreflightBulkScenePublication(
		const BulkScenePublicationPacket& packet,
		const ObjectManager& objects);
	TERRAENGINE_API BulkScenePublicationResult PublishBulkScenePacket(
		const BulkScenePublicationPacket& packet,
		ObjectManager& objects,
		const std::function<bool()>& cancellationRequested = {});
	TERRAENGINE_API BulkScenePublicationPacket BuildRegionBulkScenePublicationPacket(
		const PreparedSceneDecode& prepared,
		WorldRegionKey homeRegion,
		std::uint64_t expectedSceneGeneration,
		std::uint64_t preparedGeneration);

}
