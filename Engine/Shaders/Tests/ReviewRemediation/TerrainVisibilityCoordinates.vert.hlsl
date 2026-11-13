// Validation-only controlled inputs for the actual TerrainVisibility fragment.
// This stage supplies clip position, authored UV and normal only. No terrain
// mask address or material-coordinate formula is reproduced in the producer.
struct ProbeInputs { float2 authoredUv; float2 padding; };
[[vk::push_constant]] ConstantBuffer<ProbeInputs> probe;
struct VertexOutput {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float3 normal : TEXCOORD1;
};
VertexOutput main(uint vertexId : SV_VertexID) {
	const float2 positions[3] = {float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f)};
	VertexOutput output;
	output.position = float4(positions[vertexId], 0.5f, 1.0f);
	output.uv = probe.authoredUv;
	output.normal = float3(0.0f, 1.0f, 0.0f);
	return output;
}
