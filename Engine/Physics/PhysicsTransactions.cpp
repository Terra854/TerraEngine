// PhysicsTransactions.cpp - dual preflight, staging, commit, and rollback.

#include "PhysicsTransactions.h"

#include "Core/Object/ObjectManager.h"

#include <algorithm>

namespace TerraEngine::Scene {
	TransactionReadSet CaptureTransactionReadSet(
		ObjectManager& objects,
		std::initializer_list<TerraEngine::UUID> objectIds) {
		TransactionReadSet readSet;
		readSet.sceneRevision = objects.GetHierarchyRevision();
		readSet.objects.reserve(objectIds.size());
		for(const TerraEngine::UUID& objectId : objectIds) {
			const std::optional<Object> object = objects.getObject(objectId);
			if(!object) {
				// Missing targets are retained as zero-generation observations so
				// preflight reports a conflict instead of silently losing the read.
				readSet.objects.push_back(TransactionObjectObservation{.objectId = objectId});
				continue;
			}
			readSet.objects.push_back(TransactionObjectObservation{
				.objectId = objectId,
				.lifetimeGeneration = objects.getObjectLifetimeGeneration(*object),
				.metadataGeneration = objects.getObjectMetadataGeneration(*object),
				.componentGeneration = objects.getObjectComponentGeneration(*object),
				.hierarchyRevision = objects.GetHierarchyRevision()});
		}
		return readSet;
	}
}

namespace TerraEngine::Physics {
	namespace {
		using GenerationMap = std::unordered_map<TerraEngine::UUID, PhysicsGeneration>;

		PhysicsGeneration NextGeneration(PhysicsGeneration generation) {
			++generation;
			return generation == 0U ? 1U : generation;
		}

		bool RequiresObjectTarget(PhysicsCommandType type) {
			switch(type) {
			case PhysicsCommandType::ScheduleQuery:
			case PhysicsCommandType::CancelQuery:
			case PhysicsCommandType::UpdateWorldSettings:
			case PhysicsCommandType::ReplaceWorld:
				return false;
			default:
				return true;
			}
		}

		bool IsBodyCommand(PhysicsCommandType type) {
			switch(type) {
			case PhysicsCommandType::CreateBody:
			case PhysicsCommandType::RebuildBody:
			case PhysicsCommandType::EnableBody:
			case PhysicsCommandType::WakeBody:
			case PhysicsCommandType::SleepBody:
			case PhysicsCommandType::ApplyForce:
			case PhysicsCommandType::ApplyAcceleration:
			case PhysicsCommandType::ApplyImpulse:
			case PhysicsCommandType::ApplyVelocityChange:
			case PhysicsCommandType::SetVelocity:
			case PhysicsCommandType::Teleport:
			case PhysicsCommandType::SetKinematicTarget:
			case PhysicsCommandType::RebuildColliderSet:
			case PhysicsCommandType::UpdateMaterialFilter:
			case PhysicsCommandType::DestroyBody:
				return true;
			default:
				return false;
			}
		}

		bool IsConstraintCommand(PhysicsCommandType type) {
			return type == PhysicsCommandType::CreateConstraint
				|| type == PhysicsCommandType::UpdateConstraint
				|| type == PhysicsCommandType::ResetConstraint
				|| type == PhysicsCommandType::DestroyConstraint;
		}

		bool IsCharacterCommand(PhysicsCommandType type) {
			return type == PhysicsCommandType::CreateCharacter
				|| type == PhysicsCommandType::MoveCharacter
				|| type == PhysicsCommandType::JumpCharacter
				|| type == PhysicsCommandType::ResizeCharacter
				|| type == PhysicsCommandType::UpdateCharacter
				|| type == PhysicsCommandType::DestroyCharacter;
		}

		bool IsCreateCommand(PhysicsCommandType type) {
			return type == PhysicsCommandType::CreateBody
				|| type == PhysicsCommandType::CreateConstraint
				|| type == PhysicsCommandType::CreateCharacter;
		}

