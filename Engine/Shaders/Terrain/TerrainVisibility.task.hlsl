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
	uint terrainPageBacked;
	uint terrainPagePublished;
	uint terrainPageTriangleCount;
};

groupshared TerrainTaskPayload gTerrainTaskPayload;

[numthreads(1, 1, 1)]
void main(uint3 groupId : SV_GroupID)
{
	// Page data is visible only after the protected VG publication boundary.
	// A decoded/uploading page never produces task work, so partial residency
	// cannot expose uninitialized vertices or leave a coverage claim behind.
	if(terrainPageBacked != 0u && terrainPagePublished == 0u)
		return;
	const uint selectedTriangleCount = terrainPageBacked != 0u
		? terrainPageTriangleCount : terrainTriangleCount;
	const uint firstTriangle = groupId.x * kTerrainTrianglesPerMeshGroup;
	if(firstTriangle >= selectedTriangleCount)
	{
		return;
	}

	gTerrainTaskPayload.firstTriangle = firstTriangle;
	DispatchMesh(1u, 1u, 1u, gTerrainTaskPayload);
}
