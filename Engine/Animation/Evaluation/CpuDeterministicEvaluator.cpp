// CpuDeterministicEvaluator.cpp - Fixed-point projection and advanced CPU stages.

#include "Animation/Evaluation/CpuDeterministicEvaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace TerraEngine::Animation {
	namespace {
		constexpr double kFixedScale = 65536.0;
		constexpr double kFixedMaximum = static_cast<double>(std::numeric_limits<std::int32_t>::max());

		bool IsRecoverableAdvancedFailure(AnimationStatus status) noexcept {
			switch(status) {
			case AnimationStatus::NotReady:
			case AnimationStatus::MissingAsset:
			case AnimationStatus::IncompatibleAsset:
			case AnimationStatus::StaleHandle:
			case AnimationStatus::Unsupported:
			case AnimationStatus::DeterminismViolation:
			case AnimationStatus::RendererUnavailable:
				return true;
			default:
				return false;
			}
		}

		bool Quantize(float input, float& output) noexcept {
			if(!std::isfinite(input)) return false;
			const double scaled = static_cast<double>(input) * kFixedScale;
			if(!std::isfinite(scaled) || scaled < -kFixedMaximum || scaled > kFixedMaximum) return false;
			const double rounded = scaled >= 0.0 ? std::floor(scaled + 0.5) : std::ceil(scaled - 0.5);
			output = static_cast<float>(rounded / kFixedScale);
			return std::isfinite(output);
		}

		bool Quantize(AnimationFloat3& value) noexcept {
			return Quantize(value.x, value.x) && Quantize(value.y, value.y) && Quantize(value.z, value.z);
		}

		bool Quantize(AnimationQuaternion& value) noexcept {
			return Quantize(value.x, value.x) && Quantize(value.y, value.y)
				&& Quantize(value.z, value.z) && Quantize(value.w, value.w);
		}

		AnimationStatus QuantizePose(GraphPoseContribution& pose) noexcept {
			for(AnimationTransform& transform : pose.joints) {
				if(!Quantize(transform.translation) || !Quantize(transform.rotation) || !Quantize(transform.scale)) {
					return AnimationStatus::RangeOverflow;
				}
			}
			for(AnimationNamedFloatValue& value : pose.morphs) {
				if(!Quantize(value.value, value.value)) return AnimationStatus::RangeOverflow;
			}
			for(AnimationNamedFloatValue& value : pose.curves) {
				if(!Quantize(value.value, value.value)) return AnimationStatus::RangeOverflow;
			}
			if(pose.rootMotion.has_value()) {
				AnimationRootMotionDelta& root = *pose.rootMotion;
				if(!Quantize(root.localTranslation) || !Quantize(root.worldTranslation)
					|| !Quantize(root.localYawRadians, root.localYawRadians)
					|| !Quantize(root.worldYawRadians, root.worldYawRadians)) {
					return AnimationStatus::RangeOverflow;
				}
			}
			return ValidateGraphPoseContribution(pose);
		}

		AnimationStatus ValidateCompatibleFallback(
			const GraphPoseContribution& input, const GraphPoseContribution* fallback) noexcept {
			if(fallback == nullptr) return AnimationStatus::Ok;
			const AnimationStatus status = ValidateGraphPoseContribution(*fallback);
			if(status != AnimationStatus::Ok) return status;
			return fallback->compatibilityFingerprint == input.compatibilityFingerprint
				&& fallback->jointCount == input.jointCount && fallback->jointStableIds == input.jointStableIds
				? AnimationStatus::Ok : AnimationStatus::IncompatibleAsset;
		}

		AnimationStatus CanonicalizeRagdollRequest(
			const CpuDeterministicRagdollRequest& request,
			PhysicsPoseSnapshot& snapshot, std::vector<RagdollBlendInput>& inputs) {
			if(request.physicsPose == nullptr) return AnimationStatus::InvalidArgument;
			snapshot = *request.physicsPose;
			inputs = request.jointInputs;
			std::sort(snapshot.bodies.begin(), snapshot.bodies.end(), [](const auto& left, const auto& right) {
				return left.stableJointId != right.stableJointId
					? left.stableJointId < right.stableJointId : left.bodyId < right.bodyId;
			});
			for(std::size_t index = 1U; index < snapshot.bodies.size(); ++index) {
				if(snapshot.bodies[index - 1U].stableJointId == snapshot.bodies[index].stableJointId) {
					return AnimationStatus::InvalidArgument;
				}
			}
			std::sort(inputs.begin(), inputs.end(), [](const auto& left, const auto& right) {
				return left.stableJointId < right.stableJointId;
			});
			for(std::size_t index = 1U; index < inputs.size(); ++index) {
				if(inputs[index - 1U].stableJointId == inputs[index].stableJointId) {
					return AnimationStatus::InvalidArgument;
				}
			}
			return AnimationStatus::Ok;
		}

		AnimationStatus ApplyRagdollStage(
			const CpuDeterministicRagdollRequest& request, GraphPoseContribution& pose) {
			PhysicsPoseSnapshot canonicalSnapshot;
			std::vector<RagdollBlendInput> canonicalInputs;
			const AnimationStatus canonicalStatus = CanonicalizeRagdollRequest(
				request, canonicalSnapshot, canonicalInputs);
			if(canonicalStatus != AnimationStatus::Ok) return canonicalStatus;
			RagdollBlendResult blend;
			const AnimationStatus status = BlendRagdollPose(pose.jointStableIds, pose.joints,
				canonicalSnapshot, request.expectedGeneration, canonicalInputs, request.timeWeight, blend);
			if(status != AnimationStatus::Ok) return status;
			if(blend.modelPose.size() != pose.joints.size()) return AnimationStatus::InternalError;
			pose.joints = std::move(blend.modelPose);
			return AnimationStatus::Ok;
		}
	}

	AnimationStatus EvaluateCpuDeterministicAdvancedPose(
		const CpuDeterministicEvaluationRequest& request,
		CpuDeterministicEvaluationResult& output) noexcept {
		if(request.inputPose == nullptr) return AnimationStatus::InvalidArgument;
		const AnimationStatus inputStatus = ValidateGraphPoseContribution(*request.inputPose);
		if(inputStatus != AnimationStatus::Ok) return inputStatus;
		const AnimationStatus fallbackStatus = ValidateCompatibleFallback(*request.inputPose, request.fallbackPose);
		if(fallbackStatus != AnimationStatus::Ok) return fallbackStatus;

		try {
			CpuDeterministicEvaluationResult candidate;
			candidate.pose = *request.inputPose;
			if(const AnimationStatus quantizeStatus = QuantizePose(candidate.pose); quantizeStatus != AnimationStatus::Ok) {
				return quantizeStatus;
			}

			auto fallbackOrFail = [&](CpuDeterministicAdvancedStage stage, AnimationStatus status) {
				candidate.failureStatus = status;
				candidate.failedStage = stage;
				if(!IsRecoverableAdvancedFailure(status) || request.fallbackPose == nullptr) return false;
				candidate.pose = *request.fallbackPose;
				const AnimationStatus quantizeStatus = QuantizePose(candidate.pose);
				if(quantizeStatus != AnimationStatus::Ok) return false;
				candidate.status = AnimationStatus::Ok;
				candidate.usedFallback = true;
				return true;
			};

			if(request.motionMatch != nullptr) {
				MotionMatchGraphNodeRequest stage = *request.motionMatch;
				stage.currentPose = &candidate.pose;
				// GPU candidates are diagnostic only. Never let their availability affect
				// the CPU authority path or a fallback/replay decision.
				stage.gpuReadback = nullptr;
				MotionMatchGraphNodeResult result;
				const AnimationStatus status = EvaluateMotionMatchGraphNode(stage, result);
				if(status != AnimationStatus::Ok) {
					if(fallbackOrFail(CpuDeterministicAdvancedStage::MotionMatch, status)) { output = std::move(candidate); return AnimationStatus::Ok; }
					return status;
				}
				candidate.pose = std::move(result.pose);
				candidate.evaluatedStages.push_back(CpuDeterministicAdvancedStage::MotionMatch);
				if(const AnimationStatus quantizeStatus = QuantizePose(candidate.pose); quantizeStatus != AnimationStatus::Ok) return quantizeStatus;
			}

			if(request.retarget != nullptr) {
				RetargetGraphNodeRequest stage = *request.retarget;
				stage.targetBasePose = &candidate.pose;
				RetargetGraphNodeResult result;
				const AnimationStatus status = EvaluateRetargetGraphNode(stage, result);
				if(status != AnimationStatus::Ok) {
					if(fallbackOrFail(CpuDeterministicAdvancedStage::Retarget, status)) { output = std::move(candidate); return AnimationStatus::Ok; }
					return status;
				}
				candidate.pose = std::move(result.pose);
				candidate.evaluatedStages.push_back(CpuDeterministicAdvancedStage::Retarget);
				if(const AnimationStatus quantizeStatus = QuantizePose(candidate.pose); quantizeStatus != AnimationStatus::Ok) return quantizeStatus;
			}

			if(request.procedural != nullptr) {
				ProceduralGraphEvaluationRequest stage = *request.procedural;
				stage.inputPose = &candidate.pose;
				stage.authority = ProceduralGraphAuthority::CpuAuthoritative;
				stage.authoritativeConsumer = true;
				stage.rollbackRequested = true;
				ProceduralGraphEvaluationResult result;
				const AnimationStatus status = EvaluateProceduralGraphNodes(stage, result);
				if(status != AnimationStatus::Ok) {
					if(fallbackOrFail(CpuDeterministicAdvancedStage::Procedural, status)) { output = std::move(candidate); return AnimationStatus::Ok; }
					return status;
				}
				candidate.pose = std::move(result.pose);
				candidate.evaluatedStages.push_back(CpuDeterministicAdvancedStage::Procedural);
				if(const AnimationStatus quantizeStatus = QuantizePose(candidate.pose); quantizeStatus != AnimationStatus::Ok) return quantizeStatus;
			}

			if(request.ragdoll != nullptr) {
				const AnimationStatus status = ApplyRagdollStage(*request.ragdoll, candidate.pose);
				if(status != AnimationStatus::Ok) {
					if(fallbackOrFail(CpuDeterministicAdvancedStage::Ragdoll, status)) { output = std::move(candidate); return AnimationStatus::Ok; }
					return status;
				}
				candidate.evaluatedStages.push_back(CpuDeterministicAdvancedStage::Ragdoll);
				if(const AnimationStatus quantizeStatus = QuantizePose(candidate.pose); quantizeStatus != AnimationStatus::Ok) return quantizeStatus;
			}
			candidate.status = AnimationStatus::Ok;
			output = std::move(candidate);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}
}
