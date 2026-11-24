// Shared current owner-mesh deformation; include after descriptor/uniform declarations.
// Existing mesh fallback behavior is preserved byte-for-byte below. The indexed
// route uses the strict wrapper and requires native CPU publication admission.
#ifndef TERRAENGINE_DEFAULT_OWNER_DEFORMATION
#define TERRAENGINE_DEFAULT_OWNER_DEFORMATION

bool TryLoadGpuAnimationGraphicsRecordAt(uint graphicsBufferIndex,
	uint graphicsRecordIndex, out DefaultGpuAnimationGraphicsRecord record)
{
	record = (DefaultGpuAnimationGraphicsRecord)0;
	if (graphicsBufferIndex == kInvalidAnimationGraphicsBufferIndex
		|| graphicsRecordIndex == kInvalidAnimationGraphicsBufferIndex)
	{
		return false;
	}

	uint elementCount;
	uint stride;
	globalBuffers[NonUniformResourceIndex(graphicsBufferIndex)].GetDimensions(elementCount, stride);
	if (elementCount > 0x3FFFFFFFu)
	{
		return false;
	}
	const uint byteCount = elementCount * 4u;
	if (graphicsRecordIndex > (0xFFFFFFFFu - kAnimationGraphicsRecordBytes) / kAnimationGraphicsRecordBytes)
	{
		return false;
	}
	const uint byteOffset = graphicsRecordIndex * kAnimationGraphicsRecordBytes;
	if (byteOffset > byteCount || kAnimationGraphicsRecordBytes > byteCount - byteOffset)
	{
		return false;
	}

	const uint wordOffset = byteOffset >> 2u;
	record.planGeneration = globalBuffers[NonUniformResourceIndex(graphicsBufferIndex)][wordOffset + 0u];
	record.rendererGeneration = globalBuffers[NonUniformResourceIndex(graphicsBufferIndex)][wordOffset + 1u];
	record.workPacketIndex = globalBuffers[NonUniformResourceIndex(graphicsBufferIndex)][wordOffset + 2u];
	record.generationRecordIndex = globalBuffers[NonUniformResourceIndex(graphicsBufferIndex)][wordOffset + 3u];
	record.paletteBufferIndex = globalBuffers[NonUniformResourceIndex(graphicsBufferIndex)][wordOffset + 4u];
	record.morphWeightBufferIndex = globalBuffers[NonUniformResourceIndex(graphicsBufferIndex)][wordOffset + 5u];
	record.paletteOffsetBytes = uint2(
		globalBuffers[NonUniformResourceIndex(graphicsBufferIndex)][wordOffset + 6u],
		globalBuffers[NonUniformResourceIndex(graphicsBufferIndex)][wordOffset + 7u]);
	record.morphWeightOffsetBytes = uint2(
		globalBuffers[NonUniformResourceIndex(graphicsBufferIndex)][wordOffset + 8u],
		globalBuffers[NonUniformResourceIndex(graphicsBufferIndex)][wordOffset + 9u]);
	record.jointCount = globalBuffers[NonUniformResourceIndex(graphicsBufferIndex)][wordOffset + 10u];
	record.morphCount = globalBuffers[NonUniformResourceIndex(graphicsBufferIndex)][wordOffset + 11u];
	record.paletteEntryCount = globalBuffers[NonUniformResourceIndex(graphicsBufferIndex)][wordOffset + 12u];
	record.workFlags = globalBuffers[NonUniformResourceIndex(graphicsBufferIndex)][wordOffset + 13u];
	record.stageMask = globalBuffers[NonUniformResourceIndex(graphicsBufferIndex)][wordOffset + 14u];
	record.validMarker = globalBuffers[NonUniformResourceIndex(graphicsBufferIndex)][wordOffset + 15u];
	return DefaultGpuAnimationRecordHeaderIsValid(record, graphicsRecordIndex);
}

bool TryLoadGpuAnimationGraphicsRecord(out DefaultGpuAnimationGraphicsRecord record)
{
	return TryLoadGpuAnimationGraphicsRecordAt(SceneUniforms.meshDataIndex,
		SceneUniforms.textureIndex, record);
}

bool TryReadGpuMorphWeight(DefaultGpuAnimationGraphicsRecord record, uint targetIndex, out float weight)
{
	weight = 0.0f;
	if (targetIndex >= record.morphCount
		|| record.morphWeightBufferIndex == kInvalidAnimationGraphicsBufferIndex
		|| record.morphWeightOffsetBytes.y != 0u
		|| targetIndex > (0xFFFFFFFFu - record.morphWeightOffsetBytes.x) / 4u)
	{
		return false;
	}

	uint elementCount;
	uint stride;
	globalBuffers[NonUniformResourceIndex(record.morphWeightBufferIndex)].GetDimensions(elementCount, stride);
	if (elementCount > 0x3FFFFFFFu)
	{
		return false;
	}
	const uint byteCount = elementCount * 4u;
	const uint byteOffset = record.morphWeightOffsetBytes.x + targetIndex * 4u;
	if (byteOffset > byteCount || 4u > byteCount - byteOffset)
	{
		return false;
	}
	weight = asfloat(globalBuffers[NonUniformResourceIndex(record.morphWeightBufferIndex)][byteOffset >> 2u]);
	return DefaultGpuAnimationFinite(weight);
}

