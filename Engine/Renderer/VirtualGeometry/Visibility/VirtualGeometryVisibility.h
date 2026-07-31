#pragma once
#include <cstdint>
#include <vector>
namespace TerraEngine::Renderer
{
	// Layout selection is data-only so every backend writes the same stable identity.
	enum class VirtualGeometryVisibilityLayout : std::uint8_t
	{
		Packed,
		Wide,
		Separate
	};
	enum class VirtualGeometryRasterRoute : std::uint8_t
	{
		Hardware,
		Compute,
		Indexed,
		Fallback
	};
	struct VirtualGeometryVisibilityRequirements
	{
		std::uint32_t primitiveBits = 0, instanceBits = 0, materialBits = 0;
	};
	// Stable identity is route-neutral: hardware, compute, and indexed fallback
	// all describe the same primitive/instance/material before packing it for a
	// particular attachment layout.
	struct VirtualGeometryVisibilityIdentity
	{
		std::uint32_t primitiveId = 0u;
		std::uint32_t instanceId = 0u;
		std::uint32_t materialId = 0u;
		std::uint32_t flags = 0u;
	};
	struct VirtualGeometryPackedVisibility
	{
		std::uint64_t value = 0u;
		bool representable = false;
	};
	struct VirtualGeometryVisibilitySample
	{
		float depth = 1.f;
		std::uint64_t stablePrimitive = 0;
	};
	[[nodiscard]] VirtualGeometryVisibilityLayout SelectVirtualGeometryVisibilityLayout(const VirtualGeometryVisibilityRequirements &);
	[[nodiscard]] bool WinsVirtualGeometryDepthTie(const VirtualGeometryVisibilitySample &candidate, const VirtualGeometryVisibilitySample &current);
	[[nodiscard]] VirtualGeometryPackedVisibility PackVirtualGeometryVisibilityIdentity(
		const VirtualGeometryVisibilityIdentity &, const VirtualGeometryVisibilityRequirements &);
	[[nodiscard]] VirtualGeometryVisibilityIdentity UnpackVirtualGeometryVisibilityIdentity(
		const VirtualGeometryPackedVisibility &, const VirtualGeometryVisibilityRequirements &);
} // namespace TerraEngine::Renderer
