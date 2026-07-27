// MotionMatchNodeEvaluator.cpp - Stable selection, fallback, and composition.

#include "Animation/Graph/MotionMatchNodeEvaluator.h"

#include <cmath>
#include <new>

namespace TerraEngine::Animation {
	namespace {
		bool SameCandidate(
			const MotionMatchCandidate& left,
			const MotionMatchCandidate& right) noexcept {
			return left.stableSampleId == right.stableSampleId
				&& left.sourceClipId == right.sourceClipId
				&& left.sourceTick == right.sourceTick;
		}

		bool SamePoseLayout(
			const GraphPoseContribution& left,
			const GraphPoseContribution& right) noexcept {
			return left.compatibilityFingerprint == right.compatibilityFingerprint
				&& left.jointCount == right.jointCount
				&& left.jointStableIds == right.jointStableIds;
		}
	}

	AnimationStatus EvaluateMotionMatchGraphNode(
		const MotionMatchGraphNodeRequest& request,
		MotionMatchGraphNodeResult& output) noexcept {
		if(request.database == nullptr || request.query == nullptr
			|| request.currentPose == nullptr || !std::isfinite(request.transitionWeight)
			|| request.transitionWeight < 0.0F || request.transitionWeight > 1.0F
			|| request.evaluationTick < 0) {
			return AnimationStatus::InvalidArgument;
		}
		const AnimationStatus currentStatus = ValidateGraphPoseContribution(*request.currentPose);
		if(currentStatus != AnimationStatus::Ok) return currentStatus;

		try {
			MotionMatchGraphNodeResult candidate;
			const AnimationStatus selectionStatus = SelectMotionMatchAuthoritatively(
				*request.database, *request.query, request.gpuReadback, candidate.selection);
			const GraphPoseContribution* selectedPose = nullptr;
			if(selectionStatus == AnimationStatus::Ok) {
				if(request.resolvedPose.pose == nullptr
					|| !SameCandidate(request.resolvedPose.candidate, candidate.selection.chosen)) {
					return AnimationStatus::StaleHandle;
				}
				selectedPose = request.resolvedPose.pose;
			} else if(selectionStatus == AnimationStatus::NotReady
				&& request.fallbackPose != nullptr) {
				// A graph-authored fallback state is a complete pose, not a partially
				// scored candidate. The unsuccessful selection remains visible in the
				// result while evaluation continues deterministically.
				selectedPose = request.fallbackPose;
				candidate.usedFallbackState = true;
			} else {
				return selectionStatus;
			}

			const AnimationStatus selectedStatus = ValidateGraphPoseContribution(*selectedPose);
			if(selectedStatus != AnimationStatus::Ok) return selectedStatus;
			if(!SamePoseLayout(*request.currentPose, *selectedPose)) {
				return AnimationStatus::IncompatibleAsset;
			}
			GraphLayerInput transition;
			transition.stableId = candidate.usedFallbackState
				? 1U : candidate.selection.chosen.stableSampleId;
			transition.mode = AnimationGraphLayerMode::Override;
			transition.layerWeight = request.transitionWeight;
			transition.mask = request.mask;
			transition.contribution = selectedPose;
			const AnimationStatus blendStatus = EvaluateAnimationLayers(
				*request.currentPose, std::span{&transition, 1U}, candidate.pose);
			if(blendStatus != AnimationStatus::Ok) return blendStatus;

			candidate.status = AnimationStatus::Ok;
			candidate.orderedStages = {
				MotionMatchCompositionStage::Selection,
				MotionMatchCompositionStage::TransitionBlend};
			if(request.retargetFollows) {
				candidate.orderedStages.push_back(MotionMatchCompositionStage::Retarget);
			}
			if(request.proceduralFollows) {
				candidate.orderedStages.push_back(MotionMatchCompositionStage::Procedural);
			}
			if(request.ragdollFollows) {
				candidate.orderedStages.push_back(MotionMatchCompositionStage::Ragdoll);
			}
			if(request.recordingRequested || request.rollbackRequested) {
				candidate.orderedStages.push_back(MotionMatchCompositionStage::Recording);
				MotionMatchGraphSnapshot snapshot;
				snapshot.database = request.query->database;
				snapshot.schemaFingerprint = request.query->expectedSchemaFingerprint;
				snapshot.evaluationTick = request.evaluationTick;
				snapshot.chosen = candidate.selection.chosen;
				snapshot.pose = candidate.pose;
				if(request.recordingRequested) candidate.recordingSnapshot = snapshot;
				if(request.rollbackRequested) candidate.rollbackSnapshot = std::move(snapshot);
			}
			output = std::move(candidate);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}
}