		bool IsDestroyCommand(PhysicsCommandType type) {
			return type == PhysicsCommandType::DestroyBody
				|| type == PhysicsCommandType::DestroyConstraint
				|| type == PhysicsCommandType::DestroyCharacter;
		}

		bool IsRebuildCommand(PhysicsCommandType type) {
			return type == PhysicsCommandType::RebuildBody
				|| type == PhysicsCommandType::RebuildColliderSet
				|| type == PhysicsCommandType::UpdateConstraint
				|| type == PhysicsCommandType::UpdateCharacter;
		}

		TerraEngine::UUID CommandIdentity(const PhysicsCommand& command, PhysicsProviderRecordDomain domain) {
			if(domain == PhysicsProviderRecordDomain::Constraint
				&& command.recordId != TerraEngine::EMPTY_UUID) {
				return command.recordId;
			}
			return command.targetObjectId;
		}

		const GenerationMap* RuntimeMap(
			const PhysicsTransactionRuntimeState& runtime,
			PhysicsProviderRecordDomain domain) {
			switch(domain) {
			case PhysicsProviderRecordDomain::Body: return &runtime.bodyGenerations;
			case PhysicsProviderRecordDomain::Constraint: return &runtime.constraintGenerations;
			case PhysicsProviderRecordDomain::Character: return &runtime.characterGenerations;
			case PhysicsProviderRecordDomain::None: return nullptr;
			}
			return nullptr;
		}

		GenerationMap* RuntimeMap(
			PhysicsTransactionRuntimeState& runtime,
			PhysicsProviderRecordDomain domain) {
			return const_cast<GenerationMap*>(RuntimeMap(
				static_cast<const PhysicsTransactionRuntimeState&>(runtime), domain));
		}

		const GenerationMap* ReadSetMap(
			const PhysicsTransactionReadSet& readSet,
			PhysicsProviderRecordDomain domain) {
			switch(domain) {
			case PhysicsProviderRecordDomain::Body: return &readSet.bodyGenerations;
			case PhysicsProviderRecordDomain::Constraint: return &readSet.constraintGenerations;
			case PhysicsProviderRecordDomain::Character: return &readSet.characterGenerations;
			case PhysicsProviderRecordDomain::None: return nullptr;
			}
			return nullptr;
		}

		PhysicsProviderRecordDomain CommandDomain(PhysicsCommandType type) {
			if(IsBodyCommand(type)) return PhysicsProviderRecordDomain::Body;
			if(IsConstraintCommand(type)) return PhysicsProviderRecordDomain::Constraint;
			if(IsCharacterCommand(type)) return PhysicsProviderRecordDomain::Character;
			return PhysicsProviderRecordDomain::None;
		}

		TerraEngine::Scene::Transaction MakeSceneTransaction(
			const PhysicsTransaction& transaction,
			TerraEngine::Scene::TransactionMode mode) {
			TerraEngine::Scene::Transaction scene;
			scene.id = transaction.id + ":scene";
			scene.origin = TerraEngine::Scene::TransactionOrigin::System;
			scene.originName = "PhysicsTransaction";
			scene.mode = mode;
			scene.readSet = transaction.sceneReadSet;
			scene.writeSet.operations = transaction.sceneWrites;
			scene.canceled = transaction.cancelled;
			return scene;
		}

