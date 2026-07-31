#include "VirtualGeometryVisibility.h"
namespace TerraEngine::Renderer
{
	VirtualGeometryVisibilityLayout SelectVirtualGeometryVisibilityLayout(const VirtualGeometryVisibilityRequirements &r)
	{
		if (r.primitiveBits <= 16 && r.instanceBits <= 12 && r.materialBits <= 4)
			return VirtualGeometryVisibilityLayout::Packed;
		if (r.primitiveBits <= 32 && r.instanceBits <= 24)
			return VirtualGeometryVisibilityLayout::Wide;
		return VirtualGeometryVisibilityLayout::Separate;
	}
	bool WinsVirtualGeometryDepthTie(const VirtualGeometryVisibilitySample &c, const VirtualGeometryVisibilitySample &o)
	{
		return c.depth < o.depth || (c.depth == o.depth && c.stablePrimitive < o.stablePrimitive);
	}
	VirtualGeometryPackedVisibility PackVirtualGeometryVisibilityIdentity(
		const VirtualGeometryVisibilityIdentity &identity, const VirtualGeometryVisibilityRequirements &requirements)
	{
		const std::uint32_t totalBits = requirements.primitiveBits + requirements.instanceBits + requirements.materialBits;
		if(totalBits == 0u || totalBits > 64u) return {};
		const auto fits = [](std::uint32_t value, std::uint32_t bits) {
			return bits != 0u && (bits >= 32u || value < (1u << bits));
		};
		if(!fits(identity.primitiveId, requirements.primitiveBits)
			|| !fits(identity.instanceId, requirements.instanceBits)
			|| !fits(identity.materialId, requirements.materialBits)) return {};
		VirtualGeometryPackedVisibility packed{};
		packed.value = static_cast<std::uint64_t>(identity.primitiveId);
		packed.value |= static_cast<std::uint64_t>(identity.instanceId) << requirements.primitiveBits;
		packed.value |= static_cast<std::uint64_t>(identity.materialId) << (requirements.primitiveBits + requirements.instanceBits);
		packed.representable = true;
		return packed;
	}
	VirtualGeometryVisibilityIdentity UnpackVirtualGeometryVisibilityIdentity(
		const VirtualGeometryPackedVisibility &packed, const VirtualGeometryVisibilityRequirements &requirements)
	{
		if(!packed.representable) return {};
		const auto mask = [](std::uint32_t bits) -> std::uint64_t { return bits >= 64u ? ~0ull : ((1ull << bits) - 1ull); };
		VirtualGeometryVisibilityIdentity identity{};
		identity.primitiveId = static_cast<std::uint32_t>(packed.value & mask(requirements.primitiveBits));
		identity.instanceId = static_cast<std::uint32_t>((packed.value >> requirements.primitiveBits) & mask(requirements.instanceBits));
		identity.materialId = static_cast<std::uint32_t>((packed.value >> (requirements.primitiveBits + requirements.instanceBits)) & mask(requirements.materialBits));
		return identity;
	}
} // namespace TerraEngine::Renderer
