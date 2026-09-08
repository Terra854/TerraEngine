// WorldRegion.h - sparse exact-key residency contracts for unbounded scenes.
//
// Region keys are derived from canonical positions, never stored as a second
// position authority. The directory is persistent metadata; resident snapshots
// are immutable value packets that may safely cross worker boundaries.

#pragma once

#include "Core/Export.h"
#include "Scene/SpatialQuery.h"
#include "Scene/WorldCoordinate.h"
#include "Utilities/UUID.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace TerraEngine::Scene
{
	class SpatialIndex;
	inline constexpr double kWorldRegionEdgeMetres = 8'192.0;
	inline constexpr std::int64_t kWorldRegionsPerCellAxis = 8;
	inline constexpr std::size_t kMaximumWorldRegionCoverCount = 64U;
	inline constexpr std::string_view kTerraRegionUnboundedFormat = "TerraRegionUnboundedV1";

	struct WorldRegionKey
	{
		std::int64_t x = 0;
		std::int64_t y = 0;
		std::int64_t z = 0;

		[[nodiscard]] constexpr bool operator==(const WorldRegionKey&) const = default;
		[[nodiscard]] constexpr bool operator<(const WorldRegionKey& other) const
		{
			return x != other.x ? x < other.x : (y != other.y ? y < other.y : z < other.z);
		}
	};

	struct WorldRegionKeyHash
	{
		[[nodiscard]] std::size_t operator()(const WorldRegionKey& key) const noexcept;
	};

	enum class WorldRegionState : std::uint8_t
	{
		Unloaded,
		Requested,
		Loading,
		Prepared,
		Resident,
		EvictionPending,
		Failed
	};

	struct WorldRegionLease
	{
		std::string leaseId;
		std::uint64_t generation = 0;
		std::uint64_t expiresAtFrame = 0;
		std::uint64_t expiresAtTimeNanoseconds = 0;
	};

	struct WorldRegionDirectoryEntry
	{
		WorldRegionKey key{};
		std::filesystem::path artifactPath;
		std::string contentHash;
		std::uint64_t revision = 0;
		std::uint32_t objectCount = 0;
		WorldRegionState state = WorldRegionState::Unloaded;
		std::string lastFailure;
		std::vector<WorldRegionLease> leases;
	};

	// A region artifact is independently hash-addressed.  The explicit source
	// generation prevents a save from combining records captured from two live
	// residency publications.
	struct WorldRegionArtifactManifest
	{
		std::string format = std::string(kTerraRegionUnboundedFormat);
		WorldRegionKey key{};
		std::string contentHash;
		std::uint64_t sourceGeneration = 0U;
	};

	struct WorldRegionObjectRecord
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		WorldPose worldPose{};
		std::vector<TerraEngine::UUID> assetIds;
		std::vector<WorldRegionKey> coverRegions;
		bool coarseProxy = false;
	};

	struct OversizedSpatialProxy
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		WorldRegionKey owner{};
		WorldPosition canonicalOwner{};
		SpatialAABB localBounds{};
		std::uint64_t frameGeneration = 0;
	};

	struct WorldRegionSnapshot
	{
		WorldRegionKey key{};
		std::uint64_t directoryRevision = 0;
		std::uint64_t residencyGeneration = 0;
		std::string contentHash;
		std::string correlationId;
		SpatialReferenceFrame referenceFrame{};
		std::vector<WorldRegionObjectRecord> objects;
		std::vector<SpatialObjectRecord> spatialRecords;
		// Static and movable accelerators are immutable after worker preparation.
		// Keeping them distinct lets a region retirement discard only resident
		// working-set data without any directory-wide spatial rebuild.
		std::shared_ptr<const SpatialIndex> staticSpatialIndex;
		std::shared_ptr<const SpatialIndex> movableSpatialIndex;
		std::vector<OversizedSpatialProxy> coarseProxies;
	};

	struct PublishedWorldRegionResidency
	{
		std::uint64_t generation = 0;
		std::unordered_map<WorldRegionKey, std::shared_ptr<const WorldRegionSnapshot>, WorldRegionKeyHash> residentRegions;
		std::vector<OversizedSpatialProxy> coarseProxies;
		// Keys are published before their old snapshots become reclaimable. Readers
		// retaining the previous root can finish safely while new requests fail closed.
		std::vector<WorldRegionKey> invalidatedRegions;
		std::uint32_t residentObjectCount = 0;
	};

	class TERRAENGINE_API WorldRegionDirectory
	{
	public:
		[[nodiscard]] bool Upsert(WorldRegionDirectoryEntry entry);
		[[nodiscard]] bool SetHomeRegion(const TerraEngine::UUID& objectId, WorldRegionKey key);
		[[nodiscard]] bool Transition(WorldRegionKey key, WorldRegionState next, std::string failure = {});
		[[nodiscard]] bool AcquireLease(WorldRegionKey key, WorldRegionLease lease);
		[[nodiscard]] bool RenewLease(WorldRegionKey key, WorldRegionLease lease);
		[[nodiscard]] bool ReleaseLease(WorldRegionKey key, std::string_view leaseId, std::uint64_t generation);
		[[nodiscard]] std::size_t ExpireLeases(
			std::uint64_t currentFrame,
			std::uint64_t currentTimeNanoseconds,
			std::size_t maximumExpirations);
		[[nodiscard]] std::optional<WorldRegionDirectoryEntry> Find(WorldRegionKey key) const;
		[[nodiscard]] std::optional<WorldRegionKey> FindHomeRegion(const TerraEngine::UUID& objectId) const;
		[[nodiscard]] std::vector<WorldRegionDirectoryEntry> Entries() const;
		[[nodiscard]] std::size_t Size() const;

	private:
		mutable std::shared_mutex mutex;
		std::unordered_map<WorldRegionKey, WorldRegionDirectoryEntry, WorldRegionKeyHash> entries;
		std::unordered_map<TerraEngine::UUID, WorldRegionKey> homeRegions;
	};

	[[nodiscard]] TERRAENGINE_API WorldCoordinateError WorldPositionToRegionKey(const WorldPosition& position, WorldRegionKey& key);
	[[nodiscard]] TERRAENGINE_API WorldPosition WorldRegionCanonicalOrigin(WorldRegionKey key);
	[[nodiscard]] TERRAENGINE_API bool IsValidWorldRegionTransition(WorldRegionState from, WorldRegionState to);
	[[nodiscard]] TERRAENGINE_API std::string_view ToString(WorldRegionState state);
	[[nodiscard]] TERRAENGINE_API bool IsWorldRegionEntryValid(const WorldRegionDirectoryEntry& entry);
	[[nodiscard]] TERRAENGINE_API nlohmann::json SerializeWorldRegionArtifactManifest(const WorldRegionArtifactManifest& manifest);
	[[nodiscard]] TERRAENGINE_API std::optional<WorldRegionArtifactManifest> DeserializeWorldRegionArtifactManifest(const nlohmann::json& value, std::string* outError = nullptr);
}
