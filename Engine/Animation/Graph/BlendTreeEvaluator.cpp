// BlendTreeEvaluator.cpp - Stable copied-pose interpolation for blend trees.

#include "Animation/Graph/BlendTreeEvaluator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <new>
#include <vector>

namespace TerraEngine::Animation {
namespace {
constexpr double kWeightEpsilon = 1.0e-12;

struct WeightedContribution {
	const GraphPoseContribution *contribution = nullptr;
	double weight = 0.0;
	std::uint64_t stableId = 0U;
};

struct BlendPoint {
	double x = 0.0;
	double y = 0.0;
};

struct TriangleWeights {
	double a = 0.0;
	double b = 0.0;
	double c = 0.0;
};

bool IsFiniteFloat(float value) noexcept {
	return std::isfinite(value);
}

bool StoreFiniteFloat(double value, float &output) noexcept {
	if (!std::isfinite(value) || value > static_cast<double>(std::numeric_limits<float>::max()) ||
		value < -static_cast<double>(std::numeric_limits<float>::max())) {
		return false;
	}
	output = static_cast<float>(value);
	return std::isfinite(output);
}

double QuaternionDot(const AnimationQuaternion &left, const AnimationQuaternion &right) noexcept {
	return static_cast<double>(left.x) * right.x + static_cast<double>(left.y) * right.y +
		static_cast<double>(left.z) * right.z + static_cast<double>(left.w) * right.w;
}

AnimationStatus ValidateCompatibleContribution(
	const GraphPoseContribution &reference,
	const GraphPoseContribution &candidate) noexcept {
	const AnimationStatus candidateStatus = ValidateGraphPoseContribution(candidate);
	if (candidateStatus != AnimationStatus::Ok) {
		return candidateStatus;
	}
	if (candidate.compatibilityFingerprint != reference.compatibilityFingerprint ||
		candidate.jointCount != reference.jointCount || candidate.jointStableIds != reference.jointStableIds) {
		return AnimationStatus::IncompatibleAsset;
	}
	if (candidate.rootMotion.has_value() != reference.rootMotion.has_value()) {
		return AnimationStatus::InvalidArgument;
	}
	return AnimationStatus::Ok;
}

AnimationStatus ValidateWeightedContributions(std::span<const WeightedContribution> values, double &weightSum) noexcept {
	if (values.empty() || values.front().contribution == nullptr) {
		return AnimationStatus::InvalidArgument;
	}
	const AnimationStatus firstStatus = ValidateGraphPoseContribution(*values.front().contribution);
	if (firstStatus != AnimationStatus::Ok) {
		return firstStatus;
	}
	weightSum = 0.0;
	for (const WeightedContribution &value : values) {
		if (value.contribution == nullptr || value.stableId == 0U || !std::isfinite(value.weight)) {
			return AnimationStatus::InvalidArgument;
		}
		const AnimationStatus compatibilityStatus =
			ValidateCompatibleContribution(*values.front().contribution, *value.contribution);
		if (compatibilityStatus != AnimationStatus::Ok) {
			return compatibilityStatus;
		}
		weightSum += value.weight;
	}
	if (!std::isfinite(weightSum) || std::abs(weightSum) <= kWeightEpsilon) {
		return AnimationStatus::RangeOverflow;
	}
	return AnimationStatus::Ok;
}

AnimationStatus BlendNamedValues(
	std::span<const WeightedContribution> values,
	bool morphValues,
	double weightSum,
	std::vector<AnimationNamedFloatValue> &output) {
	std::map<std::uint64_t, double> accumulated;
	const AnimationLimit limitKind = morphValues ? AnimationLimit::MeshMorphTargets : AnimationLimit::ClipChannels;
	const std::size_t hardMaximum = static_cast<std::size_t>(GetAnimationRuntimeLimit(limitKind).hardMaximum);

	for (const WeightedContribution &weighted : values) {
		const auto &source = morphValues ? weighted.contribution->morphs : weighted.contribution->curves;
		for (const AnimationNamedFloatValue &entry : source) {
			auto [iterator, inserted] = accumulated.try_emplace(entry.stableId, 0.0);
			if (inserted && accumulated.size() > hardMaximum) {
				return AnimationStatus::HardLimitExceeded;
			}
			iterator->second += weighted.weight * static_cast<double>(entry.value);
		}
	}

	output.clear();
	output.reserve(accumulated.size());
	for (const auto &[stableId, weightedValue] : accumulated) {
		float value = 0.0F;
		if (!StoreFiniteFloat(weightedValue / weightSum, value)) {
			return AnimationStatus::RangeOverflow;
		}
		output.push_back({stableId, value});
	}
	return AnimationStatus::Ok;
}

AnimationStatus BlendWeightedContributions(
	std::span<const WeightedContribution> values,
	GraphPoseContribution &output) {
	double weightSum = 0.0;
	const AnimationStatus validationStatus = ValidateWeightedContributions(values, weightSum);
	if (validationStatus != AnimationStatus::Ok) {
		return validationStatus;
	}

	std::size_t dominantIndex = 0U;
	for (std::size_t index = 1U; index < values.size(); ++index) {
		const double candidateMagnitude = std::abs(values[index].weight);
		const double dominantMagnitude = std::abs(values[dominantIndex].weight);
		if (candidateMagnitude > dominantMagnitude ||
			(candidateMagnitude == dominantMagnitude && values[index].stableId < values[dominantIndex].stableId)) {
			dominantIndex = index;
		}
	}

	GraphPoseContribution staged = *values.front().contribution;
	for (std::size_t jointIndex = 0U; jointIndex < staged.joints.size(); ++jointIndex) {
		double translationX = 0.0;
		double translationY = 0.0;
		double translationZ = 0.0;
		double scaleX = 0.0;
		double scaleY = 0.0;
		double scaleZ = 0.0;
		double rotationX = 0.0;
		double rotationY = 0.0;
		double rotationZ = 0.0;
		double rotationW = 0.0;
		const AnimationQuaternion &signReference = values[dominantIndex].contribution->joints[jointIndex].rotation;

		for (const WeightedContribution &weighted : values) {
			const AnimationTransform &source = weighted.contribution->joints[jointIndex];
			const double normalizedWeight = weighted.weight / weightSum;
			translationX += normalizedWeight * source.translation.x;
			translationY += normalizedWeight * source.translation.y;
			translationZ += normalizedWeight * source.translation.z;
			scaleX += normalizedWeight * source.scale.x;
			scaleY += normalizedWeight * source.scale.y;
			scaleZ += normalizedWeight * source.scale.z;

			const double sign = QuaternionDot(signReference, source.rotation) < 0.0 ? -1.0 : 1.0;
			rotationX += normalizedWeight * sign * source.rotation.x;
			rotationY += normalizedWeight * sign * source.rotation.y;
			rotationZ += normalizedWeight * sign * source.rotation.z;
			rotationW += normalizedWeight * sign * source.rotation.w;
		}

		const double rotationLength = std::sqrt(
			rotationX * rotationX + rotationY * rotationY + rotationZ * rotationZ + rotationW * rotationW);
		if (!std::isfinite(rotationLength) || rotationLength <= kWeightEpsilon) {
			return AnimationStatus::RangeOverflow;
		}
		rotationX /= rotationLength;
		rotationY /= rotationLength;
		rotationZ /= rotationLength;
		rotationW /= rotationLength;
		if (rotationW < 0.0) {
			rotationX = -rotationX;
			rotationY = -rotationY;
			rotationZ = -rotationZ;
			rotationW = -rotationW;
		}

		AnimationTransform &target = staged.joints[jointIndex];
		if (!StoreFiniteFloat(translationX, target.translation.x) ||
			!StoreFiniteFloat(translationY, target.translation.y) ||
			!StoreFiniteFloat(translationZ, target.translation.z) || !StoreFiniteFloat(scaleX, target.scale.x) ||
			!StoreFiniteFloat(scaleY, target.scale.y) || !StoreFiniteFloat(scaleZ, target.scale.z) ||
			!StoreFiniteFloat(rotationX, target.rotation.x) || !StoreFiniteFloat(rotationY, target.rotation.y) ||
			!StoreFiniteFloat(rotationZ, target.rotation.z) || !StoreFiniteFloat(rotationW, target.rotation.w)) {
			return AnimationStatus::RangeOverflow;
		}
	}

	AnimationStatus namedStatus = BlendNamedValues(values, true, weightSum, staged.morphs);
	if (namedStatus != AnimationStatus::Ok) {
		return namedStatus;
	}
	namedStatus = BlendNamedValues(values, false, weightSum, staged.curves);
	if (namedStatus != AnimationStatus::Ok) {
		return namedStatus;
	}

	if (staged.rootMotion.has_value()) {
		staged.rootMotion = values[dominantIndex].contribution->rootMotion;
		double localX = 0.0;
		double localY = 0.0;
		double localZ = 0.0;
		double worldX = 0.0;
		double worldY = 0.0;
		double worldZ = 0.0;
		double localYaw = 0.0;
		double worldYaw = 0.0;
		for (const WeightedContribution &weighted : values) {
			const AnimationRootMotionDelta &source = *weighted.contribution->rootMotion;
			const double normalizedWeight = weighted.weight / weightSum;
			localX += normalizedWeight * source.localTranslation.x;
			localY += normalizedWeight * source.localTranslation.y;
			localZ += normalizedWeight * source.localTranslation.z;
			worldX += normalizedWeight * source.worldTranslation.x;
			worldY += normalizedWeight * source.worldTranslation.y;
			worldZ += normalizedWeight * source.worldTranslation.z;
			localYaw += normalizedWeight * source.localYawRadians;
			worldYaw += normalizedWeight * source.worldYawRadians;
		}
		AnimationRootMotionDelta &target = *staged.rootMotion;
		if (!StoreFiniteFloat(localX, target.localTranslation.x) || !StoreFiniteFloat(localY, target.localTranslation.y) ||
			!StoreFiniteFloat(localZ, target.localTranslation.z) || !StoreFiniteFloat(worldX, target.worldTranslation.x) ||
			!StoreFiniteFloat(worldY, target.worldTranslation.y) || !StoreFiniteFloat(worldZ, target.worldTranslation.z) ||
			!StoreFiniteFloat(localYaw, target.localYawRadians) || !StoreFiniteFloat(worldYaw, target.worldYawRadians)) {
			return AnimationStatus::RangeOverflow;
		}
	}

	const AnimationStatus outputStatus = ValidateGraphPoseContribution(staged);
	if (outputStatus != AnimationStatus::Ok) {
		return AnimationStatus::RangeOverflow;
	}
	output = std::move(staged);
	return AnimationStatus::Ok;
}

AnimationStatus ValidateStandaloneSamples(std::span<const GraphBlendSample> samples, bool twoDimensional) {
	if (samples.empty()) {
		return AnimationStatus::InvalidArgument;
	}
	const AnimationStatus sampleLimit = ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, samples.size());
	if (sampleLimit != AnimationStatus::Ok) {
		return sampleLimit;
	}

