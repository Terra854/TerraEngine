// TransactionalMutation.cpp - validation and protected commit for scene STM.

#include "TransactionalMutation.h"

#include "Components/Transform.h"
#include "Components/AnimatorComponent.h"
#include "Core/Object/Component.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/Serializer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <sstream>
#include <unordered_set>

namespace TerraEngine::Scene {
	namespace {
		using Clock = std::chrono::steady_clock;

		std::uint64_t ElapsedNs(Clock::time_point start)
		{
			return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
		}

		std::optional<Object> ResolveRef(ObjectManager& objects, const TransactionObjectRef& ref, const std::unordered_map<std::int32_t, TerraEngine::UUID>& localObjects)
		{
			TerraEngine::UUID objectId = ref.objectId;
			if (ref.UsesLocalObject())
			{
				const auto local = localObjects.find(ref.localObjectId);
				if (local == localObjects.end())
				{
					return std::nullopt;
				}
				objectId = local->second;
			}
			if (objectId == TerraEngine::EMPTY_UUID)
			{
				return std::nullopt;
			}
			return objects.getObject(objectId);
		}

		void AddConflict(
			TransactionResult& result,
			TransactionConflictReason reason,
			TerraEngine::UUID target,
			std::size_t operationIndex,
			std::string message,
			std::uint64_t expected = 0,
			std::uint64_t actual = 0,
			std::string component = {},
			std::string property = {},
			TransactionWriteKind writeKind = TransactionWriteKind::CreateObject)
		{
			if (result.conflicts.size() >= 128)
			{
				return;
			}
			result.conflicts.push_back(TransactionConflictRecord{
				reason,
				target,
				std::move(component),
				std::move(property),
				writeKind,
				expected,
				actual,
				operationIndex,
				std::move(message)
			});
		}

		bool RequiresExistingTarget(TransactionWriteKind kind)
		{
			return kind != TransactionWriteKind::CreateObject;
		}

		bool IsComponentWrite(TransactionWriteKind kind)
		{
			return kind == TransactionWriteKind::AddComponent
				|| kind == TransactionWriteKind::RemoveComponent
				|| kind == TransactionWriteKind::UpdateComponentProperty;
		}

		bool IsSpatialWrite(TransactionWriteKind kind)
		{
			return kind == TransactionWriteKind::SetWorldTransform
				|| kind == TransactionWriteKind::TranslateWorld
				|| kind == TransactionWriteKind::ChangeSimulationFrame;
		}

		std::size_t LogicalWriteCount(const Transaction& transaction)
		{
			std::size_t count = 0U;
			for (const TransactionWriteOperation& operation : transaction.writeSet.operations)
			{
				// Each transform pair is one authored-object write even though it
				// stores position and rotation together for atomic publication.
				if (operation.kind == TransactionWriteKind::UpdateTransformBatch)
				{
					count += operation.transformUpdates.size();
				}
				else if (operation.kind == TransactionWriteKind::PublishAnimationMutations)
				{
					// Each copied animation record consumes independent validation and
					// receipt capacity, so its count must participate in the budget.
					count += operation.animationPublications.size();
				}
				else
				{
					++count;
				}
			}
			return count;
		}

		bool SameObjectRef(const TransactionObjectRef& left, const TransactionObjectRef& right)
		{
			return left.objectId == right.objectId && left.localObjectId == right.localObjectId;
		}

		bool HasObjectRef(const TransactionObjectRef& ref)
		{
			return ref.UsesLocalObject() || ref.objectId != TerraEngine::EMPTY_UUID;
		}

		std::string MakeObjectRefKey(const TransactionObjectRef& ref)
		{
			return ref.UsesLocalObject()
				? "local:" + std::to_string(ref.localObjectId)
				: "uuid:" + TerraEngine::ToHexString(ref.objectId);
		}

		bool CanCoalesceWrite(const TransactionWriteOperation& operation)
		{
			switch (operation.kind)
			{
			case TransactionWriteKind::SetName:
			case TransactionWriteKind::SetTag:
			case TransactionWriteKind::SetLayer:
			case TransactionWriteKind::SetActive:
			case TransactionWriteKind::SetStatic:
			case TransactionWriteKind::UpdateComponentProperty:
				return true;
			default:
				return false;
			}
		}

		bool ReplacesEarlierWrite(const TransactionWriteOperation& earlier, const TransactionWriteOperation& later)
		{
			if (!CanCoalesceWrite(earlier) || earlier.kind != later.kind || !SameObjectRef(earlier.target, later.target))
			{
				return false;
			}
			if (earlier.kind == TransactionWriteKind::UpdateComponentProperty)
			{
				return earlier.componentName == later.componentName && earlier.propertyName == later.propertyName;
			}
			return true;
		}

		std::string MakeCoalescingKey(const TransactionWriteOperation& operation)
		{
			std::ostringstream stream;
			stream << static_cast<int>(operation.kind)
				<< "|uuid=" << TerraEngine::ToHexString(operation.target.objectId)
				<< "|local=" << operation.target.localObjectId;
			if (operation.kind == TransactionWriteKind::UpdateComponentProperty)
			{
				stream << "|component=" << operation.componentName
					<< "|property=" << operation.propertyName;
			}
			return stream.str();
		}

		Transaction NormalizeTransaction(Transaction transaction)
		{
			// Keep structural operations in source order, but collapse repeated
			// same-target scalar/property writes so worker batches do not spend
			// commit time applying values that are immediately overwritten. The
			// reverse scan is O(n) on average, which matters for script/editor
			// batches that may submit thousands of property writes in one frame.
			std::vector<TransactionWriteOperation> reversed;
			reversed.reserve(transaction.writeSet.operations.size());
			std::unordered_set<std::string> seenCoalescedWrites;
			for (auto operation = transaction.writeSet.operations.rbegin(); operation != transaction.writeSet.operations.rend(); ++operation)
			{
				if (CanCoalesceWrite(*operation))
				{
					const std::string key = MakeCoalescingKey(*operation);
					if (!seenCoalescedWrites.insert(key).second)
					{
						continue;
					}
				}
				reversed.push_back(std::move(*operation));
			}
			std::reverse(reversed.begin(), reversed.end());
			std::vector<TransactionWriteOperation> normalized = std::move(reversed);
			transaction.writeSet.operations = std::move(normalized);
			return transaction;
		}

		bool ValidateComponentPayload(const TransactionWriteOperation& operation)
		{
			if (!IsComponentWrite(operation.kind))
			{
				return true;
			}
			if (operation.componentName.empty())
			{
				return false;
			}
			if (operation.kind == TransactionWriteKind::UpdateComponentProperty)
			{
				return !operation.propertyName.empty() && TerraEngine::Reflection::IsValid(operation.propertyValue);
			}
			return true;
		}

