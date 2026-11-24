// SDR GT7 operator from Yasutomi, Suzuki and Uchimura, SIGGRAPH 2025.
// https://s3.amazonaws.com/gran-turismo.com/pdi_publications/s2025_PBS_Physically_Based_Tone_Mapping_GT7.pdf
// The camera and SDR compositor use linear Rec.709. The operator works in
// linear Rec.2020, with BT.2100 PQ ICtCp used only to recover highlight chroma.
static const float kGT7PaperWhite = 2.5f; // 250 nits at 100 nits per scene-linear unit.
static const float kGT7PqPaperWhite = 0.602559155f;
static const float kGT7PqM1 = 2610.0f / 16384.0f;
static const float kGT7PqM2 = 2523.0f / 32.0f;
static const float kGT7PqC1 = 3424.0f / 4096.0f;
static const float kGT7PqC2 = 2413.0f / 128.0f;
static const float kGT7PqC3 = 2392.0f / 128.0f;

float3 GT7EncodeLms(float3 rec2020)
{
	const float3x3 toLms = float3x3(1688, 2146, 262, 683, 2951, 462, 99, 309, 3688) / 4096.0f;
	// Normalize scene units to PQ's 10,000-nit domain.
	const float3 power = pow(saturate(mul(toLms, rec2020) * 0.01f), kGT7PqM1);
	return pow((kGT7PqC1 + kGT7PqC2 * power) / (1.0f + kGT7PqC3 * power), kGT7PqM2);
}

float3 GT7DecodeLms(float3 pqLms)
{
	const float3x3 fromLms = float3x3(
		3.436606694f, -2.506452119f, 0.069845424f,
		-0.791329556f, 1.983600452f, -0.192270896f,
		-0.025949900f, -0.098913715f, 1.124863614f);
	// Mixed intensity and chroma can leave the representable PQ domain.
	const float3 power = pow(saturate(pqLms), 1.0f / kGT7PqM2);
	const float3 lms = pow(max(power - kGT7PqC1, 0.0f) / (kGT7PqC2 - kGT7PqC3 * power), 1.0f / kGT7PqM1);
	return mul(fromLms, lms * 100.0f);
}

float GT7Curve(float value)
{
	const float midPoint = 0.538f;
	const float linearSection = 0.444f;
	const float toeStrength = 1.28f;
	const float shoulderSlope = 0.25f;
	const float knee = kGT7PaperWhite * linearSection;
	const float shoulderRange = kGT7PaperWhite * (1.0f - linearSection) / (1.0f - shoulderSlope);
	// The exponential joins the linear section with the same value and slope.
	if(value >= knee) return knee + shoulderRange * (1.0f - exp((knee - value) / shoulderRange));
	if(value >= midPoint) return value;
	const float toe = midPoint * pow(value / midPoint, toeStrength);
	return lerp(toe, value, smoothstep(0.0f, midPoint, value));
}

float3 ToneGT7(float3 radiance)
{
	const float3x3 to2020 = float3x3(
		0.627403896f, 0.329283038f, 0.043313066f,
		0.069097289f, 0.919540395f, 0.011362316f,
		0.016391439f, 0.088013308f, 0.895595253f);
	const float3x3 to709 = float3x3(
		1.660491002f, -0.587641139f, -0.072849863f,
		-0.124550475f, 1.132899897f, -0.008349423f,
		-0.018150763f, -0.100578898f, 1.118729661f);
	const float3 rec2020 = mul(to2020, radiance);
	const float3 curved = float3(GT7Curve(rec2020.r), GT7Curve(rec2020.g), GT7Curve(rec2020.b));
	const float3 sourceLms = GT7EncodeLms(rec2020);
	const float3 curvedLms = GT7EncodeLms(curved);
	const float sourceIntensity = (sourceLms.x + sourceLms.y) * 0.5f;
	const float mappedIntensity = (curvedLms.x + curvedLms.y) * 0.5f;

	// Keep the mapped intensity, recover the source ICtCp chroma, and fade
	// it above paper white before blending back with the channel curve.
	const float chroma = 1.0f - smoothstep(0.98f, 1.16f, sourceIntensity / kGT7PqPaperWhite);
	const float ct = dot(sourceLms, float3(6610, -13613, 7003) / 4096.0f) * chroma;
	const float cp = dot(sourceLms, float3(17933, -17390, -543) / 4096.0f) * chroma;
	const float coneDelta = 0.008609037f * ct + 0.111029625f * cp;
	const float3 recovered = GT7DecodeLms(float3(mappedIntensity + coneDelta, mappedIntensity - coneDelta,
		mappedIntensity + 0.560031336f * ct - 0.320627175f * cp));
	const float3 display2020 = min(lerp(curved, recovered, 0.6f), kGT7PaperWhite) / kGT7PaperWhite;
	return saturate(mul(to709, display2020));
}
