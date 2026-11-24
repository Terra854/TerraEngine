// Exposure ABI shared by the dedicated histogram, resolve and application passes.
// All values describe one exact view; never share history between cameras.
struct ExposureState
{
	float exposureEV;
	float targetEV;
	float meteredLogLuminance;
	uint valid;
};

void MeterRange(float requestedMinimum, float requestedMaximum, out float minimumLog, out float maximumLog)
{
	minimumLog = isfinite(requestedMinimum) ? clamp(requestedMinimum, -64.0f, 64.0f) : -12.0f;
	maximumLog = isfinite(requestedMaximum) ? clamp(requestedMaximum, -64.0f, 64.0f) : 16.0f;
	if(maximumLog <= minimumLog + 0.0001f)
	{
		minimumLog = -12.0f;
		maximumLog = 16.0f;
	}
}
