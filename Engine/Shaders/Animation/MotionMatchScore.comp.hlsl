// MotionMatchScore.comp.hlsl - Integer scoring over the frozen quantized schema.
// CPU remains authoritative; this kernel narrows resident candidates for async top-K.

static const uint MOTION_FEATURE_COUNT = 48;
static const uint MOTION_FEATURE_BLOCK_COUNT = 9;
static const uint MOTION_MAX_EXCLUDED_SAMPLES = 256;

static const uint EXCLUSION_MISSING_REQUIRED_TAGS = 1u << 0u;
static const uint EXCLUSION_CONTAINS_EXCLUDED_TAGS = 1u << 1u;
static const uint EXCLUSION_MISSING_REQUIRED_CONTACTS = 1u << 2u;
static const uint EXCLUSION_PHASE_OUTSIDE_WINDOW = 1u << 3u;
static const uint EXCLUSION_EXPLICIT_SAMPLE = 1u << 4u;
static const uint EXCLUSION_DATABASE_REGION = 1u << 5u;
static const uint EXCLUSION_MISSING_TRANSITION_FLAGS = 1u << 6u;
static const uint EXCLUSION_CONTAINS_TRANSITION_FLAGS = 1u << 7u;

struct MotionSample
{
	uint64_t stableSampleId;
	uint64_t sourceClipId;
	int64_t sourceTick;
	// The CPU asset stores int16 lanes. Upload expands them to int32 because the
	// supported SPIR-V profile does not require native 16-bit storage buffers.
	int features[MOTION_FEATURE_COUNT];
	uint64_t tagMask;
	uint contactMask;
	// phase occupies the low 16 bits and transition flags occupy the high 16.
	uint phaseAndFlags;
	uint continuityGroup;
	uint exclusionFlags;
	uint64_t continuityPredecessorId;
	uint64_t continuitySuccessorId;
};

// This exact 160-byte record mirrors MotionMatchGpuCandidateRecord in C++.
struct CandidateCost
{
	uint64_t stableSampleId;
	uint64_t sourceClipId;
	int64_t sourceTick;
	uint64_t featureBlockCosts[MOTION_FEATURE_BLOCK_COUNT];
	uint64_t featureCost;
	uint64_t phaseCost;
	uint64_t tagCost;
	uint64_t contactCost;
	uint64_t continuityCost;
	uint64_t transitionCost;
	uint64_t totalCost;
	uint exclusionReasons;
	uint reserved;
};

cbuffer MotionMatchDispatch : register(b0)
{
	uint gFirstSample;
	uint gSampleCount;
	uint gOutputOffset;
	uint gFeatureCount;
	uint gExpectedDatabaseGeneration;
	uint gObservedDatabaseGeneration;
	uint gCancelled;
	uint gMaximumPhaseDistance;
	uint64_t gExpectedSchemaFingerprint;
	uint64_t gObservedSchemaFingerprint;
	uint64_t gRequiredTags;
	uint64_t gExcludedTags;
	uint64_t gPreferredTags;
	uint64_t gLastSampleId;
	uint64_t gLastSourceClipId;
	uint gRequiredContacts;
	uint gPreferredContacts;
	uint gPreferredContinuityGroup;
	uint gExcludedRegionFlags;
	uint gPreferredPhase;
	uint gRequiredTransitionFlags;
	uint gExcludedTransitionFlags;
	uint gPreferredTransitionFlags;
	uint gPhaseWeight;
	uint gContinuityPenalty;
	uint gContinuityBreakPenalty;
	uint gSameClipPenalty;
	uint gMissingPreferredTagPenalty;
	uint gContactMismatchPenalty;
	uint gTransitionMismatchPenalty;
	uint gExcludedSampleCount;
};

StructuredBuffer<MotionSample> gSamples : register(t0);
StructuredBuffer<int> gQueryFeatures : register(t1);
StructuredBuffer<uint> gFeatureWeights : register(t2);
StructuredBuffer<uint64_t> gExcludedSampleIds : register(t3);
RWStructuredBuffer<CandidateCost> gCosts : register(u0);

uint FeatureBlockIndex(uint feature)
{
	if(feature < 12) return 0; // trajectory
	if(feature < 30) return 1; // pose
	if(feature < 39) return 2; // velocity
	if(feature < 42) return 3; // facing
	if(feature < 44) return 4; // contact
	if(feature < 45) return 5; // phase
	if(feature < 46) return 6; // tag
	if(feature < 47) return 7; // transition
	return 8;                  // continuity
}

uint PhaseDistance(uint left, uint right)
{
	const uint direct = left >= right ? left - right : right - left;
	return min(direct, 65536u - direct);
}

uint CountBits64(uint64_t value)
{
	return countbits(uint(value)) + countbits(uint(value >> 32u));
}

