// Keep integer object/primitive identity in the same coverage sample as
// the hardware depth resolve. Averaging visibility IDs would corrupt picking.
[[vk::binding(0, 0)]] Texture2DMS<uint4, 4> rasterIdentity;
[[vk::binding(1, 0)]] [[vk::image_format("rgba32ui")]] RWTexture2D<uint4> resolvedIdentity;

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
	uint width, height;
	resolvedIdentity.GetDimensions(width, height);
	if (dispatchId.x >= width || dispatchId.y >= height) return;
	resolvedIdentity[dispatchId.xy] = rasterIdentity.Load(int2(dispatchId.xy), 0);
}
