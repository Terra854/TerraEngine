// AnimationPrefabIntegration.cpp - copied preparation and atomic publication.

#include "AnimationPrefabIntegration.h"

#include <algorithm>
#include <limits>
#include <set>
#include <type_traits>
#include <utility>

namespace TerraEngine::Editor::Animation {
	namespace {
		constexpr std::size_t kMaximumPlayModeAuthoredBytes = 64U * 1024U * 1024U;

		[[nodiscard]] bool HasIdentity(const TerraEngine::UUID& value) noexcept {
			return value != TerraEngine::EMPTY_UUID;
		}

		[[nodiscard]] bool IsCollectionProperty(AnimationPrefabPropertyId property) noexcept {
			return property == AnimationPrefabPropertyId::AttachmentTarget
				|| property == AnimationPrefabPropertyId::AttachmentJoint
				|| property == AnimationPrefabPropertyId::AttachmentOffset
				|| property == AnimationPrefabPropertyId::MorphDefault;
		}

		[[nodiscard]] bool IsKnownProperty(AnimationPrefabPropertyId property) noexcept {
			return property >= AnimationPrefabPropertyId::ControllerAssetId
				&& property <= AnimationPrefabPropertyId::MorphDefault;
		}

		[[nodiscard]] bool IsValidPath(const AnimationPrefabPropertyPath& path) noexcept {
			return HasIdentity(path.sourceObjectId)
				&& IsKnownProperty(path.property)
				&& (IsCollectionProperty(path.property) == HasIdentity(path.elementId));
		}

		[[nodiscard]] bool IsUuidProperty(AnimationPrefabPropertyId property) noexcept {
			return property >= AnimationPrefabPropertyId::ControllerAssetId
				&& property <= AnimationPrefabPropertyId::MotionDatabaseOverrideAssetId;
		}

		[[nodiscard]] bool IsUnsignedProperty(AnimationPrefabPropertyId property) noexcept {
			return property >= AnimationPrefabPropertyId::RequestedMode
				&& property <= AnimationPrefabPropertyId::TimePolicy
				|| property >= AnimationPrefabPropertyId::EventPolicy
					&& property <= AnimationPrefabPropertyId::DiagnosticOverrides;
		}

		[[nodiscard]] bool IsValidValue(
			AnimationPrefabPropertyId property,
			const AnimationPrefabValue& value) noexcept {
			if(std::holds_alternative<std::monostate>(value)) {
				return false;
			}
			if(IsUuidProperty(property)) {
				// EMPTY_UUID is a valid authored optional reference and must not be
				// silently replaced merely because its asset is currently missing.
				return std::holds_alternative<TerraEngine::UUID>(value);
			}
			if(IsUnsignedProperty(property)) {
				return std::holds_alternative<std::uint32_t>(value);
			}
			switch(property) {
			case AnimationPrefabPropertyId::InitialParameters:
			case AnimationPrefabPropertyId::AttachmentJoint:
			case AnimationPrefabPropertyId::AttachmentOffset:
				return std::holds_alternative<std::string>(value);
			case AnimationPrefabPropertyId::AttachmentTarget:
				if(const auto* connected = std::get_if<AnimationPrefabConnectedReference>(&value)) {
					return HasIdentity(connected->sourceObjectId);
				}
				return std::holds_alternative<TerraEngine::UUID>(value);
			case AnimationPrefabPropertyId::MorphDefault:
				return std::holds_alternative<float>(value);
			default:
				return false;
			}
		}

		[[nodiscard]] bool CompatibleValue(
			const AnimationPrefabValue& expected,
			const AnimationPrefabValue& candidate) noexcept {
			return expected.index() == candidate.index()
				|| std::holds_alternative<AnimationPrefabConnectedReference>(expected)
					&& std::holds_alternative<TerraEngine::UUID>(candidate)
				|| std::holds_alternative<TerraEngine::UUID>(expected)
					&& std::holds_alternative<AnimationPrefabConnectedReference>(candidate);
		}

		[[nodiscard]] AnimationPrefabValue ResolveConnectedValueForInstance(
			AnimationPrefabValue value,
			const AnimationPrefabInstance& instance) {
			auto* connected = std::get_if<AnimationPrefabConnectedReference>(&value);
			if(connected == nullptr) {
				return value;
			}
			const auto binding = instance.objects.find(connected->sourceObjectId);
			if(binding == instance.objects.end()) {
				return {};
			}
			connected->resolvedInstanceObjectId = binding->second.instanceObjectId;
			return value;
		}

		[[nodiscard]] AnimationPrefabValue NormalizeConnectedSourceValue(
			AnimationPrefabValue value) {
			if(auto* connected = std::get_if<AnimationPrefabConnectedReference>(&value)) {
				connected->resolvedInstanceObjectId = TerraEngine::EMPTY_UUID;
			}
			return value;
		}

