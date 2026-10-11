// Validation-only cooperative dispatch uses dedicated input/output descriptors.
// Coverage counts are incremented by actual texture loads, not rectangle sizes.
#define LIGHTING_DEPTH_COOPERATIVE 1
#define LIGHTING_DEPTH_ORACLE 1
#include "ClusterDepthReduction.hlsli"
