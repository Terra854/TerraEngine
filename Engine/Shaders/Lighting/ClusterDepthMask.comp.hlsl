// Production cooperative reduction: dispatch exactly 16x9 groups of 64 lanes.
// Resources retain the existing global descriptor set and float2 bounds ABI.
#define LIGHTING_DEPTH_COOPERATIVE 1
#define LIGHTING_DEPTH_ORACLE 0
#include "ClusterDepthReduction.hlsli"