		[[nodiscard]] TerraEngine::Reflection::Variant ToReflectedValue(
			const AnimationPrefabValue& value) {
			return std::visit([](const auto& current) -> TerraEngine::Reflection::Variant {
				using Value = std::decay_t<decltype(current)>;
				if constexpr(std::is_same_v<Value, std::monostate>) {
					return {};
				} else if constexpr(std::is_same_v<Value, AnimationPrefabConnectedReference>) {
					return TerraEngine::Reflection::Variant{current.resolvedInstanceObjectId};
				} else {
					return TerraEngine::Reflection::Variant{current};
				}
			}, value);
		}

		[[nodiscard]] AnimationPrefabStatus ValidateDocument(
			const AnimationPrefabDocument& source) noexcept {
			if(!HasIdentity(source.sourceId) || source.generation == 0U) {
				return AnimationPrefabStatus::InvalidArgument;
			}
			if(source.defaults.size() > kMaximumAnimationPrefabProperties) {
				return AnimationPrefabStatus::CapacityExceeded;
			}
			for(const auto& [path, value] : source.defaults) {
				if(!IsValidPath(path) || !IsValidValue(path.property, value)) {
					return AnimationPrefabStatus::InvalidArgument;
				}
			}
			return AnimationPrefabStatus::Ok;
		}

		[[nodiscard]] AnimationPrefabStatus ValidateInstance(
			const AnimationPrefabDocument& source,
			const AnimationPrefabInstance& instance) noexcept {
			if(!HasIdentity(instance.instanceId)
				|| instance.sourceId != source.sourceId
				|| instance.generation == 0U
				|| instance.sceneGeneration == 0U
				|| instance.sceneRevision == 0U) {
				return AnimationPrefabStatus::Incompatible;
			}
			if(instance.objects.size() > kMaximumAnimationPrefabProperties
				|| instance.overrides.size() > kMaximumAnimationPrefabProperties) {
				return AnimationPrefabStatus::CapacityExceeded;
			}
			for(const auto& [sourceObjectId, binding] : instance.objects) {
				if(!HasIdentity(sourceObjectId)
					|| !HasIdentity(binding.instanceObjectId)
					|| binding.lifetimeGeneration == 0U
					|| binding.componentGeneration == 0U) {
					return AnimationPrefabStatus::InvalidArgument;
				}
			}
			for(const auto& [path, value] : instance.overrides) {
				const auto sourceValue = source.defaults.find(path);
				if(sourceValue == source.defaults.end()
					|| !instance.objects.contains(path.sourceObjectId)
					|| !IsValidValue(path.property, value)
					|| !CompatibleValue(sourceValue->second, value)) {
					return AnimationPrefabStatus::Incompatible;
				}
				if(const auto* connected = std::get_if<AnimationPrefabConnectedReference>(&value)) {
					const auto binding = instance.objects.find(connected->sourceObjectId);
					if(binding == instance.objects.end()
						|| connected->resolvedInstanceObjectId != binding->second.instanceObjectId) {
						return AnimationPrefabStatus::Incompatible;
					}
				}
			}
			return AnimationPrefabStatus::Ok;
		}

		[[nodiscard]] bool CheckedIncrement(std::uint64_t& value) noexcept {
			if(value == std::numeric_limits<std::uint64_t>::max()) {
				return false;
			}
			++value;
			return true;
		}

		void InitializeSceneTransaction(
			AnimationPrefabPreparedMutation& prepared,
			const AnimationPrefabDocument& source,
			AnimationPrefabMutationKind kind) {
			prepared.sceneTransaction.id = "animation-prefab-"
				+ TerraEngine::ToHexString(source.sourceId)
				+ "-" + std::to_string(source.generation)
				+ "-" + std::to_string(static_cast<std::uint32_t>(kind));
			prepared.sceneTransaction.origin = TerraEngine::Scene::TransactionOrigin::Editor;
			prepared.sceneTransaction.mode = TerraEngine::Scene::TransactionMode::Commit;
			prepared.sceneTransaction.originName = "AnimationPrefabIntegration";
			prepared.sceneTransaction.undoGroupName = kind == AnimationPrefabMutationKind::GroupedOverride
				? "Edit animation prefab overrides"
				: kind == AnimationPrefabMutationKind::ApplyOverrides
					? "Apply animation prefab overrides"
					: "Revert animation prefab overrides";
			prepared.sceneTransaction.budget.maxWriteCount = kMaximumAnimationPrefabSelection;
			prepared.sceneTransaction.budget.maxConflictCount = kMaximumAnimationPrefabSelection;
		}

		[[nodiscard]] AnimationPrefabStatus MergeSceneGenerations(
			TerraEngine::Scene::Transaction& transaction,
			const AnimationPrefabInstance& instance) noexcept {
			if(transaction.readSet.sceneGeneration != 0U
				&& transaction.readSet.sceneGeneration != instance.sceneGeneration) {
				return AnimationPrefabStatus::Incompatible;
			}
			if(transaction.readSet.sceneRevision != 0U
				&& transaction.readSet.sceneRevision != instance.sceneRevision) {
				return AnimationPrefabStatus::Incompatible;
			}
			transaction.readSet.sceneGeneration = instance.sceneGeneration;
			transaction.readSet.sceneRevision = instance.sceneRevision;
			return AnimationPrefabStatus::Ok;
		}

