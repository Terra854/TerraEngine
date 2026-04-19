struct SkyTaskPayload
{
	uint dispatchToken;
};

struct PSInput
{
	float4 position : SV_Position;
	float2 ndc : TEXCOORD0;
};

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void MSMain(
	in payload SkyTaskPayload taskPayload,
	uint3 groupId : SV_GroupID,
	out vertices PSInput outputVertices[3],
	out indices uint3 outputTriangles[1])
{
	// Emit the same fullscreen triangle the old vertex path used so the
	// fragment shader keeps reconstructing rays from NDC exactly as before.
	static const float2 kPositions[3] = {
		float2(-1.0, -1.0),
		float2(-1.0,  3.0),
		float2( 3.0, -1.0)
	};

	SetMeshOutputCounts(3, 1);

	[unroll]
	for (uint vertexIndex = 0u; vertexIndex < 3u; ++vertexIndex)
	{
		PSInput outputVertex;
		outputVertex.position = float4(kPositions[vertexIndex], 0.0, 1.0);
		outputVertex.ndc = kPositions[vertexIndex];
		outputVertices[vertexIndex] = outputVertex;
	}

	outputTriangles[0] = uint3(0u, 1u, 2u);
}
