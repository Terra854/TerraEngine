// InvalidInterfaceMismatch.frag.hlsl
// The input deliberately does not match TerraSurfaceInput.

struct InvalidInput
{
	float3 unsupported : TEXCOORD12;
};

float4 main(InvalidInput input) : SV_TARGET
{
	return float4(input.unsupported, 1.0F);
}