		void AppendObservation(
			TerraEngine::Scene::Transaction& transaction,
			const AnimationPrefabObjectBinding& binding) {
			const auto existing = std::find_if(
				transaction.readSet.objects.begin(),
				transaction.readSet.objects.end(),
				[&](const TerraEngine::Scene::TransactionObjectObservation& value) {
					return value.objectId == binding.instanceObjectId;
				});
			if(existing != transaction.readSet.objects.end()) {
				return;
			}
			TerraEngine::Scene::TransactionObjectObservation observation;
			observation.objectId = binding.instanceObjectId;
			observation.lifetimeGeneration = binding.lifetimeGeneration;
			observation.metadataGeneration = binding.metadataGeneration;
			observation.componentGeneration = binding.componentGeneration;
			observation.hierarchyRevision = binding.hierarchyRevision;
			observation.transformRevision = binding.transformRevision;
			observation.parentObjectId = binding.parentObjectId;
			observation.parentLifetimeGeneration = binding.parentLifetimeGeneration;
			observation.parentTransformRevision = binding.parentTransformRevision;
			transaction.readSet.objects.push_back(std::move(observation));
		}

		[[nodiscard]] AnimationPrefabStatus AppendTypedSceneWrite(
			AnimationPrefabPreparedMutation& prepared,
			const AnimationPrefabInstance& instance,
			const AnimationPrefabPropertyPath& path,
			const AnimationPrefabValue& value) {
			const auto binding = instance.objects.find(path.sourceObjectId);
			if(binding == instance.objects.end()) {
				return AnimationPrefabStatus::Incompatible;
			}
			if(const AnimationPrefabStatus status = MergeSceneGenerations(
				prepared.sceneTransaction, instance); status != AnimationPrefabStatus::Ok) {
				return status;
			}

			TerraEngine::Scene::TransactionWriteOperation operation;
			operation.kind = TerraEngine::Scene::TransactionWriteKind::UpdateComponentProperty;
			operation.target.objectId = binding->second.instanceObjectId;
			operation.componentName = std::string(ToStableComponentName(path.property));
			operation.propertyName = std::string(ToStablePropertyName(path.property));
			operation.propertyValue = ToReflectedValue(value);
			if(operation.componentName.empty()
				|| operation.propertyName.empty()
				|| !TerraEngine::Reflection::IsValid(operation.propertyValue)) {
				return AnimationPrefabStatus::InvalidArgument;
			}
			AppendObservation(prepared.sceneTransaction, binding->second);
			prepared.sceneTransaction.writeSet.operations.push_back(std::move(operation));
			return AnimationPrefabStatus::Ok;
		}

		[[nodiscard]] bool IsUniqueSelection(
			std::span<const AnimationPrefabPropertyPath> paths) {
			std::set<AnimationPrefabPropertyPath> unique;
			return std::all_of(paths.begin(), paths.end(), [&](const AnimationPrefabPropertyPath& path) {
				return unique.insert(path).second;
			});
		}

		[[nodiscard]] bool FitsPlayModeByteBudget(
			const AnimationPlayModeAuthoredState& state) noexcept {
			std::size_t total = 0U;
			for(const std::size_t count : {
				state.sceneDocumentBytes.size(),
				state.animatorComponentBytes.size(),
				state.hierarchyBytes.size(),
				state.attachmentBytes.size(),
				state.animationAssetBytes.size()}) {
				if(count > kMaximumPlayModeAuthoredBytes - total) {
					return false;
				}
				total += count;
			}
			return true;
		}
	}

	std::string_view ToString(AnimationPrefabStatus status) noexcept {
		switch(status) {
		case AnimationPrefabStatus::Ok: return "Ok";
		case AnimationPrefabStatus::InvalidArgument: return "InvalidArgument";
		case AnimationPrefabStatus::NotFound: return "NotFound";
		case AnimationPrefabStatus::Incompatible: return "Incompatible";
		case AnimationPrefabStatus::Stale: return "Stale";
		case AnimationPrefabStatus::CapacityExceeded: return "CapacityExceeded";
		case AnimationPrefabStatus::Rejected: return "Rejected";
		case AnimationPrefabStatus::InternalError: return "InternalError";
		}
		return "Unknown";
	}

	std::string_view ToStableComponentName(AnimationPrefabPropertyId property) noexcept {
		if(property <= AnimationPrefabPropertyId::DiagnosticOverrides) {
			return "AnimatorComponent";
		}
		if(property >= AnimationPrefabPropertyId::AttachmentTarget
			&& property <= AnimationPrefabPropertyId::AttachmentOffset) {
			return "AnimationAttachmentComponent";
		}
		return property == AnimationPrefabPropertyId::MorphDefault
			? "AnimationMorphDefaultsComponent" : std::string_view{};
	}

