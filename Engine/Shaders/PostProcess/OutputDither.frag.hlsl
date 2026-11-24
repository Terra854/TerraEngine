// Dither the finished camera image in 8-bit sRGB code-value units. The target
// still receives linear color so the usual presentation transfer runs once.
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] Texture2D<float4> cameraColor;
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] SamplerState cameraSampler;

float Bayer8(uint2 pixel)
{
	uint rank = 0u;
	[unroll] for (uint bit = 0u; bit < 3u; ++bit)
	{
		uint2 bits = (pixel >> bit) & 1u;
		rank = rank * 4u + ((bits.x ^ bits.y) * 2u + bits.y);
	}
	return (float(rank) + 0.5f) / 64.0f - 0.5f;
}

float DitherChannel(float v, float offset)
{
	// Leave HDR above SDR white, black and alpha to the normal compositor.
	if (v <= 0.0f || v >= 1.0f) return v;
	float encoded = v <= 0.0031308f ? 12.92f * v : 1.055f * pow(v, 1.0f / 2.4f) - 0.055f;
	encoded = saturate(encoded + offset);
	return encoded <= 0.04045f ? encoded / 12.92f : pow((encoded + 0.055f) / 1.055f, 2.4f);
}

float4 PSMain(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
	float4 source = cameraColor.SampleLevel(cameraSampler, uv, 0.0f);
	float offset = Bayer8(uint2(position.xy)) / 255.0f;
	return float4(DitherChannel(source.r, offset), DitherChannel(source.g, offset), DitherChannel(source.b, offset), source.a);
}