	std::vector<const GraphBlendSample *> byStableId;
	std::vector<const GraphBlendSample *> byPosition;
	byStableId.reserve(samples.size());
	byPosition.reserve(samples.size());
	for (const GraphBlendSample &sample : samples) {
		if (sample.stableId == 0U || !IsFiniteFloat(sample.positionX) ||
			(twoDimensional && !IsFiniteFloat(sample.positionY))) {
			return AnimationStatus::InvalidArgument;
		}
		const AnimationStatus contributionStatus =
			ValidateCompatibleContribution(samples.front().contribution, sample.contribution);
		if (contributionStatus != AnimationStatus::Ok) {
			return contributionStatus;
		}
		byStableId.push_back(&sample);
		byPosition.push_back(&sample);
	}
	std::sort(byStableId.begin(), byStableId.end(), [](const auto *left, const auto *right) {
		return left->stableId < right->stableId;
	});
	if (std::adjacent_find(byStableId.begin(), byStableId.end(), [](const auto *left, const auto *right) {
			return left->stableId == right->stableId;
		}) != byStableId.end()) {
		return AnimationStatus::InvalidArgument;
	}
	std::sort(byPosition.begin(), byPosition.end(), [twoDimensional](const auto *left, const auto *right) {
		if (left->positionX != right->positionX) {
			return left->positionX < right->positionX;
		}
		if (twoDimensional && left->positionY != right->positionY) {
			return left->positionY < right->positionY;
		}
		return left->stableId < right->stableId;
	});
	if (std::adjacent_find(byPosition.begin(), byPosition.end(), [twoDimensional](const auto *left, const auto *right) {
			return left->positionX == right->positionX &&
			(!twoDimensional || left->positionY == right->positionY);
		}) != byPosition.end()) {
		return AnimationStatus::InvalidArgument;
	}
	return AnimationStatus::Ok;
}

TriangleWeights ComputeBarycentric(
	const BlendPoint &point,
	const BlendPoint &a,
	const BlendPoint &b,
	const BlendPoint &c) noexcept {
	const double determinant = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
	const double weightA = ((b.y - c.y) * (point.x - c.x) + (c.x - b.x) * (point.y - c.y)) / determinant;
	const double weightB = ((c.y - a.y) * (point.x - c.x) + (a.x - c.x) * (point.y - c.y)) / determinant;
	return {weightA, weightB, 1.0 - weightA - weightB};
}

// Return barycentric weights for the nearest point on a triangle. This is the
// clamp policy's geometric boundary rule and avoids bounding-box corner bias.
TriangleWeights ComputeClosestTriangleWeights(
	const BlendPoint &point,
	const BlendPoint &a,
	const BlendPoint &b,
	const BlendPoint &c) noexcept {
	const BlendPoint ab{b.x - a.x, b.y - a.y};
	const BlendPoint ac{c.x - a.x, c.y - a.y};
	const BlendPoint ap{point.x - a.x, point.y - a.y};
	const double d1 = ab.x * ap.x + ab.y * ap.y;
	const double d2 = ac.x * ap.x + ac.y * ap.y;
	if (d1 <= 0.0 && d2 <= 0.0) {
		return {1.0, 0.0, 0.0};
	}

	const BlendPoint bp{point.x - b.x, point.y - b.y};
	const double d3 = ab.x * bp.x + ab.y * bp.y;
	const double d4 = ac.x * bp.x + ac.y * bp.y;
	if (d3 >= 0.0 && d4 <= d3) {
		return {0.0, 1.0, 0.0};
	}

	const double vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
		const double edgeWeight = d1 / (d1 - d3);
		return {1.0 - edgeWeight, edgeWeight, 0.0};
	}

