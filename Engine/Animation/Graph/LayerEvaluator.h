// LayerEvaluator.h - Ordered override and additive copied-pose composition.

#pragma once

#include "Animation/Assets/AnimationMaskAsset.h"
#include "Animation/Graph/GraphEvaluationTypes.h"

#include <span>

namespace TerraEngine::Animation {
// Explicit values are retained in worker packets and mirrored by GPU lowering.
enum class AnimationGraphLayerMode : std::uint8_t {
	Override = 0U,
	Additive = 1U
};

// Every pointer refers to immutable data whose lifetime covers this call. The
// three controls remain separate so controller, node, and authored layer weight
// changes can be inspected even though their effective weight is a product.
struct GraphLayerInput {
	std::uint64_t stableId = 0U;
	AnimationGraphLayerMode mode = AnimationGraphLayerMode::Override;
	float controllerWeight = 1.0F;
	float nodeWeight = 1.0F;
	float layerWeight = 1.0F;
	const AnimationMaskAsset *mask = nullptr;
	const GraphPoseContribution *contribution = nullptr;
	const GraphPoseContribution *additiveReference = nullptr;
};

// Layers are composed in the supplied authored order. The caller's output is
// replaced only after every layer and the complete staged packet validate.
TERRAENGINE_API AnimationStatus EvaluateAnimationLayers(const GraphPoseContribution &base,
	std::span<const GraphLayerInput> layers, GraphPoseContribution &output);
} // namespace TerraEngine::Animation