		TransactionResult ValidateTransaction(ObjectManager& objects, const Transaction& transaction)
		{
			TransactionResult result;
			result.transactionId = transaction.id;
			result.status = TransactionStatus::PreflightAccepted;

			if (transaction.canceled)
			{
				result.status = TransactionStatus::Canceled;
				AddConflict(result, TransactionConflictReason::Canceled, TerraEngine::EMPTY_UUID, 0, "Transaction was canceled before commit.");
				return result;
			}

			if (transaction.injectedFailureOperationIndex && transaction.origin != TransactionOrigin::Test)
			{
				result.status = TransactionStatus::Rejected;
				AddConflict(result, TransactionConflictReason::PolicyRejection, TerraEngine::EMPTY_UUID, *transaction.injectedFailureOperationIndex,
					"Commit fault injection is available only to test-origin transactions.");
				return result;
			}

			if (transaction.writeSet.operations.empty() && transaction.mode == TransactionMode::Commit)
			{
				result.status = TransactionStatus::Rejected;
				AddConflict(result, TransactionConflictReason::InvalidPayload, TerraEngine::EMPTY_UUID, 0, "Commit transaction contains no writes.");
				return result;
			}

			const std::size_t logicalWriteCount = LogicalWriteCount(transaction);
			if (logicalWriteCount > transaction.budget.maxWriteCount)
			{
				result.status = TransactionStatus::Rejected;
				result.rejectedWriteCount = logicalWriteCount;
				AddConflict(result, TransactionConflictReason::CommitBudgetRejection, TerraEngine::EMPTY_UUID, 0, "Transaction exceeds its configured write budget.", transaction.budget.maxWriteCount, logicalWriteCount);
				return result;
			}

			if (transaction.readSet.sceneGeneration != 0 && transaction.readSet.sceneGeneration != objects.GetSceneGeneration())
			{
				result.status = TransactionStatus::Conflicted;
				AddConflict(result, TransactionConflictReason::SceneUnloadedOrSwitched, TerraEngine::EMPTY_UUID, 0, "Scene lifetime changed after transaction preparation.", transaction.readSet.sceneGeneration, objects.GetSceneGeneration());
			}

			if (transaction.readSet.sceneRevision != 0 && transaction.readSet.sceneRevision != objects.GetHierarchyRevision())
			{
				result.status = TransactionStatus::Conflicted;
				AddConflict(result, TransactionConflictReason::HierarchyConflict, TerraEngine::EMPTY_UUID, 0, "Scene hierarchy revision changed after transaction preparation.", transaction.readSet.sceneRevision, objects.GetHierarchyRevision());
			}

			for (const TransactionObjectObservation& observation : transaction.readSet.objects)
			{
				const std::optional<Object> object = objects.getObject(observation.objectId);
				if (!object)
				{
					result.status = TransactionStatus::Conflicted;
					AddConflict(result, TransactionConflictReason::MissingTarget, observation.objectId, 0, "Observed object no longer exists.");
					continue;
				}
				const std::uint64_t lifetimeGeneration = objects.getObjectLifetimeGeneration(*object);
				if (observation.lifetimeGeneration != 0 && observation.lifetimeGeneration != lifetimeGeneration)
				{
					result.status = TransactionStatus::Conflicted;
					AddConflict(result, TransactionConflictReason::StaleObjectIdentity, observation.objectId, 0, "Observed object lifetime generation changed.", observation.lifetimeGeneration, lifetimeGeneration);
				}
				const std::uint64_t metadataGeneration = objects.getObjectMetadataGeneration(*object);
				if (observation.metadataGeneration != 0 && observation.metadataGeneration != metadataGeneration)
				{
					result.status = TransactionStatus::Conflicted;
					AddConflict(result, TransactionConflictReason::MetadataVersionMismatch, observation.objectId, 0, "Observed metadata revision changed.", observation.metadataGeneration, metadataGeneration);
				}
				const std::uint64_t componentGeneration = objects.getObjectComponentGeneration(*object);
				if (observation.componentGeneration != 0 && observation.componentGeneration != componentGeneration)
				{
					result.status = TransactionStatus::Conflicted;
					AddConflict(result, TransactionConflictReason::ComponentVersionMismatch, observation.objectId, 0, "Observed component revision changed.", observation.componentGeneration, componentGeneration);
				}
				if (observation.hierarchyRevision != 0 && observation.hierarchyRevision != objects.GetHierarchyRevision())
				{
					result.status = TransactionStatus::Conflicted;
					AddConflict(result, TransactionConflictReason::HierarchyConflict, observation.objectId, 0, "Observed hierarchy revision changed.", observation.hierarchyRevision, objects.GetHierarchyRevision());
				}
				const std::uint64_t transformRevision = objects.GetObjectTransformRevision(*object);
				if (observation.transformRevision != 0 && observation.transformRevision != transformRevision)
				{
					result.status = TransactionStatus::Conflicted;
					AddConflict(result, TransactionConflictReason::ComponentVersionMismatch, observation.objectId, 0, "Observed transform revision changed.", observation.transformRevision, transformRevision, std::string(Transform::kComponentName));
				}
				const std::optional<WorldPose> worldPose = objects.GetCanonicalWorldPose(*object);
				const std::uint64_t coordinateFrameRevision = worldPose ? worldPose->coordinateFrameRevision : 0U;
				if (observation.coordinateFrameRevision != 0U
					&& observation.coordinateFrameRevision != coordinateFrameRevision)
				{
					result.status = TransactionStatus::Conflicted;
					AddConflict(result, TransactionConflictReason::StaleSpatialPublication, observation.objectId, 0,
						"Observed canonical coordinate-frame revision changed.", observation.coordinateFrameRevision,
						coordinateFrameRevision, std::string(Transform::kComponentName));
				}

				const std::optional<Object> parent = objects.getParent(*object);
				const TerraEngine::UUID parentObjectId = parent ? objects.getUUID(*parent) : TerraEngine::EMPTY_UUID;
				if (observation.parentObjectId != parentObjectId)
				{
					result.status = TransactionStatus::Conflicted;
					AddConflict(result, TransactionConflictReason::HierarchyConflict, observation.objectId, 0, "Observed parent identity changed.");
				}
				if (parent)
				{
					const std::uint64_t parentLifetimeGeneration = objects.getObjectLifetimeGeneration(*parent);
					const std::uint64_t parentTransformRevision = objects.GetObjectTransformRevision(*parent);
					if (observation.parentLifetimeGeneration != 0 && observation.parentLifetimeGeneration != parentLifetimeGeneration)
					{
						result.status = TransactionStatus::Conflicted;
						AddConflict(result, TransactionConflictReason::StaleObjectIdentity, observation.objectId, 0, "Observed parent lifetime changed.", observation.parentLifetimeGeneration, parentLifetimeGeneration);
					}
					if (observation.parentTransformRevision != 0 && observation.parentTransformRevision != parentTransformRevision)
					{
						result.status = TransactionStatus::Conflicted;
						AddConflict(result, TransactionConflictReason::HierarchyConflict, observation.objectId, 0, "Observed parent transform revision changed.", observation.parentTransformRevision, parentTransformRevision);
					}
				}
			}

			std::unordered_set<std::int32_t> localCreates;
			std::unordered_set<std::string> structurallyTouchedTargets;
			std::unordered_set<std::string> deletedTargets;
			std::unordered_set<std::string> spatiallyTouchedTargets;
			std::unordered_map<std::string, bool> preparedComponentPresence;
			for (std::size_t index = 0; index < transaction.writeSet.operations.size(); ++index)
			{
				const TransactionWriteOperation& operation = transaction.writeSet.operations[index];
				if (operation.kind == TransactionWriteKind::CreateObject)
				{
					if (operation.createdLocalObjectId == 0 || !localCreates.insert(operation.createdLocalObjectId).second)
					{
						result.status = TransactionStatus::Rejected;
						AddConflict(result, TransactionConflictReason::InvalidPayload, TerraEngine::EMPTY_UUID, index, "Created objects need unique nonzero transaction-local ids.", 0, 0, {}, {}, operation.kind);
					}
					if (operation.parent.UsesLocalObject() && !localCreates.contains(operation.parent.localObjectId))
					{
						result.status = TransactionStatus::Conflicted;
						AddConflict(result, TransactionConflictReason::MissingTarget, TerraEngine::EMPTY_UUID, index, "Created-object parent must refer to an earlier local object.", 0, 0, {}, {}, operation.kind);
					}
					else if (!operation.parent.UsesLocalObject() && operation.parent.objectId != TerraEngine::EMPTY_UUID && !objects.getObject(operation.parent.objectId))
					{
						result.status = TransactionStatus::Conflicted;
						AddConflict(result, TransactionConflictReason::MissingTarget, operation.parent.objectId, index, "Created-object parent no longer exists.", 0, 0, {}, {}, operation.kind);
					}
					continue;
				}

				if (operation.kind == TransactionWriteKind::UpdateTransformBatch)
				{
					if (operation.transformUpdates.empty())
					{
						result.status = TransactionStatus::Rejected;
						AddConflict(result, TransactionConflictReason::InvalidPayload, TerraEngine::EMPTY_UUID, index, "Transform publication batch contains no updates.", 0, 0, {}, {}, operation.kind);
						continue;
					}
					std::unordered_set<TerraEngine::UUID> batchTargets;
					batchTargets.reserve(operation.transformUpdates.size());
					for (const TransactionTransformUpdate& update : operation.transformUpdates)
					{
						if (update.objectId == TerraEngine::EMPTY_UUID || !batchTargets.insert(update.objectId).second)
						{
							result.status = TransactionStatus::Rejected;
							AddConflict(result, TransactionConflictReason::InvalidPayload, update.objectId, index, "Transform publication batch has an empty or duplicate target.", 0, 0, std::string(Transform::kComponentName), {}, operation.kind);
							continue;
						}
						const std::optional<Object> target = objects.getObject(update.objectId);
						if (!target || !objects.GetComponent<Transform>(*target))
						{
							result.status = TransactionStatus::Conflicted;
							AddConflict(result, TransactionConflictReason::MissingTarget, update.objectId, index, "Transform publication target no longer has a Transform component.", 0, 0, std::string(Transform::kComponentName), {}, operation.kind);
						}
					}
					continue;
				}
				if (IsSpatialWrite(operation.kind))
				{
					if (operation.target.UsesLocalObject() || operation.target.objectId == TerraEngine::EMPTY_UUID)
					{
						result.status = TransactionStatus::Rejected;
						AddConflict(result, TransactionConflictReason::InvalidPayload, operation.target.objectId, index,
							"Canonical spatial writes require one existing stable object identity.", 0, 0, {}, {}, operation.kind);
						continue;
					}
					if (!spatiallyTouchedTargets.insert(MakeObjectRefKey(operation.target)).second)
					{
						result.status = TransactionStatus::Conflicted;
						AddConflict(result, TransactionConflictReason::PolicyRejection, operation.target.objectId, index,
							"A transaction cannot publish more than one canonical spatial write for an object.", 0, 0, {}, {}, operation.kind);
					}
					if (operation.kind == TransactionWriteKind::SetWorldTransform
						&& !NormalizeWorldPosition(operation.worldPose.position).Succeeded())
					{
						result.status = TransactionStatus::Rejected;
						AddConflict(result, TransactionConflictReason::InvalidCoordinate, operation.target.objectId, index,
							"SetWorldTransform contains a non-normalizable canonical position.", 0, 0, {}, {}, operation.kind);
					}
					if (operation.kind == TransactionWriteKind::TranslateWorld
						&& !NormalizeWorldDelta(operation.worldDelta).Succeeded())
					{
						result.status = TransactionStatus::Rejected;
						AddConflict(result, TransactionConflictReason::InvalidCoordinate, operation.target.objectId, index,
							"TranslateWorld contains a non-normalizable canonical delta.", 0, 0, {}, {}, operation.kind);
					}
					if (operation.kind == TransactionWriteKind::ChangeSimulationFrame
						&& (operation.sourceSimulationFrameGeneration == 0U
							|| operation.destinationSimulationFrameGeneration == 0U
							|| operation.sourceSimulationFrameGeneration == operation.destinationSimulationFrameGeneration))
					{
						result.status = TransactionStatus::Rejected;
						AddConflict(result, TransactionConflictReason::SimulationFrameConflict, operation.target.objectId, index,
							"Simulation-frame changes require distinct nonzero source and destination generations.",
							operation.sourceSimulationFrameGeneration, operation.destinationSimulationFrameGeneration, {}, {}, operation.kind);
					}
					continue;
				}

				if (operation.kind == TransactionWriteKind::PublishAnimationMutations)
				{
					if (operation.animationPublications.empty())
					{
						result.status = TransactionStatus::Rejected;
						AddConflict(result, TransactionConflictReason::InvalidPayload, TerraEngine::EMPTY_UUID, index, "Animation publication batch contains no typed records.", 0, 0, {}, {}, operation.kind);
						continue;
					}
					std::unordered_set<std::string> publicationTargets;
					for (const AnimationMutationPublication& publication : operation.animationPublications)
					{
						const std::string key = TerraEngine::ToHexString(publication.targetObjectId) + "|" + std::to_string(publication.animatorSlot);
						if (publication.targetObjectId == TerraEngine::EMPTY_UUID || publication.expectedComponentGeneration == 0U
							|| publication.animatorSlot == 0xffffffffU || publication.animatorGeneration == 0U
							|| publication.producerSequence == 0U || publication.commandCount == 0U
							|| !publicationTargets.insert(key).second)
						{
							result.status = TransactionStatus::Rejected;
							AddConflict(result, TransactionConflictReason::InvalidPayload, publication.targetObjectId, index, "Animation publication has an empty, stale-shaped, or duplicate typed record.", 0, 0, {}, {}, operation.kind);
							continue;
						}
						const std::optional<Object> target = objects.getObject(publication.targetObjectId);
						if (!target || objects.getObjectComponentGeneration(*target) != publication.expectedComponentGeneration)
						{
							result.status = TransactionStatus::Conflicted;
							AddConflict(result, TransactionConflictReason::ComponentVersionMismatch, publication.targetObjectId, index, "Animator component generation changed before typed publication.", publication.expectedComponentGeneration, target ? objects.getObjectComponentGeneration(*target) : 0U, {}, {}, operation.kind);
						}
						else if (objects.GetComponent<AnimatorComponent>(*target) == nullptr)
						{
							result.status = TransactionStatus::Conflicted;
							AddConflict(result, TransactionConflictReason::MissingTarget, publication.targetObjectId, index, "Animation publication target no longer has an AnimatorComponent.", 0U, 0U, std::string(AnimatorComponent::kComponentName), {}, operation.kind);
						}
					}
					continue;
				}

				if (operation.target.UsesLocalObject() && !localCreates.contains(operation.target.localObjectId))
				{
					result.status = TransactionStatus::Conflicted;
					AddConflict(result, TransactionConflictReason::MissingTarget, TerraEngine::EMPTY_UUID, index, "Write target refers to an undeclared or later local object.", 0, 0, operation.componentName, operation.propertyName, operation.kind);
					continue;
				}
				if (RequiresExistingTarget(operation.kind) && !operation.target.UsesLocalObject() && objects.getObject(operation.target.objectId) == std::nullopt)
				{
					result.status = TransactionStatus::Conflicted;
					AddConflict(result, TransactionConflictReason::MissingTarget, operation.target.objectId, index, "Write target no longer exists.", 0, 0, operation.componentName, operation.propertyName, operation.kind);
				}

				const std::string targetKey = MakeObjectRefKey(operation.target);
				if (deletedTargets.contains(targetKey))
				{
					result.status = TransactionStatus::Conflicted;
					AddConflict(result, TransactionConflictReason::PolicyRejection, operation.target.objectId, index, "A transaction cannot write an object after deleting it.", 0, 0, operation.componentName, operation.propertyName, operation.kind);
				}
				if (operation.kind == TransactionWriteKind::DeleteObject)
				{
					if (structurallyTouchedTargets.contains(targetKey) || !deletedTargets.insert(targetKey).second)
					{
						result.status = TransactionStatus::Conflicted;
						AddConflict(result, TransactionConflictReason::PolicyRejection, operation.target.objectId, index, "Object deletion conflicts with another structural write in the same transaction.", 0, 0, {}, {}, operation.kind);
					}
					structurallyTouchedTargets.insert(targetKey);
				}
				else if (operation.kind == TransactionWriteKind::ReparentObject)
				{
					const std::string structuralKey = targetKey + "|parent";
					if (!structurallyTouchedTargets.insert(structuralKey).second)
					{
						result.status = TransactionStatus::Conflicted;
						AddConflict(result, TransactionConflictReason::HierarchyConflict, operation.target.objectId, index, "Object is reparented more than once in one transaction.", 0, 0, {}, {}, operation.kind);
					}
					if (operation.parent.UsesLocalObject() && !localCreates.contains(operation.parent.localObjectId))
					{
						result.status = TransactionStatus::Conflicted;
						AddConflict(result, TransactionConflictReason::MissingTarget, TerraEngine::EMPTY_UUID, index, "Parent refers to an undeclared local object.", 0, 0, {}, {}, operation.kind);
					}
					else if (!operation.parent.UsesLocalObject() && operation.parent.objectId != TerraEngine::EMPTY_UUID && !objects.getObject(operation.parent.objectId))
					{
						result.status = TransactionStatus::Conflicted;
						AddConflict(result, TransactionConflictReason::MissingTarget, operation.parent.objectId, index, "Parent target no longer exists.", 0, 0, {}, {}, operation.kind);
					}
				}
				if (!ValidateComponentPayload(operation))
				{
					result.status = TransactionStatus::Rejected;
					AddConflict(result, TransactionConflictReason::InvalidPayload, operation.target.objectId, index, "Component write payload is incomplete.", 0, 0, operation.componentName, operation.propertyName, operation.kind);
				}
				if (IsComponentWrite(operation.kind))
				{
					std::lock_guard registryLock(ComponentAccessRegistryMutex());
					auto component = ComponentAccessRegistry().find(operation.componentName);
					if (component == ComponentAccessRegistry().end())
					{
						result.status = TransactionStatus::Rejected;
						AddConflict(result, TransactionConflictReason::InvalidPayload, operation.target.objectId, index, "Component type is not registered.", 0, 0, operation.componentName, operation.propertyName, operation.kind);
						continue;
					}

					const std::string componentKey = targetKey + "|component=" + operation.componentName;
					bool componentPresent = false;
					if (const auto prepared = preparedComponentPresence.find(componentKey); prepared != preparedComponentPresence.end())
					{
						componentPresent = prepared->second;
					}
					else if (!operation.target.UsesLocalObject())
					{
						if (const std::optional<Object> target = objects.getObject(operation.target.objectId))
						{
							componentPresent = component->second.entityHasComponent(objects.getRegistry(), target->EntityId());
						}
					}
					else
					{
						// ObjectManager creates Transform with every object. Other component
						// types are absent until an earlier add operation publishes them.
						componentPresent = operation.componentName == Transform::kComponentName;
					}

					if (operation.kind == TransactionWriteKind::AddComponent)
					{
						if (componentPresent)
						{
							result.status = TransactionStatus::Conflicted;
							AddConflict(result, TransactionConflictReason::PolicyRejection, operation.target.objectId, index, "Component is already present.", 0, 0, operation.componentName, {}, operation.kind);
						}
						preparedComponentPresence[componentKey] = true;
					}
					else if (operation.kind == TransactionWriteKind::RemoveComponent)
					{
						if (!componentPresent)
						{
							result.status = TransactionStatus::Conflicted;
							AddConflict(result, TransactionConflictReason::MissingTarget, operation.target.objectId, index, "Component is not present.", 0, 0, operation.componentName, {}, operation.kind);
						}
						preparedComponentPresence[componentKey] = false;
					}
					else
					{
						const Reflection::Property property = Reflection::FindProperty(component->second.reflectionType, operation.propertyName);
						Reflection::Variant converted = Reflection::IsValid(property)
							? operation.propertyValue.allow_cast(Reflection::GetPropertyType(property))
							: Reflection::Variant{};
						if (!componentPresent || !Reflection::IsValid(property) || !Reflection::IsValid(converted))
						{
							result.status = TransactionStatus::Rejected;
							AddConflict(result, TransactionConflictReason::InvalidPayload, operation.target.objectId, index, "Reflected component property or value type is invalid.", 0, 0, operation.componentName, operation.propertyName, operation.kind);
						}
					}
				}
			}

			if (!result.conflicts.empty() && result.status == TransactionStatus::PreflightAccepted)
			{
				result.status = TransactionStatus::Conflicted;
			}
			result.rejectedWriteCount = result.conflicts.empty() ? 0 : LogicalWriteCount(transaction);
			return result;
		}
	}

