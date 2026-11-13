#pragma pack_matrix(column_major)

[[vk::binding(15, 0)]] Texture2D<float4> currentFrame;
// [[vk::binding(1, 0)]] Texture2D<float2> motionVectors; // Not available in current engine
// The runtime storage/fragment-rate attachment is R8_UINT, not default R32ui.
[[vk::binding(14, 0), vk::image_format("r8ui")]] RWTexture2D<uint> shadingRateImage;

struct PushConstants
{
	uint2 viewportSize;
    uint vrsTexelSize; // e.g. 8 or 16
};

[[vk::push_constant]] ConstantBuffer<PushConstants> config;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 tileCoords = dispatchThreadId.xy;
    uint2 pixelStart = tileCoords * config.vrsTexelSize;

    if (any(pixelStart >= config.viewportSize)) return;

    float luminanceVariance = 0.0;

    float3 avgColor = 0.0;
    float3 minColor = 0.0;
    float3 maxColor = 0.0;

    // Sample tile to find variance
    uint samples = 0;
    for (uint y = 0; y < config.vrsTexelSize; y += 2) {
        for (uint x = 0; x < config.vrsTexelSize; x += 2) {
            uint2 p = pixelStart + uint2(x, y);
            if (any(p >= config.viewportSize)) continue;

            float3 color = currentFrame.Load(uint3(p, 0)).rgb;
            avgColor += color;
			// Seed extrema from a real sample: authored HDR values can exceed
			// one or be negative, so a fixed [0,1] seed invents false variation.
			if (samples == 0u) { minColor = color; maxColor = color; }
			else { minColor = min(minColor, color); maxColor = max(maxColor, color); }
            samples++;
        }
    }

    float range = length(maxColor - minColor);

    // Shading Rate Encoding (Vulkan):
    // 1x1: 0x0
    // 1x2: 0x1
    // 2x1: 0x4
    // 2x2: 0x5
    // 4x4: 0xA

    uint rate = 0x0; // Default 1x1

    if (range < 0.1) {
        rate = 0x5; // 2x2
    }
    if (range < 0.02) {
        rate = 0x0A; // 4x4
    }

    shadingRateImage[tileCoords] = rate;
}