	const BlendPoint cp{point.x - c.x, point.y - c.y};
	const double d5 = ab.x * cp.x + ab.y * cp.y;
	const double d6 = ac.x * cp.x + ac.y * cp.y;
	if (d6 >= 0.0 && d5 <= d6) {
		return {0.0, 0.0, 1.0};
	}

	const double vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
		const double edgeWeight = d2 / (d2 - d6);
		return {1.0 - edgeWeight, 0.0, edgeWeight};
	}

	const double va = d3 * d6 - d5 * d4;
	if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
		const double edgeWeight = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		return {0.0, 1.0 - edgeWeight, edgeWeight};
	}

	const double denominator = 1.0 / (va + vb + vc);
	const double weightB = vb * denominator;
	const double weightC = vc * denominator;
	return {1.0 - weightB - weightC, weightB, weightC};
}

AnimationStatus BlendCollinear2D(
	std::span<const GraphBlendSample> samples,
	float parameterX,
	float parameterY,
	AnimationBlendOutOfDomainPolicy policy,
	GraphPoseContribution &output) {
	float minimumX = samples.front().positionX;
	float maximumX = minimumX;
	float minimumY = samples.front().positionY;
	float maximumY = minimumY;
	for (const GraphBlendSample &sample : samples) {
		minimumX = std::min(minimumX, sample.positionX);
		maximumX = std::max(maximumX, sample.positionX);
		minimumY = std::min(minimumY, sample.positionY);
		maximumY = std::max(maximumY, sample.positionY);
	}
	const bool useX = static_cast<double>(maximumX) - minimumX >= static_cast<double>(maximumY) - minimumY;
	std::vector<const GraphBlendSample *> ordered;
	ordered.reserve(samples.size());
	for (const GraphBlendSample &sample : samples) {
		ordered.push_back(&sample);
	}
	std::sort(ordered.begin(), ordered.end(), [useX](const auto *left, const auto *right) {
		const float leftCoordinate = useX ? left->positionX : left->positionY;
		const float rightCoordinate = useX ? right->positionX : right->positionY;
		return leftCoordinate != rightCoordinate ? leftCoordinate < rightCoordinate : left->stableId < right->stableId;
	});

	double coordinate = useX ? parameterX : parameterY;
	const double minimum = useX ? ordered.front()->positionX : ordered.front()->positionY;
	const double maximum = useX ? ordered.back()->positionX : ordered.back()->positionY;
	if (policy == AnimationBlendOutOfDomainPolicy::Clamp) {
		coordinate = std::clamp(coordinate, minimum, maximum);
	}
	std::size_t rightIndex = 1U;
	while (rightIndex + 1U < ordered.size() &&
		coordinate > static_cast<double>(useX ? ordered[rightIndex]->positionX : ordered[rightIndex]->positionY)) {
		++rightIndex;
	}
	const GraphBlendSample &left = *ordered[rightIndex - 1U];
	const GraphBlendSample &right = *ordered[rightIndex];
	const double leftCoordinate = useX ? left.positionX : left.positionY;
	const double rightCoordinate = useX ? right.positionX : right.positionY;
	const double factor = (coordinate - leftCoordinate) / (rightCoordinate - leftCoordinate);
	const std::array<WeightedContribution, 2U> weighted{{
		{&left.contribution, 1.0 - factor, left.stableId},
		{&right.contribution, factor, right.stableId}}};
	return BlendWeightedContributions(weighted, output);
}

