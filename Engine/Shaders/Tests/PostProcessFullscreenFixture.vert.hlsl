// A vertex-free triangle covers the output without a shared mesh allocation.
struct FullscreenInput
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

FullscreenInput VSMain(uint vertexId : SV_VertexID)
{
	FullscreenInput output;
	output.uv = float2((vertexId << 1) & 2, vertexId & 2);
	output.position = float4(output.uv * 2.0f - 1.0f, 0.0f, 1.0f);
	return output;
}
