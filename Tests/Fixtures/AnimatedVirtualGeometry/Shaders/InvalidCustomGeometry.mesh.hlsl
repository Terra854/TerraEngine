// InvalidCustomGeometry.mesh.hlsl
// A geometry replacement without the compatible-replacement ABI must fail.

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void main(
	out vertices float4 vertices[3],
	out indices uint3 triangles[1])
{
	SetMeshOutputCounts(3, 1);
	vertices[0] = float4(0.0F, 0.0F, 0.0F, 1.0F);
	vertices[1] = float4(1.0F, 0.0F, 0.0F, 1.0F);
	vertices[2] = float4(0.0F, 1.0F, 0.0F, 1.0F);
	triangles[0] = uint3(0, 1, 2);
}
