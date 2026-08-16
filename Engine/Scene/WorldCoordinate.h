// WorldCoordinate.h - exact, normalized world addresses for unbounded scenes.
//
// A world location is never represented as one giant floating-point value.
// The signed cell is exact and the bounded offset retains local precision. This
// contract is intentionally separate from streaming regions and physics frames.

#pragma once

#include "Core/Export.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace TerraEngine::Scene {

	inline constexpr double kWorldCellEdgeMetres = 65'536.0;
	inline constexpr double kWorldCellHalfEdgeMetres = kWorldCellEdgeMetres * 0.5;
	inline constexpr double kTransformSpaceMaximumRelativeRadiusMetres = 8'192.0;

	// Stable failure categories let transactions, persistence, and editor tools
	// reject invalid input without guessing whether to clamp or migrate it.
	enum class WorldCoordinateError : std::uint8_t {
		None,
		NonFiniteOffset,
		CellOverflow,
		UnsafeLocalRange
	};

	// Exact canonical address unit. Each axis uses checked signed arithmetic.
	struct WorldCell {
		std::int64_t x = 0;
		std::int64_t y = 0;
		std::int64_t z = 0;

		[[nodiscard]] constexpr bool operator==(const WorldCell&) const = default;
	};

	// The authoritative absolute location: exact cell plus a normalized local
	// displacement in the half-open interval [-32768, 32768) on every axis.
	struct WorldPosition {
		WorldCell cell{};
		glm::dvec3 offsetMetres{0.0};

		[[nodiscard]] bool operator==(const WorldPosition& other) const
		{
			return cell == other.cell
				&& offsetMetres.x == other.offsetMetres.x
				&& offsetMetres.y == other.offsetMetres.y
				&& offsetMetres.z == other.offsetMetres.z;
		}
	};

	// A displacement is deliberately distinct from an absolute position so a
	// typed transaction cannot accidentally treat a translation as an address.
	struct WorldDelta {
		WorldCell cellDelta{};
		glm::dvec3 offsetMetres{0.0};

		[[nodiscard]] bool operator==(const WorldDelta& other) const
		{
			return cellDelta == other.cellDelta
				&& offsetMetres.x == other.offsetMetres.x
				&& offsetMetres.y == other.offsetMetres.y
				&& offsetMetres.z == other.offsetMetres.z;
		}
	};

	// Canonical pose data travels with one object. Transform keeps its bounded
	// parent-relative relationship separately so hierarchy math cannot rotate a
	// distant absolute translation through a float matrix.
	struct WorldPose {
		WorldPosition position{};
		glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
		glm::vec3 scale{1.0f, 1.0f, 1.0f};
		std::uint64_t transformRevision = 0;
		std::uint64_t coordinateFrameRevision = 0;
	};

	struct WorldCoordinateResult {
		WorldPosition position{};
		WorldCoordinateError error = WorldCoordinateError::None;

		[[nodiscard]] constexpr bool Succeeded() const { return error == WorldCoordinateError::None; }
	};

	struct WorldDeltaResult {
		WorldDelta delta{};
		WorldCoordinateError error = WorldCoordinateError::None;

		[[nodiscard]] constexpr bool Succeeded() const { return error == WorldCoordinateError::None; }
	};

	struct WorldLocalizationResult {
		glm::dvec3 offsetMetres{0.0};
		WorldCoordinateError error = WorldCoordinateError::None;

		[[nodiscard]] constexpr bool Succeeded() const { return error == WorldCoordinateError::None; }
	};

	struct WorldCellHash {
		[[nodiscard]] std::size_t operator()(const WorldCell& cell) const noexcept;
	};

	struct WorldPositionHash {
		[[nodiscard]] std::size_t operator()(const WorldPosition& position) const noexcept;
	};

	TERRAENGINE_API std::string_view ToString(WorldCoordinateError error);
	TERRAENGINE_API WorldCoordinateResult NormalizeWorldPosition(WorldPosition position);
	TERRAENGINE_API WorldDeltaResult NormalizeWorldDelta(WorldDelta delta);
	TERRAENGINE_API WorldCoordinateResult ApplyWorldDelta(const WorldPosition& origin, WorldDelta delta);
	TERRAENGINE_API WorldLocalizationResult LocalizeWorldPosition(
		const WorldPosition& position,
		const WorldPosition& origin,
		double maximumSafeDistanceMetres);
	TERRAENGINE_API std::string EncodeWorldCellAxis(std::int64_t axis);

}
