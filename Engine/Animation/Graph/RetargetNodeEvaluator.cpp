// RetargetNodeEvaluator.cpp - Atomic graph/layer integration for retargeted poses.

#include "Animation/Graph/RetargetNodeEvaluator.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace TerraEngine::Animation {
	namespace {
		bool ValidWeight(float value) noexcept {
			return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
		}

		AnimationStatus ValidateTargetLayout(
			const GraphPoseContribution& pose,
			const SkeletonAsset& skeleton) noexcept {
			const AnimationStatus poseStatus = ValidateGraphPoseContribution(pose);
			if(poseStatus != AnimationStatus::Ok) return poseStatus;
			if(pose.compatibilityFingerprint != skeleton.compatibilityFingerprint
				|| pose.joints.size() != skeleton.joints.size()) {
				return AnimationStatus::IncompatibleAsset;
			}
			for(std::size_t index = 0U; index < skeleton.joints.size(); ++index) {
				if(pose.jointStableIds[index] != skeleton.joints[index].stableId) {
					return AnimationStatus::IncompatibleAsset;
				}
			}
			return AnimationStatus::Ok;
		}

		AnimationStatus BuildHybridClosure(
			const SkeletonAsset& skeleton,
			std::span<const std::uint64_t> requiredJointIds,
			std::vector<std::uint32_t>& output) {
			output.clear();
			for(const std::uint64_t stableId : requiredJointIds) {
				if(stableId == 0U) return AnimationStatus::InvalidArgument;
				const auto found = std::find_if(skeleton.joints.begin(), skeleton.joints.end(),
					[stableId](const SkeletonJoint& joint) { return joint.stableId == stableId; });
				if(found == skeleton.joints.end()) return AnimationStatus::MissingAsset;
				std::int32_t index = static_cast<std::int32_t>(
					std::distance(skeleton.joints.begin(), found));
				while(index >= 0) {
					const std::uint32_t jointIndex = static_cast<std::uint32_t>(index);
					if(std::find(output.begin(), output.end(), jointIndex) == output.end()) {
						output.push_back(jointIndex);
					}
					const std::int32_t parent = skeleton.joints[jointIndex].parentIndex;
					if(parent >= static_cast<std::int32_t>(jointIndex)
						|| parent >= static_cast<std::int32_t>(skeleton.joints.size())) {
						return AnimationStatus::IncompatibleAsset;
					}
					index = parent;
				}
			}
			std::sort(output.begin(), output.end());
			return AnimationStatus::Ok;
		}
	}

	AnimationStatus EvaluateRetargetGraphNode(
		const RetargetGraphNodeRequest& request,
		RetargetGraphNodeResult& output) noexcept {
		if(request.targetBasePose == nullptr || request.evaluation.targetSkeleton == nullptr
			|| request.layerMode > AnimationGraphLayerMode::Additive
			|| !ValidWeight(request.controllerWeight) || !ValidWeight(request.nodeWeight)
			|| !ValidWeight(request.layerWeight)) {
			return AnimationStatus::InvalidArgument;
		}
		const AnimationStatus layoutStatus = ValidateTargetLayout(
			*request.targetBasePose, *request.evaluation.targetSkeleton);
		if(layoutStatus != AnimationStatus::Ok) return layoutStatus;

		try {
			RetargetGraphNodeResult candidate;
			const AnimationStatus closureStatus = BuildHybridClosure(
				*request.evaluation.targetSkeleton, request.hybridConsumerJointIds,
				candidate.hybridJointClosure);
			if(closureStatus != AnimationStatus::Ok) return closureStatus;

			const AnimationStatus retargetStatus = EvaluateRetargetPose(
				request.evaluation, candidate.retarget);
			if(retargetStatus != AnimationStatus::Ok) return retargetStatus;
			if(candidate.retarget.localTransforms.size()
				!= request.evaluation.targetSkeleton->joints.size()) {
				return AnimationStatus::InternalError;
			}

			GraphPoseContribution retargeted = *request.targetBasePose;
			retargeted.joints = candidate.retarget.localTransforms;
			// Morphs, curves, and root motion remain on the target graph packet. The
			// rig-transfer pass changes skeletal transforms only, so later layers and
			// authoritative consumers retain their already selected channels.
			GraphLayerInput layer;
			layer.stableId = request.evaluation.profile->stableHash;
			layer.mode = request.layerMode;
			layer.controllerWeight = request.controllerWeight;
			layer.nodeWeight = request.nodeWeight;
			layer.layerWeight = request.layerWeight;
			layer.mask = request.mask;
			layer.contribution = &retargeted;
			layer.additiveReference = request.additiveReference;
			const AnimationStatus layerStatus = EvaluateAnimationLayers(
				*request.targetBasePose, std::span{&layer, 1U}, candidate.pose);
			if(layerStatus != AnimationStatus::Ok) return layerStatus;

			if(request.motionSelectionApplied) {
				candidate.orderedStages.push_back(RetargetCompositionStage::MotionSelection);
			}
			candidate.orderedStages.push_back(RetargetCompositionStage::Retarget);
			if(request.proceduralFollows) {
				candidate.orderedStages.push_back(RetargetCompositionStage::Procedural);
			}
			if(request.ragdollFollows) {
				candidate.orderedStages.push_back(RetargetCompositionStage::Ragdoll);
			}
			if(request.recordingRequested || request.rollbackRequested) {
				candidate.orderedStages.push_back(RetargetCompositionStage::Recording);
			}
			if(request.recordingRequested) candidate.recordingPose = candidate.pose;
			if(request.rollbackRequested) candidate.rollbackPose = candidate.pose;
			output = std::move(candidate);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}
}
