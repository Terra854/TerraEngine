// InvalidReservedBinding.frag.hlsl
// Set zero is reserved for renderer-owned resources.

Texture2D<float4> AuthoredTextureInReservedSet : register(t0, space0);

float4 main() : SV_TARGET
{
	return AuthoredTextureInReservedSet.Load(int3(0, 0, 0));
}