bool IsExplicitlyExcluded(uint64_t stableSampleId)
{
	// The CPU packet validates both this bound and sorted uniqueness before the
	// renderer receives it. The loop is nevertheless bounded in shader code.
	[loop]
	for(uint index = 0; index < min(gExcludedSampleCount, MOTION_MAX_EXCLUDED_SAMPLES); ++index)
	{
		if(gExcludedSampleIds[index] == stableSampleId) return true;
	}
	return false;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
	const uint localIndex = dispatchId.x;
	// Generation/cancellation checks happen before every write. An old dispatch
	// therefore cannot overwrite a newer request after hot reload or device loss.
	if(localIndex >= gSampleCount || gFeatureCount != MOTION_FEATURE_COUNT
		|| gExcludedSampleCount > MOTION_MAX_EXCLUDED_SAMPLES || gCancelled != 0
		|| gExpectedDatabaseGeneration != gObservedDatabaseGeneration
		|| gExpectedSchemaFingerprint != gObservedSchemaFingerprint)
	{
		return;
	}

	const MotionSample sample = gSamples[gFirstSample + localIndex];
	const uint phase = sample.phaseAndFlags & 0xffffu;
	const uint transitionFlags = sample.phaseAndFlags >> 16u;
	uint exclusionReasons = 0;
	if((sample.tagMask & gRequiredTags) != gRequiredTags)
		exclusionReasons |= EXCLUSION_MISSING_REQUIRED_TAGS;
	if((sample.tagMask & gExcludedTags) != 0)
		exclusionReasons |= EXCLUSION_CONTAINS_EXCLUDED_TAGS;
	if((sample.contactMask & gRequiredContacts) != gRequiredContacts)
		exclusionReasons |= EXCLUSION_MISSING_REQUIRED_CONTACTS;
	if(PhaseDistance(phase, gPreferredPhase) > gMaximumPhaseDistance)
		exclusionReasons |= EXCLUSION_PHASE_OUTSIDE_WINDOW;
	if(IsExplicitlyExcluded(sample.stableSampleId))
		exclusionReasons |= EXCLUSION_EXPLICIT_SAMPLE;
	if((sample.exclusionFlags & gExcludedRegionFlags) != 0)
		exclusionReasons |= EXCLUSION_DATABASE_REGION;
	if((transitionFlags & gRequiredTransitionFlags) != gRequiredTransitionFlags)
		exclusionReasons |= EXCLUSION_MISSING_TRANSITION_FLAGS;
	if((transitionFlags & gExcludedTransitionFlags) != 0)
		exclusionReasons |= EXCLUSION_CONTAINS_TRANSITION_FLAGS;

	CandidateCost output = (CandidateCost)0;
	output.stableSampleId = sample.stableSampleId;
	output.sourceClipId = sample.sourceClipId;
	output.sourceTick = sample.sourceTick;
	output.exclusionReasons = exclusionReasons;
	if(exclusionReasons != 0)
	{
		output.totalCost = 0xffffffffffffffffULL;
		gCosts[gOutputOffset + localIndex] = output;
		return;
	}

	[unroll]
	for(uint feature = 0; feature < MOTION_FEATURE_COUNT; ++feature)
	{
		const int delta = sample.features[feature] - gQueryFeatures[feature];
		const uint64_t cost = uint64_t(abs(delta)) * uint64_t(gFeatureWeights[feature]);
		output.featureBlockCosts[FeatureBlockIndex(feature)] += cost;
		output.featureCost += cost;
	}
	output.phaseCost = uint64_t(PhaseDistance(phase, gPreferredPhase)) * uint64_t(gPhaseWeight);
	output.tagCost = uint64_t(CountBits64(gPreferredTags & ~sample.tagMask))
		* uint64_t(gMissingPreferredTagPenalty);
	output.contactCost = uint64_t(countbits(gPreferredContacts ^ sample.contactMask))
		* uint64_t(gContactMismatchPenalty);
	if(gPreferredContinuityGroup != 0 && sample.continuityGroup != gPreferredContinuityGroup)
		output.continuityCost += uint64_t(gContinuityPenalty);
	if(gLastSampleId != 0 && sample.stableSampleId != gLastSampleId
		&& sample.continuityPredecessorId != gLastSampleId
		&& sample.continuitySuccessorId != gLastSampleId)
		output.continuityCost += uint64_t(gContinuityBreakPenalty);
	if(sample.stableSampleId != gLastSampleId && sample.sourceClipId == gLastSourceClipId)
		output.transitionCost += uint64_t(gSameClipPenalty);
	output.transitionCost += uint64_t(countbits((transitionFlags ^ gPreferredTransitionFlags) & 0xffffu))
		* uint64_t(gTransitionMismatchPenalty);
	output.totalCost = output.featureCost + output.phaseCost + output.tagCost
		+ output.contactCost + output.continuityCost + output.transitionCost;
	gCosts[gOutputOffset + localIndex] = output;
}
