// AnimationPrefabPlayModeTests.cpp - prefab and Play Mode authoring contracts.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Animation/AnimationPrefabIntegration.h"

#include "Engine/Scene/TransactionalMutation.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests {
	using namespace TerraEngine::Editor::Animation;

	namespace {
		TerraEngine::UUID MakeUuid(std::uint8_t seed) {
			TerraEngine::UUID value{};
			for(std::size_t index = 0U; index < value.size(); ++index) {
				value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return value;
		}

		std::vector<std::byte> Bytes(std::initializer_list<std::uint8_t> values) {
			std::vector<std::byte> bytes;
			bytes.reserve(values.size());
			for(const std::uint8_t value : values) {
				bytes.push_back(static_cast<std::byte>(value));
			}
			return bytes;
		}

		AnimationPrefabObjectBinding MakeBinding(
			TerraEngine::UUID instanceObjectId,
			std::uint64_t generation) {
			AnimationPrefabObjectBinding binding;
			binding.instanceObjectId = instanceObjectId;
			binding.lifetimeGeneration = generation;
			binding.metadataGeneration = generation + 1U;
			binding.componentGeneration = generation + 2U;
			binding.hierarchyRevision = generation + 3U;
			binding.transformRevision = generation + 4U;
			binding.parentObjectId = MakeUuid(static_cast<std::uint8_t>(generation + 30U));
			binding.parentLifetimeGeneration = generation + 5U;
			binding.parentTransformRevision = generation + 6U;
			return binding;
		}

		AnimationPrefabInstance MakeInstance(
			TerraEngine::UUID instanceId,
			TerraEngine::UUID sourceId,
			TerraEngine::UUID sourceObjectId,
			TerraEngine::UUID instanceObjectId,
			std::uint64_t generation) {
			AnimationPrefabInstance instance;
			instance.instanceId = instanceId;
			instance.sourceId = sourceId;
			instance.generation = generation;
			instance.sceneGeneration = 41U;
			instance.sceneRevision = 73U;
			instance.objects.emplace(sourceObjectId, MakeBinding(instanceObjectId, generation));
			return instance;
		}
	}

	void RegisterAnimationPrefabPlayModeTests(TestRegistry& registry) {
		registry.Add("Editor.Animation.Prefab.StablePathsAndConnectedDuplication", [](TestContext& context) {
			const TerraEngine::UUID sourceId = MakeUuid(1U);
			const TerraEngine::UUID sourceRoot = MakeUuid(21U);
			const TerraEngine::UUID sourceSocketTarget = MakeUuid(41U);
			const TerraEngine::UUID firstRoot = MakeUuid(61U);
			const TerraEngine::UUID firstSocketTarget = MakeUuid(81U);
			const TerraEngine::UUID secondRoot = MakeUuid(101U);
			const TerraEngine::UUID secondSocketTarget = MakeUuid(121U);
			const AnimationPrefabPropertyPath attachmentPath{
				sourceRoot,
				AnimationPrefabPropertyId::AttachmentTarget,
				MakeUuid(141U)};

			AnimationPrefabInstance original = MakeInstance(
				MakeUuid(161U), sourceId, sourceRoot, firstRoot, 7U);
			original.objects.emplace(sourceSocketTarget, MakeBinding(firstSocketTarget, 9U));
			original.overrides.emplace(attachmentPath, AnimationPrefabConnectedReference{
				sourceSocketTarget,
				firstSocketTarget});

			std::map<TerraEngine::UUID, AnimationPrefabObjectBinding> duplicateObjects;
			duplicateObjects.emplace(sourceRoot, MakeBinding(secondRoot, 12U));
			duplicateObjects.emplace(sourceSocketTarget, MakeBinding(secondSocketTarget, 13U));
			AnimationPrefabInstance duplicate;
			AnimationPrefabIntegration integration;
			context.Expect(integration.DuplicateConnected(
				original, MakeUuid(181U), duplicateObjects, duplicate) == AnimationPrefabStatus::Ok,
				"Connected duplication should preserve source identities and resolve a fresh instance mapping.");

			const auto copied = duplicate.overrides.find(attachmentPath);
			const auto* copiedReference = copied == duplicate.overrides.end()
				? nullptr
				: std::get_if<AnimationPrefabConnectedReference>(&copied->second);
			context.Expect(copiedReference != nullptr
				&& copiedReference->sourceObjectId == sourceSocketTarget
				&& copiedReference->resolvedInstanceObjectId == secondSocketTarget,
				"A connected override should retain its stable source ID while remapping only the duplicate instance ID.");
			context.Expect(StableAnimationPrefabPropertyPath(attachmentPath)
				== StableAnimationPrefabPropertyPath(copied->first),
				"Stable override paths must not depend on the current instance object UUID or collection order.");

			original.objects.at(sourceSocketTarget).instanceObjectId = MakeUuid(201U);
			std::get<AnimationPrefabConnectedReference>(original.overrides.at(attachmentPath))
				.resolvedInstanceObjectId = MakeUuid(221U);
			context.Expect(duplicate.objects.at(sourceSocketTarget).instanceObjectId == secondSocketTarget
				&& std::get<AnimationPrefabConnectedReference>(duplicate.overrides.at(attachmentPath))
					.resolvedInstanceObjectId == secondSocketTarget,
				"Duplicated prefab authoring data must own independent copied maps and connected references.");
		});

		registry.Add("Editor.Animation.Prefab.MixedGroupedEditUsesTypedDeferredTransaction", [](TestContext& context) {
			const TerraEngine::UUID sourceId = MakeUuid(2U);
			const TerraEngine::UUID sourceRoot = MakeUuid(22U);
			const AnimationPrefabPropertyPath modePath{
				sourceRoot,
				AnimationPrefabPropertyId::RequestedMode,
				TerraEngine::EMPTY_UUID};
			AnimationPrefabDocument source;
			source.sourceId = sourceId;
			source.generation = 10U;
			source.defaults.emplace(modePath, std::uint32_t{0U});

			std::vector<AnimationPrefabInstance> instances;
			instances.push_back(MakeInstance(MakeUuid(42U), sourceId, sourceRoot, MakeUuid(62U), 3U));
			instances.push_back(MakeInstance(MakeUuid(82U), sourceId, sourceRoot, MakeUuid(102U), 5U));
			instances.front().overrides.emplace(modePath, std::uint32_t{1U});

			AnimationPrefabIntegration integration;
			AnimationPrefabMixedValue mixed;
			context.Expect(integration.InspectMixedValue(source, instances, modePath, mixed)
				== AnimationPrefabStatus::Ok
				&& mixed.state == AnimationPrefabMixedState::Mixed
				&& mixed.compatibleCount == 2U,
				"A grouped inspector should distinguish a real mixed value from unavailable or incompatible data.");

			AnimationPrefabInstance incompatible = MakeInstance(
				MakeUuid(122U), MakeUuid(142U), sourceRoot, MakeUuid(162U), 9U);
			instances.push_back(incompatible);
			const std::vector<AnimationPrefabInstance> before = instances;
			AnimationPrefabPreparedMutation prepared;
			context.Expect(integration.PrepareGroupedOverride(
				source, instances, modePath, std::uint32_t{3U}, prepared) == AnimationPrefabStatus::Ok,
				"Worker-safe grouped preparation should validate and copy every compatible selected instance.");
			context.Expect(instances == before
				&& prepared.declaration.workerSafePreparation
				&& prepared.declaration.protectedPublication
				&& prepared.sceneTransaction.origin == TerraEngine::Scene::TransactionOrigin::Editor
				&& prepared.sceneTransaction.writeSet.operations.size() == 2U
				&& prepared.compatibleInstanceCount == 2U
				&& prepared.skippedIncompatibleInstanceCount == 1U,
				"Preparation must not mutate authored models and must expose its worker/protected boundary declaration.");
			bool everyWriteTyped = true;
			for(const TerraEngine::Scene::TransactionWriteOperation& operation
				: prepared.sceneTransaction.writeSet.operations) {
				everyWriteTyped = everyWriteTyped
					&& operation.kind == TerraEngine::Scene::TransactionWriteKind::UpdateComponentProperty
					&& operation.componentName == "AnimatorComponent"
					&& operation.propertyName == "requestedMode"
					&& TerraEngine::Reflection::IsValid(operation.propertyValue);
			}
			context.Expect(everyWriteTyped,
				"Grouped edits must leave workers as typed deferred scene writes, never JSON or live ObjectManager mutation.");
			const AnimationPrefabObjectBinding& firstBinding =
				before.front().objects.at(sourceRoot);
			const TerraEngine::Scene::TransactionObjectObservation* firstObservation =
				prepared.sceneTransaction.readSet.objects.empty()
					? nullptr
					: &prepared.sceneTransaction.readSet.objects.front();
			context.Expect(firstObservation != nullptr
				&& firstObservation->objectId == firstBinding.instanceObjectId
				&& firstObservation->lifetimeGeneration == firstBinding.lifetimeGeneration
				&& firstObservation->metadataGeneration == firstBinding.metadataGeneration
				&& firstObservation->componentGeneration == firstBinding.componentGeneration
				&& firstObservation->hierarchyRevision == firstBinding.hierarchyRevision
				&& firstObservation->transformRevision == firstBinding.transformRevision
				&& firstObservation->parentObjectId == firstBinding.parentObjectId
				&& firstObservation->parentLifetimeGeneration == firstBinding.parentLifetimeGeneration
				&& firstObservation->parentTransformRevision == firstBinding.parentTransformRevision,
				"Prepared writes must preserve a complete copied read observation so child-object transactions survive real preflight validation.");

			context.Expect(integration.PublishPrepared(prepared, source, instances)
				== AnimationPrefabStatus::Ok
				&& std::get<std::uint32_t>(instances[0U].overrides.at(modePath)) == 3U
				&& std::get<std::uint32_t>(instances[1U].overrides.at(modePath)) == 3U
				&& instances[0U].generation == 4U
				&& instances[1U].generation == 6U
				&& instances[2U] == incompatible,
				"The protected boundary should publish the whole compatible grouped edit as one atomic value.");
		});

		registry.Add("Editor.Animation.Prefab.ApplyRevertAndStaleBatchAreAtomic", [](TestContext& context) {
			const TerraEngine::UUID sourceId = MakeUuid(3U);
			const TerraEngine::UUID sourceRoot = MakeUuid(23U);
			const AnimationPrefabPropertyPath controllerPath{
				sourceRoot,
				AnimationPrefabPropertyId::ControllerAssetId,
				TerraEngine::EMPTY_UUID};
			const TerraEngine::UUID walk = MakeUuid(43U);
			const TerraEngine::UUID run = MakeUuid(63U);
			AnimationPrefabDocument source;
			source.sourceId = sourceId;
			source.generation = 14U;
			source.defaults.emplace(controllerPath, walk);

			AnimationPrefabInstance instance = MakeInstance(
				MakeUuid(83U), sourceId, sourceRoot, MakeUuid(103U), 8U);
			instance.overrides.emplace(controllerPath, run);
			std::vector<AnimationPrefabInstance> instances{instance};
			AnimationPrefabIntegration integration;

			AnimationPrefabPreparedMutation apply;
			const std::array selected{controllerPath};
			context.Expect(integration.PrepareApplyOverrides(
				source, instances.front(), selected, apply) == AnimationPrefabStatus::Ok
				&& source.defaults.at(controllerPath) == AnimationPrefabValue{walk}
				&& instances.front().overrides.contains(controllerPath),
				"Apply should stage a source authoring transaction without exposing it before publication.");
			context.Expect(integration.PublishPrepared(apply, source, instances)
				== AnimationPrefabStatus::Ok
				&& source.defaults.at(controllerPath) == AnimationPrefabValue{run}
				&& !instances.front().overrides.contains(controllerPath),
				"Applying an override should update the prefab default and clear that connected instance override atomically.");

			instances.front().overrides.emplace(controllerPath, walk);
			++instances.front().generation;
			AnimationPrefabPreparedMutation revert;
			context.Expect(integration.PrepareRevertOverrides(
				source, instances.front(), selected, revert) == AnimationPrefabStatus::Ok
				&& revert.sceneTransaction.writeSet.operations.size() == 1U,
				"Revert should stage the effective source default as one typed deferred component write.");
			context.Expect(integration.PublishPrepared(revert, source, instances)
				== AnimationPrefabStatus::Ok
				&& !instances.front().overrides.contains(controllerPath)
				&& ResolveAnimationPrefabValue(source, instances.front(), controllerPath)
					== source.defaults.at(controllerPath),
				"Revert should remove only the selected override and reveal the current prefab source value.");

			instances.push_back(MakeInstance(MakeUuid(123U), sourceId, sourceRoot, MakeUuid(143U), 2U));
			AnimationPrefabPreparedMutation stale;
			context.Expect(integration.PrepareGroupedOverride(
				source, instances, controllerPath, walk, stale) == AnimationPrefabStatus::Ok,
				"A second grouped operation should prepare against both current generations.");
			const std::vector<AnimationPrefabInstance> beforeStalePublish = instances;
			++instances.back().generation;
			const AnimationPrefabInstance firstBeforeRejection = instances.front();
			context.Expect(integration.PublishPrepared(stale, source, instances)
				== AnimationPrefabStatus::Stale
				&& instances.front() == firstBeforeRejection
				&& instances.back().overrides == beforeStalePublish.back().overrides,
				"One stale grouped target must reject the complete publication without partially changing an earlier instance.");
		});

		registry.Add("Editor.Animation.PlayMode.ByteExactRestorationAndGenerationCleanup", [](TestContext& context) {
			AnimationPlayModeAuthoredState captured;
			captured.sceneId = MakeUuid(4U);
			captured.sceneGeneration = 91U;
			captured.sceneDocumentBytes = Bytes({0U, 1U, 2U, 0U, 255U});
			captured.animatorComponentBytes = Bytes({9U, 8U, 0U, 7U});
			captured.hierarchyBytes = Bytes({6U, 5U, 4U});
			captured.attachmentBytes = Bytes({3U, 0U, 2U});
			captured.animationAssetBytes = Bytes({1U, 0U, 255U, 0U, 1U});
			const AnimationPlayModeAuthoredState expected = captured;

			AnimationPrefabIntegration integration;
			AnimationPlayModeCaptureHandle handle;
			context.Expect(integration.CapturePlayMode(captured, handle) == AnimationPrefabStatus::Ok
				&& handle.IsValid()
				&& integration.ActivePlayModeCaptureCount() == 1U,
				"Entering Play Mode should copy every authored animation-related byte range into a generation-safe capture.");
			captured.sceneDocumentBytes.front() = static_cast<std::byte>(44U);
			captured.attachmentBytes.clear();

			AnimationPlayModePreparedRestore prepared;
			context.Expect(integration.PreparePlayModeRestore(
				handle, expected.sceneId, expected.sceneGeneration, prepared) == AnimationPrefabStatus::Ok
				&& prepared.declaration.workerSafePreparation
				&& prepared.declaration.protectedPublication,
				"Worker-safe exit preparation should copy the capture and declare the protected authoring restore boundary.");
			AnimationPlayModeAuthoredState runtimeMutated = expected;
			runtimeMutated.sceneDocumentBytes = Bytes({77U});
			runtimeMutated.animatorComponentBytes = Bytes({88U});
			runtimeMutated.hierarchyBytes = Bytes({99U});
			runtimeMutated.attachmentBytes = Bytes({111U});
			runtimeMutated.animationAssetBytes = Bytes({122U});
			context.Expect(integration.PublishPlayModeRestore(prepared, runtimeMutated)
				== AnimationPrefabStatus::Ok
				&& runtimeMutated == expected
				&& integration.ActivePlayModeCaptureCount() == 0U,
				"Play Mode exit must restore scene, component, hierarchy, attachment, and asset buffers byte-for-byte as one publication.");
			context.Expect(integration.PreparePlayModeRestore(
				handle, expected.sceneId, expected.sceneGeneration, prepared) == AnimationPrefabStatus::Stale,
				"A consumed capture handle must never alias a later Play Mode session.");

			AnimationPlayModeCaptureHandle mismatchedHandle;
			context.Expect(integration.CapturePlayMode(expected, mismatchedHandle) == AnimationPrefabStatus::Ok,
				"A later Play Mode entry should receive a fresh generation.");
			const AnimationPlayModeAuthoredState beforeMismatch = runtimeMutated;
			context.Expect(integration.PreparePlayModeRestore(
				mismatchedHandle, MakeUuid(24U), expected.sceneGeneration, prepared) == AnimationPrefabStatus::Stale
				&& runtimeMutated == beforeMismatch,
				"A scene switch must reject restoration without changing any authored byte range.");
			integration.ClearPlayModeCaptures();
			context.Expect(integration.ActivePlayModeCaptureCount() == 0U,
				"Scene teardown must release every retained Play Mode capture and invalidate its generation.");
		});
	}
}
