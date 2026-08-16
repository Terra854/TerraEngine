#include "../Geometry/TerraWorldReferenceFrame.hlsli"

static const float kPi = 3.14159265359f;
// The procedural sky uses a few hand-tuned height thresholds to mimic the
// long, warm sunrise transition seen in DCC tools and engines such as Unity.
// A sun height of about -0.17 corresponds to roughly ten degrees below the
// horizon, which is the point where the user wants the sky fully black.
static const float kSunFullyHiddenHeight = -0.18f;
static const float kSunDiskVisibleHeight = 0.0f;
static const float kSunTwilightStartHeight = -0.16f;
static const float kSunDaylightHeight = 0.22f;
static const uint kSkyFlagTextured = 1u;
static const uint kSkyFlagForceBlack = 2u;
static const uint kSkyFlagSceneViewOrthographic = 4u;

[[vk::binding(0, 0)]] Texture2D globalTextures[];
[[vk::binding(0, 0)]] SamplerState globalSamplers[];

struct PSInput
{
	float4 position : SV_Position;
	float2 ndc : TEXCOORD0;
};

struct SkyPushConstants
{
	float4 cameraOrientation;
	float4 horizonTintIntensity;
	float4 zenithTintExposure;
	float4 groundTintPadding;
	float4 sunDirectionTextureIndex;
	float4 skyParams;
	float4 editorTint;
};

[[vk::push_constant]]
SkyPushConstants SkyData;

float3 RotateByQuaternion(float3 vectorValue, float4 quaternionValue)
{
	float3 q = quaternionValue.xyz;
	float3 t = 2.0f * cross(q, vectorValue);
	return vectorValue + quaternionValue.w * t + cross(q, t);
}

float3 RotateAroundY(float3 direction, float radians)
{
	float sineValue = sin(radians);
	float cosineValue = cos(radians);
	return normalize(float3(
		direction.x * cosineValue + direction.z * sineValue,
		direction.y,
		-direction.x * sineValue + direction.z * cosineValue));
}

float2 DirectionToLatLongUv(float3 direction)
{
	float2 uv;
	uv.x = atan2(direction.x, -direction.z) / (2.0f * kPi) + 0.5f;
	uv.y = acos(clamp(direction.y, -1.0f, 1.0f)) / kPi;
	return uv;
}

float GetSunVisibility(float sunHeight)
{
	// Keep the solar disk and its bloom alive until the light has actually
	// slipped beneath the horizon instead of disappearing too early.
	return smoothstep(kSunFullyHiddenHeight, kSunDiskVisibleHeight, sunHeight);
}

float GetSunTwilight(float sunHeight)
{
	// Keep twilight confined to a narrow band near the horizon so the sky does
	// not stay lit through the whole under-horizon half rotation.
	float dawnRamp = smoothstep(kSunTwilightStartHeight, 0.015f, sunHeight);
	float daylightFade = 1.0f - smoothstep(0.08f, 0.30f, sunHeight);
	return saturate(dawnRamp * daylightFade);
}

float GetSunDaylight(float sunHeight)
{
	return smoothstep(kSunFullyHiddenHeight, kSunDaylightHeight, sunHeight);
}

float GetSkyVisibility(float sunHeight)
{
	// Fade the entire sky over the last part of sunset so textured skies do not
	// snap to black the moment the threshold is crossed.
	return smoothstep(kSunFullyHiddenHeight, 0.02f, sunHeight);
}

float GetSunDiscVisibility(float sunHeight)
{
	// The solar disc itself should never render below the horizon, even while
	// the surrounding sky is still easing through twilight.
	return smoothstep(0.0f, 0.02f, sunHeight);
}

float GetHorizonFacing(float3 worldDirection, float3 sunDirection)
{
	// Project both directions onto the horizon so dawn light is strongest on the
	// actual sun side instead of wrapping evenly around the whole sky.
	float2 horizonWorld = worldDirection.xz;
	float2 horizonSun = sunDirection.xz;
	float horizonWorldLengthSquared = max(dot(horizonWorld, horizonWorld), 0.0001f);
	float horizonSunLengthSquared = max(dot(horizonSun, horizonSun), 0.0001f);
	float2 horizonWorldDirection = horizonWorld * rsqrt(horizonWorldLengthSquared);
	float2 horizonSunDirection = horizonSun * rsqrt(horizonSunLengthSquared);
	return saturate(dot(horizonWorldDirection, horizonSunDirection) * 0.5f + 0.5f);
}

