// Both rectangles and SDF glyphs use the same CPU-authored triangle stream.
// One workgroup emits up to 16 triangles without changing their blend order.
struct UIVertex
{
	float2 position;
	float2 uv;
	float4 color;
};
struct UIOutput
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
};
struct UIPush
{
	float2 viewportSize;
	uint textureIndex;
	uint useTexture;
	uint firstVertex;
	uint vertexCount;
};
[[vk::binding(0, 1)]] StructuredBuffer<UIVertex> triangles;
[[vk::push_constant]] ConstantBuffer<UIPush> push;

[outputtopology("triangle")]
[numthreads(16, 1, 1)]
void main(uint lane : SV_GroupIndex, uint3 group : SV_GroupID,
	out vertices UIOutput vertices[48], out indices uint3 indices[16])
{
	const uint first = group.x * 48u;
	const uint count = first < push.vertexCount ? min(48u, push.vertexCount - first) / 3u : 0u;
	SetMeshOutputCounts(count * 3u, count);
	if(lane >= count) return;
	const uint corner = lane * 3u;
	const float2 viewport = max(push.viewportSize, float2(1.0f, 1.0f));
	[unroll]
	for(uint index = 0u; index < 3u; ++index)
	{
		const UIVertex input = triangles[push.firstVertex + first + corner + index];
		UIOutput output;
		output.position = float4(input.position / viewport * 2.0f - 1.0f, 0.0f, 1.0f);
		output.uv = input.uv;
		output.color = input.color;
		vertices[corner + index] = output;
	}
	indices[lane] = uint3(corner, corner + 1u, corner + 2u);
}
