cbuffer UIFrame : register(b0)
{
	float2 gViewportSize;
	float2 gPadding;
};

struct VSInput
{
	float2 localPosition : POSITION;
	float2 localUV : TEXCOORD0;
	float2 instancePosition : TEXCOORD1;
	float2 instanceScale : TEXCOORD2;
	float4 instanceColor : COLOR1;
};

struct PSInput
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
};

// Expands one unit quad into a screen-space HUD quad using per-instance data.
PSInput VSMain(VSInput input)
{
	PSInput output;
	const float2 safeViewport = max(gViewportSize, float2(1.0f, 1.0f));
	const float2 worldPosition = input.instancePosition + (input.localPosition * input.instanceScale);
	const float2 clipPosition = float2(
		(worldPosition.x / safeViewport.x) * 2.0f - 1.0f,
		1.0f - (worldPosition.y / safeViewport.y) * 2.0f);

	output.position = float4(clipPosition, 0.0f, 1.0f);
	output.uv = input.localUV;
	output.color = input.instanceColor;
	return output;
}

float4 PSMain(PSInput input) : SV_Target0
{
	return input.color;
}
