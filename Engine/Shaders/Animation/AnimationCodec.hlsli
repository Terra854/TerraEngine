// AnimationCodec.hlsli - Bounded shared helpers for GPU animation stages.
//
// The packet ABI lives in AnimationGpuAbi.hlsli.  This file deliberately keeps
// stage algorithms out of that ABI while documenting the byte-address resource
// views used by the first GPU lowering.  Every address is checked before load
// or store so a corrupt upload produces a diagnostic and a safe result.

#ifndef TERRAENGINE_ANIMATION_CODEC_HLSLI
#define TERRAENGINE_ANIMATION_CODEC_HLSLI

#include "AnimationGpuAbi.hlsli"

#define TE_ANIMATION_MAX_JOINTS 4096u
#define TE_ANIMATION_MAX_MORPHS 4096u
#define TE_ANIMATION_MAX_KEYS 4096u
#define TE_ANIMATION_MAX_DISPATCH_RECORDS 1024u
#define TE_ANIMATION_MAX_PARENT_DEPTH 128u
#define TE_ANIMATION_INVALID_RESOURCE 0xffffffffu

struct TeAnimationPose {
	float4 translation;
	float4 rotation;
	float4 scale;
};

struct TeAnimationAffine {
	float4 row0;
	float4 row1;
	float4 row2;
};

// The raw views retain the byte offsets in TeAnimationGpuWorkPacket exactly.
// Later descriptor planning owns which immutable resource is packed at a
// resource index; shaders never infer a CPU pointer or an unchecked stride.
[[vk::binding(TE_ANIMATION_GPU_DESCRIPTOR_GENERATION_RECORDS, TE_ANIMATION_GPU_DESCRIPTOR_SET)]] StructuredBuffer<TeAnimationGpuGenerationRecord> gAnimationGenerations;
[[vk::binding(TE_ANIMATION_GPU_DESCRIPTOR_WORK_PACKETS, TE_ANIMATION_GPU_DESCRIPTOR_SET)]] StructuredBuffer<TeAnimationGpuWorkPacket> gAnimationWorkPackets;
[[vk::binding(TE_ANIMATION_GPU_DESCRIPTOR_DISPATCH_RECORDS, TE_ANIMATION_GPU_DESCRIPTOR_SET)]] StructuredBuffer<TeAnimationGpuDispatchRecord> gAnimationDispatches;
[[vk::binding(TE_ANIMATION_GPU_DESCRIPTOR_PROGRAM_WORDS, TE_ANIMATION_GPU_DESCRIPTOR_SET)]] ByteAddressBuffer gAnimationProgram;
[[vk::binding(TE_ANIMATION_GPU_DESCRIPTOR_PACKED_CLIP_BYTES, TE_ANIMATION_GPU_DESCRIPTOR_SET)]] ByteAddressBuffer gAnimationClip;
[[vk::binding(TE_ANIMATION_GPU_DESCRIPTOR_SKELETON_TABLES, TE_ANIMATION_GPU_DESCRIPTOR_SET)]] ByteAddressBuffer gAnimationSkeleton;
[[vk::binding(TE_ANIMATION_GPU_DESCRIPTOR_MASK_TABLES, TE_ANIMATION_GPU_DESCRIPTOR_SET)]] ByteAddressBuffer gAnimationMasks;
[[vk::binding(TE_ANIMATION_GPU_DESCRIPTOR_MORPH_INPUTS, TE_ANIMATION_GPU_DESCRIPTOR_SET)]] ByteAddressBuffer gAnimationMorphInputs;
[[vk::binding(TE_ANIMATION_GPU_DESCRIPTOR_SCRATCH, TE_ANIMATION_GPU_DESCRIPTOR_SET)]] RWByteAddressBuffer gAnimationScratch;
[[vk::binding(TE_ANIMATION_GPU_DESCRIPTOR_LOCAL_POSES, TE_ANIMATION_GPU_DESCRIPTOR_SET)]] RWByteAddressBuffer gAnimationLocalPoses;
[[vk::binding(TE_ANIMATION_GPU_DESCRIPTOR_MODEL_POSES, TE_ANIMATION_GPU_DESCRIPTOR_SET)]] RWByteAddressBuffer gAnimationModelPoses;
[[vk::binding(TE_ANIMATION_GPU_DESCRIPTOR_PALETTE_MATRICES, TE_ANIMATION_GPU_DESCRIPTOR_SET)]] RWByteAddressBuffer gAnimationPalette;
[[vk::binding(TE_ANIMATION_GPU_DESCRIPTOR_MORPH_OUTPUTS, TE_ANIMATION_GPU_DESCRIPTOR_SET)]] RWByteAddressBuffer gAnimationMorphOutputs;
[[vk::binding(TE_ANIMATION_GPU_DESCRIPTOR_BOUNDS_OUTPUTS, TE_ANIMATION_GPU_DESCRIPTOR_SET)]] RWByteAddressBuffer gAnimationBounds;
[[vk::binding(TE_ANIMATION_GPU_DESCRIPTOR_DIAGNOSTICS, TE_ANIMATION_GPU_DESCRIPTOR_SET)]] RWByteAddressBuffer gAnimationDiagnostics;