	TransactionalMutationBuilder::TransactionalMutationBuilder(std::string id, TransactionOrigin origin, std::string originName)
	{
		transaction.id = std::move(id);
		transaction.origin = origin;
		transaction.originName = std::move(originName);
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::SetMode(TransactionMode mode)
	{
		transaction.mode = mode;
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::SetUndoGroup(std::string undoGroupName)
	{
		transaction.undoGroupName = std::move(undoGroupName);
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::SetBudget(TransactionBudget budget)
	{
		transaction.budget = budget;
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::ObserveScene(ObjectManager& objects)
	{
		transaction.readSet.sceneGeneration = objects.GetSceneGeneration();
		transaction.readSet.sceneRevision = objects.GetHierarchyRevision();
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::ObserveObject(ObjectManager& objects, TerraEngine::UUID objectId)
	{
		if (const std::optional<Object> object = objects.getObject(objectId))
		{
			const std::optional<Object> parent = objects.getParent(*object);
			const std::optional<WorldPose> canonicalPose = objects.GetCanonicalWorldPose(*object);
			transaction.readSet.objects.push_back(TransactionObjectObservation{
				.objectId = objectId,
				.lifetimeGeneration = objects.getObjectLifetimeGeneration(*object),
				.metadataGeneration = objects.getObjectMetadataGeneration(*object),
				.componentGeneration = objects.getObjectComponentGeneration(*object),
				.hierarchyRevision = objects.GetHierarchyRevision(),
				.transformRevision = objects.GetObjectTransformRevision(*object),
				.coordinateFrameRevision = canonicalPose ? canonicalPose->coordinateFrameRevision : 0U,
				.parentObjectId = parent ? objects.getUUID(*parent) : TerraEngine::EMPTY_UUID,
				.parentLifetimeGeneration = parent ? objects.getObjectLifetimeGeneration(*parent) : 0,
				.parentTransformRevision = parent ? objects.GetObjectTransformRevision(*parent) : 0
			});
		}
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::CreateObject(std::int32_t localObjectId, std::string displayName, TransactionObjectRef parent)
	{
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::CreateObject;
		operation.createdLocalObjectId = localObjectId;
		operation.displayName = std::move(displayName);
		operation.parent = parent;
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::DeleteObject(TerraEngine::UUID objectId)
	{
		return DeleteObject(ExistingObject(objectId));
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::DeleteObject(TransactionObjectRef object)
	{
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::DeleteObject;
		operation.target = object;
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::ReparentObject(TerraEngine::UUID objectId, TransactionObjectRef parent, bool keepWorld)
	{
		return ReparentObject(ExistingObject(objectId), parent, keepWorld);
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::ReparentObject(TransactionObjectRef object, TransactionObjectRef parent, bool keepWorld)
	{
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::ReparentObject;
		operation.target = object;
		operation.parent = parent;
		operation.keepWorldOnReparent = keepWorld;
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::SetName(TerraEngine::UUID objectId, std::string name)
	{
		return SetName(ExistingObject(objectId), std::move(name));
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::SetName(TransactionObjectRef object, std::string name)
	{
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::SetName;
		operation.target = object;
		operation.stringValue = std::move(name);
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::SetTag(TerraEngine::UUID objectId, std::string tag)
	{
		return SetTag(ExistingObject(objectId), std::move(tag));
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::SetTag(TransactionObjectRef object, std::string tag)
	{
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::SetTag;
		operation.target = object;
		operation.stringValue = std::move(tag);
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::SetLayer(TerraEngine::UUID objectId, std::string layer)
	{
		return SetLayer(ExistingObject(objectId), std::move(layer));
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::SetLayer(TransactionObjectRef object, std::string layer)
	{
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::SetLayer;
		operation.target = object;
		operation.stringValue = std::move(layer);
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::SetActive(TerraEngine::UUID objectId, bool active)
	{
		return SetActive(ExistingObject(objectId), active);
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::SetActive(TransactionObjectRef object, bool active)
	{
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::SetActive;
		operation.target = object;
		operation.boolValue = active;
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::SetStatic(TerraEngine::UUID objectId, bool isStatic)
	{
		return SetStatic(ExistingObject(objectId), isStatic);
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::SetStatic(TransactionObjectRef object, bool isStatic)
	{
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::SetStatic;
		operation.target = object;
		operation.boolValue = isStatic;
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::AddComponent(TerraEngine::UUID objectId, std::string componentName)
	{
		return AddComponent(ExistingObject(objectId), std::move(componentName));
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::AddComponent(TransactionObjectRef object, std::string componentName)
	{
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::AddComponent;
		operation.target = object;
		operation.componentName = std::move(componentName);
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::RemoveComponent(TerraEngine::UUID objectId, std::string componentName)
	{
		return RemoveComponent(ExistingObject(objectId), std::move(componentName));
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::RemoveComponent(TransactionObjectRef object, std::string componentName)
	{
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::RemoveComponent;
		operation.target = object;
		operation.componentName = std::move(componentName);
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::UpdateComponentProperty(TerraEngine::UUID objectId, std::string componentName, std::string propertyName, TerraEngine::Reflection::Variant value)
	{
		return UpdateComponentProperty(ExistingObject(objectId), std::move(componentName), std::move(propertyName), std::move(value));
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::UpdateComponentProperty(TransactionObjectRef object, std::string componentName, std::string propertyName, TerraEngine::Reflection::Variant value)
	{
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::UpdateComponentProperty;
		operation.target = object;
		operation.componentName = std::move(componentName);
		operation.propertyName = std::move(propertyName);
		operation.propertyValue = std::move(value);
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::SetWorldTransform(ObjectManager& objects, TerraEngine::UUID objectId, WorldPose pose)
	{
		// Capture the source revision before a worker releases its immutable input.
		// The protected boundary will reject the complete batch if this pose is stale.
		ObserveScene(objects);
		ObserveObject(objects, objectId);
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::SetWorldTransform;
		operation.target = ExistingObject(objectId);
		operation.worldPose = std::move(pose);
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::TranslateWorld(ObjectManager& objects, TerraEngine::UUID objectId, WorldDelta delta)
	{
		ObserveScene(objects);
		ObserveObject(objects, objectId);
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::TranslateWorld;
		operation.target = ExistingObject(objectId);
		operation.worldDelta = std::move(delta);
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::ChangeSimulationFrame(
		ObjectManager& objects,
		TerraEngine::UUID objectId,
		std::uint64_t sourceGeneration,
		std::uint64_t destinationGeneration)
	{
		ObserveScene(objects);
		ObserveObject(objects, objectId);
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::ChangeSimulationFrame;
		operation.target = ExistingObject(objectId);
		operation.sourceSimulationFrameGeneration = sourceGeneration;
		operation.destinationSimulationFrameGeneration = destinationGeneration;
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::InjectCommitFailureForTesting(std::size_t operationIndex)
	{
		transaction.injectedFailureOperationIndex = operationIndex;
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::UpdateTransformBatch(ObjectManager& objects, std::vector<TransactionTransformUpdate> updates)
	{
		// Reserve once before capturing observations. Physics can publish thousands
		// of bodies, and repeated read-set growth would otherwise defeat batching.
		transaction.readSet.objects.reserve(transaction.readSet.objects.size() + updates.size());
		for (const TransactionTransformUpdate& update : updates)
		{
			ObserveObject(objects, update.objectId);
		}
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::UpdateTransformBatch;
		operation.transformUpdates = std::move(updates);
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::PublishAnimationMutations(ObjectManager& objects, std::vector<AnimationMutationPublication> publications)
	{
		// Capture every target generation while the producer owns ObjectManager. A
		// later protected flush rejects the complete batch if any target went stale.
		transaction.readSet.objects.reserve(transaction.readSet.objects.size() + publications.size());
		for (const AnimationMutationPublication& publication : publications)
		{
			ObserveObject(objects, publication.targetObjectId);
		}
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::PublishAnimationMutations;
		operation.animationPublications = std::move(publications);
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	Transaction TransactionalMutationBuilder::Build() &
	{
		return NormalizeTransaction(transaction);
	}

	Transaction TransactionalMutationBuilder::Build() &&
	{
		return NormalizeTransaction(std::move(transaction));
	}

	std::string_view ToString(TransactionStatus status)
	{
		switch (status)
		{
		case TransactionStatus::Pending: return "pending";
		case TransactionStatus::Committed: return "committed";
		case TransactionStatus::Rejected: return "rejected";
		case TransactionStatus::Conflicted: return "conflicted";
		case TransactionStatus::Canceled: return "canceled";
		case TransactionStatus::Expired: return "expired";
		case TransactionStatus::PreflightAccepted: return "preflight-accepted";
		case TransactionStatus::PreflightRejected: return "preflight-rejected";
		}
		return "unknown";
	}

	std::string_view ToString(TransactionConflictReason reason)
	{
		switch (reason)
		{
		case TransactionConflictReason::None: return "none";
		case TransactionConflictReason::StaleObjectIdentity: return "stale-object-identity";
		case TransactionConflictReason::MissingTarget: return "missing-target";
		case TransactionConflictReason::ComponentVersionMismatch: return "component-version-mismatch";
		case TransactionConflictReason::HierarchyConflict: return "hierarchy-conflict";
		case TransactionConflictReason::MetadataVersionMismatch: return "metadata-version-mismatch";
		case TransactionConflictReason::PolicyRejection: return "policy-rejection";
		case TransactionConflictReason::InvalidPayload: return "invalid-payload";
		case TransactionConflictReason::CommitBudgetRejection: return "commit-budget-rejection";
		case TransactionConflictReason::SceneUnloadedOrSwitched: return "scene-unloaded-or-switched";
		case TransactionConflictReason::InvalidCoordinate: return "invalid-coordinate";
		case TransactionConflictReason::CoordinateOverflow: return "coordinate-overflow";
		case TransactionConflictReason::StaleSpatialPublication: return "stale-spatial-publication";
		case TransactionConflictReason::SimulationFrameConflict: return "simulation-frame-conflict";
		case TransactionConflictReason::InjectedCommitFailure: return "injected-commit-failure";
		case TransactionConflictReason::Canceled: return "canceled";
		case TransactionConflictReason::Expired: return "expired";
		}
		return "unknown";
	}

	TransactionObjectRef ExistingObject(TerraEngine::UUID objectId)
	{
		TransactionObjectRef ref;
		ref.objectId = objectId;
		return ref;
	}

	TransactionObjectRef LocalObject(std::int32_t localObjectId)
	{
		TransactionObjectRef ref;
		ref.localObjectId = localObjectId;
		return ref;
	}

	TransactionResult PreflightTransaction(ObjectManager& objects, const Transaction& transaction)
	{
		const Clock::time_point start = Clock::now();
		TransactionResult result = ValidateTransaction(objects, transaction);
		if (result.status == TransactionStatus::PreflightAccepted && transaction.mode == TransactionMode::PreflightOnly)
		{
			result.message = "Transaction preflight accepted.";
		}
		else if (transaction.mode == TransactionMode::PreflightOnly && result.status != TransactionStatus::PreflightAccepted)
		{
			result.status = TransactionStatus::PreflightRejected;
			result.message = "Transaction preflight rejected.";
		}
		result.validationCostNs = ElapsedNs(start);
		return result;
	}

	TransactionResult CommitTransaction(ObjectManager& objects, const Transaction& transaction)
	{
		const Clock::time_point validateStart = Clock::now();
		TransactionResult result = ValidateTransaction(objects, transaction);
		result.validationCostNs = ElapsedNs(validateStart);
		if (transaction.mode == TransactionMode::PreflightOnly)
		{
			result.status = result.conflicts.empty() ? TransactionStatus::PreflightAccepted : TransactionStatus::PreflightRejected;
			result.message = result.conflicts.empty() ? "Transaction preflight accepted." : "Transaction preflight rejected.";
			return result;
		}
		if (!result.conflicts.empty() || result.status == TransactionStatus::Canceled || result.status == TransactionStatus::Rejected || result.status == TransactionStatus::Conflicted)
		{
			result.message = "Transaction rejected before commit.";
			return result;
		}

		const Clock::time_point commitStart = Clock::now();
		std::unordered_map<std::int32_t, TerraEngine::UUID> localObjects;
		for (std::size_t index = 0; index < transaction.writeSet.operations.size(); ++index)
		{
			const TransactionWriteOperation& operation = transaction.writeSet.operations[index];
			TransactionInverseDelta inverse;
			inverse.sourceOperationIndex = index;
			if (transaction.injectedFailureOperationIndex && *transaction.injectedFailureOperationIndex == index)
			{
				// This explicit test-only seam runs on the protected boundary after
				// earlier writes. It proves inverse capture restores a partial commit.
				AddConflict(result, TransactionConflictReason::InjectedCommitFailure, TerraEngine::EMPTY_UUID, index,
					"Test fault injection stopped protected transaction publication.", 0, 0, {}, {}, operation.kind);
				break;
			}

			if (operation.kind == TransactionWriteKind::CreateObject)
			{
				Object created = objects.createObject(operation.displayName.empty() ? "Object" : operation.displayName);
				const TerraEngine::UUID createdId = objects.getUUID(created);
				localObjects[operation.createdLocalObjectId] = createdId;
				result.createdObjects[operation.createdLocalObjectId] = createdId;
				inverse.operation.kind = TransactionWriteKind::DeleteObject;
				inverse.operation.target = ExistingObject(createdId);
				result.inverseDeltas.push_back(std::move(inverse));

				if (HasObjectRef(operation.parent))
				{
					const std::optional<Object> parent = ResolveRef(objects, operation.parent, localObjects);
					if (!parent || !objects.setParent(created, parent))
					{
						AddConflict(result, TransactionConflictReason::HierarchyConflict, createdId, index, "Created object could not be attached to its prepared parent.", 0, 0, {}, {}, operation.kind);
						break;
					}
				}
				continue;
			}

			if (operation.kind == TransactionWriteKind::UpdateTransformBatch)
			{
				// Resolve every target before the first write. This retains all-or-nothing
				// semantics even though the fast path deliberately bypasses reflection.
				std::vector<std::pair<Object, Transform*>> targets;
				targets.reserve(operation.transformUpdates.size());
				for (const TransactionTransformUpdate& update : operation.transformUpdates)
				{
					const std::optional<Object> target = objects.getObject(update.objectId);
					Transform* transform = target ? objects.GetComponent<Transform>(*target) : nullptr;
					if (!target || !transform)
					{
						AddConflict(result, TransactionConflictReason::MissingTarget, update.objectId, index, "Transform publication target disappeared before publication.", 0, 0, std::string(Transform::kComponentName), {}, operation.kind);
						break;
					}
					targets.emplace_back(*target, transform);
				}
				if (!result.conflicts.empty()) break;

				inverse.operation.kind = TransactionWriteKind::UpdateTransformBatch;
				inverse.operation.transformUpdates.reserve(operation.transformUpdates.size());
				for (std::size_t updateIndex = 0U; updateIndex < operation.transformUpdates.size(); ++updateIndex)
				{
					const TransactionTransformUpdate& update = operation.transformUpdates[updateIndex];
					inverse.operation.transformUpdates.push_back({update.objectId, *objects.GetCanonicalWorldPose(targets[updateIndex].first)});
				}
				for (std::size_t updateIndex = 0U; updateIndex < operation.transformUpdates.size(); ++updateIndex)
				{
					const TransactionTransformUpdate& update = operation.transformUpdates[updateIndex];
					if (objects.SetCanonicalWorldPose(targets[updateIndex].first, update.pose) != WorldCoordinateError::None)
					{
						AddConflict(result, TransactionConflictReason::InvalidCoordinate, update.objectId, index,
							"Canonical transform batch entry was rejected during protected publication.", 0, 0,
							std::string(Transform::kComponentName), {}, operation.kind);
						break;
					}
				}
				result.inverseDeltas.push_back(std::move(inverse));
				if (!result.conflicts.empty()) break;
				continue;
			}

			if (operation.kind == TransactionWriteKind::PublishAnimationMutations)
			{
				// Preflight already validated every copied record. This receipt becomes
				// observable only when the enclosing protected transaction commits.
				result.acceptedAnimationPublications.insert(result.acceptedAnimationPublications.end(),
					operation.animationPublications.begin(), operation.animationPublications.end());
				continue;
			}

			const std::optional<Object> target = ResolveRef(objects, operation.target, localObjects);
			if (!target)
			{
				AddConflict(result, TransactionConflictReason::MissingTarget, operation.target.objectId, index, "Prepared write target disappeared before publication.", 0, 0, operation.componentName, operation.propertyName, operation.kind);
				break;
			}
			const TerraEngine::UUID targetId = objects.getUUID(*target);
			inverse.operation.target = ExistingObject(targetId);

			switch (operation.kind)
			{
			case TransactionWriteKind::SetWorldTransform:
			{
				inverse.operation.kind = TransactionWriteKind::SetWorldTransform;
				inverse.operation.worldPose = *objects.GetCanonicalWorldPose(*target);
				if (objects.SetCanonicalWorldPose(*target, operation.worldPose) != WorldCoordinateError::None)
				{
					AddConflict(result, TransactionConflictReason::InvalidCoordinate, targetId, index,
						"Canonical world pose was rejected during protected publication.", 0, 0,
						std::string(Transform::kComponentName), {}, operation.kind);
					break;
				}
				result.inverseDeltas.push_back(std::move(inverse));
				break;
			}
			case TransactionWriteKind::TranslateWorld:
			{
				const WorldPose previousPose = *objects.GetCanonicalWorldPose(*target);
				const auto translatedPosition = ApplyWorldDelta(previousPose.position, operation.worldDelta);
				if (!translatedPosition.Succeeded())
				{
					AddConflict(result, TransactionConflictReason::CoordinateOverflow, targetId, index,
						"Canonical translation overflowed or used an invalid world delta.", 0, 0,
						std::string(Transform::kComponentName), {}, operation.kind);
					break;
				}
				WorldPose translatedPose = previousPose;
				translatedPose.position = translatedPosition.position;
				inverse.operation.kind = TransactionWriteKind::SetWorldTransform;
				inverse.operation.worldPose = previousPose;
				if (objects.SetCanonicalWorldPose(*target, translatedPose) != WorldCoordinateError::None)
				{
					AddConflict(result, TransactionConflictReason::InvalidCoordinate, targetId, index,
						"Translated canonical pose was rejected during protected publication.", 0, 0,
						std::string(Transform::kComponentName), {}, operation.kind);
					break;
				}
				result.inverseDeltas.push_back(std::move(inverse));
				break;
			}
			case TransactionWriteKind::ChangeSimulationFrame:
				// Physics owns provider staging. This typed scene receipt records the
				// validated boundary decision; later physics work consumes its identity.
				inverse.operation.kind = TransactionWriteKind::ChangeSimulationFrame;
				inverse.operation.sourceSimulationFrameGeneration = operation.destinationSimulationFrameGeneration;
				inverse.operation.destinationSimulationFrameGeneration = operation.sourceSimulationFrameGeneration;
				result.inverseDeltas.push_back(std::move(inverse));
				break;
			case TransactionWriteKind::DeleteObject:
			{
				inverse.operation.kind = TransactionWriteKind::CreateObject;
				inverse.serializedObject = Serializer::SerializeObject(target->EntityId());
				inverse.restoresSerializedObject = true;
				if (const std::optional<Object> parent = objects.getParent(*target))
				{
					inverse.operation.parent = ExistingObject(objects.getUUID(*parent));
				}
				objects.removeObject(*target);
				result.inverseDeltas.push_back(std::move(inverse));
				break;
			}
			case TransactionWriteKind::ReparentObject:
			{
				inverse.operation.kind = TransactionWriteKind::ReparentObject;
				const std::optional<WorldPose> previousPose = objects.GetCanonicalWorldPose(*target);
				if (const std::optional<Object> previousParent = objects.getParent(*target))
				{
					inverse.operation.parent = ExistingObject(objects.getUUID(*previousParent));
				}
				const std::optional<Object> nextParent = HasObjectRef(operation.parent)
					? ResolveRef(objects, operation.parent, localObjects)
					: std::optional<Object>{};
				if (HasObjectRef(operation.parent) && !nextParent)
				{
					AddConflict(result, TransactionConflictReason::MissingTarget, targetId, index, "Prepared parent disappeared before publication.", 0, 0, {}, {}, operation.kind);
					break;
				}
				if (!objects.setParent(*target, nextParent, operation.keepWorldOnReparent ? ReparentMode::KeepWorld : ReparentMode::KeepLocal))
				{
					AddConflict(result, TransactionConflictReason::HierarchyConflict, targetId, index, "Hierarchy rejected the prepared parent change.", 0, 0, {}, {}, operation.kind);
					break;
				}
				result.inverseDeltas.push_back(std::move(inverse));
				if (!operation.keepWorldOnReparent && previousPose)
				{
					// Rollback runs inverse deltas in reverse. Restore the previous
					// canonical pose first, then KeepWorld-reparent it to the old parent
					// so a rejected KeepLocal change is fully atomic.
					TransactionInverseDelta poseInverse;
					poseInverse.sourceOperationIndex = index;
					poseInverse.operation.kind = TransactionWriteKind::SetWorldTransform;
					poseInverse.operation.target = operation.target;
					poseInverse.operation.worldPose = *previousPose;
					result.inverseDeltas.push_back(std::move(poseInverse));
				}
				break;
			}
			case TransactionWriteKind::SetName:
				inverse.operation.kind = operation.kind;
				inverse.operation.stringValue = objects.getObjectName(*target);
				objects.setObjectName(*target, operation.stringValue);
				result.inverseDeltas.push_back(std::move(inverse));
				break;
			case TransactionWriteKind::SetTag:
				inverse.operation.kind = operation.kind;
				inverse.operation.stringValue = objects.getObjectTag(*target);
				objects.setObjectTag(*target, operation.stringValue);
				result.inverseDeltas.push_back(std::move(inverse));
				break;
			case TransactionWriteKind::SetLayer:
				inverse.operation.kind = operation.kind;
				inverse.operation.stringValue = objects.getObjectLayer(*target);
				objects.setObjectLayer(*target, operation.stringValue);
				result.inverseDeltas.push_back(std::move(inverse));
				break;
			case TransactionWriteKind::SetActive:
				inverse.operation.kind = operation.kind;
				inverse.operation.boolValue = objects.isObjectActive(*target);
				objects.setObjectActive(*target, operation.boolValue);
				result.inverseDeltas.push_back(std::move(inverse));
				break;
			case TransactionWriteKind::SetStatic:
				inverse.operation.kind = operation.kind;
				inverse.operation.boolValue = objects.isObjectStatic(*target);
				objects.setObjectStatic(*target, operation.boolValue);
				result.inverseDeltas.push_back(std::move(inverse));
				break;
			case TransactionWriteKind::AddComponent:
			case TransactionWriteKind::RemoveComponent:
			case TransactionWriteKind::UpdateComponentProperty:
			{
				std::lock_guard registryLock(ComponentAccessRegistryMutex());
				auto component = ComponentAccessRegistry().find(operation.componentName);
				if (component == ComponentAccessRegistry().end())
				{
					AddConflict(result, TransactionConflictReason::InvalidPayload, targetId, index, "Component type disappeared before publication.", 0, 0, operation.componentName, operation.propertyName, operation.kind);
					break;
				}
				entt::registry& enttRegistry = objects.getRegistry();
				const entt::entity targetEntity = target->EntityId();
				inverse.operation.componentName = operation.componentName;
				if (operation.kind == TransactionWriteKind::AddComponent)
				{
					inverse.operation.kind = TransactionWriteKind::RemoveComponent;
					component->second.entityAddComponent(enttRegistry, targetEntity);
					objects.notifyObjectStructureChanged(*target);
				}
				else if (operation.kind == TransactionWriteKind::RemoveComponent)
				{
					inverse.operation.kind = TransactionWriteKind::AddComponent;
					inverse.serializedComponent = component->second.entitySerialize(enttRegistry, targetEntity);
					inverse.restoresSerializedComponent = true;
					component->second.entityRemoveComponent(enttRegistry, targetEntity);
					objects.notifyObjectStructureChanged(*target);
				}
				else
				{
					inverse.operation.kind = TransactionWriteKind::UpdateComponentProperty;
					inverse.operation.propertyName = operation.propertyName;
					inverse.operation.propertyValue = component->second.entityGetter(enttRegistry, targetEntity, operation.propertyName);
					if (!Reflection::IsValid(inverse.operation.propertyValue)
						|| !component->second.entitySetter(enttRegistry, targetEntity, operation.propertyName, operation.propertyValue))
					{
						AddConflict(result, TransactionConflictReason::InvalidPayload, targetId, index, "Reflected component property write failed during publication.", 0, 0, operation.componentName, operation.propertyName, operation.kind);
						break;
					}
					if (operation.componentName != Transform::kComponentName) {
						// The reflected Transform setter already publishes one
						// spatial revision. Avoid double-incrementing it while
						// retaining structural invalidation for other components.
						objects.notifyObjectStructureChanged(*target);
					}
				}
				result.inverseDeltas.push_back(std::move(inverse));
				break;
			}
			case TransactionWriteKind::CreateObject:
				break;
			}

			if (!result.conflicts.empty())
			{
				break;
			}
		}
		result.commitCostNs = ElapsedNs(commitStart);
		if (!result.conflicts.empty())
		{
			const bool rollbackSucceeded = ApplyTransactionInverseDeltas(objects, result.inverseDeltas);
			result.status = TransactionStatus::Rejected;
			result.appliedWriteCount = 0;
			result.rejectedWriteCount = LogicalWriteCount(transaction);
			result.message = rollbackSucceeded
				? "Transaction publication failed and all applied writes were rolled back."
				: "Transaction publication failed and rollback could not restore every write.";
			result.inverseDeltas.clear();
			result.createdObjects.clear();
			result.acceptedAnimationPublications.clear();
			return result;
		}
		result.status = TransactionStatus::Committed;
		result.appliedWriteCount = LogicalWriteCount(transaction);
		result.message = "Transaction committed.";
		return result;
	}

	TransactionPublicationGroupResult PreflightTransactionPublicationGroup(ObjectManager& objects, const TransactionPublicationGroup& group)
	{
		TransactionPublicationGroupResult result;
		result.groupId = group.groupId;
		result.aggregateResult.transactionId = group.groupId;
		if (group.groupId.empty() || group.chunks.empty() || group.canceled)
		{
			result.status = group.canceled ? TransactionStatus::Canceled : TransactionStatus::Rejected;
			result.aggregateResult.status = result.status;
			result.message = group.canceled
				? "Publication group was canceled before preflight."
				: "Publication group needs a nonempty identity and at least one chunk.";
			return result;
		}

		Transaction flattened;
		flattened.id = group.groupId;
		flattened.origin = group.chunks.front().origin;
		flattened.originName = group.chunks.front().originName;
		flattened.mode = TransactionMode::PreflightOnly;
		// Worker completion order is intentionally irrelevant. Sorting complete
		// chunks by their stable producer transaction id gives one repeatable group
		// order without exposing a live scene object to worker threads.
		std::vector<const Transaction*> orderedChunks;
		orderedChunks.reserve(group.chunks.size());
		for (const Transaction& chunk : group.chunks)
		{
			orderedChunks.push_back(&chunk);
		}
		std::sort(orderedChunks.begin(), orderedChunks.end(), [](const Transaction* left, const Transaction* right) {
			return left->id < right->id;
		});

		std::unordered_set<std::string> transactionIds;
		std::unordered_set<TerraEngine::UUID> transformTargets;
		std::size_t totalWrites = 0U;
		result.chunkReceipts.reserve(orderedChunks.size());
		for (std::size_t chunkIndex = 0U; chunkIndex < orderedChunks.size(); ++chunkIndex)
		{
			result.chunkReceipts.push_back(TransactionPublicationChunkReceipt{
				.transactionId = orderedChunks[chunkIndex]->id,
				.chunkIndex = chunkIndex,
				.writeCount = LogicalWriteCount(*orderedChunks[chunkIndex])});
		}
		for (std::size_t chunkIndex = 0U; chunkIndex < orderedChunks.size(); ++chunkIndex)
		{
			const Transaction& chunk = *orderedChunks[chunkIndex];
			if (chunk.id.empty() || !transactionIds.insert(chunk.id).second
				|| chunk.canceled || chunk.writeSet.operations.empty()
				|| LogicalWriteCount(chunk) > chunk.budget.maxWriteCount)
			{
				result.status = TransactionStatus::Rejected;
				result.aggregateResult.status = TransactionStatus::Rejected;
				result.message = "A publication chunk has an invalid identity, payload, cancellation state, or write budget.";
				for (TransactionPublicationChunkReceipt& current : result.chunkReceipts)
				{
					current.status = TransactionStatus::Rejected;
				}
				return result;
			}

			const auto mergeExpectedGeneration = [&](std::uint64_t& aggregate, std::uint64_t expected) {
				if (expected == 0U) return true;
				if (aggregate == 0U) aggregate = expected;
				return aggregate == expected;
			};
			if (!mergeExpectedGeneration(flattened.readSet.sceneGeneration, chunk.readSet.sceneGeneration)
				|| !mergeExpectedGeneration(flattened.readSet.sceneRevision, chunk.readSet.sceneRevision))
			{
				result.status = TransactionStatus::Rejected;
				result.aggregateResult.status = TransactionStatus::Rejected;
				result.message = "Publication chunks were prepared from different scene or hierarchy generations.";
				for (TransactionPublicationChunkReceipt& current : result.chunkReceipts)
				{
					current.status = TransactionStatus::Rejected;
				}
				return result;
			}

			for (const TransactionWriteOperation& operation : chunk.writeSet.operations)
			{
				if (operation.kind != TransactionWriteKind::UpdateTransformBatch) continue;
				for (const TransactionTransformUpdate& update : operation.transformUpdates)
				{
					if (!transformTargets.insert(update.objectId).second)
					{
						result.status = TransactionStatus::Rejected;
						result.aggregateResult.status = TransactionStatus::Rejected;
						result.message = "A transform target appears in more than one publication chunk.";
						for (TransactionPublicationChunkReceipt& current : result.chunkReceipts)
						{
							current.status = TransactionStatus::Rejected;
						}
						return result;
					}
				}
			}

			totalWrites += LogicalWriteCount(chunk);
			flattened.readSet.objects.insert(flattened.readSet.objects.end(), chunk.readSet.objects.begin(), chunk.readSet.objects.end());
			flattened.writeSet.operations.insert(flattened.writeSet.operations.end(), chunk.writeSet.operations.begin(), chunk.writeSet.operations.end());
		}
		flattened.budget.maxWriteCount = totalWrites;
		flattened.budget.maxConflictCount = 128U;
		result.aggregateResult = PreflightTransaction(objects, flattened);
		result.status = result.aggregateResult.status;
		result.preflightAccepted = result.status == TransactionStatus::PreflightAccepted;
		result.rejectedWriteCount = result.status == TransactionStatus::PreflightAccepted ? 0U : totalWrites;
		for (TransactionPublicationChunkReceipt& receipt : result.chunkReceipts)
		{
			receipt.status = result.status;
		}
		result.message = result.status == TransactionStatus::PreflightAccepted
			? "Every publication chunk passed one tick-wide preflight."
			: "Tick-wide publication preflight rejected the complete group.";
		return result;
	}

	TransactionPublicationGroupResult CommitTransactionPublicationGroup(ObjectManager& objects, const TransactionPublicationGroup& group)
	{
		TransactionPublicationGroupResult result = PreflightTransactionPublicationGroup(objects, group);
		if (result.status != TransactionStatus::PreflightAccepted)
		{
			return result;
		}

		// Rebuild the same flattened transaction at the protected boundary. No live
		// write occurs between this revalidation and CommitTransaction's own preflight.
		Transaction flattened;
		flattened.id = group.groupId;
		flattened.origin = group.chunks.front().origin;
		flattened.originName = group.chunks.front().originName;
		flattened.mode = TransactionMode::Commit;
		std::size_t totalWrites = 0U;
		std::vector<const Transaction*> orderedChunks;
		orderedChunks.reserve(group.chunks.size());
		for (const Transaction& chunk : group.chunks)
		{
			orderedChunks.push_back(&chunk);
		}
		std::sort(orderedChunks.begin(), orderedChunks.end(), [](const Transaction* left, const Transaction* right) {
			return left->id < right->id;
		});
		for (const Transaction* chunkPointer : orderedChunks)
		{
			const Transaction& chunk = *chunkPointer;
			if (flattened.readSet.sceneGeneration == 0U) flattened.readSet.sceneGeneration = chunk.readSet.sceneGeneration;
			if (flattened.readSet.sceneRevision == 0U) flattened.readSet.sceneRevision = chunk.readSet.sceneRevision;
			totalWrites += LogicalWriteCount(chunk);
			flattened.readSet.objects.insert(flattened.readSet.objects.end(), chunk.readSet.objects.begin(), chunk.readSet.objects.end());
			flattened.writeSet.operations.insert(flattened.writeSet.operations.end(), chunk.writeSet.operations.begin(), chunk.writeSet.operations.end());
		}
		flattened.budget.maxWriteCount = totalWrites;
		flattened.budget.maxConflictCount = 128U;
		result.aggregateResult = CommitTransaction(objects, flattened);
		result.status = result.aggregateResult.status;
		result.appliedWriteCount = result.aggregateResult.appliedWriteCount;
		result.rejectedWriteCount = result.aggregateResult.rejectedWriteCount;
		for (TransactionPublicationChunkReceipt& receipt : result.chunkReceipts)
		{
			receipt.status = result.status;
		}
		result.message = result.status == TransactionStatus::Committed
			? "Every publication chunk committed as one atomic group."
			: "The complete publication group was rejected without partial visibility.";
		return result;
	}

	bool ApplyTransactionInverseDeltas(ObjectManager& objects, const std::vector<TransactionInverseDelta>& inverseDeltas)
	{
		bool restoredEveryDelta = true;
		const std::unordered_map<std::int32_t, TerraEngine::UUID> noLocalObjects;
		for (auto delta = inverseDeltas.rbegin(); delta != inverseDeltas.rend(); ++delta)
		{
			const TransactionWriteOperation& operation = delta->operation;
			if (delta->restoresSerializedObject)
			{
				if (!delta->serializedObject.is_object() || delta->serializedObject.empty())
				{
					restoredEveryDelta = false;
					continue;
				}
				const entt::entity restoredEntity = Serializer::DeserializeObjectEntity(delta->serializedObject);
				const std::optional<Object> restored = objects.getObject(restoredEntity);
				if (!restored)
				{
					restoredEveryDelta = false;
					continue;
				}
				if (HasObjectRef(operation.parent))
				{
					const std::optional<Object> parent = ResolveRef(objects, operation.parent, noLocalObjects);
					restoredEveryDelta = parent && objects.setParent(*restored, parent) && restoredEveryDelta;
				}
				continue;
			}

			if (operation.kind == TransactionWriteKind::UpdateTransformBatch)
			{
				for (const TransactionTransformUpdate& update : operation.transformUpdates)
				{
					const std::optional<Object> target = objects.getObject(update.objectId);
					if (!target || objects.SetCanonicalWorldPose(*target, update.pose) != WorldCoordinateError::None)
					{
						restoredEveryDelta = false;
						continue;
					}
				}
				continue;
			}

			const std::optional<Object> target = ResolveRef(objects, operation.target, noLocalObjects);
			if (!target)
			{
				restoredEveryDelta = false;
				continue;
			}

			switch (operation.kind)
			{
			case TransactionWriteKind::SetWorldTransform:
				if (objects.SetCanonicalWorldPose(*target, operation.worldPose) != WorldCoordinateError::None)
				{
					restoredEveryDelta = false;
				}
				break;
			case TransactionWriteKind::TranslateWorld:
				// Translation never appears as an inverse: commit captures the exact
				// prior WorldPose as SetWorldTransform to avoid inverse drift.
				restoredEveryDelta = false;
				break;
			case TransactionWriteKind::ChangeSimulationFrame:
				// Provider ownership is restored by the physics migration boundary. The
				// scene transaction's inverse records only its paired frame identity.
				break;
			case TransactionWriteKind::DeleteObject:
				objects.removeObject(*target);
				break;
			case TransactionWriteKind::ReparentObject:
			{
				const std::optional<Object> parent = HasObjectRef(operation.parent)
					? ResolveRef(objects, operation.parent, noLocalObjects)
					: std::optional<Object>{};
				if (HasObjectRef(operation.parent) && !parent)
				{
					restoredEveryDelta = false;
					break;
				}
				restoredEveryDelta = objects.setParent(*target, parent, operation.keepWorldOnReparent ? ReparentMode::KeepWorld : ReparentMode::KeepLocal) && restoredEveryDelta;
				break;
			}
			case TransactionWriteKind::SetName:
				objects.setObjectName(*target, operation.stringValue);
				break;
			case TransactionWriteKind::SetTag:
				objects.setObjectTag(*target, operation.stringValue);
				break;
			case TransactionWriteKind::SetLayer:
				objects.setObjectLayer(*target, operation.stringValue);
				break;
			case TransactionWriteKind::SetActive:
				objects.setObjectActive(*target, operation.boolValue);
				break;
			case TransactionWriteKind::SetStatic:
				objects.setObjectStatic(*target, operation.boolValue);
				break;
			case TransactionWriteKind::AddComponent:
			case TransactionWriteKind::RemoveComponent:
			case TransactionWriteKind::UpdateComponentProperty:
			{
				std::lock_guard registryLock(ComponentAccessRegistryMutex());
				auto component = ComponentAccessRegistry().find(operation.componentName);
				if (component == ComponentAccessRegistry().end())
				{
					restoredEveryDelta = false;
					break;
				}
				entt::registry& registry = objects.getRegistry();
				const entt::entity entity = target->EntityId();
				if (operation.kind == TransactionWriteKind::AddComponent)
				{
					component->second.entityAddComponent(registry, entity);
					if (delta->restoresSerializedComponent)
					{
						component->second.entityDeserialize(registry, entity, delta->serializedComponent);
					}
				}
				else if (operation.kind == TransactionWriteKind::RemoveComponent)
				{
					component->second.entityRemoveComponent(registry, entity);
				}
				else if (!component->second.entitySetter(registry, entity, operation.propertyName, operation.propertyValue))
				{
					restoredEveryDelta = false;
				}
				if (operation.componentName != Transform::kComponentName) {
					// Rollback uses the same reflected setter, so Transform
					// restoration has already invalidated spatial snapshots.
					objects.notifyObjectStructureChanged(*target);
				}
				break;
			}
			case TransactionWriteKind::CreateObject:
				// Serialized restoration is handled before target resolution above.
				restoredEveryDelta = false;
				break;
			}
		}
		return restoredEveryDelta;
	}

	void AccumulateTransactionDiagnostics(TransactionDiagnostics& diagnostics, const Transaction& transaction, const TransactionResult& result)
	{
		++diagnostics.submittedCount;
		diagnostics.largestSubmittedBatchSize = std::max(diagnostics.largestSubmittedBatchSize, LogicalWriteCount(transaction));
		switch (result.status)
		{
		case TransactionStatus::Committed:
			++diagnostics.committedCount;
			diagnostics.largestCommittedBatchSize = std::max(diagnostics.largestCommittedBatchSize, result.appliedWriteCount);
			break;
		case TransactionStatus::Rejected:
		case TransactionStatus::PreflightRejected:
			++diagnostics.rejectedCount;
			break;
		case TransactionStatus::Conflicted:
			++diagnostics.conflictCount;
			break;
		case TransactionStatus::Canceled:
			++diagnostics.canceledCount;
			break;
		case TransactionStatus::Expired:
			++diagnostics.expiredCount;
			break;
		default:
			break;
		}
		const std::uint64_t validationSamples = std::max<std::uint64_t>(1, diagnostics.submittedCount);
		diagnostics.averageValidationCostNs = ((diagnostics.averageValidationCostNs * (validationSamples - 1)) + result.validationCostNs) / validationSamples;
		diagnostics.averageCommitCostNs = ((diagnostics.averageCommitCostNs * (validationSamples - 1)) + result.commitCostNs) / validationSamples;
		// Diagnostics retain receipt metadata for inspection, not executable undo
		// payloads. A physics transform publication can carry thousands of inverse
		// poses, so copying those vectors into each of the 64 history entries would
		// turn a bounded receipt list into a large recurring allocation hot path.
		// The caller still receives the complete TransactionResult for rollback and
		// editor undo; only this diagnostic copy discards heavyweight payloads.
		TransactionResult diagnosticResult = result;
		diagnosticResult.inverseDeltas.clear();
		diagnosticResult.createdObjects.clear();
		diagnosticResult.acceptedAnimationPublications.clear();
		diagnostics.recentResults.push_back(std::move(diagnosticResult));
		if (diagnostics.recentResults.size() > 64)
		{
			diagnostics.recentResults.erase(diagnostics.recentResults.begin());
		}
	}

	TransactionDiagnostics MergeTransactionDiagnostics(const TransactionDiagnostics& left, const TransactionDiagnostics& right)
	{
		TransactionDiagnostics merged = left;
		merged.submittedCount += right.submittedCount;
		merged.committedCount += right.committedCount;
		merged.rejectedCount += right.rejectedCount;
		merged.conflictCount += right.conflictCount;
		merged.canceledCount += right.canceledCount;
		merged.expiredCount += right.expiredCount;
		merged.largestSubmittedBatchSize = std::max(left.largestSubmittedBatchSize, right.largestSubmittedBatchSize);
		merged.largestCommittedBatchSize = std::max(left.largestCommittedBatchSize, right.largestCommittedBatchSize);
		merged.recentResults.insert(merged.recentResults.end(), right.recentResults.begin(), right.recentResults.end());
		while (merged.recentResults.size() > 64)
		{
			merged.recentResults.erase(merged.recentResults.begin());
		}
		return merged;
	}

	nlohmann::json SerializeSpatialTransaction(const Transaction& transaction)
	{
		nlohmann::json operations = nlohmann::json::array();
		for (const TransactionWriteOperation& operation : transaction.writeSet.operations)
		{
			if (operation.kind != TransactionWriteKind::SetWorldTransform && operation.kind != TransactionWriteKind::TranslateWorld && operation.kind != TransactionWriteKind::ChangeSimulationFrame) continue;
			nlohmann::json encoded{{"target", TerraEngine::ToHexString(operation.target.objectId)}};
			if (operation.kind == TransactionWriteKind::SetWorldTransform)
			{
				encoded["kind"] = "setWorldPose";
				encoded["position"] = Serializer::SerializeWorldPosition(operation.worldPose.position);
				encoded["rotation"] = {operation.worldPose.rotation.w, operation.worldPose.rotation.x, operation.worldPose.rotation.y, operation.worldPose.rotation.z};
				encoded["scale"] = {operation.worldPose.scale.x, operation.worldPose.scale.y, operation.worldPose.scale.z};
			}
			else if (operation.kind == TransactionWriteKind::TranslateWorld)
			{
				encoded["kind"] = "translateWorld";
				encoded["delta"] = {{"cell", {{"x", std::to_string(operation.worldDelta.cellDelta.x)}, {"y", std::to_string(operation.worldDelta.cellDelta.y)}, {"z", std::to_string(operation.worldDelta.cellDelta.z)}}}, {"offsetMetres", {operation.worldDelta.offsetMetres.x, operation.worldDelta.offsetMetres.y, operation.worldDelta.offsetMetres.z}}};
			}
			else { encoded["kind"] = "changeSimulationFrame"; encoded["sourceGeneration"] = operation.sourceSimulationFrameGeneration; encoded["destinationGeneration"] = operation.destinationSimulationFrameGeneration; }
			operations.push_back(std::move(encoded));
		}
		return {{"format", kTerraSpatialTransactionFormat}, {"id", transaction.id}, {"operations", std::move(operations)}};
	}

	std::optional<Transaction> LowerSpatialTransaction(const nlohmann::json& document, std::string* outError)
	{
		auto fail = [outError](std::string reason) -> std::optional<Transaction> { if (outError) *outError = std::move(reason); return std::nullopt; };
		if (!document.is_object() || document.value("format", std::string{}) != kTerraSpatialTransactionFormat || !document.contains("id") || !document["id"].is_string() || !document.contains("operations") || !document["operations"].is_array()) return fail("transaction format must be TerraSpatialTransactionV1 with id and operations");
		Transaction result; result.id = document["id"].get<std::string>(); result.origin = TransactionOrigin::ManagedCallback;
		for (const nlohmann::json& encoded : document["operations"])
		{
			if (!encoded.is_object() || !encoded.contains("kind") || !encoded["kind"].is_string() || !encoded.contains("target") || !encoded["target"].is_string()) return fail("spatial operation is missing kind or target");
			const auto target = TerraEngine::ParseUUID(encoded["target"].get<std::string>()); if (!target) return fail("spatial operation target is not a UUID");
			TransactionWriteOperation operation; operation.target = ExistingObject(*target); const std::string kind = encoded["kind"].get<std::string>();
			if (kind == "setWorldPose")
			{
				std::string positionError; const auto position = encoded.contains("position") ? Serializer::DeserializeWorldPosition(encoded["position"], &positionError) : std::nullopt; if (!position) return fail("setWorldPose: " + positionError);
				if (!encoded.contains("rotation") || !encoded["rotation"].is_array() || encoded["rotation"].size() != 4U || !encoded.contains("scale") || !encoded["scale"].is_array() || encoded["scale"].size() != 3U) return fail("setWorldPose requires rotation and scale arrays");
				operation.kind = TransactionWriteKind::SetWorldTransform; operation.worldPose.position = *position;
				for (std::size_t index = 0U; index < 4U; ++index) { if (!encoded["rotation"][index].is_number()) return fail("setWorldPose rotation must be numeric"); }
				for (std::size_t index = 0U; index < 3U; ++index) { if (!encoded["scale"][index].is_number()) return fail("setWorldPose scale must be numeric"); operation.worldPose.scale[static_cast<glm::length_t>(index)] = encoded["scale"][index].get<float>(); }
				operation.worldPose.rotation = glm::quat(encoded["rotation"][0].get<float>(), encoded["rotation"][1].get<float>(), encoded["rotation"][2].get<float>(), encoded["rotation"][3].get<float>());
			}
			else if (kind == "translateWorld")
			{
				std::string deltaError; const auto deltaPosition = encoded.contains("delta") ? Serializer::DeserializeWorldPosition(encoded["delta"], &deltaError) : std::nullopt; if (!deltaPosition) return fail("translateWorld: " + deltaError);
				operation.kind = TransactionWriteKind::TranslateWorld; operation.worldDelta.cellDelta = deltaPosition->cell; operation.worldDelta.offsetMetres = deltaPosition->offsetMetres;
			}
			else if (kind == "changeSimulationFrame")
			{
				if (!encoded.contains("sourceGeneration") || !encoded["sourceGeneration"].is_number_unsigned() || !encoded.contains("destinationGeneration") || !encoded["destinationGeneration"].is_number_unsigned()) return fail("changeSimulationFrame requires unsigned generations");
				operation.kind = TransactionWriteKind::ChangeSimulationFrame; operation.sourceSimulationFrameGeneration = encoded["sourceGeneration"].get<std::uint64_t>(); operation.destinationSimulationFrameGeneration = encoded["destinationGeneration"].get<std::uint64_t>();
			}
			else return fail("unsupported unbounded spatial operation");
			result.writeSet.operations.push_back(std::move(operation));
		}
		return result;
	}
}