		bool ValidatePhysicsReadSet(
			const PhysicsTransactionRuntimeState& runtime,
			const PhysicsTransaction& transaction,
			std::string& message) {
			if(!runtime.providerHealthy || !runtime.world.IsValid()) {
				message = "The physics provider or world is not available.";
				return false;
			}
			if(transaction.physicsReadSet.worldGeneration == 0U
				|| transaction.physicsReadSet.worldGeneration != runtime.world.worldGeneration
				|| (transaction.physicsReadSet.worldId.IsValid()
					&& transaction.physicsReadSet.worldId != runtime.world.worldId)) {
				message = "The physics world read set is stale.";
				return false;
			}

			const auto validateMap = [&message](const GenerationMap& expected, const GenerationMap& actual) {
				for(const auto& [identity, generation] : expected) {
					const auto live = actual.find(identity);
					if(generation == 0U || live == actual.end() || live->second != generation) {
						message = "A physics target generation changed after preparation.";
						return false;
					}
				}
				return true;
			};
			if(!validateMap(transaction.physicsReadSet.bodyGenerations, runtime.bodyGenerations)
				|| !validateMap(transaction.physicsReadSet.constraintGenerations, runtime.constraintGenerations)
				|| !validateMap(transaction.physicsReadSet.characterGenerations, runtime.characterGenerations)) {
				return false;
			}

			for(const PhysicsCommand& command : transaction.physicsCommands) {
				if(command.worldId != runtime.world.worldId
					|| command.worldGeneration != runtime.world.worldGeneration
					|| (RequiresObjectTarget(command.type)
						&& command.targetObjectId == TerraEngine::EMPTY_UUID)) {
					message = "A physics command targets an invalid or stale world/object.";
					return false;
				}
				const PhysicsProviderRecordDomain domain = CommandDomain(command.type);
				if(domain == PhysicsProviderRecordDomain::None || IsCreateCommand(command.type)) {
					continue;
				}
				const TerraEngine::UUID identity = CommandIdentity(command, domain);
				const GenerationMap* liveMap = RuntimeMap(runtime, domain);
				const GenerationMap* expectedMap = ReadSetMap(transaction.physicsReadSet, domain);
				const auto live = liveMap->find(identity);
				const auto expected = expectedMap->find(identity);
				if(live == liveMap->end() || expected == expectedMap->end()
					|| live->second != expected->second
					|| (command.componentGeneration != 0U && command.componentGeneration != live->second)) {
					message = "A command target is absent from the coordinated physics read set.";
					return false;
				}
			}
			return true;
		}

		bool HasSceneConflict(const TerraEngine::Scene::TransactionResult& sceneResult) {
			return sceneResult.status != TerraEngine::Scene::TransactionStatus::PreflightAccepted;
		}

		void ApplyProviderRecord(
			PhysicsTransactionRuntimeState& runtime,
			PhysicsProviderRecordDomain domain,
			PhysicsProviderRecordOperation operation,
			const TerraEngine::UUID& identity,
			PhysicsGeneration generation) {
			GenerationMap* map = RuntimeMap(runtime, domain);
			if(map == nullptr || operation == PhysicsProviderRecordOperation::None) {
				return;
			}
			if(operation == PhysicsProviderRecordOperation::Erase) {
				map->erase(identity);
			} else {
				(*map)[identity] = generation;
			}
		}

		void RollbackProviderRecords(
			PhysicsTransactionRuntimeState& runtime,
			const std::vector<PhysicsProviderInverseRecord>& inverses) {
			for(auto inverse = inverses.rbegin(); inverse != inverses.rend(); ++inverse) {
				ApplyProviderRecord(runtime, inverse->domain, inverse->operation,
					inverse->identity, inverse->restoredGeneration);
			}
		}
	}

	PhysicsTransaction::PhysicsTransaction(std::string transactionId)
		: id(std::move(transactionId)), correlationId(id) {
	}

	PhysicsTransactionResult PreflightPhysicsTransaction(
		ObjectManager& objects,
		const PhysicsTransactionRuntimeState& runtime,
		const PhysicsTransaction& transaction) {
		PhysicsTransactionResult result;
		result.state = PhysicsTransactionState::Preparing;
		if(transaction.cancelled) {
			result.state = PhysicsTransactionState::Cancelled;
			result.message = "Transaction was cancelled before preflight.";
			return result;
		}
		if(transaction.sceneWrites.empty() && transaction.physicsCommands.empty()) {
			result.state = PhysicsTransactionState::PreflightFailed;
			result.message = "Transaction contains no scene or physics writes.";
			return result;
		}

		const bool hasSceneReadSet = transaction.sceneReadSet.sceneRevision != 0U
			|| !transaction.sceneReadSet.objects.empty();
		if(hasSceneReadSet || !transaction.sceneWrites.empty()) {
			result.sceneResult = TerraEngine::Scene::PreflightTransaction(
				objects, MakeSceneTransaction(transaction, TerraEngine::Scene::TransactionMode::PreflightOnly));
			if(HasSceneConflict(result.sceneResult)) {
				result.state = PhysicsTransactionState::Conflicted;
				result.message = "Scene preflight rejected the coordinated transaction.";
				return result;
			}
		}

		std::string physicsMessage;
		if(!ValidatePhysicsReadSet(runtime, transaction, physicsMessage)) {
			result.state = PhysicsTransactionState::Conflicted;
			result.message = std::move(physicsMessage);
			return result;
		}
		result.accepted = true;
		result.state = PhysicsTransactionState::Prepared;
		result.message = "Scene and physics read sets passed dual preflight.";
		return result;
	}

