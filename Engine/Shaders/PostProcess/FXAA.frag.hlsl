// Independent adaptation of the method described by Timothy Lottes:
// https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/FXAA_WhitePaper.pdf
// No external implementation code is included. Detection uses perceptual luma;
// Input/output RGB remain HDR-linear Rec.709 and original alpha is preserved.
#include "SpatialAALuminance.hlsli"
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] Texture2D<float4> inputColor;
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] SamplerState inputSampler;

struct FxaaConstants
{
	uint2 sourceSize;
	float relativeThreshold; // Recommended 0.125 in the perceptual luma domain.
	float minimumThreshold; // Recommended 0.03125; retains low-contrast dark detail.
	float subpixelQuality; // [0, 1], default 0.5; maximum shift is half a texel.
	uint endpointSteps; // [1, 12], default 8. Bounded integer search schedule.
	uint enabled;
	uint reserved;
};
[[vk::push_constant]] ConstantBuffer<FxaaConstants> config;

float LumaAt(int2 pixel, int2 maximum)
{
	return SpatialPerceptualLuminance(inputColor.Load(int3(clamp(pixel, int2(0, 0), maximum), 0)).rgb);
}

bool PairInside(int2 pixel, int2 normal, int2 maximum)
{
	return all(pixel >= 0) && all(pixel <= maximum)
		&& all(pixel + normal >= 0) && all(pixel + normal <= maximum);
}

float PairLuma(int2 pixel, int2 normal, int2 maximum)
{
	// Integer search locations and a half-normal edge center let us interpolate
	// two perceptual samples exactly. Applying perception after a linear fetch
	// would bias the endpoint test on strong edges in this linear-input adapter.
	return 0.5f * (LumaAt(pixel, maximum) + LumaAt(pixel + normal, maximum));
}

