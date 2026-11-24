// Genuine indexed-raster vertex assembly for ordinary Default owner meshes.
// The uint32 index buffer contains corner tokens, not owner vertex indices:
// ((meshletIndex * 124 + primitiveIndex) * 3 + corner). Native preparation must
// validate the complete indexed slice and call vkCmdDrawIndexed with vertexOffset0.
#pragma pack_matrix(column_major)

// Reuse the exact installed surface/push ABI without compiling another fragment
// entry point. Dead-code elimination removes unused material shading from VS.
#define TERRAENGINE_DEFAULT_SURFACE_LIBRARY
#include "Default.frag.hlsl"
#include "DefaultDeformation.hlsli"
#include "../VirtualGeometry/VirtualGeometryPackedVertex.hlsli"

struct IndexedMeshletRecord
{
	uint vertexOffset;
	uint vertexCount;
	uint primitiveIndexOffset;
	uint primitiveIndexCount;
};
struct MeshDecodeInfo
{
	float4 positionOffset;
	float4 positionScale;
	float4 uvOffsetScale;
	// z is primitive-index BYTE ELEMENT count, as populated by
	// VulkanContextResources, not the triangle count suggested by older comments.
	uint4 counts;
};
[[vk::binding(0,1)]] ByteAddressBuffer indexedVertexBytes;
[[vk::binding(1,1)]] ByteAddressBuffer indexedPrimitiveBytes;
[[vk::binding(2,1)]] StructuredBuffer<IndexedMeshletRecord> indexedMeshlets;
[[vk::binding(3,1)]] ByteAddressBuffer indexedVertexRemap;
[[vk::binding(4,1)]] StructuredBuffer<MeshDecodeInfo> indexedDecode;
// Actual owner skin/morph streams; set1 must expose these bindings to VS.
[[vk::binding(6,1)]] ByteAddressBuffer meshSkinningBytes;
[[vk::binding(7,1)]] ByteAddressBuffer meshMorphDeltaBytes;
[[vk::push_constant]] ConstantBuffer<PushConstants> SceneUniforms;
#include "DefaultOwnerDeformation.hlsli"

struct DefaultIndexedVertexOutput
{
	TerraSurfaceInput surface;
	// Every corner in the index-buffer triangle carries the same primitive ID.
	// No fragment SV_PrimitiveID/geometryShader optional feature is required.
	nointerpolation uint stablePrimitiveId : TEXCOORD6;
};

DefaultIndexedVertexOutput RejectedIndexedVertex(uint stablePrimitiveId)
{
	DefaultIndexedVertexOutput result=(DefaultIndexedVertexOutput)0;
	// All rejected corners collapse behind Vulkan's near plane. Native admission
	// rejects malformed whole assets; this is a final shader-side safety boundary.
	result.surface.position=float4(0,0,-1,1);
	result.surface.normal=float3(0,0,1);
	result.surface.tangent=float4(1,0,0,1);
	result.stablePrimitiveId=stablePrimitiveId;
	return result;
}
bool ReadIndexedPrimitiveByte(uint address,out uint value)
{
	value=0;
	uint bytes=0;indexedPrimitiveBytes.GetDimensions(bytes);
	uint aligned=address&~3u;
	if(aligned>bytes||4u>bytes-aligned)return false;
	value=(indexedPrimitiveBytes.Load(aligned)>>((address&3u)*8u))&255u;
	return true;
}
bool ReadIndexedOwnerVertex(uint remapIndex,bool isU16,out uint ownerVertex)
{
	ownerVertex=0;
	uint bytes=0;indexedVertexRemap.GetDimensions(bytes);
	uint stride=isU16?2u:4u;
	if(remapIndex>0xffffffffu/stride)return false;
	uint address=remapIndex*stride,aligned=address&~3u;
	if(aligned>bytes||4u>bytes-aligned)return false;
	uint word=indexedVertexRemap.Load(aligned);
	ownerVertex=isU16?(word>>((address&2u)*8u))&65535u:word;
	return true;
}
float3 IndexedTransformDirection(float3 value)
{
	return normalize(mul((float3x3)SceneUniforms.model,value));
}
float3 IndexedTransformNormal(float3 value)
{
	// Identical to Default.mesh's normal assembly. Positions/tangents use
	// forward scale; normals use reciprocal axis scale without an extra matrix.
	// These two small local helpers should join a common assembly include when
	// existing mesh sources can be edited, rather than evolving independently.
	float3 x=mul((float3x3)SceneUniforms.model,float3(1,0,0));
	float3 y=mul((float3x3)SceneUniforms.model,float3(0,1,0));
	float3 z=mul((float3x3)SceneUniforms.model,float3(0,0,1));
	float3 normalAxisX=x/max(dot(x,x),1e-8f);
	float3 normalAxisY=y/max(dot(y,y),1e-8f);
	float3 normalAxisZ=z/max(dot(z,z),1e-8f);
	return normalize(value.x*normalAxisX+value.y*normalAxisY+value.z*normalAxisZ);
}

