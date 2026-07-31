// VirtualGeometryQuantization.h - globally anchored position quantization.
//
// Every cluster in one asset uses the same bounds. Shared vertices therefore
// produce the same integer representation even when neighboring clusters are
// encoded by different worker jobs or placed on different physical pages.

#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace TerraEngine::Editor
{
	struct VirtualGeometryQuantizationBounds
	{
		std::array<float, 3> minimum{};
		std::array<float, 3> maximum{};

		[[nodiscard]] bool Valid() const noexcept;
	};

	using VirtualGeometryQuantizedPosition = std::array<std::uint16_t, 3>;

	[[nodiscard]] VirtualGeometryQuantizedPosition QuantizeVirtualGeometryPosition(
		const std::array<float, 3> &position, const VirtualGeometryQuantizationBounds &bounds);

	[[nodiscard]] std::array<float, 3> DequantizeVirtualGeometryPosition(
		const VirtualGeometryQuantizedPosition &position, const VirtualGeometryQuantizationBounds &bounds);

	// The two spans contain corresponding shared-boundary vertices from
	// independently encoded clusters. Equality proves the encoded seam cannot
	// crack because both sides reconstruct the same integer lattice points.
	[[nodiscard]] bool ValidateVirtualGeometryWatertightBoundary(
		std::span<const VirtualGeometryQuantizedPosition> first, std::span<const VirtualGeometryQuantizedPosition> second) noexcept;
} // namespace TerraEngine::Editor