[[vk::push_constant]] ConstantBuffer<TeAnimationGpuPushConstants> gAnimationPush;

bool TeRangeValid(uint byteCount, uint offset, uint size)
{
	return offset <= byteCount && size <= byteCount - offset;
}

// HLSL uint arithmetic wraps.  Keep offset construction separate from range
// validation so a hostile packet cannot wrap an address back into valid memory.
bool TeAddOffset(uint base, uint relative, out uint offset)
{
	offset = base + relative;
	return offset >= base;
}

bool TeFinite(float value)
{
	return !isnan(value) && !isinf(value);
}

bool TeFinite3(float3 value)
{
	return TeFinite(value.x) && TeFinite(value.y) && TeFinite(value.z);
}

bool TeFinite4(float4 value)
{
	return TeFinite(value.x) && TeFinite(value.y) && TeFinite(value.z) && TeFinite(value.w);
}

TeAnimationPose TeIdentityPose()
{
	TeAnimationPose pose;
	pose.translation = float4(0.0f, 0.0f, 0.0f, 0.0f);
	pose.rotation = float4(0.0f, 0.0f, 0.0f, 1.0f);
	pose.scale = float4(1.0f, 1.0f, 1.0f, 0.0f);
	return pose;
}

void TeReport(in TeAnimationGpuDispatchRecord dispatch, in TeAnimationGpuWorkPacket packet, uint flags, uint detail0, uint detail1)
{
	uint diagnosticBytes;
	gAnimationDiagnostics.GetDimensions(diagnosticBytes);
	const uint index = dispatch.diagnosticIndex;
	if (index > 0xffffffffu / 32u) return;
	const uint offset = index * 32u;
	if (!TeRangeValid(diagnosticBytes, offset, 32u))
	{
		return;
	}
	uint ignored;
	gAnimationDiagnostics.InterlockedOr(offset, flags, ignored);
	// These identity fields are idempotent: every invocation for one dispatch
	// writes the same producer identity, while the flag word accumulates errors.
	gAnimationDiagnostics.Store(offset + 4u, packet.stableInstanceId.x);
	gAnimationDiagnostics.Store(offset + 8u, packet.stableInstanceId.y);
	gAnimationDiagnostics.Store(offset + 12u, dispatch.stage);
	gAnimationDiagnostics.Store(offset + 16u, dispatch.workPacketIndex);
	gAnimationDiagnostics.Store(offset + 20u, dispatch.generationRecordIndex);
	gAnimationDiagnostics.Store(offset + 24u, detail0);
	gAnimationDiagnostics.Store(offset + 28u, detail1);
}

bool TeGenerationValid(TeAnimationGpuGenerationRecord generation)
{
	return generation.instanceGeneration != 0u && generation.componentGeneration != 0u
		&& generation.workGeneration != 0u && generation.programGeneration != 0u
		&& generation.controllerGeneration != 0u && generation.skeletonGeneration != 0u
		&& generation.clipGeneration != 0u && generation.meshGeneration != 0u
		&& generation.resourceGeneration != 0u && generation.rendererGeneration != 0u
		&& generation.sceneGeneration != 0u;
}