DefaultIndexedVertexOutput main(uint cornerToken : SV_VertexID)
{
	uint stable=cornerToken/3u,meshletIndex=stable/124u,primitiveIndex=stable%124u,corner=cornerToken%3u;
	DefaultIndexedVertexOutput rejected=RejectedIndexedVertex(stable);
	// Only a native-retained ordinary CPU bridge is eligible for current skin.
	// This POD shape check is defense in depth, not a provenance certificate.
	bool ownerPaletteRequested=(SceneUniforms.virtualGeometryFlags&0x80u)!=0u;
	bool hasBuffer=SceneUniforms.meshDataIndex!=kInvalidAnimationGraphicsBufferIndex;
	bool hasRecord=SceneUniforms.textureIndex!=kInvalidAnimationGraphicsBufferIndex;
	if((SceneUniforms.virtualGeometryFlags&0x80000000u)!=0u
		||(SceneUniforms.virtualGeometryPayloadSource!=0u&&!ownerPaletteRequested)
		||hasBuffer!=hasRecord||(ownerPaletteRequested&&!hasBuffer)
		||((SceneUniforms.virtualGeometryFlags>>8u)&255u)!=0u)return rejected;
	bool currentDeformation=hasBuffer&&hasRecord;
	DefaultGpuAnimationGraphicsRecord animationRecord=(DefaultGpuAnimationGraphicsRecord)0;
	if(currentDeformation)
	{
		if(!TryLoadGpuAnimationGraphicsRecordAt(SceneUniforms.meshDataIndex,
			SceneUniforms.textureIndex,animationRecord))return rejected;
		uint expectedStages=128u|(animationRecord.morphCount!=0u?32u:0u);
		if(animationRecord.workFlags!=0u||animationRecord.generationRecordIndex!=0u
			||animationRecord.stageMask!=expectedStages)return rejected;
	}
	uint decodeCount=0,decodeStride=0;indexedDecode.GetDimensions(decodeCount,decodeStride);
	if(decodeCount==0u||decodeStride!=64u)return rejected;
	MeshDecodeInfo decode=indexedDecode[0];
	uint meshletCount=decode.counts.w&0x7fffffffu;
	bool isU16=(decode.counts.w&0x80000000u)!=0u;
	uint physicalMeshlets=0,meshletStride=0;indexedMeshlets.GetDimensions(physicalMeshlets,meshletStride);
	if(meshletIndex>=meshletCount||meshletIndex>=physicalMeshlets||meshletStride!=16u
		||decode.counts.x==0u||decode.counts.y==0u||decode.counts.z==0u)return rejected;
	IndexedMeshletRecord record=indexedMeshlets[meshletIndex];
	if(record.vertexCount==0u||record.vertexCount>64u||record.primitiveIndexCount%3u!=0u
		||record.primitiveIndexCount>124u*3u||primitiveIndex>=record.primitiveIndexCount/3u
		||record.vertexOffset>decode.counts.y||record.vertexCount>decode.counts.y-record.vertexOffset
		||record.primitiveIndexOffset>decode.counts.z||record.primitiveIndexCount>decode.counts.z-record.primitiveIndexOffset)return rejected;
	uint vertexBytes=0;indexedVertexBytes.GetDimensions(vertexBytes);
	if(decode.counts.x>vertexBytes/16u)return rejected;
	// Validate all three corners before pulling this invocation's vertex. A bad
	// neighboring remap must reject the whole triangle, not leave a clipped wedge.
	uint owner=0;
	for(uint k=0u;k<3u;++k)
	{
		uint local=0,mapped=0;
		if(!ReadIndexedPrimitiveByte(record.primitiveIndexOffset+primitiveIndex*3u+k,local)
			||local>=record.vertexCount||!ReadIndexedOwnerVertex(record.vertexOffset+local,isU16,mapped)
			||mapped>=decode.counts.x)return rejected;
		if(k==corner)owner=mapped;
	}
	uint4 packed=indexedVertexBytes.Load4(owner*16u);
	TeDecodedPackedVertex vertex=TeDecodePackedVertex(packed);
	float3 position=decode.positionOffset.xyz+vertex.normalizedPosition*decode.positionScale.xyz;
	float3 localNormal=vertex.normal,localTangent=vertex.tangent;
	if(currentDeformation&&!TryApplyOwnerMeshDeformationStrict(animationRecord,
		decode,owner,position,localNormal,localTangent))return rejected;
	float2 uv=decode.uvOffsetScale.xy+vertex.normalizedTextureCoordinate*decode.uvOffsetScale.zw;
	TerraTranslatedWorldPosition world=TerraLocalizeModelPosition(SceneUniforms.model,position);
	float3 normal=IndexedTransformNormal(localNormal),tangent=IndexedTransformDirection(localTangent);
	float4 clip=mul(SceneUniforms.viewProj,float4(world.value,1));
	if(!all(isfinite(clip))||!all(isfinite(world.value))||!all(isfinite(normal))||!all(isfinite(tangent))||!all(isfinite(uv)))return rejected;
	DefaultIndexedVertexOutput output=(DefaultIndexedVertexOutput)0;
	output.surface.position=clip;output.surface.uv=uv;output.surface.worldPosition=world.value;
	output.surface.normal=normal;output.surface.tangent=float4(tangent,vertex.tangentSign);
	output.surface.meshletIndex=meshletIndex;
	// Debug geometry modes are not qualified by this first route. They are
	// rejected above; ordinary material shading never consumes these fields.
	output.surface.meshletBoundsRadius=0;output.surface.meshletCullFlags=4;
	output.surface.virtualGeometryDebugData=SceneUniforms.selectedVirtualLod&255u;
	// Match existing ungrouped Default.mesh: the unchanged fragment resolves
	// the immutable object table key from selectedVirtualPageNeedOffset.
	output.surface.objectIdentity=uint4(0,0,0,0);
	output.stablePrimitiveId=stable;
	return output;
}
