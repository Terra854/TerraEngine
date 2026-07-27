// AnimationAutomationTransactions.cpp - validates before retaining a receipt.

#include "AnimationAutomationTransactions.h"

#include <exception>

namespace TerraEngine::Editor::Automation
{
	AutomationDispatchResult AnimationAutomationTransactions::Execute(
		const AnimationAutomationMutation& request,
		std::uint64_t currentGeneration)
	{
		return ExecuteWithCommit(request, currentGeneration, []() {
			AutomationDispatchResult committed;
			committed.ok = true;
			committed.message = "Published the animation mutation receipt.";
			return committed;
		});
	}

	AutomationDispatchResult AnimationAutomationTransactions::ExecuteWithCommit(
		const AnimationAutomationMutation& request,
		std::uint64_t currentGeneration,
		const std::function<AutomationDispatchResult()>& commit)
	{
		AutomationDispatchResult result;
		if(request.operation.empty() || request.idempotencyKey.empty()
			|| request.expectedGeneration == 0U || !commit)
		{
			result.error = ErrorCategory::Validation;
			result.message = "A mutation needs an operation, idempotency key, expected generation, and commit callback.";
			return result;
		}

		std::string fingerprint;
		try
		{
			fingerprint = BuildFingerprint(request);
		}
		catch(const std::exception& exception)
		{
			result.error = ErrorCategory::Internal;
			result.message = std::string("Could not fingerprint the bounded animation mutation: ") + exception.what();
			return result;
		}
		std::scoped_lock lock(mutex_);
		if(const auto retained = receipts_.find(request.idempotencyKey); retained != receipts_.end())
		{
			if(retained->second.fingerprint != fingerprint)
			{
				result.error = ErrorCategory::Conflict;
				result.message = "The animation idempotency key was already used for different work.";
				return result;
			}
			result = retained->second.result;
			result.data["idempotencyDisposition"] = "replay";
			return result;
		}
		if(receipts_.size() >= kCapacity)
		{
			result.error = ErrorCategory::QueueFull;
			result.message = "Animation mutation receipt retention is full.";
			return result;
		}
		if(request.expectedGeneration != currentGeneration)
		{
			result.error = ErrorCategory::StaleTarget;
			result.message = "The animation generation changed; no mutation was published.";
			return result;
		}
		// Allocate the durable map node before invoking the typed commit. Inserting
		// this extracted node after success performs no map allocation, so an out-of-
		// memory failure cannot queue work and then lose its at-most-once receipt.
		std::map<std::string, Receipt> stagedReceipt;
		try
		{
			stagedReceipt.emplace(request.idempotencyKey, Receipt{fingerprint, {}});
		}
		catch(const std::exception& exception)
		{
			result.error = ErrorCategory::Internal;
			result.message = std::string("Could not reserve the animation mutation receipt: ") + exception.what();
			return result;
		}
		auto receiptNode = stagedReceipt.extract(stagedReceipt.begin());

		try
		{
			result = commit();
		}
		catch(const std::exception& exception)
		{
			result.error = ErrorCategory::Internal;
			result.message = std::string("The animation mutation commit failed: ") + exception.what();
			return result;
		}
		catch(...)
		{
			result.error = ErrorCategory::Internal;
			result.message = "The animation mutation commit failed with an unknown exception.";
			return result;
		}
		if(!result.ok)
		{
			// No receipt is retained for rejection. The owning typed service is
			// responsible for making its callback atomic before reporting failure.
			return result;
		}
		if(!result.data.is_object())
		{
			result.data = nlohmann::json::object();
		}
		result.data["correlationId"] = request.correlationId;
		result.data["generation"] = request.dryRun ? currentGeneration : currentGeneration + 1U;
		result.data["dryRun"] = request.dryRun;
		result.data["idempotencyDisposition"] = "new";
		receiptNode.mapped().result = std::move(result);
		const auto inserted = receipts_.insert(std::move(receiptNode));
		return inserted.position->second.result;
	}

	std::size_t AnimationAutomationTransactions::RetainedCount() const noexcept
	{
		std::scoped_lock lock(mutex_);
		return receipts_.size();
	}

	void AnimationAutomationTransactions::Reset() noexcept
	{
		std::scoped_lock lock(mutex_);
		receipts_.clear();
	}

	std::string AnimationAutomationTransactions::BuildFingerprint(const AnimationAutomationMutation& request)
	{
		// nlohmann::json objects use ordered keys by default, so dump() produces a
		// stable representation for equivalent bounded automation payloads. A
		// correlation ID identifies one transport attempt, so it must not make an
		// otherwise identical retry look like different work.
		nlohmann::json semanticPayload = request.payload;
		if (semanticPayload.is_object())
		{
			semanticPayload.erase("correlationId");
		}
		return request.operation
			+ "\n" + std::to_string(request.expectedGeneration)
			+ "\n" + (request.dryRun ? "dry-run" : "publish")
			+ "\n" + semanticPayload.dump();
	}
}
