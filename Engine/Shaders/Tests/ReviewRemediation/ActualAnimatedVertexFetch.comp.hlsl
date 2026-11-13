// Validation-only entry point calls the actual graphics fetch helper. The
// predicate is not mirrored: production code owns every range/finite check.
[[vk::binding(0, 0)]] StructuredBuffer<uint> globalBuffers[1];
#include "VirtualGeometry/AnimatedDeformedGeometry.hlsli"
[[vk::binding(1, 0)]] RWStructuredBuffer<uint4> reviewFetchResult;
[numthreads(1, 1, 1)]
void main()
{
	TeAnimatedGraphicsBridge bridge = (TeAnimatedGraphicsBridge)0;
	TeAnimatedGraphicsPage page = (TeAnimatedGraphicsPage)0;
	TeAnimatedDeformedVertex vertex;
	const bool accepted = TeTryLoadAnimatedVertex(bridge, page, 0u, vertex);
	reviewFetchResult[0] = uint4(accepted ? 1u : 0u, asuint(vertex.currentPosition.xyz));
}
