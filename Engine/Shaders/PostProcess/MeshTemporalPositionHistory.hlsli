// A motion raster draws the actual mesh shader and records its final clip
// positions. Positions are indexed by draw, meshlet and local corner, so the
// same author-defined deformation runs in the color and motion draws.
#ifndef TERRAENGINE_MESH_TEMPORAL_POSITION_HISTORY_HLSLI
#define TERRAENGINE_MESH_TEMPORAL_POSITION_HISTORY_HLSLI

#include "MotionVectors.hlsli"

struct MeshTemporalVertex
{
	float4 position : SV_Position;
	float4 previousRasterClip : TEXCOORD0;
	float invalidity : TEXCOORD2;
};

struct MeshTemporalDrawHistory
{
	uint meshletOffset;
	uint meshletCount;
	uint currentPositionBase;
	uint previousPositionBase;
};

// One dynamic constant-buffer slot selects the exact draw row. Both position
// buffers are private to the view. A missing previous row uses the sentinel.
[[vk::binding(1, 2)]] ConstantBuffer<MeshTemporalDrawHistory> meshTemporalDraw;
[[vk::binding(2, 2)]] RWStructuredBuffer<float4> meshTemporalCurrentPositions;
[[vk::binding(3, 2)]] StructuredBuffer<float4> meshTemporalPreviousPositions;

MeshTemporalVertex CaptureMeshTemporalVertex(
	float4 rasterClip, uint meshletIndex, uint localVertex, uint maxVertices)
{
	MeshTemporalVertex output;
	output.position = rasterClip;
	output.previousRasterClip = float4(0, 0, 0, 1);
	output.invalidity = 1.0f;
	if(meshletIndex < meshTemporalDraw.meshletOffset || localVertex >= maxVertices
		|| !MotionFiniteClip(rasterClip)) return output;
	const uint localMeshlet = meshletIndex - meshTemporalDraw.meshletOffset;
	if(localMeshlet >= meshTemporalDraw.meshletCount
		|| localMeshlet > (0xffffffffu - localVertex) / maxVertices) return output;
	const uint corner = localMeshlet * maxVertices + localVertex;

	uint capacity, stride;
	meshTemporalCurrentPositions.GetDimensions(capacity, stride);
	const uint currentBase = meshTemporalDraw.currentPositionBase;
	if(stride == 16u && currentBase < capacity && corner < capacity - currentBase)
		meshTemporalCurrentPositions[currentBase + corner] = rasterClip;

	// Only a matching previous draw gets a row. Unwritten corners are cleared
	// by the recorder and fail the positive-W validity check.
	meshTemporalPreviousPositions.GetDimensions(capacity, stride);
	const uint previousBase = meshTemporalDraw.previousPositionBase;
	if(stride == 16u && previousBase < capacity && corner < capacity - previousBase)
	{
		const float4 previous = meshTemporalPreviousPositions[previousBase + corner];
		if(MotionFiniteClip(previous))
		{
			output.previousRasterClip = previous;
			output.invalidity = 0.0f;
		}
	}
	return output;
}

#endif
