cbuffer UIFrame : register(b0)
{
	float2 gViewportSize;
	float2 gPadding;
};

struct PSInput
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
};

// The existing solid-color fragment contract reads the interpolated UI color.
float4 PSMain(PSInput input) : SV_Target0
{
	return input.color;
}