bool TeResolveWork(uint globalElement, uint expectedStage, out TeAnimationGpuDispatchRecord dispatch, out TeAnimationGpuWorkPacket packet, out uint element)
{
	dispatch = (TeAnimationGpuDispatchRecord)0;
	packet = (TeAnimationGpuWorkPacket)0;
	element = 0u;
	uint dispatchCount;
	uint dispatchStride;
	gAnimationDispatches.GetDimensions(dispatchCount, dispatchStride);
	uint packetCount;
	uint packetStride;
	gAnimationWorkPackets.GetDimensions(packetCount, packetStride);
	const uint commandCount = min(gAnimationPush.dispatchRecordCount, TE_ANIMATION_MAX_DISPATCH_RECORDS);
	if (gAnimationPush.stage != expectedStage || gAnimationPush.firstElementHigh != 0u)
	{
		return false;
	}
	const uint absoluteElement = gAnimationPush.firstElementLow + globalElement;
	if (absoluteElement < gAnimationPush.firstElementLow || globalElement >= gAnimationPush.elementCount)
	{
		return false;
	}
	[loop]
	for (uint candidate = 0u; candidate < TE_ANIMATION_MAX_DISPATCH_RECORDS; ++candidate)
	{
		if (candidate >= commandCount)
		{
			break;
		}
		uint dispatchIndex;
		if (!TeAddOffset(gAnimationPush.firstDispatchRecord, candidate, dispatchIndex) || dispatchIndex >= dispatchCount)
		{
			return false;
		}
		const TeAnimationGpuDispatchRecord value = gAnimationDispatches[dispatchIndex];
		if (value.stage != expectedStage || value.elementOffset.y != 0u || value.elementCount == 0u
			|| absoluteElement < value.elementOffset.x || absoluteElement - value.elementOffset.x >= value.elementCount)
		{
			continue;
		}
		if (value.workPacketIndex >= packetCount || value.resourceLayoutId != 1u)
		{
			return false;
		}
		dispatch = value;
		packet = gAnimationWorkPackets[value.workPacketIndex];
		element = absoluteElement - value.elementOffset.x;
		return packet.resourceLayoutId == 1u && packet.generationRecordIndex == value.generationRecordIndex;
	}
	return false;
}

bool TeValidatePacket(in TeAnimationGpuDispatchRecord dispatch, in TeAnimationGpuWorkPacket packet, uint maximumElement, uint stage)
{
	uint generationCount;
	uint generationStride;
	gAnimationGenerations.GetDimensions(generationCount, generationStride);
	if (packet.jointCount > TE_ANIMATION_MAX_JOINTS || packet.morphCount > TE_ANIMATION_MAX_MORPHS
		|| packet.generationRecordIndex >= generationCount || packet.resourceLayoutId != 1u
		|| packet.clipResourceIndex == TE_ANIMATION_INVALID_RESOURCE || packet.programResourceIndex == TE_ANIMATION_INVALID_RESOURCE
		|| packet.skeletonResourceIndex == TE_ANIMATION_INVALID_RESOURCE || packet.programResourceIndex > 0x03ffffffu
		|| (packet.stageMask & (1u << stage)) == 0u || maximumElement == 0xffffffffu
		|| packet.reserved0 != 0u || packet.reserved1 != 0u)
	{
		TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_INVALID_RECORD, maximumElement, packet.stageMask);
		return false;
	}
	if (!TeGenerationValid(gAnimationGenerations[packet.generationRecordIndex]))
	{
		TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_STALE_GENERATION, packet.generationRecordIndex, 0u);
		return false;
	}
	return true;
}

bool TeLoadPose(ByteAddressBuffer source, uint offset, out TeAnimationPose pose)
{
	uint bytes;
	source.GetDimensions(bytes);
	pose = TeIdentityPose();
	if (!TeRangeValid(bytes, offset, 48u))
	{
		return false;
	}
	pose.translation = asfloat(source.Load4(offset));
	pose.rotation = asfloat(source.Load4(offset + 16u));
	pose.scale = asfloat(source.Load4(offset + 32u));
	return TeFinite4(pose.translation) && TeFinite4(pose.rotation) && TeFinite4(pose.scale)
		&& dot(pose.rotation, pose.rotation) > 1.0e-12f;
}

bool TeLoadLocalPose(uint offset, out TeAnimationPose pose)
{
	uint bytes;
	gAnimationLocalPoses.GetDimensions(bytes);
	pose = TeIdentityPose();
	if (!TeRangeValid(bytes, offset, 48u)) return false;
	pose.translation = asfloat(gAnimationLocalPoses.Load4(offset));
	pose.rotation = asfloat(gAnimationLocalPoses.Load4(offset + 16u));
	pose.scale = asfloat(gAnimationLocalPoses.Load4(offset + 32u));
	return TeFinite4(pose.translation) && TeFinite4(pose.rotation) && TeFinite4(pose.scale)
		&& dot(pose.rotation, pose.rotation) > 1.0e-12f;
}

