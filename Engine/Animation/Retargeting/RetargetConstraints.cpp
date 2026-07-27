// RetargetConstraints.cpp - Copied root/contact/end-effector preservation pass.

#include "Animation/Retargeting/RetargetConstraints.h"

#include "Animation/Procedural/ProceduralTypes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <optional>

namespace TerraEngine::Animation {
	namespace {
		std::optional<std::uint32_t> FindJointIndex(
			const SkeletonAsset& skeleton,
			std::uint64_t stableJointId) noexcept {
			for(std::size_t index = 0U; index < skeleton.joints.size(); ++index) {
				if(skeleton.joints[index].stableId == stableJointId) {
					return static_cast<std::uint32_t>(index);
				}
			}
			return std::nullopt;
		}

		const RetargetChainMapping* FindChain(
			const RetargetProfileAsset& profile,
			std::uint64_t chainId) noexcept {
			const auto iterator = std::find_if(profile.chainMappings.begin(),
				profile.chainMappings.end(), [chainId](const RetargetChainMapping& chain) {
					return chain.chainId == chainId;
				});
			return iterator != profile.chainMappings.end() ? &*iterator : nullptr;
		}

		const RetargetContactTarget* FindContactTarget(
			const RetargetConstraintSnapshot* snapshot,
			std::uint64_t contactId) noexcept {
			if(snapshot == nullptr) {
				return nullptr;
			}
			const auto iterator = std::find_if(snapshot->contacts.begin(),
				snapshot->contacts.end(), [contactId](const RetargetContactTarget& target) {
					return target.contactId == contactId;
				});
			return iterator != snapshot->contacts.end() ? &*iterator : nullptr;
		}

		bool AllFinite(std::span<const AnimationTransform> transforms) noexcept {
			return std::all_of(transforms.begin(), transforms.end(),
				[](const AnimationTransform& transform) {
					return IsFinite(transform)
						&& IsNormalized(transform.rotation, 0.01F);
				});
		}

		float Distance(AnimationFloat3 left, AnimationFloat3 right) noexcept {
			return ProceduralMath::Length(ProceduralMath::Subtract(left, right));
		}

		float ResolveProportionScale(const RetargetProfileAsset& profile) noexcept {
			float scale = 1.0F;
			switch(profile.proportion.translationScaleMode) {
			case RetargetTranslationScaleMode::None:
				scale = 1.0F;
				break;
			case RetargetTranslationScaleMode::UniformHeight:
			case RetargetTranslationScaleMode::PerChainLength:
				scale = profile.proportion.targetReferenceHeightMeters
					/ profile.proportion.sourceReferenceHeightMeters;
				break;
			case RetargetTranslationScaleMode::Authored:
				scale = profile.proportion.authoredTranslationScale;
				break;
			}
			return std::clamp(scale, profile.proportion.minimumScale,
				profile.proportion.maximumScale);
		}

		bool RebuildModels(
			const SkeletonAsset& skeleton,
			std::span<const AnimationTransform> local,
			std::vector<AnimationTransform>& model) noexcept {
			if(local.size() != skeleton.joints.size() || model.size() != local.size()) {
				return false;
			}
			for(std::size_t index = 0U; index < local.size(); ++index) {
				const std::int32_t parentIndex = skeleton.joints[index].parentIndex;
				if(parentIndex < 0) {
					model[index] = local[index];
				} else if(static_cast<std::size_t>(parentIndex) < index) {
					model[index] = ProceduralMath::Combine(
						model[static_cast<std::size_t>(parentIndex)], local[index]);
				} else {
					return false;
				}
				if(!IsFinite(model[index]) || !IsNormalized(model[index].rotation, 0.01F)) {
					return false;
				}
			}
			return true;
		}

