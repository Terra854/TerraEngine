// Transient visibility V2: Y stores direct perspective source weights and Z
// stores the actual raster winner's normalized depth key. All writers/readers
// must compile this contract together; no persistent asset layout is changed.
#ifndef TERRAENGINE_VIRTUAL_GEOMETRY_RECONSTRUCTION_HLSLI
#define TERRAENGINE_VIRTUAL_GEOMETRY_RECONSTRUCTION_HLSLI

static const uint TE_VIRTUAL_GEOMETRY_VISIBILITY_ENCODING_TOKEN = 0x56320000u;
static const uint TE_VIRTUAL_GEOMETRY_VISIBILITY_ENCODING_MASK = 0xffff0000u;

float3 EncodeVirtualGeometryReconstructionWeights(float3 sourceWeights)
{
	// Direct perspective weights avoid the unbounded 1/W amplification of a
	// quantized screen-affine representation when a source vertex has tiny W.
	return sourceWeights / max(dot(sourceWeights, float3(1.0f, 1.0f, 1.0f)), 1.0e-20f);
}

bool DecodeVirtualGeometryPerspectiveWeights(uint packedWeights, out float3 weights)
{
	const float2 xy = float2(packedWeights & 0xffffu, packedWeights >> 16u) / 65535.0f;
	weights = float3(xy, max(0.0f, 1.0f - xy.x - xy.y));
	// Independent rounding of X/Y can exceed one by one UNORM step. Normalize
	// that bounded case; larger sums are malformed and receive diagnostics.
	if (xy.x + xy.y > 1.0f + 2.0f / 65535.0f) return false;
	weights /= max(dot(weights, float3(1.0f, 1.0f, 1.0f)), 1.0e-20f);
	return all(isfinite(weights));
}

struct VirtualGeometryReconstruction
{
	float3 sourceWeights;
	float depth;
	bool valid;
};

VirtualGeometryReconstruction ReconstructVirtualGeometrySurface(uint4 record)
{
	VirtualGeometryReconstruction result = (VirtualGeometryReconstruction)0;
	if ((record.w & TE_VIRTUAL_GEOMETRY_VISIBILITY_ENCODING_MASK) != TE_VIRTUAL_GEOMETRY_VISIBILITY_ENCODING_TOKEN) return result;
	if (!DecodeVirtualGeometryPerspectiveWeights(record.y, result.sourceWeights)) return result;
	// This is the canonical device-authored depth winner, not a division by
	// quantized weights or by an original clip W which may be zero/negative.
	result.depth = float(record.z) / 4294967295.0f;
	result.valid = true; return result;
}
#endif
