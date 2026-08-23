// Terrain visibility task stage. One task group owns one bounded triangle
// packet and dispatches a mesh workgroup only when the packet contains work.
static const uint kTerrainTrianglesPerMeshGroup = 21u;

struct TerrainTaskPayload
{
	uint firstTriangle;
};

cbuffer TerrainViewConstants : register(b0)
{
	float4x4 terrainViewProjection;
	uint terrainTriangleCount;
	uint3 terrainViewPadding;
};

groupshared TerrainTaskPayload gTerrainTaskPayload;

[numthreads(1, 1, 1)]
void main(uint3 groupId : SV_GroupID)
{
	const uint firstTriangle = groupId.x * kTerrainTrianglesPerMeshGroup;
	if(firstTriangle >= terrainTriangleCount)
	{
		return;
	}

	gTerrainTaskPayload.firstTriangle = firstTriangle;
	DispatchMesh(1u, 1u, 1u, gTerrainTaskPayload);
}
