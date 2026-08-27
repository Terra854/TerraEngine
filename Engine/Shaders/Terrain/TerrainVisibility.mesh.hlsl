// Terrain heightfield visibility mesh stage. The runtime binds the cooked
// position/UV stream and triangle index stream as storage buffers, allowing a
// mesh workgroup to assemble terrain without a legacy vertex-input pipeline.
static const uint kTerrainTrianglesPerMeshGroup = 21u;
static const uint kTerrainVertexStrideBytes = 20u;
static const uint kTerrainPackedPageVertexStrideBytes = 20u;

struct TerrainTaskPayload
{
	uint firstTriangle;
};

struct TerrainFragmentInput
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float3 normal : TEXCOORD1;
};

cbuffer TerrainViewConstants : register(b0)
{
	float4x4 terrainViewProjection;
	uint terrainTriangleCount;
	uint terrainPageBacked;
	uint terrainPagePublished;
	uint terrainPageTriangleCount;
	float2 terrainAreaWorldOriginMetres;
	float terrainSampleSpacingMetres;
	float terrainMorphFactor;
	uint terrainPageFirstVertexByte;
	uint terrainPageFirstTriangleByte;
	uint terrainPageFirstStitchIndexByte;
	uint terrainPageStitchMask;
};

ByteAddressBuffer terrainVertexBytes : register(t3);
ByteAddressBuffer terrainTriangleIndices : register(t4);
ByteAddressBuffer terrainPageBytes : register(t5);
ByteAddressBuffer terrainPageStitchIndices : register(t6);

float3 DecodeTerrainPackedNormal(uint packed)
{
	const float x = (float(packed & 0xffu) / 255.0f) * 2.0f - 1.0f;
	const float z = (float((packed >> 8u) & 0xffu) / 255.0f) * 2.0f - 1.0f;
	return normalize(float3(x, sqrt(saturate(1.0f - x * x - z * z)), z));
}

TerrainFragmentInput LoadTerrainVertex(uint vertexIndex)
{
	const uint byteOffset = vertexIndex * kTerrainVertexStrideBytes;
	TerrainFragmentInput output;
	output.position = mul(terrainViewProjection,
		float4(asfloat(terrainVertexBytes.Load3(byteOffset)), 1.0f));
	output.uv = asfloat(terrainVertexBytes.Load2(byteOffset + 12u));
	output.normal = float3(0.0f, 1.0f, 0.0f);
	return output;
}

TerrainFragmentInput LoadTerrainPageVertex(uint vertexIndex)
{
	const uint byteOffset = terrainPageFirstVertexByte
		+ vertexIndex * kTerrainPackedPageVertexStrideBytes;
	const uint packedXZ = terrainPageBytes.Load(byteOffset);
	const uint localX = packedXZ & 0xffffu;
	const uint localZ = packedXZ >> 16u;
	const float childHeight = float(asint(terrainPageBytes.Load(byteOffset + 4u))) / 1000.0f;
	const float parentHeight = float(asint(terrainPageBytes.Load(byteOffset + 8u))) / 1000.0f;
	const uint packedNormals = terrainPageBytes.Load(byteOffset + 12u);
	const float3 childNormal = DecodeTerrainPackedNormal(packedNormals & 0xffffu);
	const float3 parentNormal = DecodeTerrainPackedNormal(packedNormals >> 16u);
	const float height = lerp(parentHeight, childHeight, saturate(terrainMorphFactor));
	TerrainFragmentInput output;
	const float2 localMetres = float2(localX, localZ) * terrainSampleSpacingMetres;
	output.position = mul(terrainViewProjection, float4(
		terrainAreaWorldOriginMetres.x + localMetres.x,
		height,
		terrainAreaWorldOriginMetres.y + localMetres.y, 1.0f));
	output.uv = float2(localX, localZ) / 256.0f;
	output.normal = normalize(lerp(parentNormal, childNormal, saturate(terrainMorphFactor)));
	return output;
}

TerrainFragmentInput LoadSelectedTerrainVertex(uint vertexIndex)
{
	// DXC does not permit a conditional operator whose result is a structure.
	// Keep route selection explicit while returning the same typed mesh output.
	if(terrainPageBacked != 0u)
	{
		return LoadTerrainPageVertex(vertexIndex);
	}
	return LoadTerrainVertex(vertexIndex);
}

[outputtopology("triangle")]
[numthreads(kTerrainTrianglesPerMeshGroup, 1, 1)]
void main(
	in payload TerrainTaskPayload taskPayload,
	uint3 threadId : SV_GroupThreadID,
	out vertices TerrainFragmentInput outputVertices[63],
	out indices uint3 outputTriangles[kTerrainTrianglesPerMeshGroup])
{
	const uint selectedTriangleCount = terrainPageBacked != 0u
		? terrainPageTriangleCount : terrainTriangleCount;
	const uint remainingTriangleCount = selectedTriangleCount
		- taskPayload.firstTriangle;
	const uint triangleCount = min(
		remainingTriangleCount, kTerrainTrianglesPerMeshGroup);
	SetMeshOutputCounts(triangleCount * 3u, triangleCount);

	if(threadId.x >= triangleCount)
	{
		return;
	}

	const uint sourceTriangle = taskPayload.firstTriangle + threadId.x;
	const uint3 sourceIndices = terrainPageBacked != 0u
		? terrainPageStitchIndices.Load3(
			terrainPageFirstStitchIndexByte + sourceTriangle * 3u * 4u)
		: terrainTriangleIndices.Load3(sourceTriangle * 3u * 4u);
	const uint outputVertex = threadId.x * 3u;
	outputVertices[outputVertex + 0u] = LoadSelectedTerrainVertex(sourceIndices.x);
	outputVertices[outputVertex + 1u] = LoadSelectedTerrainVertex(sourceIndices.y);
	outputVertices[outputVertex + 2u] = LoadSelectedTerrainVertex(sourceIndices.z);
	outputTriangles[threadId.x] = uint3(
		outputVertex + 0u,
		outputVertex + 1u,
		outputVertex + 2u);
}
