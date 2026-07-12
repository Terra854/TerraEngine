// EditorAutomationTransaction.h - Typed transaction request preflight.

#pragma once

#include "EditorAutomationTypes.h"
#include "Scene/TransactionalMutation.h"

#include <optional>
#include <unordered_map>

class ObjectManager;

namespace TerraEngine::Editor::Automation
{
	struct AutomationTransactionLimits
	{
		std::size_t maximumOperations = 4096u;
		std::size_t maximumPayloadBytes = 1024u * 1024u;
	};

	struct AutomationTransactionPreflight
	{
		bool valid = false;
		ErrorCategory error = ErrorCategory::Validation;
		std::string message;
		std::size_t operationCount = 0u;
	};

	// Protected lowering resolves stable object identities and converts JSON only
	// into typed engine operations. The selection tail remains separate because
	// it belongs to editor state and publishes only after the scene commit succeeds.
	struct PreparedAutomationTransaction
	{
		bool valid = false;
		ErrorCategory error = ErrorCategory::Validation;
		std::string message;
		TerraEngine::Scene::Transaction transaction;
		std::optional<TerraEngine::Scene::TransactionObjectRef> selectionTarget;
		std::unordered_map<std::string, std::int32_t> localObjectIds;
	};

	// This worker-safe check reads only the immutable JSON request. Object and
	// scene generations are revalidated later at the protected editor boundary.
	[[nodiscard]] AutomationTransactionPreflight PreflightAutomationTransaction(
		const nlohmann::json& arguments,
		const AutomationTransactionLimits& limits = {});

	[[nodiscard]] PreparedAutomationTransaction PrepareAutomationTransaction(
		const nlohmann::json& arguments,
		ObjectManager& objects,
		std::string transactionId);
}