bool IsLeafOpcode(EvaluationOpcode opcode) noexcept {
	return opcode == EvaluationOpcode::SampleClip || opcode == EvaluationOpcode::CallSubgraph ||
		opcode == EvaluationOpcode::MotionMatch || opcode == EvaluationOpcode::ProceduralPose;
}

const EvaluationBlendNode *FindBlendNode(const EvaluationProgram &program, std::uint64_t nodeId) noexcept {
	const auto iterator = std::find_if(program.blendNodes.begin(), program.blendNodes.end(),
		[nodeId](const EvaluationBlendNode &node) { return node.nodeStableId == nodeId; });
	return iterator == program.blendNodes.end() ? nullptr : &*iterator;
}

const GraphNodeContribution *FindLeafOutput(
	std::span<const GraphNodeContribution> outputs,
	std::uint64_t nodeId) noexcept {
	const auto iterator = std::find_if(outputs.begin(), outputs.end(),
		[nodeId](const GraphNodeContribution &output) { return output.nodeId == nodeId; });
	return iterator == outputs.end() ? nullptr : &*iterator;
}

AnimationStatus ValidateCompiledBlendInputs(
	const EvaluationProgram &program,
	std::span<const GraphNodeContribution> leafOutputs) {
	if (!program.controllerAssetId.IsValid() || program.compatibilityFingerprint == 0U ||
		program.stableHash != ComputeEvaluationProgramStableHash(program)) {
		return AnimationStatus::InvalidArgument;
	}
	const AnimationStatus nodeLimit = ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, program.blendNodes.size());
	if (nodeLimit != AnimationStatus::Ok) {
		return nodeLimit;
	}
	const AnimationStatus leafLimit = ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, leafOutputs.size());
	if (leafLimit != AnimationStatus::Ok) {
		return leafLimit;
	}

	std::vector<const EvaluationBlendNode *> orderedNodes;
	orderedNodes.reserve(program.blendNodes.size());
	std::size_t totalSamples = 0U;
	const std::size_t sampleHardMaximum =
		static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::ControllerNodes).hardMaximum);
	for (const EvaluationBlendNode &node : program.blendNodes) {
		if (node.nodeStableId == 0U ||
			(node.opcode != EvaluationOpcode::BlendTree1D && node.opcode != EvaluationOpcode::BlendTree2D) ||
			node.outOfDomainPolicy > AnimationBlendOutOfDomainPolicy::Extrapolate ||
			node.outputCompatibilityFingerprint != program.compatibilityFingerprint || node.samples.empty()) {
			return AnimationStatus::InvalidArgument;
		}
		const AnimationStatus sampleLimit = ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, node.samples.size());
		if (sampleLimit != AnimationStatus::Ok) {
			return sampleLimit;
		}
		if (node.samples.size() > sampleHardMaximum - totalSamples) {
			return AnimationStatus::HardLimitExceeded;
		}
		totalSamples += node.samples.size();
		orderedNodes.push_back(&node);

		std::vector<const EvaluationBlendSample *> byStableId;
		std::vector<const EvaluationBlendSample *> byPosition;
		byStableId.reserve(node.samples.size());
		byPosition.reserve(node.samples.size());
		for (const EvaluationBlendSample &sample : node.samples) {
			if (sample.stableId == 0U || sample.childNodeId == 0U || !IsFiniteFloat(sample.positionX) ||
				!IsFiniteFloat(sample.positionY)) {
				return AnimationStatus::InvalidArgument;
			}
			byStableId.push_back(&sample);
			byPosition.push_back(&sample);
		}
		std::sort(byStableId.begin(), byStableId.end(), [](const auto *left, const auto *right) {
			return left->stableId < right->stableId;
		});
		if (std::adjacent_find(byStableId.begin(), byStableId.end(), [](const auto *left, const auto *right) {
				return left->stableId == right->stableId;
			}) != byStableId.end()) {
			return AnimationStatus::InvalidArgument;
		}
		std::sort(byPosition.begin(), byPosition.end(), [&node](const auto *left, const auto *right) {
			if (left->positionX != right->positionX) {
				return left->positionX < right->positionX;
			}
			if (node.opcode == EvaluationOpcode::BlendTree2D && left->positionY != right->positionY) {
				return left->positionY < right->positionY;
			}
			return left->stableId < right->stableId;
		});
		if (std::adjacent_find(byPosition.begin(), byPosition.end(), [&node](const auto *left, const auto *right) {
				return left->positionX == right->positionX &&
				(node.opcode == EvaluationOpcode::BlendTree1D || left->positionY == right->positionY);
			}) != byPosition.end()) {
			return AnimationStatus::InvalidArgument;
		}
	}
	std::sort(orderedNodes.begin(), orderedNodes.end(), [](const auto *left, const auto *right) {
		return left->nodeStableId < right->nodeStableId;
	});
	if (std::adjacent_find(orderedNodes.begin(), orderedNodes.end(), [](const auto *left, const auto *right) {
			return left->nodeStableId == right->nodeStableId;
		}) != orderedNodes.end()) {
		return AnimationStatus::InvalidArgument;
	}

	std::vector<const GraphNodeContribution *> orderedLeaves;
	orderedLeaves.reserve(leafOutputs.size());
	for (const GraphNodeContribution &leaf : leafOutputs) {
		if (leaf.nodeId == 0U || !IsLeafOpcode(leaf.producerOpcode)) {
			return AnimationStatus::InvalidArgument;
		}
		const AnimationStatus contributionStatus = ValidateGraphPoseContribution(leaf.contribution);
		if (contributionStatus != AnimationStatus::Ok) {
			return contributionStatus;
		}
		if (leaf.contribution.compatibilityFingerprint != program.compatibilityFingerprint) {
			return AnimationStatus::IncompatibleAsset;
		}
		orderedLeaves.push_back(&leaf);
	}
	std::sort(orderedLeaves.begin(), orderedLeaves.end(), [](const auto *left, const auto *right) {
		return left->nodeId < right->nodeId;
	});
	if (std::adjacent_find(orderedLeaves.begin(), orderedLeaves.end(), [](const auto *left, const auto *right) {
			return left->nodeId == right->nodeId;
		}) != orderedLeaves.end()) {
		return AnimationStatus::InvalidArgument;
	}
	return AnimationStatus::Ok;
}