	std::string_view ToStablePropertyName(AnimationPrefabPropertyId property) noexcept {
		switch(property) {
		case AnimationPrefabPropertyId::ControllerAssetId: return "controllerAssetId";
		case AnimationPrefabPropertyId::DefaultClipAssetId: return "defaultClipAssetId";
		case AnimationPrefabPropertyId::SkeletonOverrideAssetId: return "skeletonOverrideAssetId";
		case AnimationPrefabPropertyId::AvatarOverrideAssetId: return "avatarOverrideAssetId";
		case AnimationPrefabPropertyId::RetargetProfileOverrideAssetId: return "retargetProfileOverrideAssetId";
		case AnimationPrefabPropertyId::RagdollProfileOverrideAssetId: return "ragdollProfileOverrideAssetId";
		case AnimationPrefabPropertyId::MotionDatabaseOverrideAssetId: return "motionDatabaseOverrideAssetId";
		case AnimationPrefabPropertyId::RequestedMode: return "requestedMode";
		case AnimationPrefabPropertyId::RootMotionPolicy: return "rootMotionPolicy";
		case AnimationPrefabPropertyId::UpdatePolicy: return "updatePolicy";
		case AnimationPrefabPropertyId::TimePolicy: return "timePolicy";
		case AnimationPrefabPropertyId::InitialParameters: return "initialParameters";
		case AnimationPrefabPropertyId::EventPolicy: return "eventPolicy";
		case AnimationPrefabPropertyId::CullingPolicy: return "cullingPolicy";
		case AnimationPrefabPropertyId::DiagnosticOverrides: return "diagnosticOverrides";
		case AnimationPrefabPropertyId::AttachmentTarget: return "targetObjectId";
		case AnimationPrefabPropertyId::AttachmentJoint: return "joint";
		case AnimationPrefabPropertyId::AttachmentOffset: return "localOffset";
		case AnimationPrefabPropertyId::MorphDefault: return "weight";
		}
		return {};
	}

	std::string StableAnimationPrefabPropertyPath(
		const AnimationPrefabPropertyPath& path) {
		if(!IsValidPath(path)) {
			return {};
		}
		std::string stable = "object/" + TerraEngine::ToHexString(path.sourceObjectId)
			+ "/component/" + std::string(ToStableComponentName(path.property))
			+ "/property/" + std::string(ToStablePropertyName(path.property));
		if(HasIdentity(path.elementId)) {
			stable += "/element/" + TerraEngine::ToHexString(path.elementId);
		}
		return stable;
	}

	AnimationPrefabValue ResolveAnimationPrefabValue(
		const AnimationPrefabDocument& source,
		const AnimationPrefabInstance& instance,
		const AnimationPrefabPropertyPath& path) {
		if(const auto overrideValue = instance.overrides.find(path);
			overrideValue != instance.overrides.end()) {
			return overrideValue->second;
		}
		if(const auto sourceValue = source.defaults.find(path);
			sourceValue != source.defaults.end()) {
			return ResolveConnectedValueForInstance(sourceValue->second, instance);
		}
		return {};
	}

	AnimationPrefabIntegration::~AnimationPrefabIntegration() {
		ClearPlayModeCaptures();
	}

	AnimationPrefabStatus AnimationPrefabIntegration::InspectMixedValue(
		const AnimationPrefabDocument& source,
		std::span<const AnimationPrefabInstance> instances,
		const AnimationPrefabPropertyPath& path,
		AnimationPrefabMixedValue& output) const noexcept {
		try {
			if(const AnimationPrefabStatus status = ValidateDocument(source);
				status != AnimationPrefabStatus::Ok) {
				return status;
			}
			if(instances.empty() || instances.size() > kMaximumAnimationPrefabSelection
				|| !IsValidPath(path)) {
				return instances.size() > kMaximumAnimationPrefabSelection
					? AnimationPrefabStatus::CapacityExceeded
					: AnimationPrefabStatus::InvalidArgument;
			}

			AnimationPrefabMixedValue staged;
			std::optional<AnimationPrefabValue> first;
			for(const AnimationPrefabInstance& instance : instances) {
				if(ValidateInstance(source, instance) != AnimationPrefabStatus::Ok) {
					staged.state = AnimationPrefabMixedState::Incompatible;
					output = std::move(staged);
					return AnimationPrefabStatus::Incompatible;
				}
				AnimationPrefabValue value = ResolveAnimationPrefabValue(source, instance, path);
				if(std::holds_alternative<std::monostate>(value)) {
					++staged.unavailableCount;
					continue;
				}
				++staged.compatibleCount;
				if(!first.has_value()) {
					first = value;
					staged.uniformValue = value;
				} else if(*first != value) {
					staged.state = AnimationPrefabMixedState::Mixed;
				}
			}
			if(staged.compatibleCount == 0U) {
				staged.state = AnimationPrefabMixedState::Unavailable;
			} else if(staged.state != AnimationPrefabMixedState::Mixed) {
				staged.state = staged.unavailableCount == 0U
					? AnimationPrefabMixedState::Uniform
					: AnimationPrefabMixedState::Mixed;
			}
			output = std::move(staged);
			return AnimationPrefabStatus::Ok;
		} catch(...) {
			return AnimationPrefabStatus::InternalError;
		}
	}