float3 BuildFallbackGradient(float3 worldDirection, float3 sunDirection)
{
	float sunHeight = clamp(sunDirection.y, -1.0f, 1.0f);
	float daylight = GetSunDaylight(sunHeight);
	float twilight = GetSunTwilight(sunHeight);
	float lowSun = 1.0f - smoothstep(0.03f, 0.28f, sunHeight);
	float upwardBlend = smoothstep(-0.08f, 0.85f, worldDirection.y);
	float belowHorizonBlend = smoothstep(0.0f, 0.7f, saturate(-worldDirection.y));
	float horizonBand = exp(-abs(worldDirection.y) * 18.0f);
	float sunFacing = saturate(dot(worldDirection, sunDirection));
	float horizonFacing = GetHorizonFacing(worldDirection, sunDirection);
	float horizonFocus = pow(horizonFacing, 5.0f);
	float horizonSunLock = exp(-abs(sunHeight) * 11.0f);
	float forwardScatter = pow(sunFacing, lerp(7.0f, 3.2f, lowSun));

	float3 dayHorizon = SkyData.horizonTintIntensity.rgb;
	float3 dayZenith = SkyData.zenithTintExposure.rgb;
	float3 dayGround = SkyData.groundTintPadding.rgb;
	float3 nightHorizon = float3(0.0f, 0.0f, 0.0f);
	float3 nightZenith = float3(0.0f, 0.0f, 0.0f);
	float3 nightGround = float3(0.0f, 0.0f, 0.0f);

	float3 horizonColor = lerp(nightHorizon, dayHorizon, daylight);
	float3 zenithColor = lerp(nightZenith, dayZenith, daylight);
	float3 groundColor = lerp(nightGround, dayGround, daylight);

	float3 skyGradient = lerp(horizonColor, zenithColor, upwardBlend);
	float3 baseGradient = lerp(skyGradient, groundColor, belowHorizonBlend);

	// Keep only a faint global haze and concentrate the warm dawn band around
	// the sun-facing azimuth so the horizon does not glow in every direction.
	float3 twilightBandColor = lerp(float3(0.34f, 0.09f, 0.03f), float3(1.00f, 0.46f, 0.12f), horizonSunLock);
	float3 ambientTwilight = twilightBandColor * horizonBand * twilight * 0.08f;
	float3 twilightBand = twilightBandColor * horizonBand * twilight * (0.12f + 1.15f * horizonFocus);

	// Forward scattering adds the taller, softer sunrise column around the sun.
	float3 sunriseScatterColor = lerp(float3(0.95f, 0.42f, 0.14f), float3(1.00f, 0.78f, 0.45f), daylight);
	float sunriseScatter = forwardScatter * horizonBand * (0.15f + 1.45f * twilight + 0.40f * lowSun);

	return baseGradient + ambientTwilight + twilightBand + sunriseScatterColor * sunriseScatter;
}

float3 BuildSunContribution(float3 direction)
{
	float3 sunDirection = normalize(SkyData.sunDirectionTextureIndex.xyz);
	float sunHeight = clamp(sunDirection.y, -1.0f, 1.0f);
	float sunVisibility = GetSunDiscVisibility(sunHeight);
	if (sunVisibility <= 0.0001f)
	{
		return float3(0.0f, 0.0f, 0.0f);
	}

	float daylight = GetSunDaylight(sunHeight);
	float lowSun = 1.0f - smoothstep(0.03f, 0.28f, sunHeight);
	float sunAlignment = saturate(dot(direction, sunDirection));
	float verticalBloomMask = exp(-abs(direction.y - sunDirection.y) * lerp(6.5f, 11.0f, daylight));
	float sunDisk = smoothstep(0.99950f, 0.99988f, sunAlignment);
	float innerGlow = pow(sunAlignment, lerp(180.0f, 320.0f, daylight));
	float outerGlow = pow(sunAlignment, lerp(18.0f, 30.0f, daylight)) * verticalBloomMask;

	float3 diskColor = lerp(float3(1.00f, 0.82f, 0.52f), float3(1.00f, 0.96f, 0.90f), daylight);
	float3 glowColor = lerp(float3(1.00f, 0.58f, 0.20f), float3(1.00f, 0.88f, 0.62f), daylight);
	float diskIntensity = lerp(2.0f, 4.5f, daylight);
	float innerGlowIntensity = lerp(1.8f, 0.9f, daylight);
	float outerGlowIntensity = lerp(1.2f, 0.18f, daylight) + lowSun * 0.45f;

	return sunVisibility * (
		diskColor * (sunDisk * diskIntensity) +
		glowColor * (innerGlow * innerGlowIntensity + outerGlow * outerGlowIntensity));
}

