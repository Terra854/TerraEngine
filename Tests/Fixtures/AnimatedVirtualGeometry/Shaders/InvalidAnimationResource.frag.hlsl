// InvalidAnimationResource.frag.hlsl
// A supported surface shader may not bind animation palettes directly.

StructuredBuffer<float4x4> AnimationPalette : register(t15, space1);

float4 main() : SV_TARGET
{
	return AnimationPalette[0][0];
}
