// SceneChangePacket.h - immutable, generation-scoped scene change publication.
//
// Worker jobs receive this value after a protected scene boundary has captured
// it.  The packet owns only value data, so readers never need to touch the live
// ObjectManager or component storage while preparing incremental work.

#pragma once

#include "Core/Export.h"
#include "Core/Object/Object.h"
#include "Utilities/UUID.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class ObjectManager;

namespace TerraEngine::Scene {

	enum class SceneChangeKind : std::uint8_t {
		Added,
		Removed,
		Metadata,
		Component,
		Transform,
		Hierarchy,
		Bounds
	};

	// This is the complete generation evidence consumers need to reject stale
	// work without rebuilding an Object facade. Removed entries retain the
	// values captured before destruction, including their former parent.
	struct SceneObjectRevisionSnapshot {
		Object object;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID parentObjectId = TerraEngine::EMPTY_UUID;
		std::uint64_t lifetimeGeneration = 0;
		std::uint64_t metadataGeneration = 0;
		std::uint64_t componentGeneration = 0;
		std::uint64_t transformRevision = 0;
		std::uint64_t hierarchyRevision = 0;
		std::uint64_t spatialRevision = 0;
		std::uint32_t hierarchyDepth = 0;
		bool isStatic = false;
	};

	struct SceneChangePacketValidationResult {
		bool valid = true;
		std::string message;
	};

	// SceneChangePacket has no mutating API. Its shared storage is created only
	// by ObjectManager after protected publication, making copied packets cheap
	// and safe to retain for worker preparation.
	class TERRAENGINE_API SceneChangePacket {
	public:
		SceneChangePacket() = default;

		std::uint64_t SceneGeneration() const;
		std::uint64_t PublicationRevision() const;
		bool Empty() const;

		const std::vector<SceneObjectRevisionSnapshot>& Added() const;
		const std::vector<SceneObjectRevisionSnapshot>& Removed() const;
		const std::vector<SceneObjectRevisionSnapshot>& MetadataChanged() const;
		const std::vector<SceneObjectRevisionSnapshot>& ComponentChanged() const;
		const std::vector<SceneObjectRevisionSnapshot>& TransformChanged() const;
		const std::vector<SceneObjectRevisionSnapshot>& HierarchyChanged() const;
		const std::vector<SceneObjectRevisionSnapshot>& BoundsChanged() const;

		// Validation is intentionally usable by worker preparation. It checks
		// only packet-owned state and never resolves handles through live ECS data.
		SceneChangePacketValidationResult Validate(std::uint64_t expectedSceneGeneration = 0) const;

	private:
		struct Storage;
		explicit SceneChangePacket(std::shared_ptr<const Storage> storage);
		static SceneChangePacket Create(
			std::uint64_t sceneGeneration,
			std::uint64_t publicationRevision,
			std::vector<SceneObjectRevisionSnapshot> added,
			std::vector<SceneObjectRevisionSnapshot> removed,
			std::vector<SceneObjectRevisionSnapshot> metadataChanged,
			std::vector<SceneObjectRevisionSnapshot> componentChanged,
			std::vector<SceneObjectRevisionSnapshot> transformChanged,
			std::vector<SceneObjectRevisionSnapshot> hierarchyChanged,
			std::vector<SceneObjectRevisionSnapshot> boundsChanged);

		std::shared_ptr<const Storage> storage;

		friend class ::ObjectManager;
	};

}