		bool SetModelTransform(
			const SkeletonAsset& skeleton,
			std::uint32_t jointIndex,
			const AnimationTransform& desiredModel,
			std::vector<AnimationTransform>& local,
			std::vector<AnimationTransform>& model) noexcept {
			if(jointIndex >= local.size() || !IsFinite(desiredModel)
				|| !IsNormalized(desiredModel.rotation, 0.01F)) {
				return false;
			}
			const std::int32_t parentIndex = skeleton.joints[jointIndex].parentIndex;
			AnimationTransform desiredLocal = desiredModel;
			if(parentIndex >= 0
				&& !ProceduralMath::TryRelativeTransform(
					model[static_cast<std::size_t>(parentIndex)], desiredModel, desiredLocal)) {
				return false;
			}
			local[jointIndex] = desiredLocal;
			for(std::size_t index = jointIndex; index < local.size(); ++index) {
				const std::int32_t parent = skeleton.joints[index].parentIndex;
				model[index] = parent < 0
					? local[index]
					: ProceduralMath::Combine(model[static_cast<std::size_t>(parent)], local[index]);
				if(!IsFinite(model[index]) || !IsNormalized(model[index].rotation, 0.01F)) {
					return false;
				}
			}
			return true;
		}

		float QuaternionAngle(
			AnimationQuaternion left,
			AnimationQuaternion right) noexcept {
			AnimationQuaternion normalizedLeft;
			AnimationQuaternion normalizedRight;
			if(!ProceduralMath::TryNormalize(left, normalizedLeft)
				|| !ProceduralMath::TryNormalize(right, normalizedRight)) {
				return std::numeric_limits<float>::infinity();
			}
			const float dot = std::clamp(std::abs(ProceduralMath::Dot(
				normalizedLeft, normalizedRight)), 0.0F, 1.0F);
			return 2.0F * std::acos(dot);
		}

		float ChainReach(
			const RetargetProfileAsset& profile,
			const SkeletonAsset& skeleton,
			std::uint64_t chainId,
			std::span<const AnimationTransform> model,
			std::optional<std::uint32_t>& rootIndex) noexcept {
			const RetargetChainMapping* chain = FindChain(profile, chainId);
			if(chain == nullptr || chain->targetJointIds.empty()) {
				return std::numeric_limits<float>::infinity();
			}
			float reach = 0.0F;
			std::optional<std::uint32_t> prior;
			for(const std::uint64_t jointId : chain->targetJointIds) {
				const std::optional<std::uint32_t> index = FindJointIndex(skeleton, jointId);
				if(!index.has_value() || *index >= model.size()) {
					return -1.0F;
				}
				if(!rootIndex.has_value()) {
					rootIndex = index;
				}
				if(prior.has_value()) {
					// Chain mappings may skip target-only twist/detail joints. Walk the
					// actual parent path so reachable distance still includes every bone.
					std::uint32_t current = *index;
					bool foundPrior = false;
					for(std::size_t depth = 0U; depth <= skeleton.joints.size(); ++depth) {
						if(current == *prior) {
							foundPrior = true;
							break;
						}
						const std::int32_t parent = skeleton.joints[current].parentIndex;
						if(parent < 0
							|| static_cast<std::size_t>(parent) >= skeleton.joints.size()) {
							break;
						}
						reach += Distance(model[current].translation,
							model[static_cast<std::size_t>(parent)].translation);
						current = static_cast<std::uint32_t>(parent);
					}
					if(!foundPrior) {
						return -1.0F;
					}
				}
				prior = index;
			}
			return reach;
		}

