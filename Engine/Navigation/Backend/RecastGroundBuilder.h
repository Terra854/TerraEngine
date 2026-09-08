// RecastGroundBuilder.h - provider-neutral, deterministic ground-tile contract.

#pragma once

#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationBudget.h"
#include "Navigation/NavigationSettings.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Navigation {
	inline constexpr float kGroundTileEdgeMetres = 32.0F;
	inline constexpr float kGroundVerticalBandMetres = 128.0F;

	struct GroundTileKey {
		std::int32_t x = 0;
		std::int32_t z = 0;
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
	};

	struct RecastGroundBuildResult {
		RecastGroundBuildStatus status = RecastGroundBuildStatus::InvalidGeometry;
		std::string reason;
		std::vector<GroundTileBlob> tiles;
		[[nodiscard]] bool Succeeded() const noexcept { return status == RecastGroundBuildStatus::Success; }
	};

	// Returns canonical sorted tile keys and pristine owned bytes. Recast/Detour
	// objects and polygon references remain private to the implementation layer.
	TERRAENGINE_API RecastGroundBuildResult BuildRecastGroundTiles(const RecastGroundBuildInput& input);
	TERRAENGINE_API std::uint64_t MakeGroundProfileBuildKey(const NavigationProfile& profile) noexcept;
}
