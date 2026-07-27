// RetargetEvaluator.cpp - Finite deterministic local/model retarget evaluation.

#include "Animation/Retargeting/RetargetEvaluator.h"

#include "Animation/Procedural/ProceduralTypes.h"

#include <algorithm>
#include <bit>
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

		std::optional<std::uint32_t> FindSemanticJoint(
			const SkeletonAsset& skeleton,
			const std::string& semantic) noexcept {
			std::optional<std::uint32_t> result;
			for(std::size_t index = 0U; index < skeleton.joints.size(); ++index) {
				if(skeleton.joints[index].semanticRole != semantic) {
					continue;
				}
				if(result.has_value()) {
					return std::nullopt;
				}
				result = static_cast<std::uint32_t>(index);
			}
			return result;
		}

		bool AllFinite(std::span<const AnimationTransform> transforms) noexcept {
			return std::all_of(transforms.begin(), transforms.end(),
				[](const AnimationTransform& transform) {
					return IsFinite(transform)
						&& IsNormalized(transform.rotation, 0.01F);
				});
		}

		bool BuildModels(
			const SkeletonAsset& skeleton,
			std::span<const AnimationTransform> local,
			std::vector<AnimationTransform>& model) noexcept {
			if(local.size() != skeleton.joints.size() || model.size() != local.size()) {
				return false;
			}
			for(std::size_t index = 0U; index < local.size(); ++index) {
				const std::int32_t parent = skeleton.joints[index].parentIndex;
				if(parent < 0) {
					model[index] = local[index];
				} else if(static_cast<std::size_t>(parent) < index) {
					model[index] = ProceduralMath::Combine(
						model[static_cast<std::size_t>(parent)], local[index]);
				} else {
					return false;
				}
				if(!IsFinite(model[index]) || !IsNormalized(model[index].rotation, 0.01F)) {
					return false;
				}
			}
			return true;
		}

		float Distance(AnimationFloat3 left, AnimationFloat3 right) noexcept {
			return ProceduralMath::Length(ProceduralMath::Subtract(left, right));
		}

		float ChainLength(
			std::span<const std::uint32_t> joints,
			std::span<const AnimationTransform> model) noexcept {
			float length = 0.0F;
			for(std::size_t index = 1U; index < joints.size(); ++index) {
				if(joints[index - 1U] >= model.size() || joints[index] >= model.size()) {
					return 0.0F;
				}
				length += Distance(model[joints[index - 1U]].translation,
					model[joints[index]].translation);
			}
			return length;
		}

		float GlobalScale(const RetargetProfileAsset& profile) noexcept {
			float scale = 1.0F;
			switch(profile.proportion.translationScaleMode) {
			case RetargetTranslationScaleMode::None:
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

		float MappingScale(
			const RetargetCompiledProfile& profile,
			const RetargetCompiledJointMapping& mapping,
			std::span<const AnimationTransform> sourceReferenceModel,
			std::span<const AnimationTransform> targetReferenceModel) noexcept {
			float scale = GlobalScale(profile.profile);
			for(const RetargetCompiledChain& chain : profile.chains) {
				const bool containsSource = std::find(chain.sourceJointIndices.begin(),
					chain.sourceJointIndices.end(), mapping.sourceJointIndex)
					!= chain.sourceJointIndices.end();
				const bool containsTarget = std::find(chain.targetJointIndices.begin(),
					chain.targetJointIndices.end(), mapping.targetJointIndex)
					!= chain.targetJointIndices.end();
				if(!containsSource || !containsTarget) {
					continue;
				}
				switch(chain.translationScaleMode) {
				case RetargetTranslationScaleMode::None:
					scale = 1.0F;
					break;
				case RetargetTranslationScaleMode::UniformHeight:
					scale = GlobalScale(profile.profile);
					break;
				case RetargetTranslationScaleMode::Authored:
					scale = chain.authoredTranslationScale;
					break;
				case RetargetTranslationScaleMode::PerChainLength: {
					const float sourceLength = ChainLength(chain.sourceJointIndices,
						sourceReferenceModel);
					const float targetLength = ChainLength(chain.targetJointIndices,
						targetReferenceModel);
					if(sourceLength > 1.0e-6F && targetLength > 1.0e-6F) {
						scale = targetLength / sourceLength;
					}
					break;
				}
				}
				break;
			}
			return std::clamp(scale, profile.profile.proportion.minimumScale,
				profile.profile.proportion.maximumScale);
		}

		AnimationQuaternion ExtractTwist(
			AnimationQuaternion rotation,
			AnimationFloat3 axis) noexcept {
			AnimationQuaternion normalized;
			if(!ProceduralMath::TryNormalize(rotation, normalized)) {
				return {};
			}
			const AnimationFloat3 unitAxis = ProceduralMath::NormalizeOr(axis,
				{1.0F, 0.0F, 0.0F});
			const float projection = normalized.x * unitAxis.x
				+ normalized.y * unitAxis.y + normalized.z * unitAxis.z;
			AnimationQuaternion twist{unitAxis.x * projection,
				unitAxis.y * projection, unitAxis.z * projection, normalized.w};
			return ProceduralMath::Canonicalize(twist);
		}

		const RetargetFallbackRule* FindFallback(
			const RetargetProfileAsset& profile,
			std::uint64_t fallbackRuleId) noexcept {
			const auto iterator = std::find_if(profile.fallbackRules.begin(),
				profile.fallbackRules.end(), [fallbackRuleId](const RetargetFallbackRule& fallback) {
					return fallback.fallbackRuleId == fallbackRuleId;
				});
			return iterator != profile.fallbackRules.end() ? &*iterator : nullptr;
		}

		const RetargetCompiledJointMapping* FindCompiledMapping(
			const RetargetCompiledProfile& profile,
			std::uint64_t mappingId) noexcept {
			const auto iterator = std::find_if(profile.mappings.begin(), profile.mappings.end(),
				[mappingId](const RetargetCompiledJointMapping& mapping) {
					return mapping.mappingId == mappingId;
				});
			return iterator != profile.mappings.end() ? &*iterator : nullptr;
		}

		bool ApplyFallbackRotation(
			const RetargetFallbackRule& fallback,
			std::uint32_t sourceIndex,
			const SkeletonAsset& targetSkeleton,
			std::span<const AnimationTransform> sourceReferenceModel,
			std::span<const AnimationTransform> sourceModel,
			std::vector<AnimationTransform>& targetLocal) noexcept {
			if(fallback.mode == RetargetFallbackMode::ReferencePose
				|| fallback.mode == RetargetFallbackMode::DropOptional
				|| fallback.targetJointIds.empty()
				|| sourceIndex >= sourceModel.size()) {
				return true;
			}
			const AnimationQuaternion delta = ProceduralMath::Multiply(
				sourceModel[sourceIndex].rotation,
				ProceduralMath::Conjugate(sourceReferenceModel[sourceIndex].rotation));
			const float weight = fallback.mode == RetargetFallbackMode::DeclaredTargetChain
				? 1.0F / static_cast<float>(fallback.targetJointIds.size()) : 1.0F;
			bool applied = false;
			for(const std::uint64_t targetJointId : fallback.targetJointIds) {
				const std::optional<std::uint32_t> targetIndex = FindJointIndex(
					targetSkeleton, targetJointId);
				if(!targetIndex.has_value() || *targetIndex >= targetLocal.size()) {
					continue;
				}
				const AnimationQuaternion contribution = ProceduralMath::Nlerp({}, delta, weight);
				targetLocal[*targetIndex].rotation = ProceduralMath::Multiply(
					contribution, targetLocal[*targetIndex].rotation);
				applied = true;
				if(fallback.mode == RetargetFallbackMode::NearestMappedAncestor) {
					break;
				}
			}
			return applied;
		}

		std::uint64_t HashTransformResult(
			const RetargetEvaluationResult& result) noexcept {
			std::uint64_t hash = 1469598103934665603ULL;
			auto mix = [&hash](std::uint64_t value) {
				for(std::size_t byte = 0U; byte < sizeof(value); ++byte) {
					hash ^= static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU);
					hash *= 1099511628211ULL;
				}
			};
			mix(static_cast<std::uint64_t>(result.sourceTick));
			mix(std::bit_cast<std::uint32_t>(result.normalizedTime));
			for(const AnimationTransform& transform : result.localTransforms) {
				for(const float value : {transform.translation.x, transform.translation.y,
					transform.translation.z, transform.rotation.x, transform.rotation.y,
					transform.rotation.z, transform.rotation.w, transform.scale.x,
					transform.scale.y, transform.scale.z}) {
					mix(std::bit_cast<std::uint32_t>(value));
				}
			}
			return hash;
		}

		AnimationStatus ValidateRequest(const RetargetEvaluationRequest& request) noexcept {
			if(request.profile == nullptr || request.sourceSkeleton == nullptr
				|| request.targetSkeleton == nullptr || request.sourcePose == nullptr
				|| request.targetReferencePose == nullptr
				|| !request.generation.IsValid() || !request.expectedGeneration.IsValid()) {
				return AnimationStatus::InvalidArgument;
			}
			if(request.generation != request.expectedGeneration) {
				return AnimationStatus::StaleHandle;
			}
			const RetargetProfileAsset& profile = request.profile->profile;
			const AnimationStatus profileShape =
				ValidateRetargetProfileAssetStructure(profile);
			if(profileShape != AnimationStatus::Ok) {
				return profileShape;
			}
			if(request.profile->stableHash == 0U
				|| request.generation.profile.assetId != profile.assetId
				|| (profile.runtimeGeneration != 0U
					&& request.generation.profile.generation != profile.runtimeGeneration)
				|| request.generation.sourceSkeleton.assetId
					!= request.sourceSkeleton->assetId
				|| request.generation.targetSkeleton.assetId
					!= request.targetSkeleton->assetId
				|| (request.sourceSkeleton->runtimeGeneration != 0U
					&& request.generation.sourceSkeleton.generation
						!= request.sourceSkeleton->runtimeGeneration)
				|| (request.targetSkeleton->runtimeGeneration != 0U
					&& request.generation.targetSkeleton.generation
						!= request.targetSkeleton->runtimeGeneration)
				|| request.sourcePose->skeleton != request.generation.sourceSkeleton
				|| request.targetReferencePose->skeleton
					!= request.generation.targetSkeleton
				|| request.sourcePose->poseGeneration
					!= request.generation.sourcePoseGeneration
				|| request.targetReferencePose->poseGeneration
					!= request.generation.targetReferencePoseGeneration) {
				return AnimationStatus::StaleHandle;
			}
			if(profile.sourceSkeletonAssetId != request.sourceSkeleton->assetId
				|| profile.targetSkeletonAssetId != request.targetSkeleton->assetId
				|| profile.sourceSkeletonFingerprint
					!= request.sourceSkeleton->compatibilityFingerprint
				|| profile.targetSkeletonFingerprint
					!= request.targetSkeleton->compatibilityFingerprint
				|| profile.sourceReferencePoseHash != request.sourceSkeleton->referencePoseHash
				|| profile.targetReferencePoseHash != request.targetSkeleton->referencePoseHash) {
				return AnimationStatus::IncompatibleAsset;
			}
			if(request.profile->mappings.size() > kRetargetProfileMaximumMappings
				|| request.profile->chains.size() > kRetargetProfileMaximumChains) {
				return AnimationStatus::HardLimitExceeded;
			}
			std::uint32_t priorTargetIndex = 0U;
			bool hasPriorTarget = false;
			for(const RetargetCompiledJointMapping& mapping : request.profile->mappings) {
				const bool supportedKind = mapping.kind == RetargetMappingKind::Semantic
					|| mapping.kind == RetargetMappingKind::Explicit;
				if(mapping.mappingId == 0U || !supportedKind
					|| mapping.sourceJointIndex >= request.sourceSkeleton->joints.size()
					|| mapping.targetJointIndex >= request.targetSkeleton->joints.size()
					|| !IsNormalized(mapping.sourceBasis, 0.01F)
					|| !IsNormalized(mapping.targetBasis, 0.01F)
					|| (hasPriorTarget && mapping.targetJointIndex <= priorTargetIndex)) {
					return AnimationStatus::InvalidArgument;
				}
				priorTargetIndex = mapping.targetJointIndex;
				hasPriorTarget = true;
			}
			for(const RetargetCompiledChain& chain : request.profile->chains) {
				const bool supportedScale =
					chain.translationScaleMode == RetargetTranslationScaleMode::None
					|| chain.translationScaleMode
						== RetargetTranslationScaleMode::UniformHeight
					|| chain.translationScaleMode
						== RetargetTranslationScaleMode::PerChainLength
					|| chain.translationScaleMode
						== RetargetTranslationScaleMode::Authored;
				if(chain.chainId == 0U
					|| !supportedScale
					|| !std::isfinite(chain.authoredTranslationScale)
					|| chain.authoredTranslationScale <= 0.0F
					|| chain.sourceJointIndices.size() > kRetargetProfileMaximumMappings
					|| chain.targetJointIndices.size() > kRetargetProfileMaximumMappings) {
					return AnimationStatus::InvalidArgument;
				}
				for(const std::uint32_t index : chain.sourceJointIndices) {
					if(index >= request.sourceSkeleton->joints.size()) {
						return AnimationStatus::InvalidArgument;
					}
				}
				for(const std::uint32_t index : chain.targetJointIndices) {
					if(index >= request.targetSkeleton->joints.size()) {
						return AnimationStatus::InvalidArgument;
					}
				}
			}
			if(!std::isfinite(request.normalizedTime)
				|| request.sourcePose->localTransforms.size()
					!= request.sourceSkeleton->joints.size()
				|| request.targetReferencePose->localTransforms.size()
					!= request.targetSkeleton->joints.size()
				|| (!request.sourcePose->modelTransforms.empty()
					&& request.sourcePose->modelTransforms.size()
						!= request.sourceSkeleton->joints.size())
				|| (!request.targetReferencePose->modelTransforms.empty()
					&& request.targetReferencePose->modelTransforms.size()
						!= request.targetSkeleton->joints.size())
				|| !AllFinite(request.sourcePose->localTransforms)
				|| !AllFinite(request.targetReferencePose->localTransforms)
				|| (!request.sourcePose->modelTransforms.empty()
					&& !AllFinite(request.sourcePose->modelTransforms))
				|| (!request.targetReferencePose->modelTransforms.empty()
					&& !AllFinite(request.targetReferencePose->modelTransforms))) {
				return AnimationStatus::InvalidArgument;
			}
			if(request.constraints != nullptr) {
				if(request.constraints->generation != request.generation) {
					return AnimationStatus::StaleHandle;
				}
				if(request.constraints->sourceTick != request.sourcePose->tick
					|| request.constraints->normalizedTime != request.normalizedTime) {
					return AnimationStatus::InvalidArgument;
				}
			}
			return AnimationStatus::Ok;
		}

		RetargetEvaluationResult MakeFallback(
			const RetargetEvaluationRequest& request,
			AnimationStatus status) {
			RetargetEvaluationResult fallback;
			fallback.status = status;
			fallback.generation = request.generation;
			fallback.sourceTick = request.sourcePose != nullptr
				? request.sourcePose->tick : request.generation.tick;
			fallback.normalizedTime = request.normalizedTime;
			fallback.reducedFidelity = true;
			if(request.targetReferencePose != nullptr && request.targetSkeleton != nullptr
				&& request.targetReferencePose->localTransforms.size()
					== request.targetSkeleton->joints.size()
				&& AllFinite(request.targetReferencePose->localTransforms)) {
				fallback.localTransforms = request.targetReferencePose->localTransforms;
				fallback.modelTransforms.resize(fallback.localTransforms.size());
				if(!request.targetReferencePose->modelTransforms.empty()
					&& request.targetReferencePose->modelTransforms.size()
						== fallback.localTransforms.size()
					&& AllFinite(request.targetReferencePose->modelTransforms)) {
					fallback.modelTransforms = request.targetReferencePose->modelTransforms;
					fallback.finite = true;
				} else {
					fallback.finite = BuildModels(*request.targetSkeleton,
						fallback.localTransforms, fallback.modelTransforms);
				}
			}
			fallback.deterministicHash = fallback.finite
				? HashTransformResult(fallback) : 0U;
			return fallback;
		}
	}

	AnimationStatus EvaluateRetargetPose(
		const RetargetEvaluationRequest& request,
		RetargetEvaluationResult& output) noexcept {
		try {
			const AnimationStatus validation = ValidateRequest(request);
			if(validation != AnimationStatus::Ok) {
				RetargetEvaluationResult fallback = MakeFallback(request, validation);
				output = std::move(fallback);
				return validation;
			}

			const RetargetCompiledProfile& compiled = *request.profile;
			const RetargetProfileAsset& profile = compiled.profile;
			const SkeletonAsset& sourceSkeleton = *request.sourceSkeleton;
			const SkeletonAsset& targetSkeleton = *request.targetSkeleton;
			std::vector<AnimationTransform> sourceReferenceLocal;
			sourceReferenceLocal.reserve(sourceSkeleton.joints.size());
			for(const SkeletonJoint& joint : sourceSkeleton.joints) {
				sourceReferenceLocal.push_back(joint.localBindTransform);
			}
			std::vector<AnimationTransform> sourceReferenceModel(sourceSkeleton.joints.size());
			if(!BuildModels(sourceSkeleton, sourceReferenceLocal, sourceReferenceModel)) {
				RetargetEvaluationResult fallback = MakeFallback(request, AnimationStatus::IncompatibleAsset);
				output = std::move(fallback);
				return AnimationStatus::IncompatibleAsset;
			}
			std::vector<AnimationTransform> sourceModel = request.sourcePose->modelTransforms;
			if(sourceModel.empty()) {
				sourceModel.resize(sourceSkeleton.joints.size());
				if(!BuildModels(sourceSkeleton, request.sourcePose->localTransforms, sourceModel)) {
					RetargetEvaluationResult fallback = MakeFallback(request, AnimationStatus::InvalidArgument);
					output = std::move(fallback);
					return AnimationStatus::InvalidArgument;
				}
			}
			std::vector<AnimationTransform> targetReferenceModel =
				request.targetReferencePose->modelTransforms;
			if(targetReferenceModel.empty()) {
				targetReferenceModel.resize(targetSkeleton.joints.size());
				if(!BuildModels(targetSkeleton, request.targetReferencePose->localTransforms,
					targetReferenceModel)) {
					RetargetEvaluationResult fallback = MakeFallback(request, AnimationStatus::InvalidArgument);
					output = std::move(fallback);
					return AnimationStatus::InvalidArgument;
				}
			}

			RetargetEvaluationResult staged;
			staged.status = AnimationStatus::Ok;
			staged.generation = request.generation;
			staged.sourceTick = request.sourcePose->tick;
			staged.normalizedTime = request.normalizedTime;
			staged.localTransforms = request.targetReferencePose->localTransforms;
			staged.modelTransforms = targetReferenceModel;
			staged.metrics.appliedTranslationScale = GlobalScale(profile);

			std::size_t mappingCursor = 0U;
			for(std::size_t targetIndex = 0U; targetIndex < targetSkeleton.joints.size(); ++targetIndex) {
				const std::int32_t parent = targetSkeleton.joints[targetIndex].parentIndex;
				staged.modelTransforms[targetIndex] = parent < 0
					? staged.localTransforms[targetIndex]
					: ProceduralMath::Combine(
						staged.modelTransforms[static_cast<std::size_t>(parent)],
						staged.localTransforms[targetIndex]);
				while(mappingCursor < compiled.mappings.size()
					&& compiled.mappings[mappingCursor].targetJointIndex < targetIndex) {
					++mappingCursor;
				}
				if(mappingCursor >= compiled.mappings.size()
					|| compiled.mappings[mappingCursor].targetJointIndex != targetIndex) {
					continue;
				}
				const RetargetCompiledJointMapping& mapping = compiled.mappings[mappingCursor];
				if(mapping.sourceJointIndex >= sourceModel.size()) {
					RetargetEvaluationResult fallback = MakeFallback(request,
						AnimationStatus::IncompatibleAsset);
					output = std::move(fallback);
					return AnimationStatus::IncompatibleAsset;
				}
				const AnimationQuaternion sourceDelta = ProceduralMath::Multiply(
					sourceModel[mapping.sourceJointIndex].rotation,
					ProceduralMath::Conjugate(
						sourceReferenceModel[mapping.sourceJointIndex].rotation));
				const AnimationQuaternion alignment = ProceduralMath::Multiply(
					mapping.targetBasis, ProceduralMath::Conjugate(mapping.sourceBasis));
				const AnimationQuaternion alignedDelta = ProceduralMath::Multiply(
					ProceduralMath::Multiply(alignment, sourceDelta),
					ProceduralMath::Conjugate(alignment));
				AnimationTransform desiredModel = targetReferenceModel[targetIndex];
				desiredModel.rotation = ProceduralMath::Multiply(alignedDelta,
					targetReferenceModel[targetIndex].rotation);
				const float scale = MappingScale(compiled, mapping, sourceReferenceModel,
					targetReferenceModel);
				const AnimationFloat3 sourceDisplacement = ProceduralMath::Subtract(
					sourceModel[mapping.sourceJointIndex].translation,
					sourceReferenceModel[mapping.sourceJointIndex].translation);
				desiredModel.translation = ProceduralMath::Add(
					targetReferenceModel[targetIndex].translation,
					ProceduralMath::Scale(ProceduralMath::Rotate(alignment,
						sourceDisplacement), scale));
				const AnimationFloat3 sourceReferenceScale =
					sourceReferenceModel[mapping.sourceJointIndex].scale;
				if(std::abs(sourceReferenceScale.x) <= 1.0e-6F
					|| std::abs(sourceReferenceScale.y) <= 1.0e-6F
					|| std::abs(sourceReferenceScale.z) <= 1.0e-6F) {
					RetargetEvaluationResult fallback = MakeFallback(request,
						AnimationStatus::IncompatibleAsset);
					output = std::move(fallback);
					return AnimationStatus::IncompatibleAsset;
				}
				desiredModel.scale = {
					targetReferenceModel[targetIndex].scale.x
						* sourceModel[mapping.sourceJointIndex].scale.x / sourceReferenceScale.x,
					targetReferenceModel[targetIndex].scale.y
						* sourceModel[mapping.sourceJointIndex].scale.y / sourceReferenceScale.y,
					targetReferenceModel[targetIndex].scale.z
						* sourceModel[mapping.sourceJointIndex].scale.z / sourceReferenceScale.z};
				AnimationTransform desiredLocal = desiredModel;
				if(parent >= 0 && !ProceduralMath::TryRelativeTransform(
					staged.modelTransforms[static_cast<std::size_t>(parent)],
					desiredModel, desiredLocal)) {
					RetargetEvaluationResult fallback = MakeFallback(request,
						AnimationStatus::Rejected);
					output = std::move(fallback);
					return AnimationStatus::Rejected;
				}
				staged.localTransforms[targetIndex] = desiredLocal;
				staged.modelTransforms[targetIndex] = desiredModel;
				staged.metrics.appliedTranslationScale = scale;
				++staged.metrics.evaluatedMappingCount;
				++mappingCursor;
			}

			for(const RetargetTwistRule& twist : profile.twistRules) {
				const std::optional<std::uint32_t> sourceIndex = FindJointIndex(
					sourceSkeleton, twist.sourceJointId);
				if(!sourceIndex.has_value()) {
					continue;
				}
				const AnimationQuaternion delta = ProceduralMath::Multiply(
					sourceModel[*sourceIndex].rotation,
					ProceduralMath::Conjugate(sourceReferenceModel[*sourceIndex].rotation));
				const AnimationQuaternion sourceTwist = ExtractTwist(delta,
					twist.sourceTwistAxis);
				for(std::size_t index = 0U; index < twist.targetJointIds.size(); ++index) {
					const std::optional<std::uint32_t> targetIndex = FindJointIndex(
						targetSkeleton, twist.targetJointIds[index]);
					if(!targetIndex.has_value() || index >= twist.targetWeights.size()) {
						continue;
					}
					const AnimationQuaternion contribution = ProceduralMath::Nlerp({},
						sourceTwist, twist.targetWeights[index]);
					staged.localTransforms[*targetIndex].rotation = ProceduralMath::Multiply(
						contribution, staged.localTransforms[*targetIndex].rotation);
					++staged.metrics.twistJointCount;
				}
			}

			for(const RetargetSemanticMapping& mapping : profile.semanticMappings) {
				if(mapping.required || FindCompiledMapping(compiled, mapping.mappingId) != nullptr) {
					continue;
				}
				const std::optional<std::uint32_t> sourceIndex = mapping.sourceJointId != 0U
					? FindJointIndex(sourceSkeleton, mapping.sourceJointId)
					: FindSemanticJoint(sourceSkeleton, mapping.sourceSemantic);
				const RetargetFallbackRule* fallback = FindFallback(profile,
					mapping.fallbackRuleId);
				if(sourceIndex.has_value() && fallback != nullptr) {
					static_cast<void>(ApplyFallbackRotation(*fallback, *sourceIndex,
						targetSkeleton, sourceReferenceModel, sourceModel,
						staged.localTransforms));
				}
				++staged.metrics.fallbackCount;
				staged.reducedFidelity = true;
			}
			for(const RetargetExplicitJointMapping& mapping : profile.explicitMappings) {
				if(mapping.required || FindCompiledMapping(compiled, mapping.mappingId) != nullptr) {
					continue;
				}
				const std::optional<std::uint32_t> sourceIndex = FindJointIndex(
					sourceSkeleton, mapping.sourceJointId);
				const RetargetFallbackRule* fallback = FindFallback(profile,
					mapping.fallbackRuleId);
				if(sourceIndex.has_value() && fallback != nullptr) {
					static_cast<void>(ApplyFallbackRotation(*fallback, *sourceIndex,
						targetSkeleton, sourceReferenceModel, sourceModel,
						staged.localTransforms));
				}
				++staged.metrics.fallbackCount;
				staged.reducedFidelity = true;
			}
			if(!BuildModels(targetSkeleton, staged.localTransforms, staged.modelTransforms)) {
				RetargetEvaluationResult fallback = MakeFallback(request,
					AnimationStatus::Rejected);
				output = std::move(fallback);
				return AnimationStatus::Rejected;
			}

			RetargetConstraintSnapshot defaultConstraints;
			defaultConstraints.generation = request.generation;
			defaultConstraints.sourceTick = request.sourcePose->tick;
			defaultConstraints.normalizedTime = request.normalizedTime;
			RetargetConstraintRequest constraintRequest;
			constraintRequest.profile = &profile;
			constraintRequest.sourceSkeleton = &sourceSkeleton;
			constraintRequest.targetSkeleton = &targetSkeleton;
			constraintRequest.generation = request.generation;
			constraintRequest.expectedGeneration = request.expectedGeneration;
			constraintRequest.sourceReferenceModelTransforms = sourceReferenceModel;
			constraintRequest.sourceModelTransforms = sourceModel;
			constraintRequest.targetReferenceModelTransforms = targetReferenceModel;
			constraintRequest.inputTargetLocalTransforms = staged.localTransforms;
			constraintRequest.inputTargetModelTransforms = staged.modelTransforms;
			constraintRequest.snapshot = request.constraints != nullptr
				? request.constraints.get() : &defaultConstraints;
			RetargetConstraintResult constrained;
			const AnimationStatus constraintStatus = ApplyRetargetConstraints(
				constraintRequest, constrained);
			if(constraintStatus != AnimationStatus::Ok) {
				staged.status = constraintStatus;
				staged.finite = AllFinite(staged.localTransforms)
					&& AllFinite(staged.modelTransforms);
				staged.deterministicHash = staged.finite
					? HashTransformResult(staged) : 0U;
				output = std::move(staged);
				return constraintStatus;
			}
			staged.localTransforms = std::move(constrained.localTransforms);
			staged.modelTransforms = std::move(constrained.modelTransforms);
			staged.metrics.constraints = constrained.metrics;
			staged.metrics.fallbackCount += constrained.metrics.fallbackCount;
			staged.reducedFidelity = staged.reducedFidelity || constrained.reducedFidelity;
			staged.normalizedTime = constrained.normalizedTime;
			staged.sourceTick = constrained.sourceTick;
			staged.finite = AllFinite(staged.localTransforms)
				&& AllFinite(staged.modelTransforms);
			if(!staged.finite) {
				RetargetEvaluationResult fallback = MakeFallback(request,
					AnimationStatus::Rejected);
				output = std::move(fallback);
				return AnimationStatus::Rejected;
			}
			staged.deterministicHash = HashTransformResult(staged);
			output = std::move(staged);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}
}
