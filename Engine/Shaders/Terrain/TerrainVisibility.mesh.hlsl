// Terrain heightfield visibility mesh stage. The runtime binds the cooked
// position/UV stream and triangle index stream as storage buffers, allowing a
// mesh workgroup to assemble terrain without a legacy vertex-input pipeline.
static const uint kTerrainTrianglesPerMeshGroup = 21u;
static const uint kTerrainVertexStrideBytes = 20u;

struct TerrainTaskPayload
{
	uint firstTriangle;
};

struct TerrainFragmentInput
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

cbuffer TerrainViewConstants : register(b0)
{
	float4x4 terrainViewProjection;
	uint terrainTriangleCount;
	uint3 terrainViewPadding;
};

ByteAddressBuffer terrainVertexBytes : register(t3);
ByteAddressBuffer terrainTriangleIndices : register(t4);

TerrainFragmentInput LoadTerrainVertex(uint vertexIndex)
{
	const uint byteOffset = vertexIndex * kTerrainVertexStrideBytes;
	TerrainFragmentInput output;
	output.position = mul(terrainViewProjection,
		float4(asfloat(terrainVertexBytes.Load3(byteOffset)), 1.0f));
	output.uv = asfloat(terrainVertexBytes.Load2(byteOffset + 12u));
	return output;
}

[outputtopology("triangle")]
[numthreads(kTerrainTrianglesPerMeshGroup, 1, 1)]
void main(
	in payload TerrainTaskPayload taskPayload,
	uint3 threadId : SV_GroupThreadID,
	out vertices TerrainFragmentInput outputVertices[63],
	out indices uint3 outputTriangles[kTerrainTrianglesPerMeshGroup])
{
	const uint remainingTriangleCount = terrainTriangleCount
		- taskPayload.firstTriangle;
	const uint triangleCount = min(
		remainingTriangleCount, kTerrainTrianglesPerMeshGroup);
	SetMeshOutputCounts(triangleCount * 3u, triangleCount);

	if(threadId.x >= triangleCount)
	{
		return;
	}

	const uint sourceTriangle = taskPayload.firstTriangle + threadId.x;
	const uint3 sourceIndices = terrainTriangleIndices.Load3(
		sourceTriangle * 3u * 4u);
	const uint outputVertex = threadId.x * 3u;
	outputVertices[outputVertex + 0u] = LoadTerrainVertex(sourceIndices.x);
	outputVertices[outputVertex + 1u] = LoadTerrainVertex(sourceIndices.y);
	outputVertices[outputVertex + 2u] = LoadTerrainVertex(sourceIndices.z);
	outputTriangles[threadId.x] = uint3(
		outputVertex + 0u,
		outputVertex + 1u,
		outputVertex + 2u);
}
