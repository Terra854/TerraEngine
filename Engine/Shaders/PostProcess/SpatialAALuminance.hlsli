#ifndef TERRAENGINE_SPATIAL_AA_LUMINANCE_HLSLI
#define TERRAENGINE_SPATIAL_AA_LUMINANCE_HLSLI

// All spatial AA modes inspect the same display-linear Rec.709 input. Clamp
// only the edge metric into the SDR display range; blend the original linear
// samples and preserve the center alpha in the actual resolve.
float EncodeSpatialLuminanceChannel(float value)
{
	value = saturate(value);
	return value <= 0.0031308f ? 12.92f * value : 1.055f * pow(value, 1.0f / 2.4f) - 0.055f;
}

float SpatialPerceptualLuminance(float3 linearRec709)
{
	if(!all(isfinite(linearRec709))) return 0.0f;
	const float3 encoded = float3(
		EncodeSpatialLuminanceChannel(linearRec709.r),
		EncodeSpatialLuminanceChannel(linearRec709.g),
		EncodeSpatialLuminanceChannel(linearRec709.b));
	return dot(encoded, float3(0.2126f, 0.7152f, 0.0722f));
}

#endif
