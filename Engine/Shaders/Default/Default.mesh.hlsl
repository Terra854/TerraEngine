#pragma pack_matrix(column_major)

// This struct matches the decoded mesh-shader vertex buffer uploaded by VulkanContext.
struct MeshShaderVertex
{
	float3 position;
	float3 normal;
	float2 uv;
};

// Meshlet table emitted by the mesh compiler and mirrored from TerraEngineMeshFile.h.
struct MeshletRecord
{
	uint vertexOffset;
	uint vertexCount;
	uint primitiveIndexOffset;
	uint primitiveIndexCount;
};

[[vk::binding(0, 1)]] StructuredBuffer<MeshShaderVertex> meshletVertices;
[[vk::binding(1, 1)]] StructuredBuffer<uint> primitiveIndices;
[[vk::binding(2, 1)]] StructuredBuffer<MeshletRecord> meshletRecords;

struct PushConstants
{
	column_major float4x4 viewProj;
	column_major float4x4 model;
	uint textureIndex;
	uint materialIndex;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> SceneUniforms;

struct PSIn
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float3 normal : NORMAL;
};

static const uint kMaxMeshletVertices = 64;
static const uint kMaxMeshletTriangles = 124;

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void main(
	uint3 groupId : SV_GroupID,
	out vertices PSIn outputVertices[kMaxMeshletVertices],
	out indices uint3 outputTriangles[kMaxMeshletTriangles])
{
	const uint meshletIndex = groupId.x;
	const MeshletRecord record = meshletRecords[meshletIndex];

	// Defensive clamps keep malformed payloads from writing outside shader-declared arrays.
	const uint vertexCount = min(record.vertexCount, kMaxMeshletVertices);
	const uint primitiveCount = min(record.primitiveIndexCount / 3u, kMaxMeshletTriangles);
	SetMeshOutputCounts(vertexCount, primitiveCount);

	for (uint vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
	{
		const MeshShaderVertex src = meshletVertices[record.vertexOffset + vertexIndex];
		const float4 worldPosition = mul(SceneUniforms.model, float4(src.position, 1.0f));

		PSIn outVertex;
		outVertex.position = mul(SceneUniforms.viewProj, worldPosition);
		outVertex.uv = src.uv;
		outVertex.normal = mul((float3x3)SceneUniforms.model, src.normal);
		outputVertices[vertexIndex] = outVertex;
	}

	for (uint primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
	{
		const uint primitiveOffset = record.primitiveIndexOffset + primitiveIndex * 3u;
		const uint index0 = primitiveIndices[primitiveOffset + 0] - record.vertexOffset;
		const uint index1 = primitiveIndices[primitiveOffset + 1] - record.vertexOffset;
		const uint index2 = primitiveIndices[primitiveOffset + 2] - record.vertexOffset;

		// Invalid primitive references collapse to a degenerate triangle instead of OOB.
		if (index0 >= vertexCount || index1 >= vertexCount || index2 >= vertexCount)
		{
			outputTriangles[primitiveIndex] = uint3(0, 0, 0);
			continue;
		}

		outputTriangles[primitiveIndex] = uint3(index0, index1, index2);
	}
}
