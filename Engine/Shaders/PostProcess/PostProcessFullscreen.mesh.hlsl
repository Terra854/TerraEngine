// One mesh workgroup emits the fullscreen triangle. The previous texcoord
// convention is retained so all existing color fragments sample identically.
struct FullscreenInput
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void main(out vertices FullscreenInput vertices[3], out indices uint3 triangles[1])
{
	SetMeshOutputCounts(3, 1);
	[unroll]
	for(uint corner = 0; corner < 3; ++corner)
	{
		FullscreenInput output;
		output.uv = float2((corner << 1) & 2, corner & 2);
		output.position = float4(output.uv * 2.0f - 1.0f, 0.0f, 1.0f);
		vertices[corner] = output;
	}
	triangles[0] = uint3(0, 1, 2);
}
