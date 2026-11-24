#include "ExposureCommon.hlsli"
[[vk::binding(1, 0)]] StructuredBuffer<uint> histogram;
[[vk::binding(2, 0)]] StructuredBuffer<ExposureState> previousExposure;
[[vk::binding(3, 0)]] RWStructuredBuffer<ExposureState> nextExposure;
struct ResolveConstants
{
	float minimumLogLuminance;
	float maximumLogLuminance;
	float minimumExposureEV;
	float maximumExposureEV;
	float lowPercentile;
	float highPercentile;
	float middleGrey;
	float deltaSeconds;
	float brightenRatePerSecond;
	float darkenRatePerSecond;
	uint automaticExposureEnabled;
	uint resetHistory;
};
[[vk::push_constant]] ConstantBuffer<ResolveConstants> parameters;
groupshared uint prefixCounts[256];
groupshared float retainedCounts[256];
groupshared float weightedLogLuminance[256];

[numthreads(256, 1, 1)]
void ResolveExposure(uint lane : SV_GroupIndex)
{
	const uint count = lane == 0u ? 0u : histogram[lane];
	prefixCounts[lane] = count;
	GroupMemoryBarrierWithGroupSync();
	// Inclusive parallel scan gives each bin's rank interval. Reading the
	// preceding scan before the barrier avoids a read/write race in a wave.
	for(uint offset = 1u; offset < 256u; offset <<= 1u)
	{
		const uint preceding = lane >= offset ? prefixCounts[lane - offset] : 0u;
		GroupMemoryBarrierWithGroupSync();
		prefixCounts[lane] += preceding;
		GroupMemoryBarrierWithGroupSync();
	}
	const float total = (float)prefixCounts[255];
	float low = isfinite(parameters.lowPercentile) ? saturate(parameters.lowPercentile) : 0.02f;
	float high = isfinite(parameters.highPercentile) ? saturate(parameters.highPercentile) : 0.98f;
	if(high <= low) { low = 0.02f; high = 0.98f; }
	const float lowerRank = total * low;
	const float upperRank = total * high;
	const float binEnd = (float)prefixCounts[lane];
	const float binBegin = (float)(prefixCounts[lane] - count);
	// Fractional end bins avoid sudden quantile pops as one sample crosses a
	// percentile. The high and low tails cannot dominate a bright-window scene.
	const float retained = max(0.0f, min(binEnd, upperRank) - max(binBegin, lowerRank));
	float minimumLog, maximumLog;
	MeterRange(parameters.minimumLogLuminance, parameters.maximumLogLuminance, minimumLog, maximumLog);
	const float centerLog = minimumLog + ((max(1u, lane) - 1u + 0.5f) / 255.0f) * (maximumLog - minimumLog);
	retainedCounts[lane] = retained;
	weightedLogLuminance[lane] = retained * centerLog;
	GroupMemoryBarrierWithGroupSync();
	for(uint offset = 128u; offset > 0u; offset >>= 1u)
	{
		if(lane < offset)
		{
			retainedCounts[lane] += retainedCounts[lane + offset];
			weightedLogLuminance[lane] += weightedLogLuminance[lane + offset];
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if(lane != 0u) return;
	ExposureState state = (ExposureState)0;
	if(parameters.automaticExposureEnabled == 0u)
	{
		// Disabling auto invalidates its history. Re-enabling cannot adopt an old
		// camera's adapted EV; fixed compensation remains a separate application.
		nextExposure[0] = state;
		return;
	}
	const float minimumEV = isfinite(parameters.minimumExposureEV) ? clamp(parameters.minimumExposureEV, -24.0f, 24.0f) : -12.0f;
	const float requestedMaximum = isfinite(parameters.maximumExposureEV) ? clamp(parameters.maximumExposureEV, -24.0f, 24.0f) : 12.0f;
	const float maximumEV = max(minimumEV, requestedMaximum);
	bool previousValid = false;
	if(parameters.resetHistory == 0u)
	{
		state = previousExposure[0];
		previousValid = state.valid == 1u && isfinite(state.exposureEV) && isfinite(state.targetEV) && isfinite(state.meteredLogLuminance);
	}
	if(retainedCounts[0] <= 0.0f)
	{
		// A wholly black/invalid image provides no useful target. Hold an existing
		// valid exposure, otherwise keep invalid EV0 until a useful meter exists.
		if(previousValid) { state.exposureEV = clamp(state.exposureEV, minimumEV, maximumEV); state.targetEV = clamp(state.targetEV, minimumEV, maximumEV); }
		else state = (ExposureState)0;
		nextExposure[0] = state;
		return;
	}
	const float averageLog = weightedLogLuminance[0] / retainedCounts[0];
	const float middleGrey = isfinite(parameters.middleGrey) && parameters.middleGrey > 0.0f ? clamp(parameters.middleGrey, 0.000001f, 100.0f) : 0.18f;
	const float targetEV = clamp(log2(middleGrey) - averageLog, minimumEV, maximumEV);
	float adaptedEV = targetEV;
	if(previousValid)
	{
		const float previousEV = clamp(state.exposureEV, minimumEV, maximumEV);
		// Higher EV brightens the displayed image. These rates use1/seconds,
		// independently controlling image brightening and image darkening.
		const float requestedRate = targetEV > previousEV ? parameters.brightenRatePerSecond : parameters.darkenRatePerSecond;
		const float rate = isfinite(requestedRate) ? max(0.0f, requestedRate) : 0.0f;
		const float delta = isfinite(parameters.deltaSeconds) ? max(0.0f, parameters.deltaSeconds) : 0.0f;
		const float fraction = 1.0f - exp(-rate * delta);
		adaptedEV = lerp(previousEV, targetEV, fraction);
	}
	state.exposureEV = clamp(adaptedEV, minimumEV, maximumEV);
	state.targetEV = targetEV;
	state.meteredLogLuminance = averageLog;
	state.valid = 1u;
	nextExposure[0] = state;
}
