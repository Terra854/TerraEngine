// Bounded owner-mesh vertex pulling for the actual indexed motion pass.
// Matches DefaultIndexed.mesh.hlsl corner tokens/packed source ownership.
#ifndef TERRAENGINE_INDEXED_MOTION_GEOMETRY_HLSLI
#define TERRAENGINE_INDEXED_MOTION_GEOMETRY_HLSLI
#include "../VirtualGeometry/VirtualGeometryPackedVertex.hlsli"
struct IndexedMotionMeshlet { uint vertexOffset; uint vertexCount; uint primitiveIndexOffset; uint primitiveIndexCount; };
struct IndexedMotionDecode { float4 positionOffset; float4 positionScale; float4 uvOffsetScale; uint4 counts; };
[[vk::binding(0,1)]] ByteAddressBuffer motionVertexBytes;
[[vk::binding(1,1)]] ByteAddressBuffer motionPrimitiveBytes;
[[vk::binding(2,1)]] StructuredBuffer<IndexedMotionMeshlet> motionMeshlets;
[[vk::binding(3,1)]] ByteAddressBuffer motionVertexRemap;
[[vk::binding(4,1)]] StructuredBuffer<IndexedMotionDecode> motionDecode;
[[vk::binding(6,1)]] ByteAddressBuffer motionSkinningBytes;
[[vk::binding(7,1)]] ByteAddressBuffer motionMorphDeltaBytes;

bool MotionReadPrimitiveByte(uint address, out uint value)
{
	value = 0; uint bytes; motionPrimitiveBytes.GetDimensions(bytes); uint aligned = address & ~3u;
	if(aligned > bytes || 4u > bytes - aligned) return false;
	value = (motionPrimitiveBytes.Load(aligned) >> ((address & 3u) * 8u)) & 255u; return true;
}
bool MotionReadRemap(uint index, bool u16, out uint value)
{
	value = 0; uint bytes; motionVertexRemap.GetDimensions(bytes); uint stride = u16 ? 2u : 4u;
	if(index > 0xffffffffu / stride) return false; uint address = index * stride, aligned = address & ~3u;
	if(aligned > bytes || 4u > bytes - aligned) return false;
	uint word = motionVertexRemap.Load(aligned); value = u16 ? (word >> ((address & 2u) * 8u)) & 65535u : word; return true;
}
bool MotionLoadOwnerVertex(uint cornerToken, out IndexedMotionDecode decode,
	out uint ownerVertex, out TeDecodedPackedVertex vertex)
{
	decode = (IndexedMotionDecode)0; vertex = (TeDecodedPackedVertex)0; ownerVertex = 0;
	uint decodeCount, decodeStride; motionDecode.GetDimensions(decodeCount, decodeStride);
	if(decodeCount != 1u || decodeStride != 64u) return false; decode = motionDecode[0];
	uint stable = cornerToken / 3u, meshletIndex = stable / 124u, primitiveIndex = stable % 124u, corner = cornerToken % 3u;
	uint count, stride; motionMeshlets.GetDimensions(count, stride);
	uint meshletCount = decode.counts.w & 0x7fffffffu; bool u16 = (decode.counts.w & 0x80000000u) != 0u;
	if(stride != 16u || meshletIndex >= count || meshletIndex >= meshletCount || decode.counts.x == 0u || decode.counts.y == 0u || decode.counts.z == 0u) return false;
	IndexedMotionMeshlet record = motionMeshlets[meshletIndex];
	if(record.vertexCount == 0u || record.vertexCount > 64u || record.primitiveIndexCount % 3u != 0u
		|| record.primitiveIndexCount > 372u || primitiveIndex >= record.primitiveIndexCount / 3u
		|| record.vertexOffset > decode.counts.y || record.vertexCount > decode.counts.y - record.vertexOffset
		|| record.primitiveIndexOffset > decode.counts.z || record.primitiveIndexCount > decode.counts.z - record.primitiveIndexOffset) return false;
	uint bytes; motionVertexBytes.GetDimensions(bytes); if(decode.counts.x > bytes / 16u) return false;
	// Validate all three remaps before any invocation accepts this triangle.
	for(uint k = 0u; k < 3u; ++k) {
		uint local, mapped;
		if(!MotionReadPrimitiveByte(record.primitiveIndexOffset + primitiveIndex * 3u + k, local) || local >= record.vertexCount
			|| !MotionReadRemap(record.vertexOffset + local, u16, mapped) || mapped >= decode.counts.x) return false;
		if(k == corner) ownerVertex = mapped;
	}
	vertex = TeDecodePackedVertex(motionVertexBytes.Load4(ownerVertex * 16u)); return true;
}
#endif
