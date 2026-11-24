// Current/previous private CPU-palette evaluation. The position arithmetic and
// morph-before-skin order match DefaultOwnerDeformation.hlsli, using retained
// tightly packed private uploads instead of mutable global frame descriptors.
#ifndef TERRAENGINE_INDEXED_MOTION_DEFORMATION_HLSLI
#define TERRAENGINE_INDEXED_MOTION_DEFORMATION_HLSLI

bool MotionLoadPose(uint2 selectors, out IndexedMotionPose pose)
{
	pose = (IndexedMotionPose)0;
	// Namespace0 is this recording's single private pose table. A global bindless
	// animation-buffer index must never accidentally select an unrelated row.
	if(selectors.x != 0u) return false;
	uint count, stride; indexedMotionPoses.GetDimensions(count, stride);
	if(stride != 32u || selectors.y >= count) return false;
	pose = indexedMotionPoses[selectors.y];
	if(pose.validMarker != 1u || pose.reserved0 != 0u || pose.reserved1 != 0u
		|| pose.jointCount == 0u || pose.jointCount > 256u || pose.paletteCount < pose.jointCount || pose.paletteCount > 256u
		|| pose.morphCount > 4096u) return false;
	uint paletteCount, paletteStride, weights, weightStride;
	indexedMotionPalettes.GetDimensions(paletteCount, paletteStride); indexedMotionMorphWeights.GetDimensions(weights, weightStride);
	return paletteStride == 48u && weightStride == 4u && pose.paletteOffset <= paletteCount
		&& pose.paletteCount <= paletteCount - pose.paletteOffset && pose.morphOffset <= weights && pose.morphCount <= weights - pose.morphOffset;
}
bool MotionApplyMorphs(IndexedMotionPose pose, IndexedMotionDecode decode, uint owner,
	inout float3 position, inout float3 normal, inout float3 tangent)
{
	uint bytes; motionMorphDeltaBytes.GetDimensions(bytes);
	float3 p = position, n = normal, t = tangent;
	for(uint target = 0u; target < pose.morphCount; ++target) {
		float weight = indexedMotionMorphWeights[pose.morphOffset + target];
		if(!isfinite(weight)) return false; if(weight == 0.0f) continue;
		if(decode.counts.x == 0u || target > (0xffffffffu - owner) / decode.counts.x) return false;
		uint index = target * decode.counts.x + owner;
		if(index > (0xffffffffu - 48u) / 48u) return false; uint offset = index * 48u;
		if(offset > bytes || 48u > bytes - offset) return false;
		float3 dp = asfloat(motionMorphDeltaBytes.Load4(offset)).xyz;
		float3 dn = asfloat(motionMorphDeltaBytes.Load4(offset + 16u)).xyz;
		float3 dt = asfloat(motionMorphDeltaBytes.Load4(offset + 32u)).xyz;
		if(!all(isfinite(dp)) || !all(isfinite(dn)) || !all(isfinite(dt))) return false;
		p += dp * weight; n += dn * weight; t += dt * weight;
	}
	if(!all(isfinite(p)) || !all(isfinite(n)) || !all(isfinite(t))) return false;
	position = p; normal = n; tangent = t; return true;
}
bool MotionApplySkin(IndexedMotionPose pose, uint owner,
	inout float3 position, inout float3 normal, inout float3 tangent)
{
	uint bytes; motionSkinningBytes.GetDimensions(bytes);
	if(owner > (0xffffffffu - 8u) / 8u) return false; uint offset = owner * 8u;
	if(offset > bytes || 8u > bytes - offset) return false;
	uint joints = motionSkinningBytes.Load(offset), weights = motionSkinningBytes.Load(offset + 4u);
	uint weightSum = (weights & 255u) + ((weights >> 8u) & 255u) + ((weights >> 16u) & 255u) + ((weights >> 24u) & 255u);
	// Zero skin weights intentionally preserve a morph-only vertex, as in Default.
	if(weightSum == 0u) return true;
	float3 p = float3(0, 0, 0), n = float3(0, 0, 0), t = float3(0, 0, 0);
	for(uint influence = 0u; influence < 4u; ++influence) {
		uint weightByte = (weights >> (influence * 8u)) & 255u; if(weightByte == 0u) continue;
		uint joint = (joints >> (influence * 8u)) & 255u;
		if(joint >= pose.paletteCount) return false;
		IndexedMotionPalette matrix = indexedMotionPalettes[pose.paletteOffset + joint];
		if(!all(isfinite(matrix.row0)) || !all(isfinite(matrix.row1)) || !all(isfinite(matrix.row2))) return false;
		float weight = float(weightByte) / float(weightSum);
		p += float3(dot(matrix.row0.xyz, position) + matrix.row0.w, dot(matrix.row1.xyz, position) + matrix.row1.w, dot(matrix.row2.xyz, position) + matrix.row2.w) * weight;
		n += float3(dot(matrix.row0.xyz, normal), dot(matrix.row1.xyz, normal), dot(matrix.row2.xyz, normal)) * weight;
		t += float3(dot(matrix.row0.xyz, tangent), dot(matrix.row1.xyz, tangent), dot(matrix.row2.xyz, tangent)) * weight;
	}
	if(!all(isfinite(p)) || !all(isfinite(n)) || !all(isfinite(t)) || dot(n, n) <= 1e-12f || dot(t, t) <= 1e-12f) return false;
	position = p; normal = normalize(n); tangent = normalize(t); return true;
}
bool MotionDeformPosition(uint kind, uint2 selectors, IndexedMotionDecode decode,
	uint owner, float3 basePosition, float3 baseNormal, float3 baseTangent, out float3 position)
{
	position = basePosition;
	if(kind == kMotionStatic) return all(selectors == kMotionInvalidSelector) && all(isfinite(position));
	if(kind != kMotionOwnerPalette) return false;
	IndexedMotionPose pose; if(!MotionLoadPose(selectors, pose)) return false;
	float3 p = basePosition, n = baseNormal, t = baseTangent;
	if(!MotionApplyMorphs(pose, decode, owner, p, n, t) || !MotionApplySkin(pose, owner, p, n, t)) return false;
	position = p; return true;
}
#endif
