// TransactionalMutation.cpp - validation and protected commit for scene STM.

#include "TransactionalMutation.h"

#include "Components/Transform.h"
#include "Core/Object/Component.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectManager.h"

#include <algorithm>
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

		void AddConflict(TransactionResult& result, TransactionConflictReason reason, TerraEngine::UUID target, std::size_t operationIndex, std::string message, std::uint64_t expected = 0, std::uint64_t actual = 0, std::string component = {})
		{
			if (result.conflicts.size() >= 128)
			{
				return;
			}
			result.conflicts.push_back(TransactionConflictRecord{
				reason,
				target,
				std::move(component),
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

		bool SameObjectRef(const TransactionObjectRef& left, const TransactionObjectRef& right)
		{
			return left.objectId == right.objectId && left.localObjectId == right.localObjectId;
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
				reversed.push_back(*operation);
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

			if (transaction.writeSet.operations.empty() && transaction.mode == TransactionMode::Commit)
			{
				result.status = TransactionStatus::Rejected;
				AddConflict(result, TransactionConflictReason::InvalidPayload, TerraEngine::EMPTY_UUID, 0, "Commit transaction contains no writes.");
				return result;
			}

			if (transaction.writeSet.operations.size() > transaction.budget.maxWriteCount)
			{
				result.status = TransactionStatus::Rejected;
				AddConflict(result, TransactionConflictReason::CommitBudgetRejection, TerraEngine::EMPTY_UUID, 0, "Transaction exceeds its configured write budget.", transaction.budget.maxWriteCount, transaction.writeSet.operations.size());
				return result;
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
			}

			std::unordered_set<std::int32_t> localCreates;
			std::unordered_set<std::string> registeredComponentNames;
			{
				std::lock_guard registryLock(ComponentAccessRegistryMutex());
				for (const auto& [componentName, functions] : ComponentAccessRegistry())
				{
					(void)functions;
					registeredComponentNames.insert(componentName);
				}
			}
			for (std::size_t index = 0; index < transaction.writeSet.operations.size(); ++index)
			{
				const TransactionWriteOperation& operation = transaction.writeSet.operations[index];
				if (operation.kind == TransactionWriteKind::CreateObject)
				{
					if (operation.createdLocalObjectId == 0 || !localCreates.insert(operation.createdLocalObjectId).second)
					{
						result.status = TransactionStatus::Rejected;
						AddConflict(result, TransactionConflictReason::InvalidPayload, TerraEngine::EMPTY_UUID, index, "Created objects need unique nonzero transaction-local ids.");
					}
					continue;
				}
				if (RequiresExistingTarget(operation.kind) && !operation.target.UsesLocalObject() && objects.getObject(operation.target.objectId) == std::nullopt)
				{
					result.status = TransactionStatus::Conflicted;
					AddConflict(result, TransactionConflictReason::MissingTarget, operation.target.objectId, index, "Write target no longer exists.");
				}
				if (!ValidateComponentPayload(operation))
				{
					result.status = TransactionStatus::Rejected;
					AddConflict(result, TransactionConflictReason::InvalidPayload, operation.target.objectId, index, "Component write payload is incomplete.", 0, 0, operation.componentName);
				}
				if (IsComponentWrite(operation.kind))
				{
					if (!registeredComponentNames.contains(operation.componentName))
					{
						result.status = TransactionStatus::Rejected;
						AddConflict(result, TransactionConflictReason::InvalidPayload, operation.target.objectId, index, "Component type is not registered.", 0, 0, operation.componentName);
					}
				}
			}

			if (!result.conflicts.empty() && result.status == TransactionStatus::PreflightAccepted)
			{
				result.status = TransactionStatus::Conflicted;
			}
			result.rejectedWriteCount = result.conflicts.empty() ? 0 : transaction.writeSet.operations.size();
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
		transaction.readSet.sceneRevision = objects.GetHierarchyRevision();
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::ObserveObject(ObjectManager& objects, TerraEngine::UUID objectId)
	{
		if (const std::optional<Object> object = objects.getObject(objectId))
		{
			transaction.readSet.objects.push_back(TransactionObjectObservation{
				objectId,
				objects.getObjectLifetimeGeneration(*object),
				objects.getObjectMetadataGeneration(*object),
				objects.getObjectComponentGeneration(*object),
				objects.GetHierarchyRevision()
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
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::DeleteObject;
		operation.target = ExistingObject(objectId);
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::ReparentObject(TerraEngine::UUID objectId, TransactionObjectRef parent)
	{
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::ReparentObject;
		operation.target = ExistingObject(objectId);
		operation.parent = parent;
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::SetName(TerraEngine::UUID objectId, std::string name)
	{
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::SetName;
		operation.target = ExistingObject(objectId);
		operation.stringValue = std::move(name);
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::SetTag(TerraEngine::UUID objectId, std::string tag)
	{
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::SetTag;
		operation.target = ExistingObject(objectId);
		operation.stringValue = std::move(tag);
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::SetLayer(TerraEngine::UUID objectId, std::string layer)
	{
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::SetLayer;
		operation.target = ExistingObject(objectId);
		operation.stringValue = std::move(layer);
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::SetActive(TerraEngine::UUID objectId, bool active)
	{
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::SetActive;
		operation.target = ExistingObject(objectId);
		operation.boolValue = active;
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::SetStatic(TerraEngine::UUID objectId, bool isStatic)
	{
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::SetStatic;
		operation.target = ExistingObject(objectId);
		operation.boolValue = isStatic;
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::AddComponent(TerraEngine::UUID objectId, std::string componentName)
	{
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::AddComponent;
		operation.target = ExistingObject(objectId);
		operation.componentName = std::move(componentName);
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::RemoveComponent(TerraEngine::UUID objectId, std::string componentName)
	{
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::RemoveComponent;
		operation.target = ExistingObject(objectId);
		operation.componentName = std::move(componentName);
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	TransactionalMutationBuilder& TransactionalMutationBuilder::UpdateComponentProperty(TerraEngine::UUID objectId, std::string componentName, std::string propertyName, TerraEngine::Reflection::Variant value)
	{
		TransactionWriteOperation operation;
		operation.kind = TransactionWriteKind::UpdateComponentProperty;
		operation.target = ExistingObject(objectId);
		operation.componentName = std::move(componentName);
		operation.propertyName = std::move(propertyName);
		operation.propertyValue = std::move(value);
		transaction.writeSet.operations.push_back(std::move(operation));
		return *this;
	}

	Transaction TransactionalMutationBuilder::Build() const
	{
		return NormalizeTransaction(transaction);
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
			switch (operation.kind)
			{
			case TransactionWriteKind::CreateObject:
			{
				Object created = objects.createObject(operation.displayName.empty() ? "Object" : operation.displayName);
				const TerraEngine::UUID createdId = objects.getUUID(created);
				localObjects[operation.createdLocalObjectId] = createdId;
				result.createdObjects[operation.createdLocalObjectId] = createdId;
				if (const std::optional<Object> parent = ResolveRef(objects, operation.parent, localObjects))
				{
					objects.setParent(created, parent);
				}
				break;
			}
			case TransactionWriteKind::DeleteObject:
				if (const std::optional<Object> target = ResolveRef(objects, operation.target, localObjects))
				{
					objects.removeObject(*target);
				}
				break;
			case TransactionWriteKind::ReparentObject:
				if (const std::optional<Object> target = ResolveRef(objects, operation.target, localObjects))
				{
					objects.setParent(*target, ResolveRef(objects, operation.parent, localObjects));
				}
				break;
			case TransactionWriteKind::SetName:
				if (const std::optional<Object> target = ResolveRef(objects, operation.target, localObjects))
				{
					objects.setObjectName(*target, operation.stringValue);
				}
				break;
			case TransactionWriteKind::SetTag:
				if (const std::optional<Object> target = ResolveRef(objects, operation.target, localObjects))
				{
					objects.setObjectTag(*target, operation.stringValue);
				}
				break;
			case TransactionWriteKind::SetLayer:
				if (const std::optional<Object> target = ResolveRef(objects, operation.target, localObjects))
				{
					objects.setObjectLayer(*target, operation.stringValue);
				}
				break;
			case TransactionWriteKind::SetActive:
				if (const std::optional<Object> target = ResolveRef(objects, operation.target, localObjects))
				{
					objects.setObjectActive(*target, operation.boolValue);
				}
				break;
			case TransactionWriteKind::SetStatic:
				if (const std::optional<Object> target = ResolveRef(objects, operation.target, localObjects))
				{
					objects.setObjectStatic(*target, operation.boolValue);
				}
				break;
			case TransactionWriteKind::AddComponent:
			case TransactionWriteKind::RemoveComponent:
			case TransactionWriteKind::UpdateComponentProperty:
				if (const std::optional<Object> target = ResolveRef(objects, operation.target, localObjects))
				{
					std::lock_guard registryLock(ComponentAccessRegistryMutex());
					auto& registry = ComponentAccessRegistry();
					auto component = registry.find(operation.componentName);
					if (component == registry.end())
					{
						AddConflict(result, TransactionConflictReason::InvalidPayload, objects.getUUID(*target), index, "Component type is not registered.", 0, 0, operation.componentName);
						break;
					}
					entt::registry& enttRegistry = objects.getRegistry();
					const entt::entity targetEntity = target->EntityId();
					if (operation.kind == TransactionWriteKind::AddComponent && !component->second.entityHasComponent(enttRegistry, targetEntity))
					{
						component->second.entityAddComponent(enttRegistry, targetEntity);
						objects.notifyObjectStructureChanged(*target);
					}
					else if (operation.kind == TransactionWriteKind::RemoveComponent && component->second.entityHasComponent(enttRegistry, targetEntity))
					{
						component->second.entityRemoveComponent(enttRegistry, targetEntity);
						objects.notifyObjectStructureChanged(*target);
					}
					else if (operation.kind == TransactionWriteKind::UpdateComponentProperty)
					{
						if (!component->second.entitySetter(enttRegistry, targetEntity, operation.propertyName, operation.propertyValue))
						{
							AddConflict(result, TransactionConflictReason::InvalidPayload, objects.getUUID(*target), index, "Reflected component property write failed.", 0, 0, operation.componentName);
						}
						if (operation.componentName == Transform::kComponentName)
						{
							objects.MarkObjectTransformDirty(*target);
						}
						else
						{
							objects.notifyObjectStructureChanged(*target);
						}
					}
				}
				break;
			}
		}
		result.commitCostNs = ElapsedNs(commitStart);
		if (!result.conflicts.empty())
		{
			result.status = TransactionStatus::Rejected;
			result.appliedWriteCount = 0;
			result.rejectedWriteCount = transaction.writeSet.operations.size();
			result.message = "Transaction commit hit an unexpected apply error.";
			return result;
		}
		result.status = TransactionStatus::Committed;
		result.appliedWriteCount = transaction.writeSet.operations.size();
		result.message = "Transaction committed.";
		return result;
	}

	void AccumulateTransactionDiagnostics(TransactionDiagnostics& diagnostics, const Transaction& transaction, const TransactionResult& result)
	{
		++diagnostics.submittedCount;
		diagnostics.largestSubmittedBatchSize = std::max(diagnostics.largestSubmittedBatchSize, transaction.writeSet.operations.size());
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
		diagnostics.recentResults.push_back(result);
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
}