		AnimationStatus ValidateRequest(const RetargetConstraintRequest& request) noexcept {
			if(request.profile == nullptr || request.sourceSkeleton == nullptr
				|| request.targetSkeleton == nullptr || !request.generation.IsValid()
				|| request.generation != request.expectedGeneration) {
				return request.generation.IsValid()
					&& request.expectedGeneration.IsValid()
					? AnimationStatus::StaleHandle
					: AnimationStatus::InvalidArgument;
			}
			const AnimationStatus profileShape =
				ValidateRetargetProfileAssetStructure(*request.profile);
			if(profileShape != AnimationStatus::Ok) {
				return profileShape;
			}
			if(request.generation.profile.assetId != request.profile->assetId
				|| (request.profile->runtimeGeneration != 0U
					&& request.generation.profile.generation
						!= request.profile->runtimeGeneration)
				|| request.generation.sourceSkeleton.assetId
					!= request.sourceSkeleton->assetId
				|| request.generation.targetSkeleton.assetId
					!= request.targetSkeleton->assetId
				|| (request.sourceSkeleton->runtimeGeneration != 0U
					&& request.generation.sourceSkeleton.generation
						!= request.sourceSkeleton->runtimeGeneration)
				|| (request.targetSkeleton->runtimeGeneration != 0U
					&& request.generation.targetSkeleton.generation
						!= request.targetSkeleton->runtimeGeneration)) {
				return AnimationStatus::StaleHandle;
			}
			if(request.profile->sourceSkeletonAssetId
					!= request.sourceSkeleton->assetId
				|| request.profile->targetSkeletonAssetId
					!= request.targetSkeleton->assetId
				|| request.profile->sourceSkeletonFingerprint
					!= request.sourceSkeleton->compatibilityFingerprint
				|| request.profile->targetSkeletonFingerprint
					!= request.targetSkeleton->compatibilityFingerprint
				|| request.profile->sourceReferencePoseHash
					!= request.sourceSkeleton->referencePoseHash
				|| request.profile->targetReferencePoseHash
					!= request.targetSkeleton->referencePoseHash) {
				return AnimationStatus::IncompatibleAsset;
			}
			if(request.sourceReferenceModelTransforms.size()
					!= request.sourceSkeleton->joints.size()
				|| request.sourceModelTransforms.size()
					!= request.sourceSkeleton->joints.size()
				|| request.targetReferenceModelTransforms.size()
					!= request.targetSkeleton->joints.size()
				|| request.inputTargetLocalTransforms.size()
					!= request.targetSkeleton->joints.size()
				|| request.inputTargetModelTransforms.size()
					!= request.targetSkeleton->joints.size()
				|| !AllFinite(request.sourceReferenceModelTransforms)
				|| !AllFinite(request.sourceModelTransforms)
				|| !AllFinite(request.targetReferenceModelTransforms)
				|| !AllFinite(request.inputTargetLocalTransforms)
				|| !AllFinite(request.inputTargetModelTransforms)) {
				return AnimationStatus::InvalidArgument;
			}
			if(request.snapshot != nullptr) {
				const std::size_t maximum = static_cast<std::size_t>(
					GetAnimationRuntimeLimit(AnimationLimit::AnimatorProceduralGoals).hardMaximum);
				if(request.snapshot->generation != request.generation) {
					return AnimationStatus::StaleHandle;
				}
				// The generation tick is copied from the source pose before workers
				// start. A snapshot for another tick must never silently retime goals.
				if(request.snapshot->sourceTick != request.generation.tick) {
					return AnimationStatus::InvalidArgument;
				}
				if(request.snapshot->contacts.size() > maximum
					|| request.snapshot->endEffectors.size() > maximum) {
					return AnimationStatus::HardLimitExceeded;
				}
				if(!std::isfinite(request.snapshot->normalizedTime)) {
					return AnimationStatus::InvalidArgument;
				}
				for(std::size_t index = 0U; index < request.snapshot->contacts.size();
					++index) {
					const RetargetContactTarget& contact =
						request.snapshot->contacts[index];
					if(contact.contactId == 0U || !IsFinite(contact.positionModel)
						|| !IsFinite(contact.normalModel)
						|| (contact.hasExplicitNormal
							&& ProceduralMath::Length(contact.normalModel) <= 1.0e-6F)) {
						return AnimationStatus::InvalidArgument;
					}
					for(std::size_t prior = 0U; prior < index; ++prior) {
						if(request.snapshot->contacts[prior].contactId
							== contact.contactId) {
							return AnimationStatus::InvalidArgument;
						}
					}
				}
				for(std::size_t index = 0U;
					index < request.snapshot->endEffectors.size(); ++index) {
					const RetargetEndEffectorTarget& target =
						request.snapshot->endEffectors[index];
					if(target.chainId == 0U || target.targetJointId == 0U
						|| !IsFinite(target.positionModel)) {
						return AnimationStatus::InvalidArgument;
					}
					for(std::size_t prior = 0U; prior < index; ++prior) {
						const RetargetEndEffectorTarget& priorTarget =
							request.snapshot->endEffectors[prior];
						if(priorTarget.chainId == target.chainId
							&& priorTarget.targetJointId == target.targetJointId) {
							return AnimationStatus::InvalidArgument;
						}
					}
				}
			}
			return AnimationStatus::Ok;
		}
	}

