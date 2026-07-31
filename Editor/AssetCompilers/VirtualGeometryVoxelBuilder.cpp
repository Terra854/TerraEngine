#include "VirtualGeometryVoxelBuilder.h"
namespace TerraEngine::Editor
{
	TerraEngine::Renderer::VirtualGeometryAggregateRepresentation BuildVirtualGeometryVoxelPrototype(std::uint64_t id, bool impostor)
	{
		// Voxel and impostor transitions use separate salts and conservative
		// canonical error values; both retain the same complete fallback contract.
		const auto kind = impostor ? TerraEngine::Renderer::VirtualGeometryAggregateKind::Impostor : TerraEngine::Renderer::VirtualGeometryAggregateKind::Voxel;
		const auto stableId = id == 0 ? 0 : (id ^ (impostor ? 0x494d504f53544f52ull : 0x564f58454cull));
		return { kind, stableId, impostor ? 0.2f : 0.1f, stableId != 0, 1u, 1.0f, stableId };
	}
} // namespace TerraEngine::Editor
