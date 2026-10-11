// Production complete scan: dispatch 2x2 groups of 8x8 threads for 16x9 tiles.
#define LIGHTING_DEPTH_COOPERATIVE 0
#define LIGHTING_DEPTH_ORACLE 0
#include "ClusterDepthReduction.hlsli"
