// WorldCoordinate.cpp - checked normalization and bounded localization helpers.

#include "WorldCoordinate.h"

#include <cmath>
#include <limits>

namespace TerraEngine::Scene {
	namespace {
		bool IsFinite(const glm::dvec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		bool CheckedAdd(std::int64_t left, std::int64_t right, std::int64_t& result)
		{
			if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right)
				|| (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right))
			{
				return false;
			}
			result = left + right;
			return true;
		}

		bool CheckedSubtract(std::int64_t left, std::int64_t right, std::int64_t& result)
		{
			if ((right > 0 && left < std::numeric_limits<std::int64_t>::min() + right)
				|| (right < 0 && left > std::numeric_limits<std::int64_t>::max() + right))
			{
				return false;
			}
			result = left - right;
			return true;
		}

		bool NormalizeAxis(std::int64_t& cell, double& offset)
		{
			// Floor is crucial for negative offsets: a cast would truncate toward
			// zero and make the same point encode to two different addresses.
			const double carryAsDouble = std::floor((offset + kWorldCellHalfEdgeMetres) / kWorldCellEdgeMetres);
			// The upper limit is deliberately exclusive. `double(max int64)` rounds
			// up to 2^63 on MSVC, so comparing against max itself could cast 2^63
			// to int64 before rejecting it.
			constexpr double kInt64CarryExclusiveUpper = 9'223'372'036'854'775'808.0;
			constexpr double kInt64CarryInclusiveLower = -9'223'372'036'854'775'808.0;
			if (carryAsDouble < kInt64CarryInclusiveLower || carryAsDouble >= kInt64CarryExclusiveUpper)
			{
				return false;
			}
			const auto carry = static_cast<std::int64_t>(carryAsDouble);
			std::int64_t normalizedCell = 0;
			if (!CheckedAdd(cell, carry, normalizedCell))
			{
				return false;
			}
			cell = normalizedCell;
			offset -= carryAsDouble * kWorldCellEdgeMetres;

			// Guard tiny arithmetic drift at the half-open bound without accepting
			// a truly invalid value. A second carry remains exact and checked.
			if (offset >= kWorldCellHalfEdgeMetres)
			{
				if (!CheckedAdd(cell, 1, cell))
				{
					return false;
				}
				offset -= kWorldCellEdgeMetres;
			}
			else if (offset < -kWorldCellHalfEdgeMetres)
			{
				if (!CheckedAdd(cell, -1, cell))
				{
					return false;
				}
				offset += kWorldCellEdgeMetres;
			}
			return offset >= -kWorldCellHalfEdgeMetres && offset < kWorldCellHalfEdgeMetres;
		}