bool TryApplyGpuMorphs(
	DefaultGpuAnimationGraphicsRecord record,
	MeshDecodeInfo decode,
	uint globalVertexIndex,
	inout float3 position,
	inout float3 normal,
	inout float3 tangent)
{
	if (record.morphCount == 0u)
	{
		return true;
	}
	uint deltaByteCount;
	meshMorphDeltaBytes.GetDimensions(deltaByteCount);
	float3 morphedPosition = position;
	float3 morphedNormal = normal;
	float3 morphedTangent = tangent;
	for (uint targetIndex = 0u; targetIndex < record.morphCount; ++targetIndex)
	{
		float weight;
		if (!TryReadGpuMorphWeight(record, targetIndex, weight))
		{
			return false;
		}
		if (weight == 0.0f)
		{
			continue;
		}
		if (decode.counts.x == 0u || targetIndex > (0xFFFFFFFFu - globalVertexIndex) / decode.counts.x)
		{
			return false;
		}
		const uint deltaIndex = targetIndex * decode.counts.x + globalVertexIndex;
		if (deltaIndex > (0xFFFFFFFFu - kAnimationMorphDeltaBytes) / kAnimationMorphDeltaBytes)
		{
			return false;
		}
		const uint byteOffset = deltaIndex * kAnimationMorphDeltaBytes;
		if (byteOffset > deltaByteCount || kAnimationMorphDeltaBytes > deltaByteCount - byteOffset)
		{
			return false;
		}
		const float3 positionDelta = asfloat(meshMorphDeltaBytes.Load4(byteOffset + 0u)).xyz;
		const float3 normalDelta = asfloat(meshMorphDeltaBytes.Load4(byteOffset + 16u)).xyz;
		const float3 tangentDelta = asfloat(meshMorphDeltaBytes.Load4(byteOffset + 32u)).xyz;
		if (!DefaultGpuAnimationFinite3(positionDelta)
			|| !DefaultGpuAnimationFinite3(normalDelta)
			|| !DefaultGpuAnimationFinite3(tangentDelta))
		{
			return false;
		}
		morphedPosition += positionDelta * weight;
		morphedNormal += normalDelta * weight;
		morphedTangent += tangentDelta * weight;
	}
	if (!DefaultGpuAnimationFinite3(morphedPosition)
		|| !DefaultGpuAnimationFinite3(morphedNormal)
		|| !DefaultGpuAnimationFinite3(morphedTangent))
	{
		return false;
	}
	position = morphedPosition;
	normal = morphedNormal;
	tangent = morphedTangent;
	return true;
}

