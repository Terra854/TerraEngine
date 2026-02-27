#pragma pack_matrix(column_major)

// Meshlet table emitted by the mesh compiler and mirrored from TerraEngineMeshFile.h.
struct MeshletRecord
{
	uint vertexOffset;
	uint vertexCount;
	uint primitiveIndexOffset;
	uint primitiveIndexCount;
};

// Packed decode constants for direct GPU consumption of TEMF payloads.
struct MeshDecodeInfo
{
	float4 positionOffset; // xyz = position offset
	float4 positionScale;  // xyz = position scale
	float4 uvOffsetScale;  // xy = uv offset, zw = uv scale
	uint4 counts;          // x = vertex count, y = remap count, z = primitive count, w = meshlet count
};

// Task stage writes a compacted visible meshlet index list into payload.
static const uint kTaskBatchSize = 32u;
struct TaskPayload
{
	uint meshletIndices[kTaskBatchSize];
};

[[vk::binding(0, 1)]] ByteAddressBuffer meshletVertexBytes;
[[vk::binding(1, 1)]] ByteAddressBuffer primitiveIndices;
[[vk::binding(2, 1)]] StructuredBuffer<MeshletRecord> meshletRecords;
[[vk::binding(3, 1)]] ByteAddressBuffer meshletVertexRemap;
[[vk::binding(4, 1)]] StructuredBuffer<MeshDecodeInfo> meshDecodeInfo;

struct PushConstants
{
	column_major float4x4 viewProj;
	column_major float4x4 model;
	uint materialIndex;
	uint meshDataIndex;
	uint textureIndex;
	uint vrsActive;
	float cullingEpsilon;
	float padding0;
	float padding1;
	float padding2;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> SceneUniforms;

struct PSIn
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float3 worldPosition : TEXCOORD1;
	float3 normal : NORMAL;
};

static const uint kMaxMeshletVertices = 64;
static const uint kMaxMeshletTriangles = 124;

float DecodeSnorm10(uint packedValue, uint bitOffset)
{
	int value = int((packedValue >> bitOffset) & 0x3FFu);
	if ((value & 0x200) != 0)
	{
		value |= ~0x3FF;
	}

	float normalized = float(value) / 511.0f;
	return clamp(normalized, -1.0f, 1.0f);
}

float DecodeUnorm16(uint packedValue, uint bitOffset)
{
	const uint value = (packedValue >> bitOffset) & 0xFFFFu;
	return float(value) / 65535.0f;
}

float3 DecodeOct32Normal(uint packedValue)
{
	float2 f = float2(
		float(int(packedValue & 0xFFFF)) / 32767.0,
		float(int(packedValue >> 16)) / 32767.0
	);

	float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
	float t = saturate(-n.z);
	n.xy += select(n.xy >= 0.0, -t, t);
	return normalize(n);
}

uint LoadPrimitiveIndex(uint primitiveElementIndex)
{
	const uint alignedByteOffset = primitiveElementIndex & ~3u;
	const uint packed = primitiveIndices.Load(alignedByteOffset);
	const uint shift = (primitiveElementIndex & 3u) * 8u;
	return (packed >> shift) & 0xFFu;
}

void EmitMeshlet(
	uint meshletIndex,
	out vertices PSIn outputVertices[kMaxMeshletVertices],
	out indices uint3 outputTriangles[kMaxMeshletTriangles])
{
	const MeshDecodeInfo decode = meshDecodeInfo[0];
	const uint meshletCount = decode.counts.w & 0x7FFFFFFFu;
	const uint isU16 = (decode.counts.w >> 31u) & 1u;

	if (meshletIndex >= meshletCount)
	{
		SetMeshOutputCounts(0, 0);
		return;
	}

	const MeshletRecord record = meshletRecords[meshletIndex];

	// Defensive clamps keep malformed payloads from writing outside shader-declared arrays.
	const uint vertexCount = min(record.vertexCount, kMaxMeshletVertices);
	const uint primitiveCount = min(record.primitiveIndexCount / 3u, kMaxMeshletTriangles);
	SetMeshOutputCounts(vertexCount, primitiveCount);

	for (uint vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
	{
		const uint remapIndex = record.vertexOffset + vertexIndex;
		uint globalVertexIndex = 0u;
		if (remapIndex < decode.counts.y)
		{
			if (isU16 != 0)
			{
				const uint byteOffset = remapIndex * 2u;
				const uint alignedByteOffset = byteOffset & ~3u;
				const uint packed = meshletVertexRemap.Load(alignedByteOffset);
				const uint shift = (byteOffset & 2u) * 8u;
				globalVertexIndex = (packed >> shift) & 0xFFFFu;
			}
			else
			{
				globalVertexIndex = meshletVertexRemap.Load(remapIndex * 4u);
			}
		}
		if (globalVertexIndex >= decode.counts.x)
		{
			globalVertexIndex = 0u;
		}

		const uint packedVertexByteOffset = globalVertexIndex * 12u;
		const uint packedPosition = meshletVertexBytes.Load(packedVertexByteOffset + 0u);
		const uint packedNormal = meshletVertexBytes.Load(packedVertexByteOffset + 4u);
		const uint packedUv = meshletVertexBytes.Load(packedVertexByteOffset + 8u);

		const float3 qPosition = float3(
			DecodeSnorm10(packedPosition, 0u),
			DecodeSnorm10(packedPosition, 10u),
			DecodeSnorm10(packedPosition, 20u));
		const float3 position = decode.positionOffset.xyz + qPosition * decode.positionScale.xyz;
		const float3 normal = DecodeOct32Normal(packedNormal);
		const float2 qUv = float2(
			DecodeUnorm16(packedUv, 0u),
			DecodeUnorm16(packedUv, 16u));
		const float2 uv = decode.uvOffsetScale.xy + qUv * decode.uvOffsetScale.zw;

		const float4 worldPosition = mul(SceneUniforms.model, float4(position, 1.0f));

		PSIn outVertex;
		outVertex.position = mul(SceneUniforms.viewProj, worldPosition);
		outVertex.uv = uv;
		outVertex.worldPosition = worldPosition.xyz;
		outVertex.normal = mul((float3x3)SceneUniforms.model, normal);
		outputVertices[vertexIndex] = outVertex;
	}

	for (uint primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
	{
		const uint primitiveOffset = record.primitiveIndexOffset + primitiveIndex * 3u;
		const uint index0 = LoadPrimitiveIndex(primitiveOffset + 0u);
		const uint index1 = LoadPrimitiveIndex(primitiveOffset + 1u);
		const uint index2 = LoadPrimitiveIndex(primitiveOffset + 2u);

		// Invalid primitive references collapse to a degenerate triangle instead of OOB.
		if (index0 >= vertexCount || index1 >= vertexCount || index2 >= vertexCount)
		{
			outputTriangles[primitiveIndex] = uint3(0, 0, 0);
			continue;
		}

		outputTriangles[primitiveIndex] = uint3(index0, index1, index2);
	}
}

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void main(
	in payload TaskPayload taskPayload,
	uint3 groupId : SV_GroupID,
	out vertices PSIn outputVertices[kMaxMeshletVertices],
	out indices uint3 outputTriangles[kMaxMeshletTriangles])
{
	const uint visibleIndex = groupId.x;
	const uint meshletIndex = taskPayload.meshletIndices[visibleIndex];
	EmitMeshlet(meshletIndex, outputVertices, outputTriangles);
}
