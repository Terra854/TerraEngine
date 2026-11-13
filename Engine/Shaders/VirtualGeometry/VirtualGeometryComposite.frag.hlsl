// Production visibility-buffer composite. Compute raster selects one winning
// microtriangle per pixel; this stage reconstructs its surface and invokes the
// exact default material function used by fixed-function fragments.
#define TERRAENGINE_DEFAULT_SURFACE_LIBRARY
#include "../Default/Default.frag.hlsl"
#include "VirtualGeometryAbi.hlsli"
#include "VirtualGeometryRasterTriangle.hlsli"
#include "VirtualGeometryReconstruction.hlsli"

struct CompositeConstants
{
	uint targetWidth;
	uint targetHeight;
	uint generation;
	uint reserved;
};

[[vk::binding(1, 1)]] StructuredBuffer<uint4> resolvedVisibility;
[[vk::binding(3, 1)]] StructuredBuffer<RasterTriangle> triangles;
[[vk::push_constant]] ConstantBuffer<CompositeConstants> Composite;

struct CompositeOutput
{
	float4 color : SV_Target0;
	uint4 identity : SV_Target1;
	float depth : SV_Depth;
};

CompositeOutput main(float4 pixelPosition : SV_Position)
{
	const uint2 pixel = uint2(pixelPosition.xy);
	uint recordCount;
	uint recordStride;
	resolvedVisibility.GetDimensions(recordCount, recordStride);
	const uint recordIndex = pixel.y * Composite.targetWidth + pixel.x;
	if(Composite.targetWidth == 0u || Composite.targetHeight == 0u
		|| pixel.x >= Composite.targetWidth || pixel.y >= Composite.targetHeight
		|| recordIndex >= recordCount)
	{
		discard;
	}
	const uint4 record = resolvedVisibility[recordIndex];
	if(record.x == TE_VIRTUAL_GEOMETRY_INVALID_INDEX)
	{
		discard;
	}
	uint triangleCount;
	uint triangleStride;
	triangles.GetDimensions(triangleCount, triangleStride);
	if(record.x >= triangleCount)
	{
		discard;
	}
	const RasterTriangle source = triangles[record.x];
	if(source.identity.z != Composite.generation)
	{
		discard;
	}

	// Resolve has already diagnosed stale/malformed raw producers. Its V2 tag
	// prevents an old transient encoding from being interpreted as this surface.
	const VirtualGeometryReconstruction reconstruction = ReconstructVirtualGeometrySurface(record);
	if (!reconstruction.valid) discard;
	const float3 perspectiveWeights = reconstruction.sourceWeights;
	const float depth = reconstruction.depth;
	TerraSurfaceInput surface;
	surface.position = float4(pixelPosition.xy, depth, 1.0f);
	surface.uv = source.uv0.xy * perspectiveWeights.x
		+ source.uv1.xy * perspectiveWeights.y
		+ source.uv2.xy * perspectiveWeights.z;
	surface.worldPosition = source.world0.xyz * perspectiveWeights.x
		+ source.world1.xyz * perspectiveWeights.y
		+ source.world2.xyz * perspectiveWeights.z;
	surface.normal = normalize(source.normal0.xyz * perspectiveWeights.x
		+ source.normal1.xyz * perspectiveWeights.y
		+ source.normal2.xyz * perspectiveWeights.z);
	surface.tangent = source.tangent0 * perspectiveWeights.x
		+ source.tangent1 * perspectiveWeights.y
		+ source.tangent2 * perspectiveWeights.z;
	surface.tangent.xyz = normalize(surface.tangent.xyz);
	surface.tangent.w = surface.tangent.w < 0.0f ? -1.0f : 1.0f;
	surface.meshletIndex = source.shadingState.z;
	surface.meshletBoundsRadius = 0.0f;
	surface.meshletCullFlags = 0u;
	surface.virtualGeometryDebugData = source.debugState.x;

	PushConstants uniforms = (PushConstants)0;
	uniforms.materialIndex = source.shadingOffsets.x;
	uniforms.statsSlotIndex = source.shadingOffsets.y;
	uniforms.lightDataOffset = source.shadingOffsets.z;
	uniforms.clusterDataOffset = source.shadingOffsets.w;
	uniforms.lightIndexOffset = source.shadingState.x;
	uniforms.virtualGeometryFlags = source.shadingState.y;
	uniforms.selectedVirtualLod = source.debugState.x & 0xffu;
	uniforms.selectedVirtualClusterCount =
		(source.debugState.x >> 8u) & 0xffu;
	uniforms.selectedVirtualPageNeedCount = source.debugState.w;
	// Compute geometry is already decoded; this legacy shading selector is not
	// the immutable picking key carried in debugState.y.
	uniforms.meshDataIndex = 0u;
	uniforms.virtualGeometryPacketDescriptorIndex = source.debugState.z;
	uniforms.editorTint = source.editorTint;

	CompositeOutput output;
	output.color = ShadeDefaultSurface(surface, source.identity.x, uniforms);
	output.identity = uint4(
		source.identity.x,
		source.debugState.y,
		source.debugState.z,
		// Keep a valid first triangle distinct from the cleared attachment.
		// Readback matching uses the object-table key; the final lane is diagnostic.
		source.shadingOffsets.y + 1u);
	output.depth = depth;
	return output;
}