struct CachedBlendOutput {
	std::uint64_t nodeId = 0U;
	GraphPoseContribution contribution;
};

AnimationStatus EvaluateCompiledBlendNodeRecursive(
	const EvaluationProgram &program,
	std::uint64_t nodeId,
	float parameterX,
	float parameterY,
	std::span<const GraphNodeContribution> leafOutputs,
	std::span<std::uint64_t> activeStack,
	std::size_t depth,
	std::vector<CachedBlendOutput> &cache,
	GraphPoseContribution &output) {
	if (depth >= activeStack.size()) {
		return AnimationStatus::HardLimitExceeded;
	}
	for (std::size_t index = 0U; index < depth; ++index) {
		if (activeStack[index] == nodeId) {
			return AnimationStatus::Rejected;
		}
	}
	const auto cached = std::find_if(cache.begin(), cache.end(),
		[nodeId](const CachedBlendOutput &value) { return value.nodeId == nodeId; });
	if (cached != cache.end()) {
		output = cached->contribution;
		return AnimationStatus::Ok;
	}

	const EvaluationBlendNode *node = FindBlendNode(program, nodeId);
	if (node == nullptr) {
		const GraphNodeContribution *leaf = FindLeafOutput(leafOutputs, nodeId);
		if (leaf == nullptr) {
			return AnimationStatus::MissingAsset;
		}
		output = leaf->contribution;
		return AnimationStatus::Ok;
	}

	activeStack[depth] = nodeId;
	std::vector<GraphBlendSample> samples;
	samples.reserve(node->samples.size());
	for (const EvaluationBlendSample &sample : node->samples) {
		GraphPoseContribution child;
		const AnimationStatus childStatus = EvaluateCompiledBlendNodeRecursive(
			program, sample.childNodeId, parameterX, parameterY, leafOutputs, activeStack, depth + 1U, cache, child);
		if (childStatus != AnimationStatus::Ok) {
			return childStatus;
		}
		samples.push_back({sample.stableId, sample.positionX, sample.positionY, std::move(child)});
	}

	GraphPoseContribution staged;
	const AnimationStatus evaluationStatus = node->opcode == EvaluationOpcode::BlendTree1D
		? EvaluateBlendTree1D(samples, parameterX, node->outOfDomainPolicy, staged)
		: EvaluateBlendTree2D(samples, parameterX, parameterY, node->outOfDomainPolicy, staged);
	if (evaluationStatus != AnimationStatus::Ok) {
		return evaluationStatus;
	}
	cache.push_back({nodeId, staged});
	output = std::move(staged);
	return AnimationStatus::Ok;
}
} // namespace

