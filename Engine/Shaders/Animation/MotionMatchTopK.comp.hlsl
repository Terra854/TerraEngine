// MotionMatchTopK.comp.hlsl - Bounded tiled top-K reduction over score records.

static const uint MOTION_FEATURE_BLOCK_COUNT = 9;
static const uint MOTION_MAX_TOP_K = 256;
static const uint MOTION_REDUCTION_TILE_SIZE = 1024;

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

cbuffer MotionMatchTopKDispatch : register(b0)
{
	uint gInputCandidateCount;
	uint gTopK;
	uint gTileSize;
	uint gOutputGroupCount;
	uint gExpectedDatabaseGeneration;
	uint gObservedDatabaseGeneration;
	uint gCancelled;
	uint gReserved;
	uint64_t gExpectedSchemaFingerprint;
	uint64_t gObservedSchemaFingerprint;
};

StructuredBuffer<CandidateCost> gInputCandidates : register(t0);
RWStructuredBuffer<CandidateCost> gOutputCandidates : register(u0);

bool Better(CandidateCost left, CandidateCost right)
{
	if(left.totalCost != right.totalCost) return left.totalCost < right.totalCost;
	if(left.sourceClipId != right.sourceClipId) return left.sourceClipId < right.sourceClipId;
	if(left.sourceTick != right.sourceTick) return left.sourceTick < right.sourceTick;
	return left.stableSampleId < right.stableSampleId;
}

[numthreads(1, 1, 1)]
void main(uint3 groupId : SV_GroupID)
{
	if(gCancelled != 0 || gReserved != 0 || gTopK == 0 || gTopK > MOTION_MAX_TOP_K
		|| gTileSize != MOTION_REDUCTION_TILE_SIZE || groupId.x >= gOutputGroupCount
		|| gExpectedDatabaseGeneration != gObservedDatabaseGeneration
		|| gExpectedSchemaFingerprint != gObservedSchemaFingerprint)
	{
		return;
	}

	// Each group reduces one fixed-size tile into a disjoint top-K range. The
	// backend inserts a buffer barrier and dispatches the recorded next pass until
	// one final range remains; no pass reads results written by its own dispatch.
	const uint firstInput = groupId.x * gTileSize;
	const uint endInput = min(firstInput + gTileSize, gInputCandidateCount);
	const uint firstOutput = groupId.x * gTopK;
	[loop]
	for(uint outputIndex = 0; outputIndex < gTopK; ++outputIndex)
	{
		CandidateCost best = (CandidateCost)0;
		best.totalCost = 0xffffffffffffffffULL;
		[loop]
		for(uint candidateIndex = firstInput; candidateIndex < endInput; ++candidateIndex)
		{
			const CandidateCost candidate = gInputCandidates[candidateIndex];
			if(candidate.stableSampleId == 0 || candidate.exclusionReasons != 0
				|| candidate.totalCost == 0xffffffffffffffffULL)
				continue;
			bool alreadySelected = false;
			[loop]
			for(uint previous = 0; previous < outputIndex; ++previous)
			{
				if(gOutputCandidates[firstOutput + previous].stableSampleId == candidate.stableSampleId)
				{
					alreadySelected = true;
					break;
				}
			}
			if(!alreadySelected && Better(candidate, best)) best = candidate;
		}
		gOutputCandidates[firstOutput + outputIndex] = best;
	}
}
