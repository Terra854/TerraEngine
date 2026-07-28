// AnimatedPageDeform.comp.hlsl - engine-owned page morphing and skinning.
//
// The compute stage writes one current/history geometry record per page-local
// vertex. Surface shaders never see the palette, skin, morph, or generation
// resources consumed here.

#include "AnimatedVirtualGeometryAbi.hlsli"

[[vk::binding(TE_ANIMATED_GEOMETRY_BINDING_GENERATIONS, TE_ANIMATED_GEOMETRY_DESCRIPTOR_SET)]]
StructuredBuffer<TeAnimatedGenerationRecord> generationRecords;
[[vk::binding(TE_ANIMATED_GEOMETRY_BINDING_WORK, TE_ANIMATED_GEOMETRY_DESCRIPTOR_SET)]]
StructuredBuffer<TeAnimatedDeformationWorkRecord> deformationWork;
[[vk::binding(TE_ANIMATED_GEOMETRY_BINDING_STATIC_PAGE_BYTES, TE_ANIMATED_GEOMETRY_DESCRIPTOR_SET)]]
ByteAddressBuffer staticPageBytes;
[[vk::binding(TE_ANIMATED_GEOMETRY_BINDING_PALETTES, TE_ANIMATED_GEOMETRY_DESCRIPTOR_SET)]]
StructuredBuffer<float4> paletteRows;
[[vk::binding(TE_ANIMATED_GEOMETRY_BINDING_MORPH_WEIGHTS, TE_ANIMATED_GEOMETRY_DESCRIPTOR_SET)]]
StructuredBuffer<float> morphWeights;
[[vk::binding(TE_ANIMATED_GEOMETRY_BINDING_CURRENT_VERTICES, TE_ANIMATED_GEOMETRY_DESCRIPTOR_SET)]]
RWStructuredBuffer<TeAnimatedDeformedVertex> currentVertices;
[[vk::binding(TE_ANIMATED_GEOMETRY_BINDING_PREVIOUS_VERTICES, TE_ANIMATED_GEOMETRY_DESCRIPTOR_SET)]]
StructuredBuffer<TeAnimatedDeformedVertex> previousVertices;
[[vk::binding(TE_ANIMATED_GEOMETRY_BINDING_DIAGNOSTICS, TE_ANIMATED_GEOMETRY_DESCRIPTOR_SET)]]
RWStructuredBuffer<uint> deformationDiagnostics;

[[vk::push_constant]]
ConstantBuffer<TeAnimatedDeformationPushConstants> deformationPush;

static const uint kWorkHistoryValid = 0x01u;

bool IsFiniteFloat(float value)
{
	return !isnan(value) && !isinf(value);
}

bool IsFiniteFloat3(float3 value)
{
	return IsFiniteFloat(value.x) && IsFiniteFloat(value.y) && IsFiniteFloat(value.z);
}

float DecodeSnorm10(uint packedValue, uint bitOffset)
{
	int value = int((packedValue >> bitOffset) & 0x3ffu);
	value = (value << 22) >> 22;
	return clamp(float(value) / 511.0F, -1.0F, 1.0F);
}

float2 DecodeOctahedral(uint packedValue)
{
	const int xBits = int(packedValue & 0xffffu);
	const int yBits = int((packedValue >> 16u) & 0xffffu);
	return clamp(float2(
		float((xBits << 16) >> 16) / 32767.0F,
		float((yBits << 16) >> 16) / 32767.0F),
		-1.0F,
		1.0F);
}

float3 DecodeOct32(uint packedValue)
{
	const float2 encoded = DecodeOctahedral(packedValue);
	float3 value = float3(encoded, 1.0F - abs(encoded.x) - abs(encoded.y));
	if(value.z < 0.0F)
	{
		const float2 signValue = float2(
			value.x >= 0.0F ? 1.0F : -1.0F,
			value.y >= 0.0F ? 1.0F : -1.0F);
		value.xy = (1.0F - abs(value.yx)) * signValue;
	}
	return normalize(value);
}

float3 TransformPosition(float3 position, uint localJoint, TeAnimatedDeformationWorkRecord work)
{
	if(localJoint >= work.jointCount)
	{
		return position;
	}
	// AVPB stores a compact page-local palette whose entries are indices into
	// the animation evaluator's cooked palette. Geometry compute performs this
	// indirection once; material and canonical graphics stages never bind skin
	// or palette resources.
	const uint paletteEntry =
		staticPageBytes.Load(work.jointPaletteByteOffset + localJoint * 4u);
	const uint row = work.paletteRowOffset + paletteEntry * 3u;
	return float3(
		dot(paletteRows[row + 0u], float4(position, 1.0F)),
		dot(paletteRows[row + 1u], float4(position, 1.0F)),
		dot(paletteRows[row + 2u], float4(position, 1.0F)));
}