bool TeStoreLocalPose(uint offset, TeAnimationPose pose)
{
	uint bytes;
	gAnimationLocalPoses.GetDimensions(bytes);
	if (!TeRangeValid(bytes, offset, 48u)) return false;
	gAnimationLocalPoses.Store4(offset, asuint(pose.translation));
	gAnimationLocalPoses.Store4(offset + 16u, asuint(pose.rotation));
	gAnimationLocalPoses.Store4(offset + 32u, asuint(pose.scale));
	return true;
}

bool TeStoreModel(uint offset, TeAnimationAffine matrix)
{
	uint bytes;
	gAnimationModelPoses.GetDimensions(bytes);
	if (!TeRangeValid(bytes, offset, 48u)) return false;
	gAnimationModelPoses.Store4(offset, asuint(matrix.row0));
	gAnimationModelPoses.Store4(offset + 16u, asuint(matrix.row1));
	gAnimationModelPoses.Store4(offset + 32u, asuint(matrix.row2));
	return true;
}

bool TeLoadModel(uint offset, out TeAnimationAffine matrix)
{
	uint bytes;
	gAnimationModelPoses.GetDimensions(bytes);
	matrix = (TeAnimationAffine)0;
	if (!TeRangeValid(bytes, offset, 48u)) return false;
	matrix.row0 = asfloat(gAnimationModelPoses.Load4(offset));
	matrix.row1 = asfloat(gAnimationModelPoses.Load4(offset + 16u));
	matrix.row2 = asfloat(gAnimationModelPoses.Load4(offset + 32u));
	return TeFinite4(matrix.row0) && TeFinite4(matrix.row1) && TeFinite4(matrix.row2);
}

float4 TeNormalizeQuaternion(float4 value)
{
	const float lengthSquared = dot(value, value);
	if (!TeFinite4(value) || lengthSquared <= 1.0e-12f || !TeFinite(lengthSquared)) return float4(0.0f, 0.0f, 0.0f, 1.0f);
	return value * rsqrt(lengthSquared);
}

TeAnimationAffine TeAffineFromPose(TeAnimationPose pose)
{
	const float4 q = TeNormalizeQuaternion(pose.rotation);
	const float xx = q.x * q.x; const float yy = q.y * q.y; const float zz = q.z * q.z;
	const float xy = q.x * q.y; const float xz = q.x * q.z; const float yz = q.y * q.z;
	const float wx = q.w * q.x; const float wy = q.w * q.y; const float wz = q.w * q.z;
	TeAnimationAffine result;
	result.row0 = float4((1.0f - 2.0f * (yy + zz)) * pose.scale.x, (2.0f * (xy - wz)) * pose.scale.y, (2.0f * (xz + wy)) * pose.scale.z, pose.translation.x);
	result.row1 = float4((2.0f * (xy + wz)) * pose.scale.x, (1.0f - 2.0f * (xx + zz)) * pose.scale.y, (2.0f * (yz - wx)) * pose.scale.z, pose.translation.y);
	result.row2 = float4((2.0f * (xz - wy)) * pose.scale.x, (2.0f * (yz + wx)) * pose.scale.y, (1.0f - 2.0f * (xx + yy)) * pose.scale.z, pose.translation.z);
	return result;
}

TeAnimationAffine TeMultiplyAffine(TeAnimationAffine parent, TeAnimationAffine local)
{
	TeAnimationAffine result;
	// Three affine rows use ordinary row-major A * B multiplication. Keeping
	// each output row tied to the same parent row is essential for rotated or
	// non-uniformly scaled parents; the prior transposed expression happened to
	// work only for identity-like matrices.
	result.row0 = float4(
		parent.row0.x * local.row0.xyz + parent.row0.y * local.row1.xyz + parent.row0.z * local.row2.xyz,
		0.0f);
	result.row1 = float4(
		parent.row1.x * local.row0.xyz + parent.row1.y * local.row1.xyz + parent.row1.z * local.row2.xyz,
		0.0f);
	result.row2 = float4(
		parent.row2.x * local.row0.xyz + parent.row2.y * local.row1.xyz + parent.row2.z * local.row2.xyz,
		0.0f);
	// The W lanes hold translation.  Spell this out instead of relying on an
	// implicit float4x4 layout, which keeps the ABI's three-row representation
	// unambiguous across shader compilers.
	const float3 localTranslation = float3(local.row0.w, local.row1.w, local.row2.w);
	result.row0.w = dot(parent.row0.xyz, localTranslation) + parent.row0.w;
	result.row1.w = dot(parent.row1.xyz, localTranslation) + parent.row1.w;
	result.row2.w = dot(parent.row2.xyz, localTranslation) + parent.row2.w;
	return result;
}

