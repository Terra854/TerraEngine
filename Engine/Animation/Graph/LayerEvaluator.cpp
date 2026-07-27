// LayerEvaluator.cpp - Ordered override and additive copied-pose composition.

#include "Animation/Graph/LayerEvaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <new>

namespace TerraEngine::Animation {
namespace {
constexpr double kScaleEpsilon = 1.0e-8;

bool IsValidWeight(float value) noexcept {
	return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}

bool StoreFiniteFloat(double value, float &output) noexcept {
	if (!std::isfinite(value) || value > static_cast<double>(std::numeric_limits<float>::max()) ||
		value < -static_cast<double>(std::numeric_limits<float>::max())) {
		return false;
	}
	output = static_cast<float>(value);
	return std::isfinite(output);
}

double Dot(const AnimationQuaternion &left, const AnimationQuaternion &right) noexcept {
	return static_cast<double>(left.x) * right.x + static_cast<double>(left.y) * right.y +
		static_cast<double>(left.z) * right.z + static_cast<double>(left.w) * right.w;
}

AnimationQuaternion Negate(AnimationQuaternion value) noexcept {
	value.x = -value.x;
	value.y = -value.y;
	value.z = -value.z;
	value.w = -value.w;
	return value;
}

AnimationQuaternion Normalize(AnimationQuaternion value) noexcept {
	const double length = std::sqrt(
		static_cast<double>(value.x) * value.x + static_cast<double>(value.y) * value.y +
		static_cast<double>(value.z) * value.z + static_cast<double>(value.w) * value.w);
	if (!std::isfinite(length) || length <= kScaleEpsilon) {
		return {};
	}
	value.x = static_cast<float>(value.x / length);
	value.y = static_cast<float>(value.y / length);
	value.z = static_cast<float>(value.z / length);
	value.w = static_cast<float>(value.w / length);
	return value.w < 0.0F ? Negate(value) : value;
}

AnimationQuaternion Multiply(const AnimationQuaternion &left, const AnimationQuaternion &right) noexcept {
	return Normalize({
		left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
		left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
		left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
		left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z
	});
}

AnimationQuaternion Inverse(const AnimationQuaternion &value) noexcept {
	return {-value.x, -value.y, -value.z, value.w};
}

AnimationQuaternion Nlerp(AnimationQuaternion from, AnimationQuaternion to, float weight) noexcept {
	if (Dot(from, to) < 0.0) {
		to = Negate(to);
	}
	return Normalize({
		from.x + (to.x - from.x) * weight,
		from.y + (to.y - from.y) * weight,
		from.z + (to.z - from.z) * weight,
		from.w + (to.w - from.w) * weight
	});
}

AnimationStatus ValidateCompatiblePose(
	const GraphPoseContribution &base,
	const GraphPoseContribution &candidate) noexcept {
	const AnimationStatus status = ValidateGraphPoseContribution(candidate);
	if (status != AnimationStatus::Ok) {
		return status;
	}
	if (candidate.compatibilityFingerprint != base.compatibilityFingerprint || candidate.jointCount != base.jointCount ||
		candidate.jointStableIds != base.jointStableIds) {
		return AnimationStatus::IncompatibleAsset;
	}
	return AnimationStatus::Ok;
}

float JointMaskWeight(const AnimationMaskAsset *mask, std::uint64_t stableJointId) noexcept {
	if (mask == nullptr) {
		return 1.0F;
	}
	const auto iterator = std::find_if(mask->compiledJointWeights.begin(), mask->compiledJointWeights.end(),
		[stableJointId](const AnimationMaskDenseJointWeight &value) {
			return value.stableJointId == stableJointId;
		});
	return iterator == mask->compiledJointWeights.end() ? 0.0F : iterator->weight;
}

bool IsNamedChannelSelected(const AnimationMaskAsset *mask, std::uint64_t stableId, bool morph) noexcept {
	if (mask == nullptr) {
		return true;
	}
	const std::vector<std::uint64_t> &ids = morph ? mask->compiledMorphIds : mask->compiledCurveIds;
	return std::find(ids.begin(), ids.end(), stableId) != ids.end();
}

const AnimationNamedFloatValue *FindNamedValue(
	const std::vector<AnimationNamedFloatValue> &values,
	std::uint64_t stableId) noexcept {
	const auto iterator = std::lower_bound(values.begin(), values.end(), stableId,
		[](const AnimationNamedFloatValue &value, std::uint64_t id) {
			return value.stableId < id;
		});
	return iterator != values.end() && iterator->stableId == stableId ? &*iterator : nullptr;
}

AnimationStatus ValidateMaskForPose(const AnimationMaskAsset &mask, const GraphPoseContribution &base) {
	const AnimationStatus maskStatus = ValidateAnimationMaskAsset(mask);
	if (maskStatus != AnimationStatus::Ok) {
		return maskStatus;
	}
	if (mask.compatibilityFingerprint != base.compatibilityFingerprint) {
		return AnimationStatus::IncompatibleAsset;
	}
	// A cooked dense mask for this skeleton cannot name a joint outside the
	// complete pose layout. Checking here prevents accidental index assumptions.
	for (const AnimationMaskDenseJointWeight &weight : mask.compiledJointWeights) {
		if (!std::binary_search(base.jointStableIds.begin(), base.jointStableIds.end(), weight.stableJointId)) {
			return AnimationStatus::IncompatibleAsset;
		}
	}
	return AnimationStatus::Ok;
}

AnimationStatus ComposeNamedValues(
	std::vector<AnimationNamedFloatValue> &target,
	const std::vector<AnimationNamedFloatValue> &source,
	const std::vector<AnimationNamedFloatValue> *reference,
	const AnimationMaskAsset *mask,
	bool morph,
	AnimationGraphLayerMode mode,
	float weight) {
	if (weight == 0.0F) {
		return AnimationStatus::Ok;
	}
	std::map<std::uint64_t, float> values;
	for (const AnimationNamedFloatValue &value : target) {
		values.emplace(value.stableId, value.value);
	}
	auto addSelectedKeys = [&](const std::vector<AnimationNamedFloatValue> &entries) {
		for (const AnimationNamedFloatValue &entry : entries) {
			if (IsNamedChannelSelected(mask, entry.stableId, morph)) {
				values.try_emplace(entry.stableId, 0.0F);
			}
		}
	};
	addSelectedKeys(source);
	if (reference != nullptr) {
		addSelectedKeys(*reference);
	}

	const AnimationLimit limitKind = morph ? AnimationLimit::MeshMorphTargets : AnimationLimit::ClipChannels;
	if (ValidateAnimationHardLimit(limitKind, values.size()) != AnimationStatus::Ok) {
		return AnimationStatus::HardLimitExceeded;
	}
	for (auto &[stableId, current] : values) {
		if (!IsNamedChannelSelected(mask, stableId, morph)) {
			continue;
		}
		const AnimationNamedFloatValue *sourceValue = FindNamedValue(source, stableId);
		const double sourceScalar = sourceValue == nullptr ? 0.0 : sourceValue->value;
		double composed = current;
		if (mode == AnimationGraphLayerMode::Override) {
			composed += (sourceScalar - current) * weight;
		} else {
			const AnimationNamedFloatValue *referenceValue = FindNamedValue(*reference, stableId);
			const double referenceScalar = referenceValue == nullptr ? 0.0 : referenceValue->value;
			composed += (sourceScalar - referenceScalar) * weight;
		}
		if (!StoreFiniteFloat(composed, current)) {
			return AnimationStatus::RangeOverflow;
		}
	}

	target.clear();
	target.reserve(values.size());
	for (const auto &[stableId, value] : values) {
		target.push_back({stableId, value});
	}
	return AnimationStatus::Ok;
}

AnimationStatus ComposeJoint(
	AnimationTransform &target,
	const AnimationTransform &source,
	const AnimationTransform *reference,
	AnimationGraphLayerMode mode,
	float weight) noexcept {
	if (weight == 0.0F) {
		return AnimationStatus::Ok;
	}
	if (mode == AnimationGraphLayerMode::Override) {
		if (!StoreFiniteFloat(target.translation.x + (source.translation.x - target.translation.x) * weight,
				target.translation.x) ||
			!StoreFiniteFloat(target.translation.y + (source.translation.y - target.translation.y) * weight,
				target.translation.y) ||
			!StoreFiniteFloat(target.translation.z + (source.translation.z - target.translation.z) * weight,
				target.translation.z) ||
			!StoreFiniteFloat(target.scale.x + (source.scale.x - target.scale.x) * weight, target.scale.x) ||
			!StoreFiniteFloat(target.scale.y + (source.scale.y - target.scale.y) * weight, target.scale.y) ||
			!StoreFiniteFloat(target.scale.z + (source.scale.z - target.scale.z) * weight, target.scale.z)) {
			return AnimationStatus::RangeOverflow;
		}
		target.rotation = Nlerp(target.rotation, source.rotation, weight);
	} else {
		if (reference == nullptr || std::abs(reference->scale.x) <= kScaleEpsilon ||
			std::abs(reference->scale.y) <= kScaleEpsilon || std::abs(reference->scale.z) <= kScaleEpsilon) {
			return AnimationStatus::InvalidArgument;
		}
		if (!StoreFiniteFloat(target.translation.x + (source.translation.x - reference->translation.x) * weight,
				target.translation.x) ||
			!StoreFiniteFloat(target.translation.y + (source.translation.y - reference->translation.y) * weight,
				target.translation.y) ||
			!StoreFiniteFloat(target.translation.z + (source.translation.z - reference->translation.z) * weight,
				target.translation.z)) {
			return AnimationStatus::RangeOverflow;
		}
		const double ratioX = static_cast<double>(source.scale.x) / reference->scale.x;
		const double ratioY = static_cast<double>(source.scale.y) / reference->scale.y;
		const double ratioZ = static_cast<double>(source.scale.z) / reference->scale.z;
		if (!StoreFiniteFloat(target.scale.x * (1.0 + (ratioX - 1.0) * weight), target.scale.x) ||
			!StoreFiniteFloat(target.scale.y * (1.0 + (ratioY - 1.0) * weight), target.scale.y) ||
			!StoreFiniteFloat(target.scale.z * (1.0 + (ratioZ - 1.0) * weight), target.scale.z)) {
			return AnimationStatus::RangeOverflow;
		}

		AnimationQuaternion alignedSource = source.rotation;
		if (Dot(reference->rotation, alignedSource) < 0.0) {
			alignedSource = Negate(alignedSource);
		}
		const AnimationQuaternion delta = Multiply(alignedSource, Inverse(reference->rotation));
		target.rotation = Multiply(target.rotation, Nlerp(AnimationQuaternion{}, delta, weight));
	}
	return IsFinite(target) && IsNormalized(target.rotation) ? AnimationStatus::Ok : AnimationStatus::RangeOverflow;
}

AnimationStatus ComposeRootMotion(
	AnimationRootMotionDelta &target,
	const AnimationRootMotionDelta &source,
	const AnimationRootMotionDelta *reference,
	AnimationGraphLayerMode mode,
	float weight) noexcept {
	if (weight == 0.0F) {
		return AnimationStatus::Ok;
	}
	auto overrideValue = [weight](double current, double next) {
		return current + (next - current) * weight;
	};
	auto additiveValue = [weight](double current, double next, double referenceValue) {
		return current + (next - referenceValue) * weight;
	};
	auto compose = [&](double current, double next, double referenceValue) {
		return mode == AnimationGraphLayerMode::Override
			? overrideValue(current, next)
			: additiveValue(current, next, referenceValue);
	};
	if (mode == AnimationGraphLayerMode::Additive && reference == nullptr) {
		return AnimationStatus::InvalidArgument;
	}
	const AnimationRootMotionDelta emptyReference{};
	const AnimationRootMotionDelta &referenceValue = reference == nullptr ? emptyReference : *reference;
	if (!StoreFiniteFloat(compose(target.localTranslation.x, source.localTranslation.x, referenceValue.localTranslation.x),
			target.localTranslation.x) ||
		!StoreFiniteFloat(compose(target.localTranslation.y, source.localTranslation.y, referenceValue.localTranslation.y),
			target.localTranslation.y) ||
		!StoreFiniteFloat(compose(target.localTranslation.z, source.localTranslation.z, referenceValue.localTranslation.z),
			target.localTranslation.z) ||
		!StoreFiniteFloat(compose(target.worldTranslation.x, source.worldTranslation.x, referenceValue.worldTranslation.x),
			target.worldTranslation.x) ||
		!StoreFiniteFloat(compose(target.worldTranslation.y, source.worldTranslation.y, referenceValue.worldTranslation.y),
			target.worldTranslation.y) ||
		!StoreFiniteFloat(compose(target.worldTranslation.z, source.worldTranslation.z, referenceValue.worldTranslation.z),
			target.worldTranslation.z) ||
		!StoreFiniteFloat(compose(target.localYawRadians, source.localYawRadians, referenceValue.localYawRadians),
			target.localYawRadians) ||
		!StoreFiniteFloat(compose(target.worldYawRadians, source.worldYawRadians, referenceValue.worldYawRadians),
			target.worldYawRadians)) {
		return AnimationStatus::RangeOverflow;
	}

	// Tick identities and policy are provenance rather than blendable numbers.
	// An exact half-weight tie goes to the later authored layer.
	if (weight >= 0.5F) {
		target.animator = source.animator;
		target.sourceBeginTick = source.sourceBeginTick;
		target.sourceEndTick = source.sourceEndTick;
		target.accumulatedCycles = source.accumulatedCycles;
		target.sourceStateId = source.sourceStateId;
		target.layerIndex = source.layerIndex;
		target.generation = source.generation;
		target.policy = source.policy;
	}
	return AnimationStatus::Ok;
}

AnimationStatus ValidateLayerInputs(
	const GraphPoseContribution &base,
	std::span<const GraphLayerInput> layers) {
	const AnimationStatus baseStatus = ValidateGraphPoseContribution(base);
	if (baseStatus != AnimationStatus::Ok) {
		return baseStatus;
	}
	const AnimationStatus layerLimit = ValidateAnimationHardLimit(AnimationLimit::ControllerLayers, layers.size());
	if (layerLimit != AnimationStatus::Ok) {
		return layerLimit;
	}
	for (std::size_t index = 0U; index < layers.size(); ++index) {
		const GraphLayerInput &layer = layers[index];
		if (layer.stableId == 0U || layer.mode > AnimationGraphLayerMode::Additive || layer.contribution == nullptr ||
			!IsValidWeight(layer.controllerWeight) || !IsValidWeight(layer.nodeWeight) || !IsValidWeight(layer.layerWeight)) {
			return AnimationStatus::InvalidArgument;
		}
		for (std::size_t previous = 0U; previous < index; ++previous) {
			if (layers[previous].stableId == layer.stableId) {
				return AnimationStatus::InvalidArgument;
			}
		}
		const AnimationStatus contributionStatus = ValidateCompatiblePose(base, *layer.contribution);
		if (contributionStatus != AnimationStatus::Ok) {
			return contributionStatus;
		}
		if (layer.contribution->rootMotion.has_value() != base.rootMotion.has_value()) {
			return AnimationStatus::InvalidArgument;
		}
		if (layer.mask != nullptr) {
			const AnimationStatus maskStatus = ValidateMaskForPose(*layer.mask, base);
			if (maskStatus != AnimationStatus::Ok) {
				return maskStatus;
			}
		}
		if (layer.mode == AnimationGraphLayerMode::Additive) {
			if (layer.additiveReference == nullptr) {
				return AnimationStatus::InvalidArgument;
			}
			const AnimationStatus referenceStatus = ValidateCompatiblePose(base, *layer.additiveReference);
			if (referenceStatus != AnimationStatus::Ok) {
				return referenceStatus;
			}
			if (layer.additiveReference->rootMotion.has_value() != base.rootMotion.has_value()) {
				return AnimationStatus::InvalidArgument;
			}
		}
	}
	return AnimationStatus::Ok;
}
} // namespace

AnimationStatus EvaluateAnimationLayers(
	const GraphPoseContribution &base,
	std::span<const GraphLayerInput> layers,
	GraphPoseContribution &output) {
	try {
		const AnimationStatus inputStatus = ValidateLayerInputs(base, layers);
		if (inputStatus != AnimationStatus::Ok) {
			return inputStatus;
		}
		GraphPoseContribution staged = base;
		for (const GraphLayerInput &layer : layers) {
			const float controlWeight = layer.controllerWeight * layer.nodeWeight * layer.layerWeight;
			if (!IsValidWeight(controlWeight) || controlWeight == 0.0F) {
				if (!IsValidWeight(controlWeight)) {
					return AnimationStatus::InvalidArgument;
				}
				continue;
			}
			const bool additive = layer.mode == AnimationGraphLayerMode::Additive;
			for (std::size_t jointIndex = 0U; jointIndex < staged.joints.size(); ++jointIndex) {
				const float jointWeight = controlWeight * JointMaskWeight(layer.mask, staged.jointStableIds[jointIndex]);
				if (!IsValidWeight(jointWeight)) {
					return AnimationStatus::InvalidArgument;
				}
				const AnimationTransform *reference =
					additive ? &layer.additiveReference->joints[jointIndex] : nullptr;
				const AnimationStatus jointStatus = ComposeJoint(
					staged.joints[jointIndex], layer.contribution->joints[jointIndex], reference, layer.mode, jointWeight);
				if (jointStatus != AnimationStatus::Ok) {
					return jointStatus;
				}
			}

			const auto *referenceMorphs = additive ? &layer.additiveReference->morphs : nullptr;
			const auto *referenceCurves = additive ? &layer.additiveReference->curves : nullptr;
			AnimationStatus namedStatus = ComposeNamedValues(
				staged.morphs, layer.contribution->morphs, referenceMorphs, layer.mask, true, layer.mode, controlWeight);
			if (namedStatus != AnimationStatus::Ok) {
				return namedStatus;
			}
			namedStatus = ComposeNamedValues(
				staged.curves, layer.contribution->curves, referenceCurves, layer.mask, false, layer.mode, controlWeight);
			if (namedStatus != AnimationStatus::Ok) {
				return namedStatus;
			}

			if (staged.rootMotion.has_value()) {
				const float maskWeight = layer.mask == nullptr ? 1.0F : layer.mask->rootMotionWeight;
				const float rootWeight = controlWeight * maskWeight;
				if (!IsValidWeight(rootWeight)) {
					return AnimationStatus::InvalidArgument;
				}
				const AnimationRootMotionDelta *reference =
					additive ? &*layer.additiveReference->rootMotion : nullptr;
				const AnimationStatus rootStatus = ComposeRootMotion(
					*staged.rootMotion, *layer.contribution->rootMotion, reference, layer.mode, rootWeight);
				if (rootStatus != AnimationStatus::Ok) {
					return rootStatus;
				}
			}
		}

		const AnimationStatus outputStatus = ValidateGraphPoseContribution(staged);
		if (outputStatus != AnimationStatus::Ok) {
			return outputStatus == AnimationStatus::HardLimitExceeded ? outputStatus : AnimationStatus::RangeOverflow;
		}
		output = std::move(staged);
		return AnimationStatus::Ok;
	} catch (const std::bad_alloc &) {
		return AnimationStatus::InternalError;
	}
}
} // namespace TerraEngine::Animation
