// Translated-world shader contract. CPU preparation supplies view-local
// matrices and positions; shaders never reconstruct absolute float worlds.
#ifndef TERRAENGINE_WORLD_REFERENCE_FRAME_HLSLI
#define TERRAENGINE_WORLD_REFERENCE_FRAME_HLSLI
struct TerraTranslatedWorldPosition { float3 value; uint valid; };
TerraTranslatedWorldPosition TerraLocalizeModelPosition(column_major float4x4 viewLocalModel, float3 localPosition)
{
	TerraTranslatedWorldPosition result;
	result.value = mul(viewLocalModel, float4(localPosition, 1.0f)).xyz;
	result.valid = 1u;
	return result;
}
#endif
