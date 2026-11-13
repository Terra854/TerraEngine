// Validation-only dispatch of the production reconstruction helper. Inputs are
// actual production compute-raster records, not a CPU-authored expected image.
#include "../../VirtualGeometry/VirtualGeometryRasterTriangle.hlsli"
#include "../../VirtualGeometry/VirtualGeometryReconstruction.hlsli"
[[vk::binding(0, 0)]] StructuredBuffer<RasterTriangle> sourceTriangles;
[[vk::binding(1, 0)]] StructuredBuffer<uint4> rasterRecords;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> reconstructedSurface;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> validity;
struct ProbeConstants { uint pixelCount; };
[[vk::push_constant]] ConstantBuffer<ProbeConstants> probe;
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= probe.pixelCount) return;
	validity[id.x] = 0u; reconstructedSurface[id.x] = 0.0f;
	const uint4 record = rasterRecords[id.x];
	uint count, stride; sourceTriangles.GetDimensions(count, stride);
	if (record.x >= count) return;
	const VirtualGeometryReconstruction value = ReconstructVirtualGeometrySurface(record);
	validity[id.x] = value.valid ? 1u : 2u;
	reconstructedSurface[id.x] = float4(value.sourceWeights, value.depth);
}