	AnimationPrefabStatus AnimationPrefabIntegration::DuplicateConnected(
		const AnimationPrefabInstance& source,
		TerraEngine::UUID duplicateInstanceId,
		const std::map<TerraEngine::UUID, AnimationPrefabObjectBinding>& duplicateObjects,
		AnimationPrefabInstance& output) const noexcept {
		try {
			if(!HasIdentity(source.instanceId) || !HasIdentity(source.sourceId)
				|| !HasIdentity(duplicateInstanceId) || duplicateInstanceId == source.instanceId
				|| duplicateObjects.empty()
				|| duplicateObjects.size() > kMaximumAnimationPrefabProperties) {
				return AnimationPrefabStatus::InvalidArgument;
			}
			AnimationPrefabInstance candidate = source;
			candidate.instanceId = duplicateInstanceId;
			candidate.generation = 1U;
			candidate.objects = duplicateObjects;
			for(auto& [path, value] : candidate.overrides) {
				(void)path;
				auto* connected = std::get_if<AnimationPrefabConnectedReference>(&value);
				if(connected == nullptr) {
					continue;
				}
				const auto mapped = candidate.objects.find(connected->sourceObjectId);
				if(mapped == candidate.objects.end()
					|| !HasIdentity(mapped->second.instanceObjectId)) {
					return AnimationPrefabStatus::Incompatible;
				}
				connected->resolvedInstanceObjectId = mapped->second.instanceObjectId;
			}
			output = std::move(candidate);
			return AnimationPrefabStatus::Ok;
		} catch(...) {
			return AnimationPrefabStatus::InternalError;
		}
	}

	AnimationPrefabStatus AnimationPrefabIntegration::PrepareGroupedOverride(
		const AnimationPrefabDocument& source,
		std::span<const AnimationPrefabInstance> instances,
		const AnimationPrefabPropertyPath& path,
		const AnimationPrefabValue& value,
		AnimationPrefabPreparedMutation& output) const noexcept {
		try {
			if(const AnimationPrefabStatus status = ValidateDocument(source);
				status != AnimationPrefabStatus::Ok) {
				return status;
			}
			if(instances.empty() || instances.size() > kMaximumAnimationPrefabSelection
				|| !IsValidPath(path) || !IsValidValue(path.property, value)) {
				return instances.size() > kMaximumAnimationPrefabSelection
					? AnimationPrefabStatus::CapacityExceeded
					: AnimationPrefabStatus::InvalidArgument;
			}
			const auto sourceValue = source.defaults.find(path);
			if(sourceValue == source.defaults.end()) {
				return AnimationPrefabStatus::NotFound;
			}
			if(!CompatibleValue(sourceValue->second, value)) {
				return AnimationPrefabStatus::Incompatible;
			}

			AnimationPrefabPreparedMutation staged;
			staged.kind = AnimationPrefabMutationKind::GroupedOverride;
			staged.expectedSourceId = source.sourceId;
			staged.expectedSourceGeneration = source.generation;
			staged.sourceCandidate = source;
			InitializeSceneTransaction(staged, source, staged.kind);
			std::set<TerraEngine::UUID> instanceIds;
			for(const AnimationPrefabInstance& instance : instances) {
				if(!instanceIds.insert(instance.instanceId).second) {
					return AnimationPrefabStatus::InvalidArgument;
				}
				if(instance.sourceId != source.sourceId
					|| !instance.objects.contains(path.sourceObjectId)
					|| (staged.sceneTransaction.readSet.sceneGeneration != 0U
						&& staged.sceneTransaction.readSet.sceneGeneration != instance.sceneGeneration)
					|| (staged.sceneTransaction.readSet.sceneRevision != 0U
						&& staged.sceneTransaction.readSet.sceneRevision != instance.sceneRevision)) {
					++staged.skippedIncompatibleInstanceCount;
					continue;
				}
				if(const AnimationPrefabStatus status = ValidateInstance(source, instance);
					status != AnimationPrefabStatus::Ok) {
					return status;
				}
				AnimationPrefabValue instanceValue = ResolveConnectedValueForInstance(value, instance);
				if(std::holds_alternative<std::monostate>(instanceValue)) {
					++staged.skippedIncompatibleInstanceCount;
					continue;
				}
				AnimationPrefabPreparedInstance preparedInstance;
				preparedInstance.instanceId = instance.instanceId;
				preparedInstance.expectedGeneration = instance.generation;
				preparedInstance.candidate = instance;
				if(!CheckedIncrement(preparedInstance.candidate.generation)) {
					return AnimationPrefabStatus::CapacityExceeded;
				}
				preparedInstance.candidate.overrides[path] = instanceValue;
				if(const AnimationPrefabStatus status = AppendTypedSceneWrite(
					staged, instance, path, instanceValue); status != AnimationPrefabStatus::Ok) {
					return status;
				}
				staged.instances.push_back(std::move(preparedInstance));
				++staged.authoredMutationCount;
				++staged.compatibleInstanceCount;
			}
			if(staged.instances.empty()) {
				return AnimationPrefabStatus::Incompatible;
			}
			output = std::move(staged);
			return AnimationPrefabStatus::Ok;
		} catch(...) {
			return AnimationPrefabStatus::InternalError;
		}
	}

