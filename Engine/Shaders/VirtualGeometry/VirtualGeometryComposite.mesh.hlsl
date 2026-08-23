// The mesh stage emits one procedural fullscreen triangle. No vertex buffers
// are needed because the fragment stage reconstructs the compute-raster winner
// directly from the visibility buffers for each covered pixel.
struct CompositeTaskPayload
{
	uint dispatchToken;
};

struct CompositeFragmentInput
{
	float4 position : SV_Position;
};

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void main(
	in payload CompositeTaskPayload taskPayload,
	out vertices CompositeFragmentInput outputVertices[3],
	out indices uint3 outputTriangles[1])
{
	// Match the former vertex shader's clockwise fullscreen triangle after the
	// Vulkan viewport transform so fragment coverage and depth reconstruction
	// remain unchanged.
	static const float2 kPositions[3] = {
		float2(-1.0f, 1.0f),
		float2(3.0f, 1.0f),
		float2(-1.0f, -3.0f)
	};

	SetMeshOutputCounts(3u, 1u);
	[unroll]
	for(uint vertexIndex = 0u; vertexIndex < 3u; ++vertexIndex)
	{
		CompositeFragmentInput outputVertex;
		outputVertex.position = float4(kPositions[vertexIndex], 0.0f, 1.0f);
		outputVertices[vertexIndex] = outputVertex;
	}
	outputTriangles[0] = uint3(0u, 1u, 2u);
}