float3 TransformDirection(float3 direction, uint localJoint, TeAnimatedDeformationWorkRecord work)
{
	if(localJoint >= work.jointCount)
	{
		return direction;
	}
	const uint paletteEntry =
		staticPageBytes.Load(work.jointPaletteByteOffset + localJoint * 4u);
	const uint row = work.paletteRowOffset + paletteEntry * 3u;
	return float3(
		dot(paletteRows[row + 0u].xyz, direction),
		dot(paletteRows[row + 1u].xyz, direction),
		dot(paletteRows[row + 2u].xyz, direction));
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if(deformationPush.abiVersion != TE_ANIMATED_GEOMETRY_ABI_VERSION
		|| deformationPush.workRecordIndex >= deformationPush.workRecordCount)
	{
		return;
	}

	const TeAnimatedDeformationWorkRecord work =
		deformationWork[deformationPush.workRecordIndex];
	const uint vertexIndex = dispatchThreadId.x;
	if(vertexIndex >= work.vertexCount)
	{
		return;
	}

	const TeAnimatedGenerationRecord generation =
		generationRecords[work.generationRecordIndex];
	const uint outputIndex = work.currentOutputOffset + vertexIndex;
	if(all(generation.stableInstanceId == uint2(0u, 0u))
		|| all(generation.pageContentGeneration == uint2(0u, 0u))
		|| all(generation.rendererGeneration == uint2(0u, 0u)))
	{
		InterlockedOr(deformationDiagnostics[0],
			TE_ANIMATED_GEOMETRY_DIAGNOSTIC_INVALID_RECORD);
		return;
	}

	const uint vertexByteOffset = work.inputVertexByteOffset + vertexIndex * 16u;
	const uint packedPosition = staticPageBytes.Load(vertexByteOffset + 0u);
	const uint packedNormal = staticPageBytes.Load(vertexByteOffset + 4u);
	const uint packedTangent = staticPageBytes.Load(vertexByteOffset + 8u);
	const float tangentSign = ((packedPosition >> 30u) & 0x1u) != 0u ? -1.0F : 1.0F;
	const float3 normalizedPosition = float3(
		DecodeSnorm10(packedPosition, 0u),
		DecodeSnorm10(packedPosition, 10u),
		DecodeSnorm10(packedPosition, 20u));
	float3 position = work.positionOffset.xyz + normalizedPosition * work.positionScale.xyz;
	float3 normal = DecodeOct32(packedNormal);
	float3 tangent = DecodeOct32(packedTangent);

	// Morph storage is target-major: one position/normal/tangent float4 tuple
	// per local vertex. Zero weights and non-affecting targets are skipped.
	for(uint targetIndex = 0u; targetIndex < work.morphTargetCount; ++targetIndex)
	{
		const float weight = morphWeights[work.morphWeightOffset + targetIndex];
		if(weight == 0.0F)
		{
			continue;
		}
		const uint deltaByteOffset = work.morphDeltaByteOffset
			+ (targetIndex * work.vertexCount + vertexIndex) * 48u;
		position += asfloat(staticPageBytes.Load4(deltaByteOffset + 0u)).xyz * weight;
		normal += asfloat(staticPageBytes.Load4(deltaByteOffset + 16u)).xyz * weight;
		tangent += asfloat(staticPageBytes.Load4(deltaByteOffset + 32u)).xyz * weight;
	}

	const uint skinByteOffset = work.skinRecordByteOffset + vertexIndex * 8u;
	const uint packedJoints = staticPageBytes.Load(skinByteOffset + 0u);
	const uint packedWeights = staticPageBytes.Load(skinByteOffset + 4u);
	float weightSum = 0.0F;
	float3 skinnedPosition = 0.0F;
	float3 skinnedNormal = 0.0F;
	float3 skinnedTangent = 0.0F;
	for(uint influence = 0u; influence < 4u; ++influence)
	{
		const uint joint = (packedJoints >> (influence * 8u)) & 0xffu;
		const float weight = float((packedWeights >> (influence * 8u)) & 0xffu) / 255.0F;
		if(weight == 0.0F)
		{
			continue;
		}
		weightSum += weight;
		skinnedPosition += TransformPosition(position, joint, work) * weight;
		skinnedNormal += TransformDirection(normal, joint, work) * weight;
		skinnedTangent += TransformDirection(tangent, joint, work) * weight;
	}
	if(weightSum > 0.0F)
	{
		position = skinnedPosition / weightSum;
		normal = skinnedNormal / weightSum;
		tangent = skinnedTangent / weightSum;
	}

	if(!IsFiniteFloat3(position) || !IsFiniteFloat3(normal) || !IsFiniteFloat3(tangent))
	{
		InterlockedOr(deformationDiagnostics[0],
			TE_ANIMATED_GEOMETRY_DIAGNOSTIC_NONFINITE_OUTPUT);
		return;
	}

	TeAnimatedDeformedVertex output;
	output.currentPosition = float4(position, 1.0F);
	output.normal = float4(normalize(normal), 0.0F);
	output.tangentAndSign = float4(normalize(tangent), tangentSign);
	output.previousPosition = output.currentPosition;
	if((work.flags & kWorkHistoryValid) != 0u)
	{
		output.previousPosition =
			previousVertices[work.previousOutputOffset + vertexIndex].currentPosition;
	}
	currentVertices[outputIndex] = output;
}