	AnimationPrefabStatus AnimationPrefabIntegration::PrepareApplyOverrides(
		const AnimationPrefabDocument& source,
		const AnimationPrefabInstance& instance,
		std::span<const AnimationPrefabPropertyPath> selectedPaths,
		AnimationPrefabPreparedMutation& output) const noexcept {
		try {
			if(const AnimationPrefabStatus status = ValidateDocument(source);
				status != AnimationPrefabStatus::Ok) {
				return status;
			}
			if(const AnimationPrefabStatus status = ValidateInstance(source, instance);
				status != AnimationPrefabStatus::Ok) {
				return status;
			}
			if(selectedPaths.empty() || selectedPaths.size() > kMaximumAnimationPrefabSelection
				|| !IsUniqueSelection(selectedPaths)) {
				return selectedPaths.size() > kMaximumAnimationPrefabSelection
					? AnimationPrefabStatus::CapacityExceeded
					: AnimationPrefabStatus::InvalidArgument;
			}

			AnimationPrefabPreparedMutation staged;
			staged.kind = AnimationPrefabMutationKind::ApplyOverrides;
			staged.expectedSourceId = source.sourceId;
			staged.expectedSourceGeneration = source.generation;
			staged.writesSource = true;
			staged.sourceCandidate = source;
			InitializeSceneTransaction(staged, source, staged.kind);
			AnimationPrefabPreparedInstance preparedInstance;
			preparedInstance.instanceId = instance.instanceId;
			preparedInstance.expectedGeneration = instance.generation;
			preparedInstance.candidate = instance;
			for(const AnimationPrefabPropertyPath& path : selectedPaths) {
				const auto overrideValue = instance.overrides.find(path);
				const auto sourceValue = source.defaults.find(path);
				if(overrideValue == instance.overrides.end()
					|| sourceValue == source.defaults.end()) {
					return AnimationPrefabStatus::NotFound;
				}
				if(!CompatibleValue(sourceValue->second, overrideValue->second)) {
					return AnimationPrefabStatus::Incompatible;
				}
				staged.sourceCandidate.defaults[path] = NormalizeConnectedSourceValue(
					overrideValue->second);
				preparedInstance.candidate.overrides.erase(path);
				++staged.authoredMutationCount;
			}
			if(!CheckedIncrement(staged.sourceCandidate.generation)
				|| !CheckedIncrement(preparedInstance.candidate.generation)) {
				return AnimationPrefabStatus::CapacityExceeded;
			}
			staged.instances.push_back(std::move(preparedInstance));
			staged.compatibleInstanceCount = 1U;
			output = std::move(staged);
			return AnimationPrefabStatus::Ok;
		} catch(...) {
			return AnimationPrefabStatus::InternalError;
		}
	}

	AnimationPrefabStatus AnimationPrefabIntegration::PrepareRevertOverrides(
		const AnimationPrefabDocument& source,
		const AnimationPrefabInstance& instance,
		std::span<const AnimationPrefabPropertyPath> selectedPaths,
		AnimationPrefabPreparedMutation& output) const noexcept {
		try {
			if(const AnimationPrefabStatus status = ValidateDocument(source);
				status != AnimationPrefabStatus::Ok) {
				return status;
			}
			if(const AnimationPrefabStatus status = ValidateInstance(source, instance);
				status != AnimationPrefabStatus::Ok) {
				return status;
			}
			if(selectedPaths.empty() || selectedPaths.size() > kMaximumAnimationPrefabSelection
				|| !IsUniqueSelection(selectedPaths)) {
				return selectedPaths.size() > kMaximumAnimationPrefabSelection
					? AnimationPrefabStatus::CapacityExceeded
					: AnimationPrefabStatus::InvalidArgument;
			}

			AnimationPrefabPreparedMutation staged;
			staged.kind = AnimationPrefabMutationKind::RevertOverrides;
			staged.expectedSourceId = source.sourceId;
			staged.expectedSourceGeneration = source.generation;
			staged.sourceCandidate = source;
			InitializeSceneTransaction(staged, source, staged.kind);
			AnimationPrefabPreparedInstance preparedInstance;
			preparedInstance.instanceId = instance.instanceId;
			preparedInstance.expectedGeneration = instance.generation;
			preparedInstance.candidate = instance;
			for(const AnimationPrefabPropertyPath& path : selectedPaths) {
				if(!instance.overrides.contains(path)) {
					return AnimationPrefabStatus::NotFound;
				}
				const auto sourceValue = source.defaults.find(path);
				if(sourceValue == source.defaults.end()) {
					return AnimationPrefabStatus::NotFound;
				}
				preparedInstance.candidate.overrides.erase(path);
				const AnimationPrefabValue effective = ResolveConnectedValueForInstance(
					sourceValue->second, instance);
				if(std::holds_alternative<std::monostate>(effective)) {
					return AnimationPrefabStatus::Incompatible;
				}
				if(const AnimationPrefabStatus status = AppendTypedSceneWrite(
					staged, instance, path, effective);
					status != AnimationPrefabStatus::Ok) {
					return status;
				}
				++staged.authoredMutationCount;
			}
			if(!CheckedIncrement(preparedInstance.candidate.generation)) {
				return AnimationPrefabStatus::CapacityExceeded;
			}
			staged.instances.push_back(std::move(preparedInstance));
			staged.compatibleInstanceCount = 1U;
			output = std::move(staged);
			return AnimationPrefabStatus::Ok;
		} catch(...) {
			return AnimationPrefabStatus::InternalError;
		}
	}