AnimationStatus ValidateGraphPoseContribution(const GraphPoseContribution &value) noexcept {
	if (value.compatibilityFingerprint == 0U || value.jointCount != value.joints.size() ||
		value.jointStableIds.size() != value.joints.size()) {
		return AnimationStatus::InvalidArgument;
	}
	const AnimationStatus jointLimit = ValidateAnimationHardLimit(AnimationLimit::SkeletonJoints, value.joints.size());
	if (jointLimit != AnimationStatus::Ok) {
		return jointLimit;
	}
	const AnimationStatus morphLimit = ValidateAnimationHardLimit(AnimationLimit::MeshMorphTargets, value.morphs.size());
	if (morphLimit != AnimationStatus::Ok) {
		return morphLimit;
	}
	const AnimationStatus curveLimit = ValidateAnimationHardLimit(AnimationLimit::ClipChannels, value.curves.size());
	if (curveLimit != AnimationStatus::Ok) {
		return curveLimit;
	}
	for (std::size_t index = 0U; index < value.joints.size(); ++index) {
		if (value.jointStableIds[index] == 0U ||
			(index > 0U && value.jointStableIds[index - 1U] >= value.jointStableIds[index]) ||
			!IsFinite(value.joints[index]) || !IsNormalized(value.joints[index].rotation)) {
			return AnimationStatus::InvalidArgument;
		}
	}
	for (std::size_t index = 0U; index < value.morphs.size(); ++index) {
		if (value.morphs[index].stableId == 0U || !IsFiniteFloat(value.morphs[index].value) ||
			(index > 0U && value.morphs[index - 1U].stableId >= value.morphs[index].stableId)) {
			return AnimationStatus::InvalidArgument;
		}
	}
	for (std::size_t index = 0U; index < value.curves.size(); ++index) {
		if (value.curves[index].stableId == 0U || !IsFiniteFloat(value.curves[index].value) ||
			(index > 0U && value.curves[index - 1U].stableId >= value.curves[index].stableId)) {
			return AnimationStatus::InvalidArgument;
		}
	}
	if (value.rootMotion.has_value()) {
		const AnimationRootMotionDelta &rootMotion = *value.rootMotion;
		if (!IsFinite(rootMotion.localTranslation) || !IsFinite(rootMotion.worldTranslation) ||
			!IsFiniteFloat(rootMotion.localYawRadians) || !IsFiniteFloat(rootMotion.worldYawRadians) ||
			rootMotion.policy > AnimatorRootMotionPolicy::ApplyToCharacter) {
			return AnimationStatus::InvalidArgument;
		}
	}
	return AnimationStatus::Ok;
}

