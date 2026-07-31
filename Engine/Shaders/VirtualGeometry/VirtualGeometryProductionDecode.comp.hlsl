// VirtualGeometryProductionDecode.comp.hlsl
//
// Bounded device decode for upload-ready ExpandedProduction blocks. Compressed
// meshoptimizer profiles stay capability-gated until their dedicated device
// kernels execute and pass readback parity; the shader reports unsupported
// instead of pretending that a processor fallback was device execution.

#include "VirtualGeometryProductionCodecAbi.hlsli"

[[vk::binding(0, 0)]] StructuredBuffer<VirtualGeometryProductionDecodeRequest>
	DecodeRequests;
[[vk::binding(1, 0)]] ByteAddressBuffer EncodedBytes;
[[vk::binding(2, 0)]] RWByteAddressBuffer DecodedBytes;
[[vk::binding(3, 0)]] RWStructuredBuffer<VirtualGeometryProductionDecodeResult>
	DecodeResults;

[[vk::push_constant]] struct ProductionDecodeConstants
{
	uint requestIndex;
	uint encodedCapacityBytes;
	uint decodedCapacityBytes;
	uint reserved;
} Constants;

[numthreads(VG_DECODE_WORKGROUP_SIZE, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const VirtualGeometryProductionDecodeRequest request =
		DecodeRequests[Constants.requestIndex];
	if (dispatchThreadId.x == 0u)
	{
		DecodeResults[Constants.requestIndex].requestIdentity =
			request.requestIdentity;
		DecodeResults[Constants.requestIndex].status =
			VG_DECODE_STATUS_PENDING;
		DecodeResults[Constants.requestIndex].decodedByteCount = 0u;
		DecodeResults[Constants.requestIndex].reserved = 0u;
	}

	if (request.profileId != VG_CODEC_EXPANDED_PRODUCTION)
	{
		if (dispatchThreadId.x == 0u)
		{
			DecodeResults[Constants.requestIndex].status =
				VG_DECODE_STATUS_UNSUPPORTED_PROFILE;
		}
		return;
	}

	const bool invalidSize = request.encodedByteCount
			!= request.decodedByteCount
		|| request.decodedByteCount == 0u
		|| (request.decodedByteCount & 3u) != 0u
		|| request.decodedByteCount
			> VG_DECODE_MAXIMUM_BYTES_PER_DISPATCH;
	const bool invalidInput = request.encodedByteOffset
			> Constants.encodedCapacityBytes
		|| request.encodedByteCount
			> Constants.encodedCapacityBytes - request.encodedByteOffset;
	const bool invalidOutput = request.decodedByteOffset
			> min(Constants.decodedCapacityBytes, request.outputCapacityBytes)
		|| request.decodedByteCount
			> min(Constants.decodedCapacityBytes, request.outputCapacityBytes)
				- request.decodedByteOffset;
	if (invalidSize || invalidInput || invalidOutput)
	{
		if (dispatchThreadId.x == 0u)
		{
			DecodeResults[Constants.requestIndex].status =
				invalidOutput
				? VG_DECODE_STATUS_OUTPUT_OVERFLOW
				: VG_DECODE_STATUS_INVALID_RANGE;
		}
		return;
	}

	// Copy complete words in parallel. Production device-decode candidates are
	// word padded; a non-word-aligned block remains on the processor decoder
	// instead of risking an out-of-range ByteAddressBuffer access.
	for (uint byteIndex = dispatchThreadId.x * 4u;
		byteIndex < request.decodedByteCount;
		byteIndex += VG_DECODE_WORKGROUP_SIZE * 4u)
	{
		const uint value = EncodedBytes.Load(
			request.encodedByteOffset + byteIndex);
		DecodedBytes.Store(
			request.decodedByteOffset + byteIndex,
			value);
	}

	if (dispatchThreadId.x == 0u)
	{
		DecodeResults[Constants.requestIndex].status =
			VG_DECODE_STATUS_COMPLETE;
		DecodeResults[Constants.requestIndex].decodedByteCount =
			request.decodedByteCount;
	}
}
