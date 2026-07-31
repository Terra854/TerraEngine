#include "VirtualGeometryFoliageCurveBuilder.h"
namespace TerraEngine::Editor
{
	TerraEngine::Renderer::VirtualGeometryAggregateRepresentation BuildVirtualGeometryFoliageCurvePrototype(std::uint64_t id, bool curve)
	{
		// Tag each source with a domain-specific salt so foliage and curve IDs
		// cannot collide while remaining deterministic across clean builds.
		const auto kind = curve ? TerraEngine::Renderer::VirtualGeometryAggregateKind::Curve : TerraEngine::Renderer::VirtualGeometryAggregateKind::Foliage;
		const auto stableId = id == 0 ? 0 : (id ^ (curve ? 0x43555256454ull : 0x464f4c494147ull));
		return { kind, stableId, curve ? 0.025f : 0.05f, stableId != 0, 1u, 1.0f, stableId };
	}
} // namespace TerraEngine::Editor
