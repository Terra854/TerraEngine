// RecastGroundBuilder.h - provider-neutral, deterministic ground-tile contract.

#pragma once

#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationBudget.h"
#include "Navigation/NavigationSettings.h"
#include "Navigation/Persistence/SavedNavigationFormat.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Navigation {
	inline constexpr float kGroundTileEdgeMetres = 32.0F;
	inline constexpr float kGroundVerticalBandMetres = 128.0F;

	struct GroundTileKey {
		// These are canonical addresses, not provider header coordinates. They must
		// remain int64 so a world-cell shift cannot alias a distant tile.
		std::int64_t x = 0;
		std::int64_t z = 0;
		std::int32_t verticalBand = 0;
		std::uint64_t profileBuildKey = 0U;
		[[nodiscard]] constexpr bool operator==(const GroundTileKey&) const noexcept = default;
		[[nodiscard]] constexpr bool operator<(const GroundTileKey& other) const noexcept {
			return x != other.x ? x < other.x : z != other.z ? z < other.z : verticalBand != other.verticalBand ? verticalBand < other.verticalBand : profileBuildKey < other.profileBuildKey;
		}
	};

	enum class RecastGroundBuildStatus : std::uint8_t {
		Success, Cancelled, InvalidProfile, InvalidGeometry, LocalizationFailed,
		BudgetExceeded, RecastConfigurationFailed, RecastHeightfieldFailed,
		RecastRasterizationFailed, RecastCompactHeightfieldFailed, ProviderFailure
	};

	struct GroundTileBlob {
		GroundTileKey key{};
		// A cache retains provenance separately from the new world generation that
		// will be assigned when its imported shard is published.
		NavigationGeneration generation = 1U;
		std::vector<std::uint64_t> sourceFingerprints;
		std::vector<std::byte> pristineBytes;
		std::uint64_t checksum = 0U;
		// Empty is authoritative fully evaluated coverage with no traversable
		// polygons, and is therefore distinct from an absent provider tile.
		bool empty = false;
		bool supportComplete = false;
		bool clearanceComplete = false;
		// Retains the successful reservation for exactly the published blob lifetime.
		std::shared_ptr<NavigationBudgetReservation> budgetReservation;
	};

	struct RecastGroundBuildInput {
		NavigationProfile profile{};
		std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> sources;
		NavigationBudget* budget = nullptr;
		std::function<bool()> cancellationRequested;
		std::size_t maximumTiles = 4096U;
		// Empty means the ordinary complete source-derived build. Dynamic workers
		// may request a bounded tile subset before rasterization; all immutable
		// sources remain available for each tile's clearance/border halo.
		std::vector<GroundTileKey> requestedTiles;
	};

	struct RecastGroundBuildResult {
		RecastGroundBuildStatus status = RecastGroundBuildStatus::InvalidGeometry;
		std::string reason;
		std::vector<GroundTileBlob> tiles;
		[[nodiscard]] bool Succeeded() const noexcept { return status == RecastGroundBuildStatus::Success; }
	};

	// The same raster border and vertical extent used by BuildRecastGroundTiles.
	// Dynamic reuse expands every copied receipt by this profile-specific halo;
	// returning nullopt means the caller must use its established full rebuild.
	struct GroundTileBuildHalo {
		double horizontalMetres = 0.0;
		double verticalMetres = 0.0;
	};

	// This packet captures exactly the geometry-derived facts used to decide
	// whether a saved tile can stand in for a new Recast build. It contains no
	// provider allocation, rasterized heightfield, or mutable scene reference.
	struct GroundCoverageEvidence {
		bool accepted = false;
		std::string reason;
		std::uint64_t sourceFingerprint = 0U;
		std::uint64_t settingsFingerprint = 0U;
		std::vector<std::uint64_t> sourceFingerprints;
		std::vector<GroundTileKey> tileKeys;
	};

	struct SavedGroundTileExportResult {
		bool accepted = false;
		std::string reason;
		std::vector<SavedNavigationChunk> chunks;
	};

	struct SavedGroundTileImportResult {
		bool accepted = false;
		std::string reason;
		std::vector<GroundTileBlob> tiles;
	};

	// Returns canonical sorted tile keys and pristine owned bytes. Recast/Detour
	// objects and polygon references remain private to the implementation layer.
	TERRAENGINE_API RecastGroundBuildResult BuildRecastGroundTiles(const RecastGroundBuildInput& input);
	[[nodiscard]] TERRAENGINE_API std::optional<GroundTileBuildHalo> ComputeGroundTileBuildHalo(
		const NavigationProfile& profile) noexcept;
	TERRAENGINE_API std::uint64_t MakeGroundProfileBuildKey(const NavigationProfile& profile) noexcept;
	[[nodiscard]] TERRAENGINE_API NavigationGeometryBounds CanonicalGroundTileBounds(
		const GroundTileKey& key) noexcept;
	[[nodiscard]] TERRAENGINE_API GroundCoverageEvidence CaptureGroundCoverageEvidence(
		const RecastGroundBuildInput& input);
	[[nodiscard]] TERRAENGINE_API SavedGroundTileExportResult BuildSavedNavigationChunks(
		std::span<const GroundTileBlob> tiles);
	[[nodiscard]] TERRAENGINE_API SavedGroundTileImportResult ImportSavedGroundTiles(
		const SavedNavigationReadResult& read,
		const GroundCoverageEvidence& evidence,
		NavigationBudget* budget = nullptr);
}