	PhysicsTransactionResult StagePhysicsTransaction(
		const PhysicsTransactionRuntimeState& runtime,
		const PhysicsTransaction& transaction) {
		PhysicsTransactionResult result;
		result.state = PhysicsTransactionState::Preparing;
		if(transaction.cancelled) {
			result.state = PhysicsTransactionState::Cancelled;
			result.message = "Transaction was cancelled before provider staging.";
			return result;
		}
		std::string message;
		if(!ValidatePhysicsReadSet(runtime, transaction, message)) {
			result.state = PhysicsTransactionState::Conflicted;
			result.message = std::move(message);
			return result;
		}

		GenerationMap stagedBodies = runtime.bodyGenerations;
		GenerationMap stagedConstraints = runtime.constraintGenerations;
		GenerationMap stagedCharacters = runtime.characterGenerations;
		for(std::size_t index = 0; index < transaction.physicsCommands.size(); ++index) {
			const PhysicsCommand& command = transaction.physicsCommands[index];
			const PhysicsProviderRecordDomain domain = CommandDomain(command.type);
			GenerationMap* stagedMap = nullptr;
			switch(domain) {
			case PhysicsProviderRecordDomain::Body: stagedMap = &stagedBodies; break;
			case PhysicsProviderRecordDomain::Constraint: stagedMap = &stagedConstraints; break;
			case PhysicsProviderRecordDomain::Character: stagedMap = &stagedCharacters; break;
			case PhysicsProviderRecordDomain::None: break;
			}

			const TerraEngine::UUID identity = CommandIdentity(command, domain);
			const auto existing = stagedMap != nullptr ? stagedMap->find(identity) : GenerationMap::iterator{};
			const bool existed = stagedMap != nullptr && existing != stagedMap->end();
			const PhysicsGeneration priorGeneration = existed ? existing->second : 0U;
			PhysicsProviderRecordOperation operation = PhysicsProviderRecordOperation::None;
			PhysicsGeneration resultingGeneration = priorGeneration;
			PhysicsProviderRecordOperation inverseOperation = PhysicsProviderRecordOperation::None;
			PhysicsGeneration restoredGeneration = priorGeneration;

			if(stagedMap != nullptr && IsCreateCommand(command.type)) {
				if(existed) {
					result.state = PhysicsTransactionState::PreflightFailed;
					result.message = "Provider staging rejected a duplicate runtime identity.";
					return result;
				}
				resultingGeneration = command.componentGeneration != 0U ? command.componentGeneration : 1U;
				operation = PhysicsProviderRecordOperation::SetGeneration;
				inverseOperation = PhysicsProviderRecordOperation::Erase;
				(*stagedMap)[identity] = resultingGeneration;
			} else if(stagedMap != nullptr && IsDestroyCommand(command.type)) {
				if(!existed) {
					result.state = PhysicsTransactionState::Conflicted;
					result.message = "Provider staging could not find a runtime identity to destroy.";
					return result;
				}
				operation = PhysicsProviderRecordOperation::Erase;
				inverseOperation = PhysicsProviderRecordOperation::SetGeneration;
				stagedMap->erase(identity);
			} else if(stagedMap != nullptr && IsRebuildCommand(command.type)) {
				if(!existed) {
					result.state = PhysicsTransactionState::Conflicted;
					result.message = "Provider staging could not find a runtime identity to rebuild.";
					return result;
				}
				resultingGeneration = NextGeneration(priorGeneration);
				operation = PhysicsProviderRecordOperation::SetGeneration;
				inverseOperation = PhysicsProviderRecordOperation::SetGeneration;
				(*stagedMap)[identity] = resultingGeneration;
			}

			result.providerObjects.push_back(StagedPhysicsProviderObject{
				domain, operation, command.type, identity, priorGeneration,
				resultingGeneration, index});
			result.inverseRecords.push_back(PhysicsProviderInverseRecord{
				domain, inverseOperation, identity, restoredGeneration, index});
		}
		result.accepted = true;
		result.state = PhysicsTransactionState::Prepared;
		result.message = "Provider objects and complete inverse records were staged.";
		return result;
	}