AnimationStatus EvaluateBlendTree1D(
	std::span<const GraphBlendSample> samples,
	float parameter,
	AnimationBlendOutOfDomainPolicy policy,
	GraphPoseContribution &output) {
	try {
		if (!IsFiniteFloat(parameter) || policy > AnimationBlendOutOfDomainPolicy::Extrapolate) {
			return AnimationStatus::InvalidArgument;
		}
		const AnimationStatus validationStatus = ValidateStandaloneSamples(samples, false);
		if (validationStatus != AnimationStatus::Ok) {
			return validationStatus;
		}

		std::vector<const GraphBlendSample *> ordered;
		ordered.reserve(samples.size());
		for (const GraphBlendSample &sample : samples) {
			ordered.push_back(&sample);
		}
		std::sort(ordered.begin(), ordered.end(), [](const auto *left, const auto *right) {
			return left->positionX != right->positionX ? left->positionX < right->positionX : left->stableId < right->stableId;
		});
		if (ordered.size() == 1U) {
			GraphPoseContribution staged = ordered.front()->contribution;
			output = std::move(staged);
			return AnimationStatus::Ok;
		}

		double coordinate = parameter;
		if (policy == AnimationBlendOutOfDomainPolicy::Clamp) {
			coordinate = std::clamp(
				coordinate, static_cast<double>(ordered.front()->positionX), static_cast<double>(ordered.back()->positionX));
		}
		std::size_t rightIndex = 1U;
		while (rightIndex + 1U < ordered.size() && coordinate > ordered[rightIndex]->positionX) {
			++rightIndex;
		}
		const GraphBlendSample &left = *ordered[rightIndex - 1U];
		const GraphBlendSample &right = *ordered[rightIndex];
		const double factor = (coordinate - left.positionX) /
			(static_cast<double>(right.positionX) - static_cast<double>(left.positionX));
		const std::array<WeightedContribution, 2U> weighted{{
			{&left.contribution, 1.0 - factor, left.stableId},
			{&right.contribution, factor, right.stableId}}};
		GraphPoseContribution staged;
		const AnimationStatus blendStatus = BlendWeightedContributions(weighted, staged);
		if (blendStatus == AnimationStatus::Ok) {
			output = std::move(staged);
		}
		return blendStatus;
	} catch (const std::bad_alloc &) {
		return AnimationStatus::InternalError;
	}
}