	AnimationPrefabStatus AnimationPrefabIntegration::PublishPrepared(
		const AnimationPrefabPreparedMutation& prepared,
		AnimationPrefabDocument& source,
		std::vector<AnimationPrefabInstance>& instances) const noexcept {
		try {
			if(prepared.schemaVersion != kAnimationPrefabResultSchemaVersion
				|| !prepared.declaration.workerSafePreparation
				|| !prepared.declaration.protectedPublication
				|| prepared.declaration.directLiveObjectMutation
				|| prepared.expectedSourceId != source.sourceId
				|| prepared.expectedSourceGeneration != source.generation
				|| prepared.instances.empty()
				|| prepared.instances.size() > kMaximumAnimationPrefabSelection) {
				return prepared.expectedSourceId == source.sourceId
					&& prepared.expectedSourceGeneration != source.generation
					? AnimationPrefabStatus::Stale
					: AnimationPrefabStatus::InvalidArgument;
			}

			std::vector<std::pair<std::size_t, const AnimationPrefabPreparedInstance*>> matches;
			std::set<TerraEngine::UUID> unique;
			matches.reserve(prepared.instances.size());
			for(const AnimationPrefabPreparedInstance& candidate : prepared.instances) {
				if(!unique.insert(candidate.instanceId).second) {
					return AnimationPrefabStatus::InvalidArgument;
				}
				const auto current = std::find_if(instances.begin(), instances.end(),
					[&](const AnimationPrefabInstance& value) {
						return value.instanceId == candidate.instanceId;
					});
				if(current == instances.end()) {
					return AnimationPrefabStatus::Stale;
				}
				if(current->sourceId != prepared.expectedSourceId
					|| current->generation != candidate.expectedGeneration) {
					return AnimationPrefabStatus::Stale;
				}
				matches.emplace_back(
					static_cast<std::size_t>(std::distance(instances.begin(), current)),
					&candidate);
			}

			AnimationPrefabDocument stagedSource = source;
			std::vector<AnimationPrefabInstance> stagedInstances = instances;
			if(prepared.writesSource) {
				if(prepared.sourceCandidate.sourceId != source.sourceId
					|| prepared.sourceCandidate.generation != source.generation + 1U) {
					return AnimationPrefabStatus::InvalidArgument;
				}
				stagedSource = prepared.sourceCandidate;
			}
			for(const auto& [index, candidate] : matches) {
				if(candidate->candidate.instanceId != candidate->instanceId
					|| candidate->candidate.sourceId != source.sourceId
					|| candidate->candidate.generation != candidate->expectedGeneration + 1U) {
					return AnimationPrefabStatus::InvalidArgument;
				}
				stagedInstances[index] = candidate->candidate;
			}

			// Both move assignments transfer already-validated owned containers and do
			// not invoke ObjectManager. The editor queues sceneTransaction separately
			// for its normal protected scene flush and undo receipt path.
			source = std::move(stagedSource);
			instances = std::move(stagedInstances);
			return AnimationPrefabStatus::Ok;
		} catch(...) {
			return AnimationPrefabStatus::InternalError;
		}
	}

	AnimationPrefabStatus AnimationPrefabIntegration::CapturePlayMode(
		const AnimationPlayModeAuthoredState& authored,
		AnimationPlayModeCaptureHandle& output) noexcept {
		try {
			if(!HasIdentity(authored.sceneId) || authored.sceneGeneration == 0U) {
				return AnimationPrefabStatus::InvalidArgument;
			}
			if(!FitsPlayModeByteBudget(authored)) {
				return AnimationPrefabStatus::CapacityExceeded;
			}
			std::scoped_lock lock(playModeMutex_);
			for(std::size_t index = 0U; index < playModeSlots_.size(); ++index) {
				PlayModeSlot& slot = playModeSlots_[index];
				if(slot.retired || slot.captured.has_value()) {
					continue;
				}
				slot.captured = authored;
				++activePlayModeCaptures_;
				output = AnimationPlayModeCaptureHandle{
					static_cast<std::uint32_t>(index),
					slot.generation};
				return AnimationPrefabStatus::Ok;
			}
			return AnimationPrefabStatus::CapacityExceeded;
		} catch(...) {
			return AnimationPrefabStatus::InternalError;
		}
	}