	AnimationStatus ApplyRetargetConstraints(
		const RetargetConstraintRequest& request,
		RetargetConstraintResult& output) noexcept {
		try {
			const AnimationStatus validation = ValidateRequest(request);
			if(validation != AnimationStatus::Ok) {
				return validation;
			}
			RetargetConstraintResult staged;
			staged.status = AnimationStatus::Ok;
			staged.generation = request.generation;
			staged.sourceTick = request.snapshot != nullptr
				? request.snapshot->sourceTick : request.generation.tick;
			staged.normalizedTime = request.snapshot != nullptr
				? request.snapshot->normalizedTime : 0.0F;
			staged.localTransforms.assign(request.inputTargetLocalTransforms.begin(),
				request.inputTargetLocalTransforms.end());
			staged.modelTransforms.assign(request.inputTargetModelTransforms.begin(),
				request.inputTargetModelTransforms.end());
			staged.metrics.timingPreserved = true;

			const RetargetProfileAsset& profile = *request.profile;
			const SkeletonAsset& sourceSkeleton = *request.sourceSkeleton;
			const SkeletonAsset& targetSkeleton = *request.targetSkeleton;
			const std::optional<std::uint32_t> sourceRootIndex = FindJointIndex(
				sourceSkeleton, profile.root.sourceRootJointId);
			const std::optional<std::uint32_t> targetRootIndex = FindJointIndex(
				targetSkeleton, profile.root.targetRootJointId);
			if(!sourceRootIndex.has_value() || !targetRootIndex.has_value()) {
				return AnimationStatus::IncompatibleAsset;
			}

			const AnimationTransform& sourceReferenceRoot =
				request.sourceReferenceModelTransforms[*sourceRootIndex];
			const AnimationTransform& sourceRoot = request.sourceModelTransforms[*sourceRootIndex];
			const AnimationTransform& targetReferenceRoot =
				request.targetReferenceModelTransforms[*targetRootIndex];
			AnimationTransform desiredTargetRoot = staged.modelTransforms[*targetRootIndex];
			const float proportionScale = ResolveProportionScale(profile);
			AnimationFloat3 sourceDisplacement = ProceduralMath::Subtract(
				sourceRoot.translation, sourceReferenceRoot.translation);
			float displacementScale = profile.root.displacementScale;
			if(profile.root.motionMode == RetargetRootMotionMode::ScaleByProportion) {
				displacementScale *= proportionScale;
			}
			if(profile.root.motionMode == RetargetRootMotionMode::InPlace
				|| profile.trajectory.mode == RetargetTrajectoryMode::Ignore
				|| profile.trajectory.mode == RetargetTrajectoryMode::PreserveFacing) {
				sourceDisplacement = {};
			}
			sourceDisplacement.x *= displacementScale * profile.trajectory.horizontalScale;
			sourceDisplacement.z *= displacementScale * profile.trajectory.horizontalScale;
			sourceDisplacement.y *= displacementScale * profile.trajectory.verticalScale;
			if(!profile.root.preserveVerticalDisplacement) {
				sourceDisplacement.y = 0.0F;
			}
			desiredTargetRoot.translation = ProceduralMath::Add(
				targetReferenceRoot.translation, sourceDisplacement);

			const bool preserveFacing = profile.root.preserveFacing
				&& profile.trajectory.mode != RetargetTrajectoryMode::Ignore
				&& profile.trajectory.mode != RetargetTrajectoryMode::PreserveDisplacement;
			if(preserveFacing) {
				const AnimationQuaternion sourceDelta = ProceduralMath::Multiply(
					sourceRoot.rotation, ProceduralMath::Conjugate(sourceReferenceRoot.rotation));
				const AnimationQuaternion basis = ProceduralMath::FromTo(
					profile.trajectory.sourceForwardAxis, profile.trajectory.targetForwardAxis);
				const AnimationQuaternion alignedDelta = ProceduralMath::Multiply(
					ProceduralMath::Multiply(basis, sourceDelta),
					ProceduralMath::Conjugate(basis));
				desiredTargetRoot.rotation = ProceduralMath::Multiply(
					ProceduralMath::AxisAngle({0.0F, 1.0F, 0.0F},
						profile.root.yawOffsetRadians),
					ProceduralMath::Multiply(alignedDelta, targetReferenceRoot.rotation));
			} else {
				desiredTargetRoot.rotation = targetReferenceRoot.rotation;
			}
			if(!SetModelTransform(targetSkeleton, *targetRootIndex, desiredTargetRoot,
				staged.localTransforms, staged.modelTransforms)) {
				return AnimationStatus::Rejected;
			}
			staged.metrics.rootDisplacementErrorMeters = Distance(
				staged.modelTransforms[*targetRootIndex].translation,
				desiredTargetRoot.translation);
			staged.metrics.facingErrorRadians = QuaternionAngle(
				staged.modelTransforms[*targetRootIndex].rotation,
				desiredTargetRoot.rotation);
			staged.metrics.rootDisplacementPreserved =
				staged.metrics.rootDisplacementErrorMeters
					<= profile.quality.rootDisplacementToleranceMeters;
			staged.metrics.facingPreserved = !preserveFacing
				|| staged.metrics.facingErrorRadians <= profile.quality.facingToleranceRadians;

			std::vector<RetargetContactRule> contacts = profile.contacts;
			bool requiredContactFailed = false;
			std::sort(contacts.begin(), contacts.end(),
				[](const RetargetContactRule& left, const RetargetContactRule& right) {
					return left.contactId < right.contactId;
				});
			for(const RetargetContactRule& contact : contacts) {
				const RetargetContactTarget* target = FindContactTarget(
					request.snapshot, contact.contactId);
				if(target != nullptr && !target->active) {
					continue;
				}
				const std::optional<std::uint32_t> sourceIndex = FindJointIndex(
					sourceSkeleton, contact.sourceJointId);
				const std::optional<std::uint32_t> targetIndex = FindJointIndex(
					targetSkeleton, contact.targetJointId);
				if(!sourceIndex.has_value() || !targetIndex.has_value()) {
					++staged.metrics.fallbackCount;
					staged.reducedFidelity = true;
					requiredContactFailed = requiredContactFailed || contact.required;
					continue;
				}
				AnimationFloat3 desiredPosition = ProceduralMath::Add(
					request.targetReferenceModelTransforms[*targetIndex].translation,
					ProceduralMath::Scale(ProceduralMath::Subtract(
						request.sourceModelTransforms[*sourceIndex].translation,
						request.sourceReferenceModelTransforms[*sourceIndex].translation),
						proportionScale));
				if(target != nullptr && target->hasExplicitPosition) {
					desiredPosition = target->positionModel;
				}
				std::optional<std::uint32_t> chainRoot;
				const float reach = ChainReach(profile, targetSkeleton,
					contact.targetChainId, staged.modelTransforms, chainRoot);
				const bool reachable = reach == std::numeric_limits<float>::infinity()
					|| (reach >= 0.0F && chainRoot.has_value()
						&& Distance(staged.modelTransforms[*chainRoot].translation,
							desiredPosition) <= reach + contact.toleranceMeters);
				if(!reachable) {
					++staged.metrics.fallbackCount;
					staged.reducedFidelity = true;
					requiredContactFailed = requiredContactFailed || contact.required;
					continue;
				}
				AnimationTransform desired = staged.modelTransforms[*targetIndex];
				if(contact.preservePosition) {
					desired.translation = desiredPosition;
				}
				if(contact.preserveOrientation && target != nullptr
					&& target->hasExplicitNormal) {
					desired.rotation = ProceduralMath::Multiply(
						ProceduralMath::FromTo({0.0F, 1.0F, 0.0F}, target->normalModel),
						desired.rotation);
				}
				if(!SetModelTransform(targetSkeleton, *targetIndex, desired,
					staged.localTransforms, staged.modelTransforms)) {
					return AnimationStatus::Rejected;
				}
				const float error = Distance(staged.modelTransforms[*targetIndex].translation,
					desiredPosition);
				staged.metrics.maximumContactErrorMeters = std::max(
					staged.metrics.maximumContactErrorMeters, error);
				++staged.metrics.appliedContactCount;
			}

			bool requestedEndEffectorFailed = false;
			if(request.snapshot != nullptr) {
				std::vector<RetargetEndEffectorTarget> targets = request.snapshot->endEffectors;
				std::sort(targets.begin(), targets.end(),
					[](const RetargetEndEffectorTarget& left,
						const RetargetEndEffectorTarget& right) {
						return left.chainId != right.chainId
							? left.chainId < right.chainId
							: left.targetJointId < right.targetJointId;
					});
				for(const RetargetEndEffectorTarget& target : targets) {
					if(!target.active) {
						continue;
					}
					const std::optional<std::uint32_t> targetIndex = FindJointIndex(
						targetSkeleton, target.targetJointId);
					std::optional<std::uint32_t> chainRoot;
					const float reach = ChainReach(profile, targetSkeleton,
						target.chainId, staged.modelTransforms, chainRoot);
					if(!targetIndex.has_value() || reach < 0.0F || !chainRoot.has_value()
						|| Distance(staged.modelTransforms[*chainRoot].translation,
							target.positionModel) > reach
							+ profile.quality.reachableEndEffectorToleranceMeters) {
						++staged.metrics.fallbackCount;
						staged.reducedFidelity = true;
						requestedEndEffectorFailed = true;
						continue;
					}
					AnimationTransform desired = staged.modelTransforms[*targetIndex];
					desired.translation = target.positionModel;
					if(!SetModelTransform(targetSkeleton, *targetIndex, desired,
						staged.localTransforms, staged.modelTransforms)) {
						return AnimationStatus::Rejected;
					}
					const float error = Distance(staged.modelTransforms[*targetIndex].translation,
						target.positionModel);
					staged.metrics.maximumEndEffectorErrorMeters = std::max(
						staged.metrics.maximumEndEffectorErrorMeters, error);
					++staged.metrics.appliedEndEffectorCount;
				}
			}

			staged.metrics.contactsPreserved = !requiredContactFailed
				&& staged.metrics.maximumContactErrorMeters
					<= kRetargetDefaultContactToleranceMeters;
			staged.metrics.reachableEndEffectorsPreserved =
				!requestedEndEffectorFailed
				&& staged.metrics.maximumEndEffectorErrorMeters
					<= profile.quality.reachableEndEffectorToleranceMeters;
			if(!AllFinite(staged.localTransforms) || !AllFinite(staged.modelTransforms)) {
				return AnimationStatus::Rejected;
			}
			output = std::move(staged);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}
}