bool TeClipLoadU16(uint offset, out uint value)
{
	uint bytes;
	gAnimationClip.GetDimensions(bytes);
	value = 0u;
	if (!TeRangeValid(bytes, offset, 2u)) return false;
	const uint packed = gAnimationClip.Load(offset & ~3u);
	value = (packed >> ((offset & 2u) * 8u)) & 0xffffu;
	return true;
}

bool TeDecodeSmallestThreeQuaternionKey(uint offset, out float4 value)
{
	value = float4(0.0f, 0.0f, 0.0f, 1.0f);
	uint c0; uint c1; uint c2; uint largestAndReserved;
	if (!TeClipLoadU16(offset, c0)
		|| !TeClipLoadU16(offset + 2u, c1)
		|| !TeClipLoadU16(offset + 4u, c2)
		|| !TeClipLoadU16(offset + 6u, largestAndReserved))
	{
		return false;
	}
	const uint largest = largestAndReserved & 0xffu;
	const uint reserved = largestAndReserved >> 8u;
	if (largest > 3u || reserved != 0u) return false;
	const float3 stored = float3(
		int(c0 << 16u) >> 16,
		int(c1 << 16u) >> 16,
		int(c2 << 16u) >> 16) * (0.70710678118f / 32767.0f);
	if (!TeFinite3(stored)) return false;
	float components[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	uint storedIndex = 0u;
	[unroll]
	for (uint component = 0u; component < 4u; ++component)
	{
		if (component != largest) components[component] = stored[storedIndex++];
	}
	components[largest] = sqrt(max(0.0f, 1.0f - dot(stored, stored)));
	value = TeNormalizeQuaternion(float4(
		components[0], components[1], components[2], components[3]));
	return TeFinite4(value);
}

bool TeResolveTrackHeader(uint locatorOffset, uint tick, out uint headerOffset)
{
	headerOffset = 0u;
	uint bytes;
	gAnimationClip.GetDimensions(bytes);
	if (!TeRangeValid(bytes, locatorOffset, 4u)) return false;
	const uint locatorMagic = gAnimationClip.Load(locatorOffset);
	if (locatorMagic == TE_ANIMATION_CODEC_TRACK_MAGIC)
	{
		headerOffset = locatorOffset;
		return true;
	}
	if (locatorMagic != TE_ANIMATION_TRACK_DIRECTORY_MAGIC
		|| !TeRangeValid(
			bytes, locatorOffset, TE_ANIMATION_TRACK_DIRECTORY_HEADER_BYTES))
	{
		return false;
	}
	const uint versionAndStride = gAnimationClip.Load(locatorOffset + 4u);
	const uint version = versionAndStride & 0xffffu;
	const uint stride = versionAndStride >> 16u;
	const uint entryCount = gAnimationClip.Load(locatorOffset + 8u);
	if (version != TE_ANIMATION_TRACK_DIRECTORY_VERSION
		|| stride != TE_ANIMATION_TRACK_DIRECTORY_ENTRY_BYTES
		|| entryCount == 0u
		|| entryCount > TE_ANIMATION_MAX_TRACK_SEGMENTS
		|| gAnimationClip.Load(locatorOffset + 12u) != 0u
		|| entryCount > (0xffffffffu
			- locatorOffset - TE_ANIMATION_TRACK_DIRECTORY_HEADER_BYTES)
				/ TE_ANIMATION_TRACK_DIRECTORY_ENTRY_BYTES)
	{
		return false;
	}
	const uint entriesOffset =
		locatorOffset + TE_ANIMATION_TRACK_DIRECTORY_HEADER_BYTES;
	if (!TeRangeValid(
		bytes,
		entriesOffset,
		entryCount * TE_ANIMATION_TRACK_DIRECTORY_ENTRY_BYTES))
	{
		return false;
	}
	bool selected = false;
	uint previousEnd = 0u;
	[loop]
	for (uint index = 0u; index < TE_ANIMATION_MAX_TRACK_SEGMENTS; ++index)
	{
		if (index >= entryCount) break;
		const uint entryOffset =
			entriesOffset + index * TE_ANIMATION_TRACK_DIRECTORY_ENTRY_BYTES;
		const uint startTick = gAnimationClip.Load(entryOffset);
		const uint endTick = gAnimationClip.Load(entryOffset + 4u);
		const uint candidateOffset = gAnimationClip.Load(entryOffset + 8u);
		const uint reserved = gAnimationClip.Load(entryOffset + 12u);
		if (endTick < startTick || reserved != 0u
			|| (index != 0u && startTick < previousEnd)
			|| !TeRangeValid(
				bytes, candidateOffset, TE_ANIMATION_CODEC_TRACK_HEADER_BYTES))
		{
			return false;
		}
		const bool contains = tick >= startTick
			&& (tick < endTick
				|| (index + 1u == entryCount && tick <= endTick));
		if (contains)
		{
			if (selected) return false;
			headerOffset = candidateOffset;
			selected = true;
		}
		previousEnd = endTick;
	}
	return selected;
}

bool TeDecodeTrack(uint locatorOffset, uint tick, out float3 vectorValue, out float4 rotationValue, out float scalarValue)
{
	vectorValue = float3(0.0f, 0.0f, 0.0f);
	rotationValue = float4(0.0f, 0.0f, 0.0f, 1.0f);
	scalarValue = 0.0f;
	uint bytes;
	gAnimationClip.GetDimensions(bytes);
	uint headerOffset;
	if (!TeResolveTrackHeader(locatorOffset, tick, headerOffset)) return false;
	if (!TeRangeValid(bytes, headerOffset, TE_ANIMATION_CODEC_TRACK_HEADER_BYTES)) return false;
	const uint magic = gAnimationClip.Load(headerOffset + TE_ANIMATION_CODEC_TRACK_MAGIC_OFFSET);
	const uint versionAndEncoding = gAnimationClip.Load(headerOffset + TE_ANIMATION_CODEC_TRACK_VERSION_OFFSET);
	const uint version = versionAndEncoding & 0xffffu;
	const uint encoding = (versionAndEncoding >> 16u) & 0xffu;
	const uint flags = (versionAndEncoding >> 24u) & 0xffu;
	const uint byteSize = gAnimationClip.Load(headerOffset + TE_ANIMATION_CODEC_TRACK_BYTE_SIZE_OFFSET);
	const uint keyCount = gAnimationClip.Load(headerOffset + TE_ANIMATION_CODEC_TRACK_KEY_COUNT_OFFSET);
	const uint startTick = gAnimationClip.Load(headerOffset + TE_ANIMATION_CODEC_TRACK_SEGMENT_START_TICK_OFFSET);
	const uint endTick = gAnimationClip.Load(headerOffset + TE_ANIMATION_CODEC_TRACK_SEGMENT_END_TICK_OFFSET);
	if (magic != TE_ANIMATION_CODEC_TRACK_MAGIC || version != TE_ANIMATION_CODEC_VERSION || (flags & ~TE_ANIMATION_CODEC_CONSTANT_TRACK) != 0u
		|| keyCount == 0u || keyCount > TE_ANIMATION_MAX_KEYS || endTick < startTick || byteSize < TE_ANIMATION_CODEC_TRACK_HEADER_BYTES
		|| !TeRangeValid(bytes, headerOffset, byteSize)) return false;
	const float3 rangeMinimum = asfloat(gAnimationClip.Load3(headerOffset + TE_ANIMATION_CODEC_TRACK_RANGE_MINIMUM_OFFSET));
	const float3 rangeExtent = asfloat(gAnimationClip.Load3(headerOffset + TE_ANIMATION_CODEC_TRACK_RANGE_EXTENT_OFFSET));
	if (!TeFinite3(rangeMinimum) || !TeFinite3(rangeExtent) || any(rangeExtent < 0.0f)) return false;
	if ((flags & TE_ANIMATION_CODEC_CONSTANT_TRACK) != 0u)
	{
		vectorValue = asfloat(gAnimationClip.Load3(headerOffset + TE_ANIMATION_CODEC_TRACK_CONSTANT_VECTOR_OFFSET));
		rotationValue = TeNormalizeQuaternion(asfloat(gAnimationClip.Load4(headerOffset + TE_ANIMATION_CODEC_TRACK_CONSTANT_ROTATION_OFFSET)));
		scalarValue = asfloat(gAnimationClip.Load(headerOffset + TE_ANIMATION_CODEC_TRACK_CONSTANT_SCALAR_OFFSET));
		return encoding == TE_ANIMATION_CODEC_ENCODING_VECTOR3_UNORM16 ? TeFinite3(vectorValue)
			: (encoding == TE_ANIMATION_CODEC_ENCODING_QUATERNION_SMALLEST_THREE16 ? true : TeFinite(scalarValue));
	}
	const uint localTick = clamp(tick, startTick, endTick) - startTick;
	const uint timeOffset = headerOffset + TE_ANIMATION_CODEC_TRACK_HEADER_BYTES;
	const uint valueStride = encoding == TE_ANIMATION_CODEC_ENCODING_SCALAR_UNORM16 ? 2u
		: (encoding == TE_ANIMATION_CODEC_ENCODING_VECTOR3_UNORM16 ? 6u : 8u);
	if ((encoding > TE_ANIMATION_CODEC_ENCODING_QUATERNION_SMALLEST_THREE16) || !TeRangeValid(bytes, timeOffset, keyCount * 4u + keyCount * valueStride)) return false;
	uint left = 0u; uint right = keyCount - 1u; uint leftTick = 0u; uint rightTick = 0u;
	[loop]
	for (uint key = 0u; key < TE_ANIMATION_MAX_KEYS; ++key)
	{
		if (key >= keyCount) break;
		const uint candidate = gAnimationClip.Load(timeOffset + key * 4u);
		if (candidate > endTick - startTick || (key > 0u && candidate <= gAnimationClip.Load(timeOffset + (key - 1u) * 4u))) return false;
		if (candidate <= localTick) { left = key; leftTick = candidate; }
		if (candidate >= localTick) { right = key; rightTick = candidate; break; }
	}
	const uint valueOffset = timeOffset + keyCount * 4u;
	uint l0; uint l1; uint l2; uint r0; uint r1; uint r2;
	if (!TeClipLoadU16(valueOffset + left * valueStride, l0) || !TeClipLoadU16(valueOffset + right * valueStride, r0)) return false;
	const float factor = right == left ? 0.0f : saturate(float(localTick - leftTick) / float(max(1u, rightTick - leftTick)));
	if (encoding == TE_ANIMATION_CODEC_ENCODING_SCALAR_UNORM16)
	{
		scalarValue = rangeMinimum.x + rangeExtent.x * lerp(float(l0), float(r0), factor) / 65535.0f;
		return TeFinite(scalarValue);
	}
	if (encoding == TE_ANIMATION_CODEC_ENCODING_VECTOR3_UNORM16)
	{
		if (!TeClipLoadU16(valueOffset + left * valueStride + 2u, l1) || !TeClipLoadU16(valueOffset + left * valueStride + 4u, l2)
			|| !TeClipLoadU16(valueOffset + right * valueStride + 2u, r1) || !TeClipLoadU16(valueOffset + right * valueStride + 4u, r2)) return false;
		vectorValue = rangeMinimum + rangeExtent * lerp(float3(l0, l1, l2), float3(r0, r1, r2), factor) / 65535.0f;
		return TeFinite3(vectorValue);
	}
	float4 leftRotation;
	float4 rightRotation;
	if (!TeDecodeSmallestThreeQuaternionKey(
			valueOffset + left * valueStride, leftRotation)
		|| !TeDecodeSmallestThreeQuaternionKey(
			valueOffset + right * valueStride, rightRotation))
	{
		return false;
	}
	// Match the CPU codec's shortest-path normalized interpolation. Quaternion
	// signs are equivalent orientations, but interpolating opposite signs without
	// this correction can collapse through a zero-length quaternion.
	if (dot(leftRotation, rightRotation) < 0.0f) rightRotation = -rightRotation;
	rotationValue = TeNormalizeQuaternion(
		lerp(leftRotation, rightRotation, factor));
	return TeFinite4(rotationValue);
}

#endif // TERRAENGINE_ANIMATION_CODEC_HLSLI