float4 PSMain(float4 position : SV_Position, float2 unusedUv : TEXCOORD0) : SV_Target0
{
	uint width, height;
	inputColor.GetDimensions(width, height);
	const int2 maximum = int2(width, height) - 1;
	const int2 pixel = clamp(int2(position.xy), int2(0, 0), maximum);
	const float4 center = inputColor.Load(int3(pixel, 0));
	// A mismatched extent must never run a filter using stale reciprocal sizes.
	if (config.enabled == 0u || any(config.sourceSize != uint2(width, height))
		|| width == 0u || height == 0u
		|| !all(isfinite(float3(config.relativeThreshold, config.minimumThreshold, config.subpixelQuality)))) return center;
	const float m = SpatialPerceptualLuminance(center.rgb);
	const float n = LumaAt(pixel + int2(0, -1), maximum);
	const float s = LumaAt(pixel + int2(0, 1), maximum);
	const float w = LumaAt(pixel + int2(-1, 0), maximum);
	const float e = LumaAt(pixel + int2(1, 0), maximum);
	const float localMin = min(m, min(min(n, s), min(w, e)));
	const float localMax = max(m, max(max(n, s), max(w, e)));
	const float range = localMax - localMin;
	if (range <= max(max(config.minimumThreshold, 0.0f), localMax * max(config.relativeThreshold, 0.0f))
		|| range <= 1.0e-6f) return center;
	const float nw = LumaAt(pixel + int2(-1, -1), maximum);
	const float ne = LumaAt(pixel + int2(1, -1), maximum);
	const float sw = LumaAt(pixel + int2(-1, 1), maximum);
	const float se = LumaAt(pixel + int2(1, 1), maximum);
	// Compare normal-axis second differences across a 3x3 neighborhood.
	const float horizontal = abs(nw - 2.0f * w + sw) + 2.0f * abs(n - 2.0f * m + s) + abs(ne - 2.0f * e + se);
	const float vertical = abs(nw - 2.0f * n + ne) + 2.0f * abs(w - 2.0f * m + e) + abs(sw - 2.0f * s + se);
	const bool edgeHorizontal = horizontal >= vertical;
	const float negative = edgeHorizontal ? n : w;
	const float positive = edgeHorizontal ? s : e;
	const bool useNegative = abs(negative - m) >= abs(positive - m);
	const float across = useNegative ? negative : positive;
	const float gradient = abs(across - m);
	if (gradient < range * 0.125f) return center;
	const int normalSign = useNegative ? -1 : 1;
	const int2 normal = edgeHorizontal ? int2(0, normalSign) : int2(normalSign, 0);
	const int2 tangent = edgeHorizontal ? int2(1, 0) : int2(0, 1);
	const float reference = 0.5f * (m + across);
	const float endpointThreshold = gradient * 0.25f;
	// Fine early probes protect short edges; later probes extend the endpoint
	// search without unbounded per-fragment work. Distances are 1..38 texels.
	static const uint increments[12] = {1u, 1u, 1u, 1u, 2u, 2u, 2u, 4u, 4u, 4u, 8u, 8u};
	uint negativeDistance = 0u, positiveDistance = 0u;
	float negativeDelta = 0.0f, positiveDelta = 0.0f;
	bool negativeDone = false, positiveDone = false;
	bool negativeFound = false, positiveFound = false;
	const uint steps = clamp(config.endpointSteps, 1u, 12u);
	[loop]
	for (uint step = 0u; step < steps; ++step) {
		if (!negativeDone) {
			negativeDistance += increments[step];
			const int2 probe = pixel - tangent * int(negativeDistance);
			if (!PairInside(probe, normal, maximum)) negativeDone = true;
			else {
				negativeDelta = PairLuma(probe, normal, maximum) - reference;
				negativeFound = abs(negativeDelta) >= endpointThreshold;
				negativeDone = negativeFound;
			}
		}
		if (!positiveDone) {
			positiveDistance += increments[step];
			const int2 probe = pixel + tangent * int(positiveDistance);
			if (!PairInside(probe, normal, maximum)) positiveDone = true;
			else {
				positiveDelta = PairLuma(probe, normal, maximum) - reference;
				positiveFound = abs(positiveDelta) >= endpointThreshold;
				positiveDone = positiveFound;
			}
		}
		if (negativeDone && positiveDone) break;
	}
	const bool nearestNegative = negativeDistance <= positiveDistance;
	const bool nearestFound = nearestNegative ? negativeFound : positiveFound;
	const float nearestDelta = nearestNegative ? negativeDelta : positiveDelta;
	float geometricShift = 0.0f;
	// Reject an unresolved/boundary endpoint and a reference crossing on the
	// wrong side of the center. This avoids bending a continuing straight edge.
	if (nearestFound && ((nearestDelta < 0.0f) != (m < reference))) {
		const float distanceSum = float(negativeDistance + positiveDistance);
		geometricShift = 0.5f - float(min(negativeDistance, positiveDistance)) / max(distanceSum, 1.0f);
	}
	const float neighborMean = (2.0f * (n + s + w + e) + nw + ne + sw + se) / 12.0f;
	// Trim weak low-pass differences before smoothing, so a long pixel-aligned
	// edge does not receive the same smoothing as isolated subpixel detail.
	const float subpixelContrast = saturate((abs(neighborMean - m) / range - 0.25f) / 0.75f);
	const float smoothContrast = subpixelContrast * subpixelContrast * (3.0f - 2.0f * subpixelContrast);
	const float subpixelShift = 0.5f * saturate(config.subpixelQuality) * smoothContrast * smoothContrast;
	const float shift = clamp(max(geometricShift, subpixelShift), 0.0f, 0.5f);
	if (shift <= 1.0e-6f) return center;
	const float2 texel = 1.0f / float2(width, height);
	const float2 uv = (float2(pixel) + 0.5f + float2(normal) * shift) * texel;
	const float3 filtered = inputColor.SampleLevel(inputSampler, uv, 0.0f).rgb;
	const float filteredLuma = SpatialPerceptualLuminance(filtered);
	const float neighborhoodMin = min(localMin, min(min(nw, ne), min(sw, se)));
	const float neighborhoodMax = max(localMax, max(max(nw, ne), max(sw, se)));
	if (!all(isfinite(filtered)) || filteredLuma < neighborhoodMin - 1.0e-5f || filteredLuma > neighborhoodMax + 1.0e-5f) return center;
	return float4(filtered, center.a);
}