AnimationStatus EvaluateBlendTree2D(
	std::span<const GraphBlendSample> samples,
	float parameterX,
	float parameterY,
	AnimationBlendOutOfDomainPolicy policy,
	GraphPoseContribution &output) {
	try {
		if (!IsFiniteFloat(parameterX) || !IsFiniteFloat(parameterY) ||
			policy > AnimationBlendOutOfDomainPolicy::Extrapolate) {
			return AnimationStatus::InvalidArgument;
		}
		const AnimationStatus validationStatus = ValidateStandaloneSamples(samples, true);
		if (validationStatus != AnimationStatus::Ok) {
			return validationStatus;
		}
		for (const GraphBlendSample &sample : samples) {
			if (sample.positionX == parameterX && sample.positionY == parameterY) {
				GraphPoseContribution staged = sample.contribution;
				output = std::move(staged);
				return AnimationStatus::Ok;
			}
		}
		if (samples.size() == 1U) {
			GraphPoseContribution staged = samples.front().contribution;
			output = std::move(staged);
			return AnimationStatus::Ok;
		}

		const BlendPoint point{parameterX, parameterY};
		std::vector<const GraphBlendSample *> nearest;
		nearest.reserve(samples.size());
		for (const GraphBlendSample &sample : samples) {
			nearest.push_back(&sample);
		}
		std::sort(nearest.begin(), nearest.end(), [&point](const auto *left, const auto *right) {
			const double leftX = static_cast<double>(left->positionX) - point.x;
			const double leftY = static_cast<double>(left->positionY) - point.y;
			const double rightX = static_cast<double>(right->positionX) - point.x;
			const double rightY = static_cast<double>(right->positionY) - point.y;
			const double leftDistance = leftX * leftX + leftY * leftY;
			const double rightDistance = rightX * rightX + rightY * rightY;
			return leftDistance != rightDistance ? leftDistance < rightDistance : left->stableId < right->stableId;
		});

		const GraphBlendSample *sampleA = nearest[0U];
		const GraphBlendSample *sampleB = nearest[1U];
		const GraphBlendSample *sampleC = nullptr;
		const BlendPoint a{sampleA->positionX, sampleA->positionY};
		const BlendPoint b{sampleB->positionX, sampleB->positionY};
		for (std::size_t index = 2U; index < nearest.size(); ++index) {
			const BlendPoint candidate{nearest[index]->positionX, nearest[index]->positionY};
			const double determinant =
				(b.x - a.x) * (candidate.y - a.y) - (b.y - a.y) * (candidate.x - a.x);
			const double scale = std::max({1.0, std::abs(b.x - a.x), std::abs(b.y - a.y),
				std::abs(candidate.x - a.x), std::abs(candidate.y - a.y)});
			if (std::abs(determinant) > kWeightEpsilon * scale * scale) {
				sampleC = nearest[index];
				break;
			}
		}
		if (sampleC == nullptr) {
			GraphPoseContribution staged;
			const AnimationStatus lineStatus =
				BlendCollinear2D(samples, parameterX, parameterY, policy, staged);
			if (lineStatus == AnimationStatus::Ok) {
				output = std::move(staged);
			}
			return lineStatus;
		}

		const BlendPoint c{sampleC->positionX, sampleC->positionY};
		const TriangleWeights weights = policy == AnimationBlendOutOfDomainPolicy::Clamp
			? ComputeClosestTriangleWeights(point, a, b, c)
			: ComputeBarycentric(point, a, b, c);
		if (!std::isfinite(weights.a) || !std::isfinite(weights.b) || !std::isfinite(weights.c)) {
			return AnimationStatus::RangeOverflow;
		}
		const std::array<WeightedContribution, 3U> weighted{{
			{&sampleA->contribution, weights.a, sampleA->stableId},
			{&sampleB->contribution, weights.b, sampleB->stableId},
			{&sampleC->contribution, weights.c, sampleC->stableId}}};
		GraphPoseContribution staged;
		const AnimationStatus blendStatus = BlendWeightedContributions(weighted, staged);
		if (blendStatus == AnimationStatus::Ok) {
			output = std::move(staged);
		}
		return blendStatus;
	} catch (const std::bad_alloc &) {
		return AnimationStatus::InternalError;
	}
}

AnimationStatus EvaluateCompiledBlendNode(
	const EvaluationProgram &program,
	std::uint64_t nodeId,
	float parameterX,
	float parameterY,
	std::span<const GraphNodeContribution> leafOutputs,
	GraphPoseContribution &output) {
	try {
		if (nodeId == 0U || !IsFiniteFloat(parameterX) || !IsFiniteFloat(parameterY)) {
			return AnimationStatus::InvalidArgument;
		}
		const AnimationStatus validationStatus = ValidateCompiledBlendInputs(program, leafOutputs);
		if (validationStatus != AnimationStatus::Ok) {
			return validationStatus;
		}

		constexpr std::size_t kMaximumCompiledBlendDepth = 32U;
		static_assert(kMaximumCompiledBlendDepth == 32U,
			"The fixed stack mirrors the version-1 reusable-subgraph hard limit.");
		std::array<std::uint64_t, kMaximumCompiledBlendDepth> activeStack{};
		std::vector<CachedBlendOutput> cache;
		cache.reserve(program.blendNodes.size());
		GraphPoseContribution staged;
		const AnimationStatus status = EvaluateCompiledBlendNodeRecursive(
			program, nodeId, parameterX, parameterY, leafOutputs, activeStack, 0U, cache, staged);
		if (status == AnimationStatus::Ok) {
			output = std::move(staged);
		}
		return status;
	} catch (const std::bad_alloc &) {
		return AnimationStatus::InternalError;
	}
}
} // namespace TerraEngine::Animation
