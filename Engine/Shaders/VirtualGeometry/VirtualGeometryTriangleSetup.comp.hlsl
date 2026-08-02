// GPU-native virtual-geometry triangle setup.
//
// CPU preparation uploads one immutable packet record plus one compact work
// record per selected meshlet. GPU lanes decode page-local TEMF vertices and
// optional demanded-page deformation output directly into the raster ABI.
#pragma pack_matrix(column_major)

#include "VirtualGeometryAbi.hlsli"
#include "VirtualGeometryPackedVertex.hlsli"
#include "VirtualGeometryRasterTriangle.hlsli"

[[vk::binding(9, 0)]] StructuredBuffer<uint> globalBuffers[];
#include "AnimatedDeformedGeometry.hlsli"

struct TriangleSetupPacket
{
	column_major float4x4 viewProjection;
	column_major float4x4 model;
	column_major float4x4 normalTransform;
	float4 editorTint;
	uint4 payloadState; // descriptor, page, material flags, deformation bridge
	uint4 identity;
	uint4 shadingOffsets;
	uint4 shadingState;
	uint4 debugState;
	uint4 deformationState; // bridge header, owner meshlet base, reserved
};

struct TriangleSetupMeshlet
{
	uint packetIndex;
	uint localMeshletIndex;
	uint primitiveBase;
	uint triangleCount;
};

struct SetupConstants
{
	uint meshletWorkCount;
	uint triangleCapacity;
	uint generation;
	uint reserved;
};

[[vk::binding(0, 1)]] StructuredBuffer<TriangleSetupPacket> setupPackets;
[[vk::binding(1, 1)]] StructuredBuffer<TriangleSetupMeshlet> setupMeshlets;
[[vk::binding(2, 1)]] StructuredBuffer<uint> pagePayload;
[[vk::binding(3, 1)]] RWStructuredBuffer<RasterTriangle> triangles;
[[vk::binding(4, 1)]] RWStructuredBuffer<uint> diagnostics;
[[vk::binding(5, 1)]] RWStructuredBuffer<uint> setupCounters;
[[vk::push_constant]] ConstantBuffer<SetupConstants> Setup;

static const uint kDescriptorStrideWords = 32u;
static const uint kDescriptorVertexWordOffset = 16u;
static const uint kDescriptorVertexWordSize = 17u;
static const uint kDescriptorPrimitiveWordOffset = 18u;
static const uint kDescriptorPrimitiveByteSize = 19u;
static const uint kDescriptorRemapWordOffset = 20u;
static const uint kDescriptorRemapCount = 21u;
static const uint kDescriptorMeshletWordOffset = 22u;
static const uint kDescriptorMeshletCount = 23u;
static const uint kDescriptorDecodeWordOffset = 26u;
static const uint kDescriptorLocalVertexCount = 29u;

uint LoadPayloadWord(uint wordOffset, uint fallbackValue)
{
	uint count;
	uint stride;
	pagePayload.GetDimensions(count, stride);
	return wordOffset < count ? pagePayload[wordOffset] : fallbackValue;
}

uint LoadDescriptorWord(uint descriptorIndex, uint field, uint fallbackValue)
{
	if(field >= kDescriptorStrideWords
		|| descriptorIndex > (0xffffffffu - field) / kDescriptorStrideWords)
	{
		return fallbackValue;
	}
	return LoadPayloadWord(descriptorIndex * kDescriptorStrideWords + field, fallbackValue);
}

uint LoadPrimitiveByte(uint wordOffset, uint byteSize, uint element)
{
	if(element >= byteSize) return 0xffu;
	const uint packed = LoadPayloadWord(wordOffset + (element >> 2u), 0u);
	return (packed >> ((element & 3u) * 8u)) & 0xffu;
}

uint4 LoadPackedVertex(uint vertexWordOffset, uint vertexWordSize, uint vertex)
{
	const uint word = vertex * 4u;
	if(word > vertexWordSize || 4u > vertexWordSize - word) return 0u;
	return uint4(
		LoadPayloadWord(vertexWordOffset + word + 0u, 0u),
		LoadPayloadWord(vertexWordOffset + word + 1u, 0u),
		LoadPayloadWord(vertexWordOffset + word + 2u, 0u),
		LoadPayloadWord(vertexWordOffset + word + 3u, 0u));
}

