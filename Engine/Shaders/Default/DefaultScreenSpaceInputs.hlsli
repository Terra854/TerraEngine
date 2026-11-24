// Real current fragment inputs for optional screen-space lighting. These terms
// belong to the same scene-linear HDR domain as Default's lighting, before
// exposure, tone mapping and UI. All attachment clears must be zero, so sky and
// uncovered pixels never pretend to contain a geometry normal or material.
#ifndef TERRAENGINE_DEFAULT_SCREEN_SPACE_INPUTS
#define TERRAENGINE_DEFAULT_SCREEN_SPACE_INPUTS
struct DefaultScreenSpaceInputs
{
	float4 viewNormalRoughness;
	float3 environmentIncident;
	float3 diffuseIncident;
	float3 environmentWeight;
	float3 diffuseWeight;
};
struct DefaultScreenSpaceOutput
{
	float4 color : SV_Target0;
	uint4 identity : SV_Target1;
	float4 viewNormalRoughness : SV_Target2;
	float4 environmentIncident : SV_Target3;
	float4 diffuseIncident : SV_Target4;
	float4 environmentWeight : SV_Target5;
	float4 diffuseWeight : SV_Target6;
};
#endif