	AnimationPrefabStatus AnimationPrefabIntegration::PreparePlayModeRestore(
		AnimationPlayModeCaptureHandle handle,
		TerraEngine::UUID currentSceneId,
		std::uint64_t currentSceneGeneration,
		AnimationPlayModePreparedRestore& output) const noexcept {
		try {
			std::scoped_lock lock(playModeMutex_);
			const PlayModeSlot* slot = nullptr;
			if(const AnimationPrefabStatus status = ResolvePlayModeSlot(
				playModeSlots_, handle, slot); status != AnimationPrefabStatus::Ok) {
				return status;
			}
			if(slot->captured->sceneId != currentSceneId
				|| slot->captured->sceneGeneration != currentSceneGeneration) {
				return AnimationPrefabStatus::Stale;
			}
			AnimationPlayModePreparedRestore staged;
			staged.handle = handle;
			staged.restored = *slot->captured;
			output = std::move(staged);
			return AnimationPrefabStatus::Ok;
		} catch(...) {
			return AnimationPrefabStatus::InternalError;
		}
	}

	AnimationPrefabStatus AnimationPrefabIntegration::PublishPlayModeRestore(
		const AnimationPlayModePreparedRestore& prepared,
		AnimationPlayModeAuthoredState& authored) noexcept {
		try {
			if(prepared.schemaVersion != kAnimationPrefabResultSchemaVersion
				|| !prepared.declaration.workerSafePreparation
				|| !prepared.declaration.protectedPublication
				|| prepared.declaration.directLiveObjectMutation) {
				return AnimationPrefabStatus::InvalidArgument;
			}
			std::scoped_lock lock(playModeMutex_);
			PlayModeSlot* slot = nullptr;
			if(const AnimationPrefabStatus status = ResolvePlayModeSlot(
				playModeSlots_, prepared.handle, slot); status != AnimationPrefabStatus::Ok) {
				return status;
			}
			if(*slot->captured != prepared.restored
				|| authored.sceneId != prepared.restored.sceneId
				|| authored.sceneGeneration != prepared.restored.sceneGeneration) {
				return AnimationPrefabStatus::Stale;
			}
			AnimationPlayModeAuthoredState staged = prepared.restored;
			authored = std::move(staged);
			RetirePlayModeSlot(*slot);
			if(activePlayModeCaptures_ != 0U) {
				--activePlayModeCaptures_;
			}
			return AnimationPrefabStatus::Ok;
		} catch(...) {
			return AnimationPrefabStatus::InternalError;
		}
	}

	void AnimationPrefabIntegration::ClearPlayModeCaptures() noexcept {
		try {
			std::scoped_lock lock(playModeMutex_);
			for(PlayModeSlot& slot : playModeSlots_) {
				if(slot.captured.has_value()) {
					RetirePlayModeSlot(slot);
				}
			}
			activePlayModeCaptures_ = 0U;
		} catch(...) {
			// Teardown must remain noexcept. A lock failure cannot expose a live
			// ObjectManager write because captures are editor-owned byte copies.
		}
	}

	std::size_t AnimationPrefabIntegration::ActivePlayModeCaptureCount() const noexcept {
		try {
			std::scoped_lock lock(playModeMutex_);
			return activePlayModeCaptures_;
		} catch(...) {
			return 0U;
		}
	}

	AnimationPrefabStatus AnimationPrefabIntegration::ResolvePlayModeSlot(
		std::array<PlayModeSlot, kMaximumPlayModeCaptures>& slots,
		AnimationPlayModeCaptureHandle handle,
		PlayModeSlot*& output) noexcept {
		if(!handle.IsValid()) {
			return AnimationPrefabStatus::InvalidArgument;
		}
		PlayModeSlot& slot = slots[handle.slotIndex];
		if(slot.retired || slot.generation != handle.generation
			|| !slot.captured.has_value()) {
			return AnimationPrefabStatus::Stale;
		}
		output = &slot;
		return AnimationPrefabStatus::Ok;
	}

	AnimationPrefabStatus AnimationPrefabIntegration::ResolvePlayModeSlot(
		const std::array<PlayModeSlot, kMaximumPlayModeCaptures>& slots,
		AnimationPlayModeCaptureHandle handle,
		const PlayModeSlot*& output) noexcept {
		if(!handle.IsValid()) {
			return AnimationPrefabStatus::InvalidArgument;
		}
		const PlayModeSlot& slot = slots[handle.slotIndex];
		if(slot.retired || slot.generation != handle.generation
			|| !slot.captured.has_value()) {
			return AnimationPrefabStatus::Stale;
		}
		output = &slot;
		return AnimationPrefabStatus::Ok;
	}

	void AnimationPrefabIntegration::RetirePlayModeSlot(PlayModeSlot& slot) noexcept {
		slot.captured.reset();
		if(slot.generation == std::numeric_limits<std::uint32_t>::max()) {
			slot.retired = true;
			return;
		}
		++slot.generation;
	}
}