float3 NormalizeOrFallback(float3 value, float3 fallbackValue)
{
	const float lengthSquared = dot(value, value);
	return lengthSquared > 1.0e-12f && all(isfinite(value))
		? value * rsqrt(lengthSquared) : fallbackValue;
}

[numthreads(64, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint3 laneId : SV_GroupThreadID)
{
	if(groupId.x >= Setup.meshletWorkCount) return;
	const TriangleSetupMeshlet work = setupMeshlets[groupId.x];
	if(laneId.x >= work.triangleCount
		|| work.primitiveBase > Setup.triangleCapacity
		|| laneId.x >= Setup.triangleCapacity - work.primitiveBase)
	{
		return;
	}

	uint packetCount;
	uint packetStride;
	setupPackets.GetDimensions(packetCount, packetStride);
	if(work.packetIndex >= packetCount)
	{
		InterlockedOr(diagnostics[0], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_INVALID_PAYLOAD);
		return;
	}
	const TriangleSetupPacket packet = setupPackets[work.packetIndex];
	const uint descriptor = packet.payloadState.x;
	const uint meshletCount = LoadDescriptorWord(descriptor, kDescriptorMeshletCount, 0u);
	if(work.localMeshletIndex >= meshletCount)
	{
		InterlockedOr(diagnostics[0], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_INVALID_PAYLOAD);
		return;
	}

	const uint meshletWord = LoadDescriptorWord(descriptor, kDescriptorMeshletWordOffset, 0u)
		+ work.localMeshletIndex * 4u;
	const uint vertexOffset = LoadPayloadWord(meshletWord + 0u, 0u);
	const uint vertexCount = LoadPayloadWord(meshletWord + 1u, 0u);
	const uint primitiveOffset = LoadPayloadWord(meshletWord + 2u, 0u);
	const uint primitiveIndexCount = LoadPayloadWord(meshletWord + 3u, 0u);
	const uint sourcePrimitive = laneId.x * 3u;
	if(sourcePrimitive + 2u >= primitiveIndexCount)
	{
		InterlockedOr(diagnostics[0], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_INVALID_PAYLOAD);
		return;
	}

	const uint primitiveWordOffset = LoadDescriptorWord(descriptor, kDescriptorPrimitiveWordOffset, 0u);
	const uint primitiveByteSize = LoadDescriptorWord(descriptor, kDescriptorPrimitiveByteSize, 0u);
	const uint remapWordOffset = LoadDescriptorWord(descriptor, kDescriptorRemapWordOffset, 0u);
	const uint remapCount = LoadDescriptorWord(descriptor, kDescriptorRemapCount, 0u);
	const uint vertexWordOffset = LoadDescriptorWord(descriptor, kDescriptorVertexWordOffset, 0u);
	const uint vertexWordSize = LoadDescriptorWord(descriptor, kDescriptorVertexWordSize, 0u);
	const uint localVertexCount = LoadDescriptorWord(descriptor, kDescriptorLocalVertexCount, 0u);
	const uint decodeWordOffset = LoadDescriptorWord(descriptor, kDescriptorDecodeWordOffset, 0u);
	const float3 positionOffset = asfloat(uint3(
		LoadPayloadWord(decodeWordOffset + 0u, 0u),
		LoadPayloadWord(decodeWordOffset + 1u, 0u),
		LoadPayloadWord(decodeWordOffset + 2u, 0u)));
	const float3 positionScale = asfloat(uint3(
		LoadPayloadWord(decodeWordOffset + 4u, 0u),
		LoadPayloadWord(decodeWordOffset + 5u, 0u),
		LoadPayloadWord(decodeWordOffset + 6u, 0u)));
	const float4 uvOffsetScale = asfloat(uint4(
		LoadPayloadWord(decodeWordOffset + 8u, 0u),
		LoadPayloadWord(decodeWordOffset + 9u, 0u),
		LoadPayloadWord(decodeWordOffset + 10u, 0u),
		LoadPayloadWord(decodeWordOffset + 11u, 0u)));

	TeAnimatedGraphicsBridge deformationBridge;
	TeAnimatedGraphicsPage deformationPage;
	const bool hasDeformation = TeTryLoadAnimatedGraphicsBridge(
		packet.payloadState.w, packet.deformationState.x, deformationBridge)
		&& TeTryResolveAnimatedGraphicsPage(
			packet.payloadState.w, deformationBridge, packet.payloadState.y,
			deformationPage);

	float4 clip[3];
	float4 world[3];
	float4 normal[3];
	float4 tangent[3];
	float4 uv[3];
	[unroll]
	for(uint corner = 0u; corner < 3u; ++corner)
	{
		const uint localMeshletVertex = LoadPrimitiveByte(
			primitiveWordOffset, primitiveByteSize,
			primitiveOffset + sourcePrimitive + corner);
		const uint remapIndex = vertexOffset + localMeshletVertex;
		if(localMeshletVertex >= vertexCount || remapIndex >= remapCount)
		{
			InterlockedOr(diagnostics[0], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_INVALID_PAYLOAD);
			return;
		}
		const uint localVertex = LoadPayloadWord(remapWordOffset + remapIndex, 0xffffffffu);
		if(localVertex >= localVertexCount)
		{
			InterlockedOr(diagnostics[0], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_INVALID_PAYLOAD);
			return;
		}
		const TeDecodedPackedVertex decoded = TeDecodePackedVertex(
			LoadPackedVertex(vertexWordOffset, vertexWordSize, localVertex));
		float3 localPosition = positionOffset + decoded.normalizedPosition * positionScale;
		float3 localNormal = decoded.normal;
		float3 localTangent = decoded.tangent;
		float tangentSign = decoded.tangentSign;
		TeAnimatedDeformedVertex deformedVertex;
		if(hasDeformation && TeTryLoadAnimatedVertex(
			deformationBridge, deformationPage, localVertex, deformedVertex))
		{
			localPosition = deformedVertex.currentPosition.xyz;
			localNormal = deformedVertex.normal.xyz;
			localTangent = deformedVertex.tangentAndSign.xyz;
			tangentSign = deformedVertex.tangentAndSign.w;
		}
		world[corner] = mul(packet.model, float4(localPosition, 1.0f));
		clip[corner] = mul(packet.viewProjection, world[corner]);
		normal[corner] = float4(NormalizeOrFallback(
			mul(packet.normalTransform, float4(localNormal, 0.0f)).xyz,
			float3(0.0f, 1.0f, 0.0f)), 0.0f);
		tangent[corner] = float4(NormalizeOrFallback(
			mul(packet.model, float4(localTangent, 0.0f)).xyz,
			float3(1.0f, 0.0f, 0.0f)), tangentSign);
		uv[corner] = float4(
			uvOffsetScale.xy + decoded.normalizedTextureCoordinate * uvOffsetScale.zw,
			0.0f, 0.0f);
	}

	RasterTriangle output;
	output.clip0 = clip[0]; output.clip1 = clip[1]; output.clip2 = clip[2];
	output.world0 = world[0]; output.world1 = world[1]; output.world2 = world[2];
	output.normal0 = normal[0]; output.normal1 = normal[1]; output.normal2 = normal[2];
	output.tangent0 = tangent[0]; output.tangent1 = tangent[1]; output.tangent2 = tangent[2];
	output.uv0 = uv[0]; output.uv1 = uv[1]; output.uv2 = uv[2];
	output.editorTint = packet.editorTint;
	output.identity = uint4(
		work.primitiveBase + laneId.x,
		packet.identity.y,
		Setup.generation,
		packet.identity.w);
	output.shadingOffsets = packet.shadingOffsets;
	output.shadingState = packet.shadingState;
	output.shadingState.z = packet.deformationState.y + work.localMeshletIndex;
	output.debugState = packet.debugState;
	triangles[work.primitiveBase + laneId.x] = output;
}