bool TryApplyGpuSkinning(
	DefaultGpuAnimationGraphicsRecord record,
	uint globalVertexIndex,
	inout float3 position,
	inout float3 normal,
	inout float3 tangent)
{
	uint skinningByteCount;
	meshSkinningBytes.GetDimensions(skinningByteCount);
	if (globalVertexIndex > (0xFFFFFFFFu - 8u) / 8u)
	{
		return false;
	}
	const uint skinningOffset = globalVertexIndex * 8u;
	if (skinningOffset > skinningByteCount || 8u > skinningByteCount - skinningOffset)
	{
		return false;
	}
	const uint packedJoints = meshSkinningBytes.Load(skinningOffset);
	const uint packedWeights = meshSkinningBytes.Load(skinningOffset + 4u);
	const uint weightSum = (packedWeights & 0xFFu)
		+ ((packedWeights >> 8u) & 0xFFu)
		+ ((packedWeights >> 16u) & 0xFFu)
		+ ((packedWeights >> 24u) & 0xFFu);
	// All-zero weights encode an intentional identity transform. Morphs have
	// already been applied, so this keeps unskinned morph-only vertices valid.
	if (weightSum == 0u)
	{
		return true;
	}
	if (record.paletteBufferIndex == kInvalidAnimationGraphicsBufferIndex
		|| record.paletteOffsetBytes.y != 0u
		|| record.jointCount == 0u
		|| record.paletteEntryCount == 0u)
	{
		return false;
	}

	uint paletteElementCount;
	uint paletteStride;
	globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)].GetDimensions(paletteElementCount, paletteStride);
	if (paletteElementCount > 0x3FFFFFFFu)
	{
		return false;
	}
	const uint paletteByteCount = paletteElementCount * 4u;
	float3 skinnedPosition = float3(0.0f, 0.0f, 0.0f);
	float3 skinnedNormal = float3(0.0f, 0.0f, 0.0f);
	float3 skinnedTangent = float3(0.0f, 0.0f, 0.0f);
	for (uint influence = 0u; influence < 4u; ++influence)
	{
		const uint weightByte = (packedWeights >> (influence * 8u)) & 0xFFu;
		if (weightByte == 0u)
		{
			continue;
		}
		const uint jointIndex = (packedJoints >> (influence * 8u)) & 0xFFu;
		// Canonical deformable streams store palette-local indices, not
		// skeleton-joint indices. A sparse
		// cooked palette may contain fewer entries than the complete skeleton; the
		// palette compaction stage has already mapped each entry to its source joint.
		if (jointIndex >= record.paletteEntryCount
			|| jointIndex > (0xFFFFFFFFu - record.paletteOffsetBytes.x) / kAnimationPaletteAffineBytes)
		{
			return false;
		}
		const uint paletteOffset = record.paletteOffsetBytes.x + jointIndex * kAnimationPaletteAffineBytes;
		if (paletteOffset > paletteByteCount || kAnimationPaletteAffineBytes > paletteByteCount - paletteOffset)
		{
			return false;
		}
		const uint paletteWordOffset = paletteOffset >> 2u;
		const float4 row0 = asfloat(uint4(
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 0u],
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 1u],
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 2u],
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 3u]));
		const float4 row1 = asfloat(uint4(
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 4u],
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 5u],
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 6u],
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 7u]));
		const float4 row2 = asfloat(uint4(
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 8u],
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 9u],
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 10u],
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 11u]));
		if (!DefaultGpuAnimationFinite3(row0.xyz) || !DefaultGpuAnimationFinite3(row1.xyz) || !DefaultGpuAnimationFinite3(row2.xyz)
			|| !DefaultGpuAnimationFinite(row0.w) || !DefaultGpuAnimationFinite(row1.w) || !DefaultGpuAnimationFinite(row2.w))
		{
			return false;
		}
		const float weight = float(weightByte) / float(weightSum);
		skinnedPosition += float3(dot(row0.xyz, position) + row0.w, dot(row1.xyz, position) + row1.w, dot(row2.xyz, position) + row2.w) * weight;
		skinnedNormal += float3(dot(row0.xyz, normal), dot(row1.xyz, normal), dot(row2.xyz, normal)) * weight;
		skinnedTangent += float3(dot(row0.xyz, tangent), dot(row1.xyz, tangent), dot(row2.xyz, tangent)) * weight;
	}
	if (!DefaultGpuAnimationFinite3(skinnedPosition)
		|| !DefaultGpuAnimationFinite3(skinnedNormal)
		|| !DefaultGpuAnimationFinite3(skinnedTangent)
		|| dot(skinnedNormal, skinnedNormal) <= 1e-12f
		|| dot(skinnedTangent, skinnedTangent) <= 1e-12f)
	{
		return false;
	}
	position = skinnedPosition;
	normal = normalize(skinnedNormal);
	tangent = normalize(skinnedTangent);
	return true;
}

void ApplyGpuDeformationIfValid(
	DefaultGpuAnimationGraphicsRecord record,
	MeshDecodeInfo decode,
	uint globalVertexIndex,
	inout float3 position,
	inout float3 normal,
	inout float3 tangent)
{
	// Work on local copies so one malformed morph/palette range atomically falls
	// back to the cooked bind pose instead of mixing deformed and static data.
	float3 deformedPosition = position;
	float3 deformedNormal = normal;
	float3 deformedTangent = tangent;
	if (TryApplyGpuMorphs(record, decode, globalVertexIndex, deformedPosition, deformedNormal, deformedTangent)
		&& TryApplyGpuSkinning(record, globalVertexIndex, deformedPosition, deformedNormal, deformedTangent))
	{
		position = deformedPosition;
		normal = deformedNormal;
		tangent = deformedTangent;
	}
}

// Indexed qualification must never silently substitute a cooked bind pose.
// Native admission validates the entire mesh and retained publication before draw.
bool TryApplyOwnerMeshDeformationStrict(DefaultGpuAnimationGraphicsRecord record,
	MeshDecodeInfo decode, uint ownerVertex, inout float3 position,
	inout float3 normal, inout float3 tangent)
{
	float3 p=position, n=normal, t=tangent;
	if(!TryApplyGpuMorphs(record,decode,ownerVertex,p,n,t)
		||!TryApplyGpuSkinning(record,ownerVertex,p,n,t))return false;
	position=p;normal=n;tangent=t;
	return true;
}

#endif