	PhysicsTransactionResult CommitPhysicsTransaction(
		ObjectManager& objects,
		PhysicsTransactionRuntimeState& runtime,
		const PhysicsTransaction& transaction) {
		PhysicsTransactionResult result = PreflightPhysicsTransaction(objects, runtime, transaction);
		if(!result.accepted) {
			return result;
		}

		PhysicsTransactionResult staged = StagePhysicsTransaction(runtime, transaction);
		if(!staged.accepted) {
			return staged;
		}
		result.providerObjects = staged.providerObjects;
		result.inverseRecords = staged.inverseRecords;
		result.state = PhysicsTransactionState::Committing;

		// The protected boundary revalidates both domains immediately before any
		// provider-visible change. A stale read therefore exits with zero writes.
		PhysicsTransactionResult revalidated = PreflightPhysicsTransaction(objects, runtime, transaction);
		if(!revalidated.accepted) {
			return revalidated;
		}

		for(const StagedPhysicsProviderObject& providerObject : result.providerObjects) {
			ApplyProviderRecord(runtime, providerObject.domain, providerObject.operation,
				providerObject.identity, providerObject.resultingGeneration);
		}
		result.providerWorldUnchanged = transaction.physicsCommands.empty();

		if(!transaction.sceneWrites.empty()) {
			result.sceneResult = TerraEngine::Scene::CommitTransaction(
				objects, MakeSceneTransaction(transaction, TerraEngine::Scene::TransactionMode::Commit));
			if(result.sceneResult.status != TerraEngine::Scene::TransactionStatus::Committed) {
				RollbackProviderRecords(runtime, result.inverseRecords);
				result.accepted = false;
				result.state = PhysicsTransactionState::RolledBack;
				result.providerWorldUnchanged = true;
				result.message = "Scene publication failed; all staged provider changes were inverted.";
				return result;
			}
		}

		if(!transaction.sceneWrites.empty()) {
			++runtime.scenePublicationGeneration;
			result.mutatedScene = true;
			result.sceneGenerationPublished = true;
		}
		if(!transaction.physicsCommands.empty()) {
			++runtime.physicsPublicationGeneration;
			result.mutatedPhysics = true;
			result.physicsGenerationPublished = true;
		}
		result.accepted = true;
		result.state = PhysicsTransactionState::Committed;
		result.publishedWriteCount = transaction.sceneWrites.size() + transaction.physicsCommands.size();
		result.message = "Scene and physics generations published atomically.";
		return result;
	}

	std::string_view ToString(PhysicsTransactionState state) {
		switch(state) {
		case PhysicsTransactionState::Queued: return "Queued";
		case PhysicsTransactionState::Preparing: return "Preparing";
		case PhysicsTransactionState::PreflightFailed: return "PreflightFailed";
		case PhysicsTransactionState::Prepared: return "Prepared";
		case PhysicsTransactionState::Committing: return "Committing";
		case PhysicsTransactionState::Committed: return "Committed";
		case PhysicsTransactionState::RolledBack: return "RolledBack";
		case PhysicsTransactionState::Cancelled: return "Cancelled";
		case PhysicsTransactionState::Conflicted: return "Conflicted";
		}
		return "Unknown";
	}
}
