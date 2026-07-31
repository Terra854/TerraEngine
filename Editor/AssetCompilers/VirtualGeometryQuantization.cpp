#include "VirtualGeometryQuantization.h"

#include <algorithm>
#include <cmath>

namespace TerraEngine::Editor
{
	bool VirtualGeometryQuantizationBounds::Valid() const noexcept
	{
		for (std::size_t axis = 0u; axis < minimum.size(); ++axis)
		{
			if (!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) || maximum[axis] <= minimum[axis])
			{
				return false;
			}
		}
		return true;
	}

	VirtualGeometryQuantizedPosition QuantizeVirtualGeometryPosition(const std::array<float, 3> &position, const VirtualGeometryQuantizationBounds &bounds)
	{
		VirtualGeometryQuantizedPosition result{};
		if (!bounds.Valid())
		{
			return result;
		}
		for (std::size_t axis = 0u; axis < result.size(); ++axis)
		{
			const float normalized = std::clamp((position[axis] - bounds.minimum[axis]) / (bounds.maximum[axis] - bounds.minimum[axis]), 0.0f, 1.0f);
			result[axis] = static_cast<std::uint16_t>(std::lround(normalized * 65535.0f));
		}
		return result;
	}

	std::array<float, 3> DequantizeVirtualGeometryPosition(const VirtualGeometryQuantizedPosition &position, const VirtualGeometryQuantizationBounds &bounds)
	{
		std::array<float, 3> result{};
		if (!bounds.Valid())
		{
			return result;
		}
		for (std::size_t axis = 0u; axis < result.size(); ++axis)
		{
			const float normalized = static_cast<float>(position[axis]) / 65535.0f;
			result[axis] = bounds.minimum[axis] + normalized * (bounds.maximum[axis] - bounds.minimum[axis]);
		}
		return result;
	}

	bool ValidateVirtualGeometryWatertightBoundary(
		std::span<const VirtualGeometryQuantizedPosition> first, std::span<const VirtualGeometryQuantizedPosition> second) noexcept
	{
		return first.size() == second.size() && std::equal(first.begin(), first.end(), second.begin());
	}
} // namespace TerraEngine::Editor