		std::size_t Mix(std::size_t seed, std::size_t value) noexcept
		{
			return seed ^ (value + 0x9e3779b9u + (seed << 6u) + (seed >> 2u));
		}
	}

	std::string_view ToString(WorldCoordinateError error)
	{
		switch (error)
		{
		case WorldCoordinateError::None: return "None";
		case WorldCoordinateError::NonFiniteOffset: return "NonFiniteOffset";
		case WorldCoordinateError::CellOverflow: return "CellOverflow";
		case WorldCoordinateError::UnsafeLocalRange: return "UnsafeLocalRange";
		}
		return "UnknownWorldCoordinateError";
	}

	WorldCoordinateResult NormalizeWorldPosition(WorldPosition position)
	{
		if (!IsFinite(position.offsetMetres))
		{
			return { {}, WorldCoordinateError::NonFiniteOffset };
		}
		if (!NormalizeAxis(position.cell.x, position.offsetMetres.x)
			|| !NormalizeAxis(position.cell.y, position.offsetMetres.y)
			|| !NormalizeAxis(position.cell.z, position.offsetMetres.z))
		{
			return { {}, WorldCoordinateError::CellOverflow };
		}
		return { position, WorldCoordinateError::None };
	}

	WorldDeltaResult NormalizeWorldDelta(WorldDelta delta)
	{
		if (!IsFinite(delta.offsetMetres))
		{
			return { {}, WorldCoordinateError::NonFiniteOffset };
		}
		if (!NormalizeAxis(delta.cellDelta.x, delta.offsetMetres.x)
			|| !NormalizeAxis(delta.cellDelta.y, delta.offsetMetres.y)
			|| !NormalizeAxis(delta.cellDelta.z, delta.offsetMetres.z))
		{
			return { {}, WorldCoordinateError::CellOverflow };
		}
		return { delta, WorldCoordinateError::None };
	}

	WorldCoordinateResult ApplyWorldDelta(const WorldPosition& origin, WorldDelta delta)
	{
		const WorldCoordinateResult normalizedOrigin = NormalizeWorldPosition(origin);
		if (!normalizedOrigin.Succeeded())
		{
			return normalizedOrigin;
		}
		const WorldDeltaResult normalizedDelta = NormalizeWorldDelta(delta);
		if (!normalizedDelta.Succeeded())
		{
			return { {}, normalizedDelta.error };
		}

		WorldPosition combined = normalizedOrigin.position;
		if (!CheckedAdd(combined.cell.x, normalizedDelta.delta.cellDelta.x, combined.cell.x)
			|| !CheckedAdd(combined.cell.y, normalizedDelta.delta.cellDelta.y, combined.cell.y)
			|| !CheckedAdd(combined.cell.z, normalizedDelta.delta.cellDelta.z, combined.cell.z))
		{
			return { {}, WorldCoordinateError::CellOverflow };
		}
		combined.offsetMetres += normalizedDelta.delta.offsetMetres;
		return NormalizeWorldPosition(combined);
	}

	WorldLocalizationResult LocalizeWorldPosition(
		const WorldPosition& position,
		const WorldPosition& origin,
		double maximumSafeDistanceMetres)
	{
		if (!std::isfinite(maximumSafeDistanceMetres) || maximumSafeDistanceMetres < 0.0)
		{
			return { {}, WorldCoordinateError::UnsafeLocalRange };
		}
		const WorldCoordinateResult normalizedPosition = NormalizeWorldPosition(position);
		const WorldCoordinateResult normalizedOrigin = NormalizeWorldPosition(origin);
		if (!normalizedPosition.Succeeded())
		{
			return { {}, normalizedPosition.error };
		}
		if (!normalizedOrigin.Succeeded())
		{
			return { {}, normalizedOrigin.error };
		}

		std::int64_t cellDeltaX = 0;
		std::int64_t cellDeltaY = 0;
		std::int64_t cellDeltaZ = 0;
		if (!CheckedSubtract(normalizedPosition.position.cell.x, normalizedOrigin.position.cell.x, cellDeltaX)
			|| !CheckedSubtract(normalizedPosition.position.cell.y, normalizedOrigin.position.cell.y, cellDeltaY)
			|| !CheckedSubtract(normalizedPosition.position.cell.z, normalizedOrigin.position.cell.z, cellDeltaZ))
		{
			return { {}, WorldCoordinateError::UnsafeLocalRange };
		}

		// Reject a separation before converting it to floating point. This keeps
		// adjacent cells near INT64_MAX exact even on MSVC where long double has
		// double precision, and it enforces the caller's bounded-local contract.
		const long double permittedCellDelta = std::ceil((static_cast<long double>(maximumSafeDistanceMetres) + kWorldCellEdgeMetres) / kWorldCellEdgeMetres);
		const auto exceedsPermittedCellDelta = [permittedCellDelta](std::int64_t value) {
			return static_cast<long double>(value) > permittedCellDelta || static_cast<long double>(value) < -permittedCellDelta;
		};
		if (exceedsPermittedCellDelta(cellDeltaX) || exceedsPermittedCellDelta(cellDeltaY) || exceedsPermittedCellDelta(cellDeltaZ))
		{
			return { {}, WorldCoordinateError::UnsafeLocalRange };
		}

		const long double deltaX = static_cast<long double>(cellDeltaX) * kWorldCellEdgeMetres
			+ static_cast<long double>(normalizedPosition.position.offsetMetres.x - normalizedOrigin.position.offsetMetres.x);
		const long double deltaY = static_cast<long double>(cellDeltaY) * kWorldCellEdgeMetres
			+ static_cast<long double>(normalizedPosition.position.offsetMetres.y - normalizedOrigin.position.offsetMetres.y);
		const long double deltaZ = static_cast<long double>(cellDeltaZ) * kWorldCellEdgeMetres
			+ static_cast<long double>(normalizedPosition.position.offsetMetres.z - normalizedOrigin.position.offsetMetres.z);
		if (std::abs(deltaX) > maximumSafeDistanceMetres || std::abs(deltaY) > maximumSafeDistanceMetres || std::abs(deltaZ) > maximumSafeDistanceMetres)
		{
			return { {}, WorldCoordinateError::UnsafeLocalRange };
		}
		return { { static_cast<double>(deltaX), static_cast<double>(deltaY), static_cast<double>(deltaZ) }, WorldCoordinateError::None };
	}

	std::size_t WorldCellHash::operator()(const WorldCell& cell) const noexcept
	{
		std::size_t seed = std::hash<std::int64_t>{}(cell.x);
		seed = Mix(seed, std::hash<std::int64_t>{}(cell.y));
		return Mix(seed, std::hash<std::int64_t>{}(cell.z));
	}

	std::size_t WorldPositionHash::operator()(const WorldPosition& position) const noexcept
	{
		std::size_t seed = WorldCellHash{}(position.cell);
		seed = Mix(seed, std::hash<double>{}(position.offsetMetres.x));
		seed = Mix(seed, std::hash<double>{}(position.offsetMetres.y));
		return Mix(seed, std::hash<double>{}(position.offsetMetres.z));
	}

	std::string EncodeWorldCellAxis(std::int64_t axis)
	{
		// Decimal strings prevent JavaScript and generic JSON tooling from
		// silently narrowing an exact signed 64-bit cell address to a double.
		return std::to_string(axis);
	}

}
