// PhysicsTransactions.h - coordinated protected scene and physics commits.
//
// Worker jobs may prepare these provider-neutral values, but the final commit
// must run at the scheduler's protected PhysicsCommandCommit boundary. This is
// what lets one decision publish both authored scene data and physics state.

#pragma once

#include "Core/Export.h"
#include "PhysicsCommands.h"
#include "Scene/TransactionalMutation.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

namespace TerraEngine::Scene {
	// Capture the exact scene generations used by a coordinated transaction.
	// Keeping this helper beside the physics protocol avoids worker code reading
	// live ObjectManager state while it prepares a mixed write batch.
	TERRAENGINE_API TransactionReadSet CaptureTransactionReadSet(
		ObjectManager& objects,
		std::initializer_list<TerraEngine::UUID> objectIds);
}

namespace TerraEngine::Physics {
	enum class PhysicsTransactionState : std::uint8_t {
		Queued,
		Preparing,
		PreflightFailed,
		Prepared,
		Committing,
		Committed,
		RolledBack,
		Cancelled,
		Conflicted
	};

	struct PhysicsTransactionReadSet {
		PhysicsWorldId worldId{1U};
		PhysicsGeneration worldGeneration = 0;
		std::unordered_map<TerraEngine::UUID, PhysicsGeneration> bodyGenerations;
		std::unordered_map<TerraEngine::UUID, PhysicsGeneration> constraintGenerations;
		std::unordered_map<TerraEngine::UUID, PhysicsGeneration> characterGenerations;
	};

	// This provider-neutral registry is written only while the protected commit
	// boundary is held. Worker code receives copies/snapshots instead of mutating
	// it, so the hot path needs no second lock or private worker pool.
	struct PhysicsTransactionRuntimeState {
		PhysicsWorldHandle world;
		std::unordered_map<TerraEngine::UUID, PhysicsGeneration> bodyGenerations;
		std::unordered_map<TerraEngine::UUID, PhysicsGeneration> constraintGenerations;
		std::unordered_map<TerraEngine::UUID, PhysicsGeneration> characterGenerations;
		std::uint64_t scenePublicationGeneration = 0;
		std::uint64_t physicsPublicationGeneration = 0;
		bool providerHealthy = true;
	};

	struct PhysicsTransaction {
		explicit PhysicsTransaction(std::string transactionId = {});

		std::string id;
		std::string correlationId;
		std::string approvalFingerprint;
		TerraEngine::Scene::TransactionReadSet sceneReadSet;
		PhysicsTransactionReadSet physicsReadSet;
		std::vector<TerraEngine::Scene::TransactionWriteOperation> sceneWrites;
		std::vector<PhysicsCommand> physicsCommands;
		bool cancelled = false;
	};

	enum class PhysicsProviderRecordDomain : std::uint8_t {
		None,
		Body,
		Constraint,
		Character
	};

	enum class PhysicsProviderRecordOperation : std::uint8_t {
		None,
		SetGeneration,
		Erase
	};

	struct StagedPhysicsProviderObject {
		PhysicsProviderRecordDomain domain = PhysicsProviderRecordDomain::None;
		PhysicsProviderRecordOperation operation = PhysicsProviderRecordOperation::None;
		PhysicsCommandType commandType = PhysicsCommandType::ApplyForce;
		TerraEngine::UUID identity{};
		PhysicsGeneration priorGeneration = 0;
		PhysicsGeneration resultingGeneration = 0;
		std::size_t commandIndex = 0;
	};

	struct PhysicsProviderInverseRecord {
		PhysicsProviderRecordDomain domain = PhysicsProviderRecordDomain::None;
		PhysicsProviderRecordOperation operation = PhysicsProviderRecordOperation::None;
		TerraEngine::UUID identity{};
		PhysicsGeneration restoredGeneration = 0;
		std::size_t commandIndex = 0;
	};

	struct PhysicsTransactionResult {
		bool accepted = false;
		bool mutatedScene = false;
		bool mutatedPhysics = false;
		bool providerWorldUnchanged = true;
		bool sceneGenerationPublished = false;
		bool physicsGenerationPublished = false;
		PhysicsTransactionState state = PhysicsTransactionState::Queued;
		std::size_t publishedWriteCount = 0;
		std::vector<StagedPhysicsProviderObject> providerObjects;
		std::vector<PhysicsProviderInverseRecord> inverseRecords;
		TerraEngine::Scene::TransactionResult sceneResult;
		std::string message;
	};

	TERRAENGINE_API PhysicsTransactionResult PreflightPhysicsTransaction(
		ObjectManager& objects,
		const PhysicsTransactionRuntimeState& runtime,
		const PhysicsTransaction& transaction);
	TERRAENGINE_API PhysicsTransactionResult StagePhysicsTransaction(
		const PhysicsTransactionRuntimeState& runtime,
		const PhysicsTransaction& transaction);
	TERRAENGINE_API PhysicsTransactionResult CommitPhysicsTransaction(
		ObjectManager& objects,
		PhysicsTransactionRuntimeState& runtime,
		const PhysicsTransaction& transaction);
	TERRAENGINE_API std::string_view ToString(PhysicsTransactionState state);
}
