// ProceduralNodeEvaluator.cpp - Atomic procedural solver and graph-layer composition.

#include "Animation/Graph/ProceduralNodeEvaluator.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		bool ValidWeight(float value) noexcept {
			return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
		}

		bool CompatiblePoseLayout(
			const GraphPoseContribution& base,
			const GraphPoseContribution& candidate) noexcept {
			return ValidateGraphPoseContribution(candidate) == AnimationStatus::Ok
				&& candidate.compatibilityFingerprint == base.compatibilityFingerprint
				&& candidate.jointCount == base.jointCount
				&& candidate.jointStableIds == base.jointStableIds;
		}

		bool ValidJointIndex(std::uint32_t index, const GraphPoseContribution& pose) noexcept {
			return index != kInvalidProceduralJointIndex
				&& static_cast<std::size_t>(index) < pose.joints.size();
		}

		bool UniqueJointIndices(std::span<const std::uint32_t> indices) noexcept {
			for(std::size_t index = 0U; index < indices.size(); ++index) {
				if(indices[index] == kInvalidProceduralJointIndex) {
					return false;
				}
				for(std::size_t previous = 0U; previous < index; ++previous) {
					if(indices[previous] == indices[index]) {
						return false;
					}
				}
			}
			return true;
		}

		AnimationStatus AppendDiagnostics(
			std::vector<ProceduralDiagnosticRecord>& destination,
			std::span<const ProceduralDiagnosticRecord> source,
			float graphContribution) {
			if(source.size() > kProceduralDiagnosticRecordHardMaximum - destination.size()) {
				return AnimationStatus::HardLimitExceeded;
			}
			for(const ProceduralDiagnosticRecord& record : source) {
				ProceduralDiagnosticRecord adjusted = record;
				adjusted.finalContribution *= graphContribution;
				if(!ValidateProceduralDiagnosticRecord(adjusted)) {
					return AnimationStatus::InternalError;
				}
				destination.push_back(adjusted);
			}
			return AnimationStatus::Ok;
		}

		float JointMaskWeight(
			const AnimationMaskAsset* mask,
			std::uint64_t stableJointId) noexcept {
			if(mask == nullptr) {
				return 1.0F;
			}
			const auto found = std::lower_bound(
				mask->compiledJointWeights.begin(), mask->compiledJointWeights.end(), stableJointId,
				[](const AnimationMaskDenseJointWeight& value, std::uint64_t id) {
					return value.stableJointId < id;
				});
			return found != mask->compiledJointWeights.end() && found->stableJointId == stableJointId
				? found->weight : 0.0F;
		}

		AnimationStatus GatherAffectedJoints(
			const ProceduralGraphNodePayload& payload,
			std::vector<std::uint32_t>& output) {
			output.clear();
			std::visit([&output](const auto& operation) {
				using Operation = std::decay_t<decltype(operation)>;
				if constexpr(std::is_same_v<Operation, ProceduralTwoBoneNodeInput>) {
					output = {operation.rootJointIndex, operation.middleJointIndex, operation.endJointIndex};
				} else if constexpr(std::is_same_v<Operation, ProceduralChainNodeInput>) {
					output = operation.jointIndices;
				} else if constexpr(std::is_same_v<Operation, ProceduralAimNodeInput>) {
					output = {operation.jointIndex};
				} else if constexpr(std::is_same_v<Operation, ProceduralLookNodeInput>) {
					output = {operation.aim.jointIndex};
				} else if constexpr(std::is_same_v<Operation, ProceduralCopyTransformNodeInput>) {
					output = {operation.jointIndex};
				} else if constexpr(std::is_same_v<Operation, ProceduralJointConstraintNodeInput>) {
					output = {operation.jointIndex};
				} else if constexpr(std::is_same_v<Operation, ProceduralTwistNodeInput>) {
					output = operation.jointIndices;
				} else if constexpr(std::is_same_v<Operation, ProceduralFootPlacementNodeInput>) {
					output.reserve(operation.feet.size() + 1U);
					output.push_back(operation.pelvisJointIndex);
					for(const ProceduralFootBinding& foot : operation.feet) {
						output.push_back(foot.jointIndex);
					}
				}
			}, payload);
			return output.empty() || !UniqueJointIndices(output)
				? AnimationStatus::InvalidArgument : AnimationStatus::Ok;
		}

		float MaximumMaskContribution(
			const AnimationMaskAsset* mask,
			std::span<const std::uint32_t> affectedJoints,
			const GraphPoseContribution& pose) noexcept {
			float maximum = 0.0F;
			for(const std::uint32_t jointIndex : affectedJoints) {
				if(!ValidJointIndex(jointIndex, pose)) {
					return -1.0F;
				}
				maximum = std::max(maximum,
					JointMaskWeight(mask, pose.jointStableIds[jointIndex]));
			}
			return maximum;
		}

		AnimationStatus ApplyTwoBone(
			const ProceduralTwoBoneNodeInput& input,
			const ProceduralGenerationStamp& generation,
			const GraphPoseContribution& source,
			GraphPoseContribution& contribution,
			std::vector<ProceduralDiagnosticRecord>& diagnostics) {
			if(!ValidJointIndex(input.rootJointIndex, source)
				|| !ValidJointIndex(input.middleJointIndex, source)
				|| !ValidJointIndex(input.endJointIndex, source)) {
				return AnimationStatus::InvalidArgument;
			}
			TwoBoneIkRequest request;
			request.generation = generation;
			request.expectedGeneration = generation;
			request.rootTransform = source.joints[input.rootJointIndex];
			request.middleTransform = source.joints[input.middleJointIndex];
			request.endTransform = source.joints[input.endJointIndex];
			request.targetPosition = input.targetPosition;
			request.polePosition = input.polePosition;
			request.middleLimit = input.middleLimit;
			request.space = input.space;
			request.goalId = input.goalId;
			request.producerOrder = input.producerOrder;
			request.priority = input.priority;
			request.weight = input.solverWeight;
			request.maximumStretchScale = input.maximumStretchScale;
			request.twistRadians = input.twistRadians;
			request.toleranceMeters = input.toleranceMeters;
			request.allowStretch = input.allowStretch;
			TwoBoneIkResult result;
			const AnimationStatus status = SolveTwoBoneIk(request, result);
			if(status != AnimationStatus::Ok) {
				return status;
			}
			contribution.joints[input.rootJointIndex] = result.rootTransform;
			contribution.joints[input.middleJointIndex] = result.middleTransform;
			contribution.joints[input.endJointIndex] = result.endTransform;
			diagnostics.push_back(result.diagnostic);
			return AnimationStatus::Ok;
		}

		AnimationStatus ApplyChain(
			const ProceduralChainNodeInput& input,
			const ProceduralGenerationStamp& generation,
			const GraphPoseContribution& source,
			GraphPoseContribution& contribution,
			std::vector<ProceduralDiagnosticRecord>& diagnostics) {
			if(input.jointIndices.size() < 2U
				|| (!input.jointLimits.empty() && input.jointLimits.size() != input.jointIndices.size())) {
				return AnimationStatus::InvalidArgument;
			}
			std::vector<ChainIkJoint> joints;
			joints.reserve(input.jointIndices.size());
			for(std::size_t index = 0U; index < input.jointIndices.size(); ++index) {
				const std::uint32_t jointIndex = input.jointIndices[index];
				if(!ValidJointIndex(jointIndex, source)) {
					return AnimationStatus::InvalidArgument;
				}
				joints.push_back({source.jointStableIds[jointIndex], source.joints[jointIndex],
					input.jointLimits.empty() ? ProceduralJointLimit{} : input.jointLimits[index]});
			}
			ChainIkRequest request;
			request.generation = generation;
			request.expectedGeneration = generation;
			request.joints = joints;
			request.goalSnapshot = &input.goals;
			request.maximumIterations = input.maximumIterations;
			request.toleranceMeters = input.toleranceMeters;
			ChainIkResult result;
			const AnimationStatus status = SolveChainIk(request, result);
			if(status != AnimationStatus::Ok) {
				return status;
			}
			for(std::size_t index = 0U; index < input.jointIndices.size(); ++index) {
				contribution.joints[input.jointIndices[index]] = result.joints[index].transform;
			}
			diagnostics = std::move(result.diagnostics);
			return AnimationStatus::Ok;
		}

		AnimationStatus ApplyAim(
			const ProceduralAimNodeInput& input,
			bool look,
			float maximumYawRadians,
			float maximumPitchRadians,
			const ProceduralGenerationStamp& generation,
			const GraphPoseContribution& source,
			GraphPoseContribution& contribution,
			std::vector<ProceduralDiagnosticRecord>& diagnostics) {
			if(!ValidJointIndex(input.jointIndex, source)) {
				return AnimationStatus::InvalidArgument;
			}
			AimConstraintRequest aim;
			aim.generation = generation;
			aim.expectedGeneration = generation;
			aim.goalSnapshot = &input.goals;
			aim.transform = source.joints[input.jointIndex];
			aim.localAimAxis = input.localAimAxis;
			aim.localUpAxis = input.localUpAxis;
			aim.worldUp = input.worldUp;
			aim.maximumAngleRadians = input.maximumAngleRadians;
			aim.dampingRate = input.dampingRate;
			aim.deltaSeconds = input.deltaSeconds;
			aim.weight = input.solverWeight;
			TransformConstraintResult result;
			AnimationStatus status = AnimationStatus::InternalError;
			if(look) {
				LookConstraintRequest request;
				request.aim = aim;
				request.maximumYawRadians = maximumYawRadians;
				request.maximumPitchRadians = maximumPitchRadians;
				status = SolveLookConstraint(request, result);
			} else {
				status = SolveAimConstraint(aim, result);
			}
			if(status != AnimationStatus::Ok) {
				return status;
			}
			contribution.joints[input.jointIndex] = result.transform;
			diagnostics.push_back(result.diagnostic);
			return AnimationStatus::Ok;
		}

		AnimationStatus ApplyCopyTransform(
			const ProceduralCopyTransformNodeInput& input,
			const ProceduralGenerationStamp& generation,
			const GraphPoseContribution& source,
			GraphPoseContribution& contribution,
			std::vector<ProceduralDiagnosticRecord>& diagnostics) {
			if(!ValidJointIndex(input.jointIndex, source)
				|| (input.parentJointIndex != kInvalidProceduralJointIndex
					&& !ValidJointIndex(input.parentJointIndex, source))) {
				return AnimationStatus::InvalidArgument;
			}
			CopyTransformConstraintRequest request;
			request.generation = generation;
			request.expectedGeneration = generation;
			request.currentLocalTransform = source.joints[input.jointIndex];
			request.targetTransform = input.targetTransform;
			request.parentWorldTransform = input.parentJointIndex == kInvalidProceduralJointIndex
				? AnimationTransform{} : source.joints[input.parentJointIndex];
			request.targetSpace = input.targetSpace;
			request.translationWeight = input.translationWeight;
			request.rotationWeight = input.rotationWeight;
			request.scaleWeight = input.scaleWeight;
			request.goalId = input.goalId;
			TransformConstraintResult result;
			const AnimationStatus status = ApplyCopyTransformConstraint(request, result);
			if(status != AnimationStatus::Ok) {
				return status;
			}
			contribution.joints[input.jointIndex] = result.transform;
			diagnostics.push_back(result.diagnostic);
			return AnimationStatus::Ok;
		}

		AnimationStatus ApplyJointConstraint(
			const ProceduralJointConstraintNodeInput& input,
			const ProceduralGenerationStamp& generation,
			const GraphPoseContribution& source,
			GraphPoseContribution& contribution,
			std::vector<ProceduralDiagnosticRecord>& diagnostics) {
			if(!ValidJointIndex(input.jointIndex, source)) {
				return AnimationStatus::InvalidArgument;
			}
			JointRotationConstraintRequest request;
			request.generation = generation;
			request.expectedGeneration = generation;
			request.referenceRotation = input.referenceRotation;
			request.candidateRotation = source.joints[input.jointIndex].rotation;
			request.localTwistAxis = input.localTwistAxis;
			request.limit = input.limit;
			request.weight = input.solverWeight;
			request.goalId = input.goalId;
			JointRotationConstraintResult result;
			const AnimationStatus status = ApplyJointRotationConstraint(request, result);
			if(status != AnimationStatus::Ok) {
				return status;
			}
			contribution.joints[input.jointIndex].rotation = result.rotation;
			diagnostics.push_back(result.diagnostic);
			return AnimationStatus::Ok;
		}

		AnimationStatus ApplyTwist(
			const ProceduralTwistNodeInput& input,
			const ProceduralGenerationStamp& generation,
			const GraphPoseContribution& source,
			GraphPoseContribution& contribution,
			std::vector<ProceduralDiagnosticRecord>& diagnostics) {
			if(input.jointIndices.empty()
				|| input.jointIndices.size() != input.distributionWeights.size()) {
				return AnimationStatus::InvalidArgument;
			}
			std::vector<AnimationTransform> transforms;
			transforms.reserve(input.jointIndices.size());
			for(const std::uint32_t jointIndex : input.jointIndices) {
				if(!ValidJointIndex(jointIndex, source)) {
					return AnimationStatus::InvalidArgument;
				}
				transforms.push_back(source.joints[jointIndex]);
			}
			TwistDistributionRequest request;
			request.generation = generation;
			request.expectedGeneration = generation;
			request.transforms = transforms;
			request.distributionWeights = input.distributionWeights;
			request.localTwistAxis = input.localTwistAxis;
			request.twistRadians = input.twistRadians;
			request.weight = input.solverWeight;
			request.goalId = input.goalId;
			TwistDistributionResult result;
			const AnimationStatus status = ApplyTwistDistribution(request, result);
			if(status != AnimationStatus::Ok) {
				return status;
			}
			for(std::size_t index = 0U; index < input.jointIndices.size(); ++index) {
				contribution.joints[input.jointIndices[index]] = result.transforms[index];
			}
			diagnostics = std::move(result.diagnostics);
			return AnimationStatus::Ok;
		}

		AnimationStatus ApplyFootPlacement(
			const ProceduralFootPlacementNodeInput& input,
			const ProceduralGenerationStamp& generation,
			const GraphPoseContribution& source,
			GraphPoseContribution& contribution,
			std::vector<ProceduralDiagnosticRecord>& diagnostics,
			ProceduralNodeStateSnapshot& state) {
			if(!ValidJointIndex(input.pelvisJointIndex, source) || input.feet.empty()
				|| input.feet.size() > kProceduralGoalHardMaximum) {
				return input.feet.size() > kProceduralGoalHardMaximum
					? AnimationStatus::HardLimitExceeded : AnimationStatus::InvalidArgument;
			}
			std::vector<FootPlacementFootInput> feet;
			feet.reserve(input.feet.size());
			for(const ProceduralFootBinding& binding : input.feet) {
				if(!ValidJointIndex(binding.jointIndex, source)) {
					return AnimationStatus::InvalidArgument;
				}
				FootPlacementFootInput foot = binding.input;
				foot.animatedTransform = source.joints[binding.jointIndex];
				feet.push_back(std::move(foot));
			}
			FootPlacementRequest request;
			request.generation = generation;
			request.expectedGeneration = generation;
			request.querySnapshot = &input.queries;
			request.feet = feet;
			request.pelvisTransform = source.joints[input.pelvisJointIndex];
			request.previousPelvisOffsetMeters = input.previousPelvisOffsetMeters;
			request.maximumPelvisDropMeters = input.maximumPelvisDropMeters;
			request.maximumPelvisRaiseMeters = input.maximumPelvisRaiseMeters;
			request.pelvisWeight = input.pelvisWeight;
			request.deltaSeconds = input.deltaSeconds;
			request.releaseSmoothingSeconds = input.releaseSmoothingSeconds;
			request.pelvisSmoothingSeconds = input.pelvisSmoothingSeconds;
			request.maximumQueryAgeTicks = input.maximumQueryAgeTicks;
			FootPlacementResult result;
			const AnimationStatus status = SolveFootPlacement(request, result);
			if(status != AnimationStatus::Ok) {
				return status;
			}
			contribution.joints[input.pelvisJointIndex] = result.pelvisTransform;
			state.pelvisOffsetMeters = result.pelvisOffsetMeters;
			state.footContacts.reserve(result.feet.size());
			for(std::size_t index = 0U; index < result.feet.size(); ++index) {
				contribution.joints[input.feet[index].jointIndex] = result.feet[index].transform;
				state.footContacts.push_back(result.feet[index].contact);
			}
			diagnostics = std::move(result.diagnostics);
			return AnimationStatus::Ok;
		}

		AnimationStatus ApplyPayload(
			const ProceduralGraphNodePayload& payload,
			const ProceduralGenerationStamp& generation,
			const GraphPoseContribution& source,
			GraphPoseContribution& contribution,
			std::vector<ProceduralDiagnosticRecord>& diagnostics,
			ProceduralNodeStateSnapshot& state) {
			return std::visit([&](const auto& operation) -> AnimationStatus {
				using Operation = std::decay_t<decltype(operation)>;
				if constexpr(std::is_same_v<Operation, ProceduralTwoBoneNodeInput>) {
					return ApplyTwoBone(operation, generation, source, contribution, diagnostics);
				} else if constexpr(std::is_same_v<Operation, ProceduralChainNodeInput>) {
					return ApplyChain(operation, generation, source, contribution, diagnostics);
				} else if constexpr(std::is_same_v<Operation, ProceduralAimNodeInput>) {
					return ApplyAim(operation, false, 0.0F, 0.0F,
						generation, source, contribution, diagnostics);
				} else if constexpr(std::is_same_v<Operation, ProceduralLookNodeInput>) {
					return ApplyAim(operation.aim, true, operation.maximumYawRadians,
						operation.maximumPitchRadians, generation, source, contribution, diagnostics);
				} else if constexpr(std::is_same_v<Operation, ProceduralCopyTransformNodeInput>) {
					return ApplyCopyTransform(operation, generation, source, contribution, diagnostics);
				} else if constexpr(std::is_same_v<Operation, ProceduralJointConstraintNodeInput>) {
					return ApplyJointConstraint(operation, generation, source, contribution, diagnostics);
				} else if constexpr(std::is_same_v<Operation, ProceduralTwistNodeInput>) {
					return ApplyTwist(operation, generation, source, contribution, diagnostics);
				} else if constexpr(std::is_same_v<Operation, ProceduralFootPlacementNodeInput>) {
					return ApplyFootPlacement(operation, generation, source, contribution, diagnostics, state);
				}
				return AnimationStatus::Unsupported;
			}, payload);
		}

		ProceduralSolverState AggregateState(
			std::span<const ProceduralDiagnosticRecord> diagnostics,
			bool hasNodes) noexcept {
			bool contributed = false;
			bool converged = false;
			bool clamped = false;
			bool unreachable = false;
			bool singular = false;
			bool maxIterations = false;
			for(const ProceduralDiagnosticRecord& diagnostic : diagnostics) {
				contributed = contributed || diagnostic.finalContribution > 0.0F;
				converged = converged || diagnostic.converged;
				clamped = clamped || diagnostic.clamped;
				unreachable = unreachable || diagnostic.unreachable;
				singular = singular || diagnostic.singular;
				maxIterations = maxIterations
					|| diagnostic.state == ProceduralSolverState::MaxIterations;
			}
			if(singular) {
				return ProceduralSolverState::Singular;
			}
			if(unreachable) {
				return ProceduralSolverState::Unreachable;
			}
			if(maxIterations) {
				return ProceduralSolverState::MaxIterations;
			}
			if(clamped) {
				return ProceduralSolverState::Clamped;
			}
			if(converged) {
				return ProceduralSolverState::Converged;
			}
			return hasNodes && contributed
				? ProceduralSolverState::Solved : ProceduralSolverState::NoContribution;
		}

		bool ValidFootContact(const FootContactState& contact) noexcept {
			if(contact.stableFootId == 0U || !IsFinite(contact.lockedTransform)
				|| !IsNormalized(contact.lockedTransform.rotation, 0.01F)
				|| !IsFinite(contact.surfaceNormal) || !std::isfinite(contact.contribution)
				|| contact.contribution < 0.0F || contact.contribution > 1.0F
				|| contact.phase > FootContactPhase::Releasing) {
				return false;
			}
			return contact.phase == FootContactPhase::Released || contact.target.IsValid();
		}

		AnimationStatus ValidateSnapshotState(const ProceduralPoseSnapshot& snapshot) noexcept {
			if(snapshot.orderedNodeIds.size() > static_cast<std::size_t>(
				GetAnimationRuntimeLimit(AnimationLimit::ControllerNodes).hardMaximum)
				|| snapshot.nodeStates.size() > snapshot.orderedNodeIds.size()) {
				return AnimationStatus::HardLimitExceeded;
			}
			for(std::size_t index = 0U; index < snapshot.orderedNodeIds.size(); ++index) {
				if(snapshot.orderedNodeIds[index] == 0U) {
					return AnimationStatus::InvalidArgument;
				}
				for(std::size_t previous = 0U; previous < index; ++previous) {
					if(snapshot.orderedNodeIds[previous] == snapshot.orderedNodeIds[index]) {
						return AnimationStatus::InvalidArgument;
					}
				}
			}
			for(std::size_t index = 0U; index < snapshot.nodeStates.size(); ++index) {
				const ProceduralNodeStateSnapshot& state = snapshot.nodeStates[index];
				if(state.stableNodeId == 0U || !std::isfinite(state.pelvisOffsetMeters)
					|| state.footContacts.size() > kProceduralGoalHardMaximum
					|| std::find(snapshot.orderedNodeIds.begin(), snapshot.orderedNodeIds.end(),
						state.stableNodeId) == snapshot.orderedNodeIds.end()) {
					return AnimationStatus::InvalidArgument;
				}
				for(std::size_t previous = 0U; previous < index; ++previous) {
					if(snapshot.nodeStates[previous].stableNodeId == state.stableNodeId) {
						return AnimationStatus::InvalidArgument;
					}
				}
				for(std::size_t footIndex = 0U; footIndex < state.footContacts.size(); ++footIndex) {
					if(!ValidFootContact(state.footContacts[footIndex])) {
						return AnimationStatus::InvalidArgument;
					}
					for(std::size_t previous = 0U; previous < footIndex; ++previous) {
						if(state.footContacts[previous].stableFootId
							== state.footContacts[footIndex].stableFootId) {
							return AnimationStatus::InvalidArgument;
						}
					}
				}
			}
			return AnimationStatus::Ok;
		}
	}

	AnimationStatus EvaluateProceduralGraphNodes(
		const ProceduralGraphEvaluationRequest& request,
		ProceduralGraphEvaluationResult& output) noexcept {
		if(request.inputPose == nullptr || !request.generation.IsValid()
			|| !request.expectedGeneration.IsValid()
			|| request.authority > ProceduralGraphAuthority::CpuAuthoritative) {
			return AnimationStatus::InvalidArgument;
		}
		if(!IsCurrentProceduralGeneration(request.generation, request.expectedGeneration)) {
			return AnimationStatus::StaleHandle;
		}
		if(request.nodes.size() > static_cast<std::size_t>(
			GetAnimationRuntimeLimit(AnimationLimit::ControllerNodes).hardMaximum)) {
			return AnimationStatus::HardLimitExceeded;
		}
		if(request.authority == ProceduralGraphAuthority::VisualOnly
			&& (request.authoritativeConsumer || request.recordingRequested || request.rollbackRequested)) {
			return AnimationStatus::DeterminismViolation;
		}
		const AnimationStatus poseStatus = ValidateGraphPoseContribution(*request.inputPose);
		if(poseStatus != AnimationStatus::Ok) {
			return poseStatus;
		}

		try {
			std::vector<const ProceduralGraphNodeInput*> ordered;
			ordered.reserve(request.nodes.size());
			for(const ProceduralGraphNodeInput& node : request.nodes) {
				if(node.stableId == 0U || node.layerMode > AnimationGraphLayerMode::Additive
					|| !ValidWeight(node.controllerWeight) || !ValidWeight(node.nodeWeight)
					|| !ValidWeight(node.layerWeight)) {
					return AnimationStatus::InvalidArgument;
				}
				if(node.mask != nullptr) {
					const AnimationStatus maskStatus = ValidateAnimationMaskAsset(*node.mask);
					if(maskStatus != AnimationStatus::Ok) {
						return maskStatus;
					}
					if(node.mask->compatibilityFingerprint
						!= request.inputPose->compatibilityFingerprint) {
						return AnimationStatus::IncompatibleAsset;
					}
				}
				if(node.layerMode == AnimationGraphLayerMode::Additive
					&& (node.additiveReference == nullptr
						|| !CompatiblePoseLayout(*request.inputPose, *node.additiveReference))) {
					return node.additiveReference == nullptr
						? AnimationStatus::InvalidArgument : AnimationStatus::IncompatibleAsset;
				}
				ordered.push_back(&node);
			}
			std::sort(ordered.begin(), ordered.end(),
				[](const ProceduralGraphNodeInput* left, const ProceduralGraphNodeInput* right) {
					return std::tie(left->layerOrder, left->nodeOrder, left->stableId)
						< std::tie(right->layerOrder, right->nodeOrder, right->stableId);
				});
			std::vector<std::uint64_t> stableIds;
			stableIds.reserve(ordered.size());
			for(const ProceduralGraphNodeInput* node : ordered) {
				stableIds.push_back(node->stableId);
			}
			std::sort(stableIds.begin(), stableIds.end());
			for(std::size_t index = 1U; index < stableIds.size(); ++index) {
				if(stableIds[index - 1U] == stableIds[index]) {
					return AnimationStatus::InvalidArgument;
				}
			}

			ProceduralGraphEvaluationResult candidate;
			candidate.header.generation = request.expectedGeneration;
			candidate.header.status = AnimationStatus::Ok;
			candidate.pose = *request.inputPose;
			candidate.cpuAuthoritative = request.authority
				== ProceduralGraphAuthority::CpuAuthoritative;
			candidate.orderedNodeIds.reserve(ordered.size());
			std::vector<std::uint32_t> affectedJoints;
			for(const ProceduralGraphNodeInput* node : ordered) {
				const AnimationStatus affectedStatus = GatherAffectedJoints(node->payload, affectedJoints);
				if(affectedStatus != AnimationStatus::Ok) {
					return affectedStatus;
				}
				const float maskContribution = MaximumMaskContribution(
					node->mask, affectedJoints, candidate.pose);
				if(maskContribution < 0.0F || !ValidWeight(maskContribution)) {
					return AnimationStatus::InvalidArgument;
				}
				const float graphContribution = node->controllerWeight * node->nodeWeight
					* node->layerWeight * maskContribution;
				if(!ValidWeight(graphContribution)) {
					return AnimationStatus::InvalidArgument;
				}

				const GraphPoseContribution& solverSource = node->layerMode
					== AnimationGraphLayerMode::Additive
					? *node->additiveReference : candidate.pose;
				GraphPoseContribution proceduralContribution = solverSource;
				std::vector<ProceduralDiagnosticRecord> nodeDiagnostics;
				ProceduralNodeStateSnapshot nodeState;
				nodeState.stableNodeId = node->stableId;
				const AnimationStatus solverStatus = ApplyPayload(node->payload,
					request.expectedGeneration, solverSource, proceduralContribution,
					nodeDiagnostics, nodeState);
				if(solverStatus != AnimationStatus::Ok) {
					return solverStatus;
				}

				GraphLayerInput layer;
				layer.stableId = node->stableId;
				layer.mode = node->layerMode;
				layer.controllerWeight = node->controllerWeight;
				layer.nodeWeight = node->nodeWeight;
				layer.layerWeight = node->layerWeight;
				layer.mask = node->mask;
				layer.contribution = &proceduralContribution;
				layer.additiveReference = node->additiveReference;
				GraphPoseContribution composed;
				const AnimationStatus layerStatus = EvaluateAnimationLayers(
					candidate.pose, std::span{&layer, 1U}, composed);
				if(layerStatus != AnimationStatus::Ok) {
					return layerStatus;
				}
				const AnimationStatus diagnosticStatus = AppendDiagnostics(
					candidate.diagnostics, nodeDiagnostics, graphContribution);
				if(diagnosticStatus != AnimationStatus::Ok) {
					return diagnosticStatus;
				}
				candidate.pose = std::move(composed);
				candidate.orderedNodeIds.push_back(node->stableId);
				if(!nodeState.footContacts.empty() && graphContribution > 0.0F) {
					candidate.nodeStates.push_back(std::move(nodeState));
				}
			}

			candidate.header.state = AggregateState(candidate.diagnostics, !ordered.empty());
			if(request.recordingRequested || request.rollbackRequested) {
				ProceduralPoseSnapshot snapshot;
				snapshot.generation = request.expectedGeneration;
				snapshot.compatibilityFingerprint = candidate.pose.compatibilityFingerprint;
				snapshot.pose = candidate.pose;
				snapshot.orderedNodeIds = candidate.orderedNodeIds;
				snapshot.nodeStates = candidate.nodeStates;
				if(request.recordingRequested) {
					candidate.recordingSnapshot = snapshot;
				}
				if(request.rollbackRequested) {
					candidate.rollbackSnapshot = std::move(snapshot);
				}
			}
			output = std::move(candidate);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus RestoreProceduralPoseSnapshot(
		const ProceduralPoseSnapshot& snapshot,
		const ProceduralGenerationStamp& expectedGeneration,
		AnimationCompatibilityFingerprint expectedCompatibilityFingerprint,
		GraphPoseContribution& outputPose,
		std::vector<ProceduralNodeStateSnapshot>& outputNodeStates) noexcept {
		if(snapshot.schemaVersion != 1U || !expectedGeneration.IsValid()
			|| expectedCompatibilityFingerprint == 0U) {
			return AnimationStatus::InvalidArgument;
		}
		if(!IsCurrentProceduralGeneration(snapshot.generation, expectedGeneration)) {
			return AnimationStatus::StaleHandle;
		}
		if(snapshot.compatibilityFingerprint != expectedCompatibilityFingerprint
			|| snapshot.pose.compatibilityFingerprint != expectedCompatibilityFingerprint) {
			return AnimationStatus::IncompatibleAsset;
		}
		const AnimationStatus poseStatus = ValidateGraphPoseContribution(snapshot.pose);
		if(poseStatus != AnimationStatus::Ok) {
			return poseStatus;
		}
		const AnimationStatus stateStatus = ValidateSnapshotState(snapshot);
		if(stateStatus != AnimationStatus::Ok) {
			return stateStatus;
		}
		try {
			GraphPoseContribution stagedPose = snapshot.pose;
			std::vector<ProceduralNodeStateSnapshot> stagedStates = snapshot.nodeStates;
			outputPose = std::move(stagedPose);
			outputNodeStates = std::move(stagedStates);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}
}
