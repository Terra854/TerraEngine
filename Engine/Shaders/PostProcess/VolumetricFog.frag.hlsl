// Integrate an exponential-height medium along the visible camera ray in HDR.
// Sun shafts use a separate screen-space sky-visibility gather from scene depth.
[[vk::binding(0, 0)]] Texture2D<float4> cameraColor;
[[vk::binding(1, 0)]] Texture2D<float> cameraDepth;

struct AtmosphereConstants
{
	column_major float4x4 inverseProjection;
	float4 worldUpAndHeight; // View-space world up, camera height above fog base in metres.
	float4 fog; // Density, height falloff, maximum distance, shaft intensity.
	float4 scatterAndProjection; // Authored sky horizon RGB, orthographic flag.
	float4 sun; // Screen UV, shaft validity, reserved.
};
[[vk::push_constant]] ConstantBuffer<AtmosphereConstants> settings;

float3 Unproject(float2 ndc, float depth)
{
	float4 view = mul(settings.inverseProjection, float4(ndc, depth, 1.0f));
	return view.xyz / view.w;
}

float4 PSMain(float4 position : SV_Position) : SV_Target0
{
	int2 pixel = int2(position.xy);
	float4 source = cameraColor.Load(int3(pixel, 0));
	uint width, height;
	cameraDepth.GetDimensions(width, height);
	float2 size = float2(width, height);
	float2 uv = position.xy / size;
	float depth = cameraDepth.Load(int3(pixel, 0));
	if (!isfinite(depth) || depth < 0.0f || depth > 1.0f) return source;
	float3 start = settings.scatterAndProjection.w > 0.0f ? Unproject(uv * 2.0f - 1.0f, 0.0f) : 0.0f;
	// A finite depth below the clear plane also works for an infinite far projection.
	float3 delta = Unproject(uv * 2.0f - 1.0f, depth == 1.0f ? 0.999999f : depth) - start;
	float rayDistance = length(delta);
	if (!isfinite(rayDistance) || rayDistance <= 0.00001f) return source;
	float distance = depth == 1.0f ? settings.fog.z : min(rayDistance, settings.fog.z);
	float3 scatter = settings.scatterAndProjection.rgb;
	float3 color = source.rgb;
	if (settings.fog.x > 0.0f)
	{
		float3 ray = delta / rayDistance;
		float startHeight = settings.worldUpAndHeight.w + dot(settings.worldUpAndHeight.xyz, start);
		float endHeight = startHeight + dot(settings.worldUpAndHeight.xyz, ray) * distance;
		// Average exponential density on a line using its two endpoint exponents.
		// Evaluate from the denser end to avoid an overflowing exponential difference.
		float a = clamp(-startHeight * settings.fog.y, -24.0f, 24.0f);
		float b = clamp(-endHeight * settings.fog.y, -24.0f, 24.0f);
		float span = abs(a - b);
		float densityIntegral = exp(max(a, b)) * (span < 0.001f ? 1.0f : (1.0f - exp(-span)) / span);
		float transmission = exp(-min(80.0f, settings.fog.x * distance * densityIntegral));
		color = lerp(scatter, source.rgb, transmission);
	}
	if (settings.sun.z > 0.0f && settings.fog.w > 0.0f)
	{
		// Only unobstructed sky taps contribute. Out-of-view taps contribute zero.
		float2 towardSun = settings.sun.xy - uv;
		float visibility = 0.0f;
		[unroll] for (int i = 1; i <= 8; ++i)
		{
			float2 tap = uv + towardSun * (float(i) / 8.0f);
			if (all(tap >= 0.0f) && all(tap < 1.0f))
				visibility += cameraDepth.Load(int3(int2(tap * size), 0)) >= 1.0f ? 0.125f : 0.0f;
		}
		float2 radial = towardSun * float2(size.x / size.y, 1.0f);
		float falloff = exp(-dot(radial, radial) * 12.0f);
		// Nearby surfaces shorten the visible shaft segment independently of
		// the maximum distance used by the height medium.
		float segment = depth == 1.0f ? 1.0f : saturate(rayDistance / 25.0f);
		color += scatter * (visibility * falloff * settings.fog.w * segment);
	}
	return float4(color, source.a);
}
