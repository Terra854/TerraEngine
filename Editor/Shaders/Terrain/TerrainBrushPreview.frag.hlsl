// TerrainBrushPreview.frag.hlsl -- Falloff, mask, loading, and reject overlay.

struct BrushFragmentInput { float4 position : SV_Position; float2 uv : TEXCOORD0; };

cbuffer TerrainBrushPreviewStyle : register(b1)
{
	float4 activeColor;
	float4 rejectedColor;
	float previewDifference;
	float maskStrength;
	float loading;
	float rejected;
};

float4 main(BrushFragmentInput input) : SV_Target0
{
	const float radialDistance = length(input.uv * 2.0 - 1.0);
	const float falloff = saturate(1.0 - radialDistance);
	const float stripes = frac((input.uv.x + input.uv.y) * 18.0) > 0.5 ? 0.55 : 1.0;
	const float4 base = rejected > 0.5 ? rejectedColor : activeColor;
	const float alpha = falloff * saturate(maskStrength) * (loading > 0.5 ? stripes : 1.0);
	// Preview differences brighten only an editor overlay; committed terrain is
	// rendered by the runtime pipeline and never sampled or modified here.
	return float4(base.rgb * (1.0 + saturate(previewDifference)), base.a * alpha);
}
