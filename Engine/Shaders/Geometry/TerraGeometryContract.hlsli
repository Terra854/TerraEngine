// TerraGeometryContract.hlsli - Installed surface-shader input contract.
//
// Engine-managed task/mesh assembly supplies these values for both static and
// animated geometry. Authored surface shaders must not bind animation inputs.

#ifndef TERRAENGINE_GEOMETRY_CONTRACT_HLSLI
#define TERRAENGINE_GEOMETRY_CONTRACT_HLSLI

#define TERRAENGINE_GEOMETRY_ABI_VERSION 1u
#define TERRAENGINE_GEOMETRY_POLICY_ENGINE_MANAGED 0u
#define TERRAENGINE_GEOMETRY_POLICY_COMPATIBLE_REPLACEMENT 1u
#define TERRAENGINE_RESERVED_DESCRIPTOR_SET 0u

struct TerraSurfaceInput
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float3 worldPosition : TEXCOORD1;
	float3 normal : NORMAL;
	float4 tangent : TANGENT;
	nointerpolation uint meshletIndex : TEXCOORD2;
	nointerpolation float meshletBoundsRadius : TEXCOORD3;
	nointerpolation uint meshletCullFlags : TEXCOORD4;
	nointerpolation uint virtualGeometryDebugData : TEXCOORD5;
};

#endif