float3 ApplyEditorTint(float3 color)
{
	return lerp(color, SkyData.editorTint.rgb, saturate(SkyData.editorTint.a));
}

float3 BuildSceneViewOrthographicBackground(float2 ndc)
{
	// Orthographic editor views should read as a flat drafting surface. Use a
	// screen-space gradient rather than world-space sky so no false horizon or
	// perspective depth appears behind the grid.
	float verticalBlend = smoothstep(0.0f, 1.0f, saturate(ndc.y * 0.5f + 0.5f));
	float3 lowerColor = float3(0.58f, 0.64f, 0.69f);
	float3 upperColor = float3(0.43f, 0.51f, 0.59f);
	float3 color = lerp(lowerColor, upperColor, verticalBlend);
	float vignette = saturate(dot(ndc, ndc) * 0.18f);
	return color - vignette * 0.035f;
}

float4 PSMain(PSInput input) : SV_Target0
{
	// The renderer provides the horizontal and vertical tangent of the half-FOV
	// so the sky pass can rebuild a view ray without needing a full matrix.
	float3 viewDirection = normalize(float3(
		input.ndc.x * SkyData.skyParams.x,
		-input.ndc.y * SkyData.skyParams.y,
		-1.0f));
	float3 worldDirection = RotateByQuaternion(viewDirection, SkyData.cameraOrientation);
	float3 skySampleDirection = RotateAroundY(worldDirection, SkyData.skyParams.z);

	uint flags = (uint)(SkyData.skyParams.w + 0.5f);
	if ((flags & kSkyFlagSceneViewOrthographic) != 0u)
	{
		return float4(ApplyEditorTint(BuildSceneViewOrthographicBackground(input.ndc)), 1.0f);
	}

	if ((flags & kSkyFlagForceBlack) != 0u)
	{
		return float4(ApplyEditorTint(float3(0.0f, 0.0f, 0.0f)), 1.0f);
	}

	float3 sunDirection = normalize(SkyData.sunDirectionTextureIndex.xyz);
	float skyVisibility = GetSkyVisibility(sunDirection.y);
	float3 color = BuildFallbackGradient(worldDirection, sunDirection);
	if ((flags & kSkyFlagTextured) != 0u)
	{
		uint textureIndex = NonUniformResourceIndex((uint)(SkyData.sunDirectionTextureIndex.w + 0.5f));
		float2 uv = DirectionToLatLongUv(skySampleDirection);
		float3 sampledSky = globalTextures[textureIndex].Sample(globalSamplers[textureIndex], uv).rgb;
		float daylight = GetSunDaylight(sunDirection.y);
		float twilight = GetSunTwilight(sunDirection.y);
		float3 sunTint = lerp(float3(0.02f, 0.02f, 0.03f), float3(1.0f, 1.0f, 1.0f), daylight);
		sunTint += float3(0.42f, 0.16f, 0.05f) * twilight;
		color = sampledSky * sunTint * skyVisibility;
	}
	else
	{
		color *= skyVisibility;
	}

	color += BuildSunContribution(worldDirection);
	color *= SkyData.horizonTintIntensity.w;
	color = 1.0f - exp(-color * SkyData.zenithTintExposure.w);
	return float4(ApplyEditorTint(color), 1.0f);
}
