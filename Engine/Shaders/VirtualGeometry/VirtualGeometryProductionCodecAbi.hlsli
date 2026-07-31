// VirtualGeometryProductionCodecAbi.hlsli
//
// Shared Vulkan decoder ABI for canonical TEMF v1 blocks. The processor and
// device paths use the same profile IDs, size limits, and completion statuses.
// Device capability remains unavailable until a real dispatch completes and
// its decoded hash matches the processor reference.

#ifndef TERRAENGINE_VIRTUAL_GEOMETRY_PRODUCTION_CODEC_ABI
#define TERRAENGINE_VIRTUAL_GEOMETRY_PRODUCTION_CODEC_ABI

static const uint VG_CODEC_EXPANDED_PRODUCTION = 1u;
static const uint VG_CODEC_MESHOPT_STATIC = 2u;
static const uint VG_CODEC_MESHOPT_UPDATEABLE = 3u;

static const uint VG_DECODE_STATUS_PENDING = 0u;
static const uint VG_DECODE_STATUS_COMPLETE = 1u;
static const uint VG_DECODE_STATUS_UNSUPPORTED_PROFILE = 2u;
static const uint VG_DECODE_STATUS_INVALID_RANGE = 3u;
static const uint VG_DECODE_STATUS_OUTPUT_OVERFLOW = 4u;

static const uint VG_DECODE_MAXIMUM_BYTES_PER_DISPATCH = 262144u;
static const uint VG_DECODE_WORKGROUP_SIZE = 64u;

struct VirtualGeometryProductionDecodeRequest
{
	uint profileId;
	uint codecVersion;
	uint encodedByteOffset;
	uint encodedByteCount;
	uint decodedByteOffset;
	uint decodedByteCount;
	uint outputCapacityBytes;
	uint requestIdentity;
};

struct VirtualGeometryProductionDecodeResult
{
	uint requestIdentity;
	uint status;
	uint decodedByteCount;
	uint reserved;
};

#endif
