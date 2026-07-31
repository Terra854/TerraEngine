// VirtualGeometryPackedVertex.hlsli - shared four-word vertex decode ABI.
//
// Cook profiles may quantize onto smaller lattices, but every runtime vertex
// expands into these fixed fields. This gives task, mesh, and deformation
// shaders one Vulkan layout without vendor extensions or format-specific
// descriptor branches.

#ifndef TERRAENGINE_VIRTUAL_GEOMETRY_PACKED_VERTEX_HLSLI
#define TERRAENGINE_VIRTUAL_GEOMETRY_PACKED_VERTEX_HLSLI

uint TePackedVertexWord(uint4 words, uint index)
{
	if(index == 0u) return words.x;
	if(index == 1u) return words.y;
	if(index == 2u) return words.z;
	return words.w;
}

uint TeExtractPackedVertexBits(
	uint4 words,
	uint bitOffset,
	uint bitCount)
{
	const uint wordIndex = bitOffset >> 5u;
	const uint wordBit = bitOffset & 31u;
	const uint mask = (1u << bitCount) - 1u;
	uint value = TePackedVertexWord(words, wordIndex) >> wordBit;
	if(wordBit + bitCount > 32u)
	{
		value |= TePackedVertexWord(words, wordIndex + 1u)
			<< (32u - wordBit);
	}
	return value & mask;
}

float TeDecodePackedVertexUnit(
	uint4 words,
	uint bitOffset,
	uint bitCount)
{
	const uint maximumValue = (1u << bitCount) - 1u;
	return float(TeExtractPackedVertexBits(
		words, bitOffset, bitCount)) / float(maximumValue);
}

float3 TeDecodePackedVertexOctahedral(float2 encodedUnit)
{
	const float2 encoded = encodedUnit * 2.0F - 1.0F;
	float3 value = float3(
		encoded, 1.0F - abs(encoded.x) - abs(encoded.y));
	if(value.z < 0.0F)
	{
		const float2 signValue = float2(
			value.x >= 0.0F ? 1.0F : -1.0F,
			value.y >= 0.0F ? 1.0F : -1.0F);
		value.xy = (1.0F - abs(value.yx)) * signValue;
	}
	return normalize(value);
}

struct TeDecodedPackedVertex
{
	float3 normalizedPosition;
	float3 normal;
	float3 tangent;
	float tangentSign;
	float2 normalizedTextureCoordinate;
};

TeDecodedPackedVertex TeDecodePackedVertex(uint4 words)
{
	TeDecodedPackedVertex output;
	output.normalizedPosition = float3(
		TeDecodePackedVertexUnit(words, 0u, 16u),
		TeDecodePackedVertexUnit(words, 16u, 16u),
		TeDecodePackedVertexUnit(words, 32u, 16u)) * 2.0F - 1.0F;
	output.normal = TeDecodePackedVertexOctahedral(float2(
		TeDecodePackedVertexUnit(words, 48u, 12u),
		TeDecodePackedVertexUnit(words, 60u, 12u)));
	output.tangent = TeDecodePackedVertexOctahedral(float2(
		TeDecodePackedVertexUnit(words, 72u, 10u),
		TeDecodePackedVertexUnit(words, 82u, 10u)));
	output.tangentSign = TeExtractPackedVertexBits(
		words, 92u, 1u) != 0u ? -1.0F : 1.0F;
	output.normalizedTextureCoordinate = float2(
		TeDecodePackedVertexUnit(words, 96u, 16u),
		TeDecodePackedVertexUnit(words, 112u, 16u));
	return output;
}

#endif
