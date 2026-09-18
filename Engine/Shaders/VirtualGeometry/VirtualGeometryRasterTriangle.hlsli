// Shared GPU-authored triangle ABI for compute raster, resolve, and composite.
// Keeping one declaration prevents a shader stage from silently reading a
// different stride after the production visibility payload evolves.
#ifndef TERRAENGINE_VIRTUAL_GEOMETRY_RASTER_TRIANGLE_HLSLI
#define TERRAENGINE_VIRTUAL_GEOMETRY_RASTER_TRIANGLE_HLSLI

struct RasterTriangle
{
	float4 clip0;
	float4 clip1;
	float4 clip2;
	float4 world0;
	float4 world1;
	float4 world2;
	float4 normal0;
	float4 normal1;
	float4 normal2;
	float4 tangent0;
	float4 tangent1;
	float4 tangent2;
	float4 uv0;
	float4 uv1;
	float4 uv2;
	float4 editorTint;
	// identity: stable primitive, packed material/route, generation, flags.
	uint4 identity;
	// shadingOffsets: material, stats, light-data, cluster-data offsets.
	uint4 shadingOffsets;
	// shadingState: light-index offset, VG flags, meshlet, packet descriptor.
	uint4 shadingState;
	// debugState: packed visualizer data, immutable object-table key, packet, page needs.
	uint4 debugState;
};

#endif
