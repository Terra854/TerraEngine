// SceneMutationQueue.cpp - validates and applies queued scene mutations.

#include "SceneMutationQueue.h"

#include "Animation/Runtime/RootMotion.h"

#include "Core/Object/Object.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Object/Component.h"
#include "Components/Transform.h"
#include "Components/ScriptComponent.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <limits>
#include <exception>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace TerraEngine::Scene {
	namespace {
		constexpr std::size_t kMaximumPendingNavigationSourceMutationIntents = 16384U;

		TransactionResult CanceledExecutionResult(const Transaction& transaction) {
			TransactionResult result;
			result.transactionId = transaction.id;
			result.status = TransactionStatus::Canceled;
			result.rejectedWriteCount = LogicalWriteCount(transaction);
			result.message = "Transaction canceled because required-frame execution failed.";
			return result;
		}

		// A spatial envelope participates in its cohort's atomic decision, but its
		// producer owns only its original operation span. Rebase inverse/conflict
		// indices so replay never includes another producer's authored writes.
		TransactionResult ProjectSpatialReceipt(const TransactionResult& aggregate, const Transaction& cohort,
			const SceneMutationResult& span, const Transaction& producer) {
			TransactionResult result;
			result.transactionId = producer.id; result.status = aggregate.status;
			result.validationCostNs = aggregate.validationCostNs; result.commitCostNs = aggregate.commitCostNs;
			result.message = aggregate.message; result.spatialPublicationGeneration = aggregate.spatialPublicationGeneration;
			const auto end = span.nativeOperationBegin + span.nativeOperationCount;
			const auto owns = [&](std::size_t index) { return index >= span.nativeOperationBegin && index < end; };
			if (result.status == TransactionStatus::Committed) result.appliedWriteCount = LogicalWriteCount(producer);
			else result.rejectedWriteCount = LogicalWriteCount(producer);
			for (const auto& source : aggregate.conflicts) if (owns(source.operationIndex)) {
				auto conflict = source; conflict.operationIndex -= span.nativeOperationBegin; result.conflicts.push_back(std::move(conflict));
			}
			for (const auto& source : aggregate.inverseDeltas) if (owns(source.sourceOperationIndex)) {
				auto inverse = source; inverse.sourceOperationIndex -= span.nativeOperationBegin; result.inverseDeltas.push_back(std::move(inverse));
			}
			std::size_t animationOffset = 0U;
			for (std::size_t index = 0U; index < cohort.writeSet.operations.size(); ++index) {
				const auto& operation = cohort.writeSet.operations[index];
				if (owns(index) && operation.kind == TransactionWriteKind::CreateObject) {
					const auto created = aggregate.createdObjects.find(operation.createdLocalObjectId);
					if (created != aggregate.createdObjects.end()) result.createdObjects.insert(*created);
				}
				if (operation.kind != TransactionWriteKind::PublishAnimationMutations) continue;
				if (owns(index) && result.status == TransactionStatus::Committed) {
					const auto animationEnd = animationOffset + operation.animationPublications.size();
					if (animationEnd <= aggregate.acceptedAnimationPublications.size())
						result.acceptedAnimationPublications.insert(result.acceptedAnimationPublications.end(),
							aggregate.acceptedAnimationPublications.begin() + animationOffset,
							aggregate.acceptedAnimationPublications.begin() + animationEnd);
				}
				animationOffset += operation.animationPublications.size();
			}
			return result;
		}

		// Producer receipts are terminal even when a committed observer stops the
		// frame. This also removes matching intents so a later flush cannot replay.
		void SettleProducerReceipts(const TransactionResult& result,
			std::vector<NavigationSourceMutationIntent>& intents,
			std::vector<NavigationSourceMutationReceipt>& completed, std::uint64_t& sequence) {
			TerraEngine::Animation::AnimationRootMotionResultPublisher::Instance().ObserveSceneTransactionResult(result);
			for (auto intent = intents.begin(); intent != intents.end();) {
				if (intent->transactionId != result.transactionId) { ++intent; continue; }
				NavigationSourceMutationReceipt receipt;
				receipt.intent = std::move(*intent);
				receipt.transactionStatus = result.status;
				receipt.accepted = result.status == TransactionStatus::Committed;
				if (receipt.accepted && sequence != 0U) receipt.intent.committedChangeSequence = sequence++;
				else if (receipt.accepted) { receipt.accepted = false; receipt.transactionStatus = TransactionStatus::Rejected; }
				receipt.message = receipt.accepted ? "Navigation source change committed with its scene transaction."
					: "Navigation source change canceled with required-frame execution.";
				completed.push_back(std::move(receipt));
				intent = intents.erase(intent);
			}
		}
		[[nodiscard]] bool NormalizeNavigationMutationBounds(NavigationSourceMutationBounds& bounds)
		{
			if(!bounds.valid) return true;
			const WorldCoordinateResult minimum = NormalizeWorldPosition(bounds.minimum);
			const WorldCoordinateResult maximum = NormalizeWorldPosition(bounds.maximum);
			if(!minimum.Succeeded() || !maximum.Succeeded()) return false;
			bounds.minimum = minimum.position;
			bounds.maximum = maximum.position;
			const auto ordered = [](std::int64_t minimumCell, double minimumOffset,
				std::int64_t maximumCell, double maximumOffset) {
				return minimumCell < maximumCell
					|| (minimumCell == maximumCell && minimumOffset <= maximumOffset);
			};
			return ordered(bounds.minimum.cell.x, bounds.minimum.offsetMetres.x,
				bounds.maximum.cell.x, bounds.maximum.offsetMetres.x)
				&& ordered(bounds.minimum.cell.y, bounds.minimum.offsetMetres.y,
					bounds.maximum.cell.y, bounds.maximum.offsetMetres.y)
				&& ordered(bounds.minimum.cell.z, bounds.minimum.offsetMetres.z,
					bounds.maximum.cell.z, bounds.maximum.offsetMetres.z);
		}

		SceneDiagnostic MakeMutationDiagnostic(SceneDiagnosticSeverity severity, const SceneMutationRequest& request, std::string message, std::string action)
		{
			return SceneDiagnostic{
				severity,
				SceneDiagnosticCategory::Mutation,
				request.targetObjectId,
				request.requestId,
				std::move(message),
				std::move(action)
			};
		}

		bool NeedsExistingTarget(SceneMutationOperation operation)
		{
			switch (operation)
			{
			case SceneMutationOperation::DeleteObject:
			case SceneMutationOperation::ReparentObject:
			case SceneMutationOperation::SetObjectName:
			case SceneMutationOperation::SetObjectTag:
			case SceneMutationOperation::SetObjectLayer:
			case SceneMutationOperation::SetObjectActive:
			case SceneMutationOperation::SetObjectStatic:
			case SceneMutationOperation::AddComponent:
			case SceneMutationOperation::RemoveComponent:
			case SceneMutationOperation::UpdateComponent:
			case SceneMutationOperation::AddScriptBinding:
			case SceneMutationOperation::ApplyPrefabDefaults:
			case SceneMutationOperation::ResetPrefabDefaults:
				return true;
			default:
				return false;
			}
		}

		bool IsSpatialMutation(SceneMutationOperation operation)
		{
			return operation == SceneMutationOperation::SetWorldTransform
				|| operation == SceneMutationOperation::TranslateWorld
				|| operation == SceneMutationOperation::ChangeSimulationFrame;
		}

		bool FiniteMutationValue(const Reflection::Variant& value) {
			if (Reflection::IsType<float>(value)) return std::isfinite(Reflection::GetValue<float>(value));
			if (Reflection::IsType<double>(value)) return std::isfinite(Reflection::GetValue<double>(value));
			if (Reflection::IsType<glm::vec2>(value)) { const auto v = Reflection::GetValue<glm::vec2>(value); return std::isfinite(v.x) && std::isfinite(v.y); }
			if (Reflection::IsType<glm::vec3>(value)) { const auto v = Reflection::GetValue<glm::vec3>(value); return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
			if (Reflection::IsType<glm::vec4>(value)) { const auto v = Reflection::GetValue<glm::vec4>(value); return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w); }
			if (Reflection::IsType<glm::quat>(value)) { const auto v = Reflection::GetValue<glm::quat>(value); return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w); }
			return Reflection::IsValid(value);
		}

		bool MutationKindMatches(const Reflection::Variant& value, int kind) {
			// Typed string/UUID ingress has no numeric wire-kind field. Its native
			// reflection value already carries an exact type, so -1 remains valid.
			switch (kind) {
			case -1: return Reflection::IsValid(value);
			case 0: return Reflection::IsType<bool>(value);
			case 1: return Reflection::IsType<int>(value);
			case 2: return Reflection::IsType<float>(value);
			case 3: return Reflection::IsType<std::uint32_t>(value);
			case 4: return Reflection::IsType<glm::vec2>(value);
			case 5: return Reflection::IsType<glm::vec3>(value);
			case 6: return Reflection::IsType<glm::vec4>(value);
			case 7: return Reflection::IsType<glm::quat>(value);
			default: return false;
			}
		}

		Reflection::Variant ReadMutationWireValue(const nlohmann::json& value, int kind) {
			// Validate complete wire shape before conversion. Short vectors, missing
			// fields and numeric strings never receive invented default values.
			if (kind == 0 && value.is_boolean()) return value.get<bool>();
			if (kind == 1 && value.is_number_integer() && value.get<double>() >= std::numeric_limits<int>::min()
				&& value.get<double>() <= std::numeric_limits<int>::max()) return value.get<int>();
			if (kind == 2 && value.is_number()) { const auto number = value.get<float>(); if (std::isfinite(number)) return number; }
			if (kind == 3 && value.is_number_integer() && value.get<double>() >= 0.0
				&& value.get<double>() <= std::numeric_limits<std::uint32_t>::max()) return value.get<std::uint32_t>();
			const std::size_t length = kind == 4 ? 2U : kind == 5 ? 3U : kind == 6 || kind == 7 ? 4U : 0U;
			if (length == 0U || !value.is_array() || value.size() != length) return {};
			for (const auto& number : value) if (!number.is_number() || !std::isfinite(number.get<float>())) return {};
			if (kind == 4) return glm::vec2(value[0].get<float>(), value[1].get<float>());
			if (kind == 5) return glm::vec3(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
			if (kind == 6) return glm::vec4(value[0].get<float>(), value[1].get<float>(), value[2].get<float>(), value[3].get<float>());
			return glm::quat(value[0].get<float>(), value[1].get<float>(), value[2].get<float>(), value[3].get<float>());
		}

		bool PrepareMutationProperty(const SceneMutationRequest& request, TransactionWriteOperation& operation, std::string& reason) {
			operation.kind = TransactionWriteKind::UpdateComponentProperty;
			if (request.typedComponentPayload.valid) {
				operation.propertyName = request.typedComponentPayload.propertyName;
				operation.propertyValue = request.typedComponentPayload.value;
				if (!MutationKindMatches(operation.propertyValue, request.typedComponentPayload.valueKind)) { reason = "Typed property value does not match its declared value kind."; return false; }
			} else {
				const auto payload = nlohmann::json::parse(request.payloadJson);
				if (!payload.is_object() || !payload.contains("propertyName") || !payload["propertyName"].is_string()
					|| !payload.contains("valueKind") || !payload["valueKind"].is_number_integer() || !payload.contains("value")) {
					reason = "Component property requires propertyName, integer valueKind and value."; return false;
				}
				operation.propertyName = payload["propertyName"].get<std::string>();
				operation.propertyValue = ReadMutationWireValue(payload["value"], payload["valueKind"].get<int>());
			}
			if (operation.propertyName.empty() || !Reflection::IsValid(operation.propertyValue) || !FiniteMutationValue(operation.propertyValue)) {
				reason = "Component property name, value kind or numeric value is invalid."; return false;
			}
			return true;
		}

		std::string FormatCommitSummary(const SceneMutationBatchResult& batch)
		{
			std::ostringstream stream;
			stream << "Scene mutation commit '" << batch.commitName << "':"
				<< " applied=" << (batch.applied ? "true" : "false")
				<< " input=" << batch.inputMutationCount
				<< " accepted=" << batch.acceptedMutationCount
				<< " rejected=" << batch.rejectedMutationCount
				<< " typed=" << batch.typedMutationCount
				<< " jsonFallbacks=" << batch.jsonMutationFallbackCount
				<< " coalesced=" << batch.coalescedMutationCount;
			return stream.str();
		}
	}

	SceneMutationQueue& SceneMutationQueue::Instance()
	{
		static SceneMutationQueue queue;
		return queue;
	}

	void SceneMutationQueue::Enqueue(SceneMutationRequest request)
	{
		std::scoped_lock lock(requestsMutex);
		if (request.requestId.empty())
		{
			request.requestId = "mutation-" + std::to_string(requests.size());
		}
		requests.push_back(std::move(request));
	}

	void SceneMutationQueue::EnqueueFromWorker(std::uint32_t workerIndex, SceneMutationRequest request)
	{
		std::scoped_lock lock(requestsMutex);
		if (request.requestId.empty())
		{
			request.requestId = "worker-" + std::to_string(workerIndex) + "-mutation-" + std::to_string(workerBuffers.size());
		}
		auto buffer = std::find_if(workerBuffers.begin(), workerBuffers.end(), [workerIndex](const SceneMutationWorkerBuffer& candidate) {
			return candidate.workerIndex == workerIndex;
		});
		if (buffer == workerBuffers.end())
		{
			SceneMutationWorkerBuffer newBuffer;
			newBuffer.workerIndex = workerIndex;
			newBuffer.requests.push_back(std::move(request));
			workerBuffers.push_back(std::move(newBuffer));
			return;
		}
		buffer->requests.push_back(std::move(request));
	}

	void SceneMutationQueue::EnqueueTransaction(Transaction transaction)
	{
		std::scoped_lock lock(requestsMutex);
		if (transaction.id.empty())
		{
			transaction.id = "transaction-" + std::to_string(transactions.size());
		}
		transactions.push_back(std::move(transaction));
	}

	void SceneMutationQueue::EnqueuePublicationGroup(TransactionPublicationGroup group)
	{
		std::scoped_lock lock(requestsMutex);
		if (group.groupId.empty())
		{
			group.groupId = "publication-group-" + std::to_string(publicationGroups.size());
		}
		publicationGroups.push_back(std::move(group));
	}

	bool SceneMutationQueue::EnqueueNavigationSourceMutationIntent(NavigationSourceMutationIntent intent)
	{
		std::scoped_lock lock(requestsMutex);
		using Operation = NavigationSourceMutationOperation;
		const bool needsOld = intent.operation != Operation::Create && intent.operation != Operation::Enable;
		const bool needsNew = intent.operation != Operation::Delete && intent.operation != Operation::Disable;
		const bool valid = !intent.intentId.empty() && !intent.transactionId.empty()
			&& intent.sourceId != TerraEngine::EMPTY_UUID && intent.committedSourceRevision != 0U
			&& intent.physicalReadyRevision != 0U && (!needsOld || intent.oldBounds.valid)
			&& (!needsNew || intent.newBounds.valid)
			&& NormalizeNavigationMutationBounds(intent.oldBounds)
			&& NormalizeNavigationMutationBounds(intent.newBounds);
		const bool hasCapacity = navigationSourceMutationIntents.size()
			< kMaximumPendingNavigationSourceMutationIntents;
		const bool duplicate = std::any_of(navigationSourceMutationIntents.begin(),
			navigationSourceMutationIntents.end(), [&intent](const NavigationSourceMutationIntent& pending) {
				return pending.intentId == intent.intentId;
			});
		if(valid && hasCapacity && !duplicate)
		{
			navigationSourceMutationIntents.push_back(std::move(intent));
			return true;
		}

		// Capacity and validation failures are observable. Silently discarding a
		// removal could leave navigation claiming removed support is traversable.
		NavigationSourceMutationReceipt receipt;
		receipt.intent = std::move(intent);
		receipt.transactionStatus = TransactionStatus::Rejected;
		receipt.accepted = false;
		receipt.message = !valid ? "Navigation source intent is missing stable identity or revision."
			: (!hasCapacity ? "Navigation source intent capacity is exhausted."
				: "Navigation source intent identity is already pending.");
		completedNavigationSourceMutationReceipts.push_back(std::move(receipt));
		return false;
	}

	std::uint64_t SceneMutationQueue::AllocateCommittedNavigationSourceChangeSequence()
	{
		std::scoped_lock lock(requestsMutex);
		// Zero is the invalid sentinel. Failing closed at wrap preserves ordering
		// rather than allowing a live navigation world to observe an aliased update.
		if(nextNavigationSourceChangeSequence == 0U) return 0U;
		return nextNavigationSourceChangeSequence++;
	}

	TransactionResult SceneMutationQueue::PreflightTransaction(const Transaction& transaction, ObjectManager& objects) const
	{
		return TerraEngine::Scene::PreflightTransaction(objects, transaction);
	}

	bool SceneMutationQueue::CancelTransaction(const std::string& transactionId)
	{
		std::scoped_lock lock(requestsMutex);
		for (Transaction& transaction : transactions)
		{
			if (transaction.id == transactionId)
			{
				transaction.canceled = true;
				return true;
			}
		}
		return false;
	}

	bool SceneMutationQueue::CancelPublicationGroup(const std::string& groupId)
	{
		std::scoped_lock lock(requestsMutex);
		for (TransactionPublicationGroup& group : publicationGroups)
		{
			if (group.groupId == groupId)
			{
				group.canceled = true;
				return true;
			}
		}
		return false;
	}

	bool SceneMutationQueue::Empty() const
	{
		std::scoped_lock lock(requestsMutex);
		return requests.empty() && transactions.empty() && publicationGroups.empty()
			&& navigationSourceMutationIntents.empty() && std::all_of(workerBuffers.begin(), workerBuffers.end(), [](const SceneMutationWorkerBuffer& buffer) {
			return buffer.requests.empty();
		});
	}

	std::size_t SceneMutationQueue::Size() const
	{
		std::scoped_lock lock(requestsMutex);
		std::size_t total = requests.size();
		for (const SceneMutationWorkerBuffer& buffer : workerBuffers)
		{
			total += buffer.requests.size();
		}
		total += transactions.size();
		total += publicationGroups.size();
		total += navigationSourceMutationIntents.size();
		return total;
	}

	std::vector<SceneMutationRequest> SceneMutationQueue::PendingRequests() const
	{
		return SnapshotRequests(true);
	}

	std::vector<SceneMutationRequest> SceneMutationQueue::SnapshotRequests(bool coalesce) const
	{
		// Return a snapshot instead of exposing the internal vector so scheduler
		// diagnostics and tests can inspect pending requests without mutating the
		// queue that frame systems are still relying on.
		std::scoped_lock lock(requestsMutex);
		std::vector<SceneMutationRequest> snapshot = requests;
		std::vector<SceneMutationWorkerBuffer> sortedBuffers = workerBuffers;
		std::stable_sort(sortedBuffers.begin(), sortedBuffers.end(), [](const SceneMutationWorkerBuffer& left, const SceneMutationWorkerBuffer& right) {
			return left.workerIndex < right.workerIndex;
		});
		for (SceneMutationWorkerBuffer& buffer : sortedBuffers)
		{
			std::stable_sort(buffer.requests.begin(), buffer.requests.end(), [](const SceneMutationRequest& left, const SceneMutationRequest& right) {
				if (left.sourceFrameOrder != right.sourceFrameOrder)
				{
					return left.sourceFrameOrder < right.sourceFrameOrder;
				}
				return left.requestId < right.requestId;
			});
			snapshot.insert(snapshot.end(), buffer.requests.begin(), buffer.requests.end());
		}
		std::sort(snapshot.begin(), snapshot.end(), [](const SceneMutationRequest& left, const SceneMutationRequest& right) {
			if (left.sourceFrameOrder != right.sourceFrameOrder)
			{
				return left.sourceFrameOrder < right.sourceFrameOrder;
			}
			return left.requestId < right.requestId;
		});

		if (!coalesce) return snapshot;
		std::vector<SceneMutationRequest> coalesced;
		coalesced.reserve(snapshot.size());
		std::unordered_map<std::string, std::size_t> coalescedIndex;
		for (SceneMutationRequest& request : snapshot)
		{
			if (request.coalescingKey.empty())
			{
				coalesced.push_back(std::move(request));
				continue;
			}

			if (auto existing = coalescedIndex.find(request.coalescingKey); existing != coalescedIndex.end())
			{
				coalesced[existing->second] = std::move(request);
				continue;
			}

			coalescedIndex.emplace(request.coalescingKey, coalesced.size());
			coalesced.push_back(std::move(request));
		}
		return coalesced;
	}

	std::vector<Transaction> SceneMutationQueue::PendingTransactions() const
	{
		std::scoped_lock lock(requestsMutex);
		return transactions;
	}

	std::vector<TransactionPublicationGroup> SceneMutationQueue::PendingPublicationGroups() const
	{
		std::scoped_lock lock(requestsMutex);
		return publicationGroups;
	}

	std::vector<TransactionResult> SceneMutationQueue::DrainCompletedEditorTransactionResults()
	{
		std::scoped_lock lock(requestsMutex);
		std::vector<TransactionResult> completed;
		completed.reserve(completedEditorTransactions.size());
		for (EditorTransactionCompletion& completion : completedEditorTransactions)
		{
			completed.push_back(std::move(completion.result));
		}
		completedEditorTransactions.clear();
		return completed;
	}

	std::vector<EditorTransactionCompletion> SceneMutationQueue::DrainCompletedEditorTransactions()
	{
		std::scoped_lock lock(requestsMutex);
		std::vector<EditorTransactionCompletion> completed;
		completed.swap(completedEditorTransactions);
		return completed;
	}

	std::optional<AutomationTransactionReceipt> SceneMutationQueue::TakeCompletedAutomationTransactionReceipt(std::string_view transactionId)
	{
		std::scoped_lock lock(requestsMutex);
		const auto found = std::find_if(completedAutomationTransactions.begin(), completedAutomationTransactions.end(),
			[transactionId](const auto& receipt) { return receipt.transactionId == transactionId; });
		if (found == completedAutomationTransactions.end()) return std::nullopt;
		auto receipt = std::move(*found);
		completedAutomationTransactions.erase(found);
		return receipt;
	}

	void SceneMutationQueue::RetainAutomationTransactionReceipt(const Transaction& transaction, const TransactionResult& result) noexcept
	{
		if (transaction.origin != TransactionOrigin::Automation || result.status == TransactionStatus::Pending) return;
		try
		{
			// Retain small observation receipts, never duplicate undo inverse deltas.
			// Eviction/allocation failure produces missing evidence, never fabricated
			// success, and must not change the outcome of an already committed scene.
			constexpr std::size_t maximumReceipts = 256U;
			AutomationTransactionReceipt receipt{result.transactionId, result.status,
				result.appliedWriteCount, result.rejectedWriteCount, result.spatialPublicationGeneration, result.message};
			if (completedAutomationTransactions.size() == maximumReceipts)
				completedAutomationTransactions.erase(completedAutomationTransactions.begin());
			completedAutomationTransactions.push_back(std::move(receipt));
		}
		catch (...) { /* Optional receipt loss cannot become an additional authored-write failure. */ }
	}

	std::vector<NavigationSourceMutationReceipt> SceneMutationQueue::DrainCompletedNavigationSourceMutationReceipts()
	{
		std::scoped_lock lock(requestsMutex);
		std::vector<NavigationSourceMutationReceipt> completed;
		completed.swap(completedNavigationSourceMutationReceipts);
		return completed;
	}

	TransactionDiagnostics SceneMutationQueue::GetTransactionDiagnostics() const
	{
		std::scoped_lock lock(requestsMutex);
		return transactionDiagnostics;
	}

	std::vector<TransactionPublicationGroupResult> SceneMutationQueue::TakeCompletedPublicationGroupResults() {
		std::scoped_lock lock(requestsMutex);
		std::vector<TransactionPublicationGroupResult> results;
		results.swap(completedPublicationGroupResults);
		return results;
	}


	void SceneMutationQueue::RestorePendingIntents(std::vector<NavigationSourceMutationIntent>& intents) {
		std::scoped_lock lock(requestsMutex);
		// Unknown transaction decisions retain their copied intents for a future
		// boundary; late ingress stays in the live collection throughout execution.
		// Reserve before the first move, so allocation failure leaves every captured
		// intent intact for the caller's terminal cleanup path.
		navigationSourceMutationIntents.reserve(navigationSourceMutationIntents.size() + intents.size());
		for (auto& intent : intents) navigationSourceMutationIntents.push_back(std::move(intent));
		intents.clear();
	}

	void SceneMutationQueue::CancelPreparedExecution(std::vector<Transaction>& transactions,
		std::vector<TransactionPublicationGroup>& publicationGroups, std::vector<NavigationSourceMutationIntent>& navigationSourceMutationIntents) {
		std::scoped_lock lock(requestsMutex);
		completedEditorTransactions.reserve(completedEditorTransactions.size() + transactions.size());
		completedPublicationGroupResults.reserve(completedPublicationGroupResults.size() + publicationGroups.size());
		completedNavigationSourceMutationReceipts.reserve(completedNavigationSourceMutationReceipts.size() + navigationSourceMutationIntents.size());
		for (auto& transaction : transactions) {
			auto result = CanceledExecutionResult(transaction);
			RetainAutomationTransactionReceipt(transaction, result);
			AccumulateTransactionDiagnostics(transactionDiagnostics, transaction, result);
			SettleProducerReceipts(result, navigationSourceMutationIntents, completedNavigationSourceMutationReceipts, nextNavigationSourceChangeSequence);
			if (transaction.origin == TransactionOrigin::Editor && (!transaction.undoGroupName.empty() || transaction.originName == "UndoRedoReplay"))
				completedEditorTransactions.push_back({std::move(transaction), std::move(result)});
		}
		transactions.clear();
		for (const auto& group : publicationGroups) {
			TransactionPublicationGroupResult result;
			result.groupId = group.groupId; result.status = TransactionStatus::Canceled;
			result.aggregateResult.transactionId = group.groupId; result.aggregateResult.status = result.status;
			result.message = "Publication group canceled because required-frame execution failed.";
			for (std::size_t index = 0; index < group.chunks.size(); ++index) {
				const auto canceled = CanceledExecutionResult(group.chunks[index]);
				AccumulateTransactionDiagnostics(transactionDiagnostics, group.chunks[index], canceled);
				SettleProducerReceipts(canceled, navigationSourceMutationIntents, completedNavigationSourceMutationReceipts, nextNavigationSourceChangeSequence);
				result.chunkReceipts.push_back({group.chunks[index].id, index, TransactionStatus::Canceled, canceled.rejectedWriteCount});
				result.rejectedWriteCount += canceled.rejectedWriteCount;
			}
			result.aggregateResult.rejectedWriteCount = result.rejectedWriteCount;
			completedPublicationGroupResults.push_back(std::move(result));
		}
		publicationGroups.clear();
		// Intents without a terminal transaction cannot remain alive after the
		// required frame has failed. Their scene transaction was never published.
		for (auto& intent : navigationSourceMutationIntents) {
			NavigationSourceMutationReceipt receipt;
			receipt.intent = std::move(intent); receipt.transactionStatus = TransactionStatus::Canceled;
			receipt.message = "Navigation source change canceled because required-frame execution failed.";
			completedNavigationSourceMutationReceipts.push_back(std::move(receipt));
		}
		navigationSourceMutationIntents.clear();
	}

	void SceneMutationQueue::Clear()
	{
		std::scoped_lock lock(requestsMutex);
		requests.clear();
		workerBuffers.clear();
		transactions.clear();
		publicationGroups.clear();
		navigationSourceMutationIntents.clear();
		completedEditorTransactions.clear();
		completedAutomationTransactions.clear();
		completedNavigationSourceMutationReceipts.clear();
		completedPublicationGroupResults.clear();
	}

	SceneMutationBatchResult SceneMutationQueue::Validate(const SceneGraph& graph) const
	{
		SceneMutationBatchResult batch;
		batch.applied = false;
		std::unordered_set<TerraEngine::UUID> deletedTargets;
		const std::size_t rawRequestCount = Size();
		const std::vector<SceneMutationRequest> requestSnapshot = PendingRequests();
		batch.inputMutationCount = requestSnapshot.size();
		batch.coalescedMutationCount = rawRequestCount > requestSnapshot.size() ? rawRequestCount - requestSnapshot.size() : 0;

		for (const SceneMutationRequest& request : requestSnapshot)
		{
			SceneMutationResult result;
			result.requestId = request.requestId;
			result.accepted = true;
			result.sourceObjectId = request.sourceObjectId;
			result.sourceObjectUuid = request.sourceObjectUuid;
			result.sourceBindingId = request.sourceBindingId;
			result.sourceFrameOrder = request.sourceFrameOrder;

			if (request.expectedGraphRevision != 0 && request.expectedGraphRevision != graph.revision)
			{
				result.accepted = false;
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request, "Mutation was captured against a stale scene graph revision.", "Refresh the request from the latest graph before applying it."));
			}

			if (NeedsExistingTarget(request.operation) && graph.objects.find(request.targetObjectId) == graph.objects.end())
			{
				result.accepted = false;
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request, "Mutation target no longer exists.", "Drop or rebuild the request from current selection state."));
			}

			// Deferred managed creation reserves its stable UUID before publication.
			// Reject collisions during preflight so the entire request batch remains
			// atomic and managed receipts never resolve to an unrelated live object.
			if (request.operation == SceneMutationOperation::CreateObject
				&& request.targetObjectId != TerraEngine::EMPTY_UUID
				&& graph.objects.find(request.targetObjectId) != graph.objects.end())
			{
				result.accepted = false;
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request, "Create mutation reserved an existing object UUID.", "Generate a fresh stable UUID before retrying the request."));
			}

			if (deletedTargets.contains(request.targetObjectId))
			{
				result.accepted = false;
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request, "Mutation targets an object already deleted earlier in the batch.", "Split the batch or remove the later request."));
			}

			if (request.operation == SceneMutationOperation::ReparentObject && request.parentObjectId == request.targetObjectId)
			{
				result.accepted = false;
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request, "Object cannot be parented to itself.", "Choose a different parent or detach to root."));
			}

			if ((request.operation == SceneMutationOperation::AddComponent
				|| request.operation == SceneMutationOperation::RemoveComponent
				|| request.operation == SceneMutationOperation::UpdateComponent)
				&& request.componentName.empty())
			{
				result.accepted = false;
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request, "Component mutation is missing a component name.", "Set componentName so the owning system can apply the payload deterministically."));
			}

			if (IsSpatialMutation(request.operation) && !request.typedSpatialTransaction)
			{
				result.accepted = false;
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request,
					"Spatial mutation ingress must be lowered to a typed transaction before it reaches the queue.",
					"Build SetWorldTransform, TranslateWorld, ReparentObject, or ChangeSimulationFrame through TransactionalMutationBuilder."));
			}

			if (request.operation == SceneMutationOperation::UpdateComponent
				&& request.componentName == Transform::kComponentName)
			{
				result.accepted = false;
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request,
					"Generic reflected Transform mutations are not a spatial authority.",
					"Lower canonical movement to a typed world-spatial transaction instead of supplying a Transform property payload."));
			}

			if (request.operation == SceneMutationOperation::AddScriptBinding && request.metadataValue.empty())
			{
				result.accepted = false;
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request, "Script binding mutation is missing a managed class name.", "Provide the fully qualified managed script type name."));
			}

			if (request.operation == SceneMutationOperation::DeleteObject && result.accepted)
			{
				deletedTargets.insert(request.targetObjectId);
			}

			if (!result.accepted)
			{
				++batch.rejectedMutationCount;
				batch.diagnostics.insert(batch.diagnostics.end(), result.diagnostics.begin(), result.diagnostics.end());
			}
			else
			{
				++batch.acceptedMutationCount;
			}
			batch.results.push_back(std::move(result));
		}

		batch.formattedCommitSummary = FormatCommitSummary(batch);
		return batch;
	}

	SceneMutationBatchResult SceneMutationQueue::Apply(ObjectManager& objects)
	{
		return Apply(objects, "SceneMutationQueue.Apply");
	}

	SceneMutationBatchResult SceneMutationQueue::Apply(ObjectManager& objects, std::string commitName)
	{
		// Detach every producer channel at one protected boundary. Preparation and
		// callbacks may enqueue new work; it stays invisible until the next flush.
		SceneMutationQueue requestCohort;
		std::vector<Transaction> capturedTransactions;
		std::vector<TransactionPublicationGroup> capturedGroups;
		std::vector<NavigationSourceMutationIntent> capturedIntents;
		SceneMutationBatchResult batch;
		try {
			{
				std::scoped_lock lock(requestsMutex);
				requestCohort.requests.swap(requests);
				requestCohort.workerBuffers.swap(workerBuffers);
				capturedTransactions.swap(transactions); capturedGroups.swap(publicationGroups);
				capturedIntents.swap(navigationSourceMutationIntents);
				completedNavigationSourceMutationReceipts.reserve(completedNavigationSourceMutationReceipts.size() + capturedIntents.size());
			}
			const auto rawSnapshot = requestCohort.SnapshotRequests(false);
			std::vector<SceneMutationRequest> requestSnapshot;
			std::vector<SceneMutationRequest> superseded;
			std::unordered_map<std::string, std::size_t> selectedKeys;
			for (const auto& request : rawSnapshot) {
				if (!request.coalescingKey.empty()) {
					const auto found = selectedKeys.find(request.coalescingKey);
					if (found != selectedKeys.end()) { superseded.push_back(std::move(requestSnapshot[found->second])); requestSnapshot[found->second] = request; continue; }
					selectedKeys.emplace(request.coalescingKey, requestSnapshot.size());
				}
				requestSnapshot.push_back(request);
			}
			batch.commitName = commitName.empty() ? "SceneMutationQueue.Apply" : std::move(commitName);
			batch.inputMutationCount = rawSnapshot.size();
			const auto rawRequests = rawSnapshot.size();
			batch.coalescedMutationCount = rawRequests > requestSnapshot.size() ? rawRequests - requestSnapshot.size() : 0U;
			Transaction generic;
			generic.id = "generic-cohort:" + batch.commitName + (requestSnapshot.empty() ? std::string{} : ":" + requestSnapshot.front().requestId);
			generic.origin = TransactionOrigin::ManagedCallback;
			generic.originName = batch.commitName;
			generic.readSet.sceneGeneration = objects.GetSceneGeneration();
			generic.readSet.sceneRevision = objects.GetHierarchyRevision();
			batch.results.reserve(rawSnapshot.size());
			std::vector<bool> informational(requestSnapshot.size(), false);
			std::vector<std::int32_t> creationLocals(requestSnapshot.size(), 0);
			std::unordered_map<TerraEngine::UUID, std::int32_t> reservedCreates;
			auto requestRef = [&](TerraEngine::UUID id) {
				const auto local = reservedCreates.find(id);
				return local == reservedCreates.end() ? ExistingObject(id) : LocalObject(local->second);
			};
			bool lowered = true;
			for (std::size_t index = 0; index < requestSnapshot.size(); ++index) {
				const auto& request = requestSnapshot[index];
				SceneMutationResult receipt;
				receipt.requestId = request.requestId; receipt.sourceObjectId = request.sourceObjectId;
				receipt.sourceObjectUuid = request.sourceObjectUuid; receipt.sourceBindingId = request.sourceBindingId;
				receipt.sourceFrameOrder = request.sourceFrameOrder;
				receipt.nativeOperationBegin = generic.writeSet.operations.size();
				TransactionWriteOperation op;
				op.target = requestRef(request.targetObjectId); op.parent = requestRef(request.parentObjectId);
				op.componentName = request.componentName; op.stringValue = request.metadataValue; op.boolValue = request.metadataBooleanValue;
				std::string reason;
				try {
					if (request.expectedGraphRevision != 0U && request.expectedGraphRevision != generic.readSet.sceneRevision)
						reason = "Mutation was captured against a stale scene graph revision.";
					else if (IsSpatialMutation(request.operation)) {
						if (!request.typedSpatialTransaction) reason = "Spatial ingress requires a lowered native transaction.";
						else {
							const auto preflight = TerraEngine::Scene::PreflightTransaction(objects, *request.typedSpatialTransaction);
							if (preflight.status != TransactionStatus::PreflightAccepted) reason = preflight.message.empty()
								? "Typed spatial envelope was rejected before generic cohort publication." : preflight.message;
							else {
								generic.readSet.objects.insert(generic.readSet.objects.end(), request.typedSpatialTransaction->readSet.objects.begin(), request.typedSpatialTransaction->readSet.objects.end());
								generic.writeSet.operations.insert(generic.writeSet.operations.end(), request.typedSpatialTransaction->writeSet.operations.begin(), request.typedSpatialTransaction->writeSet.operations.end());
							}
						}
					} else {
						switch (request.operation) {
						case SceneMutationOperation::CreateObject:
							op.kind = TransactionWriteKind::CreateObject;
							op.createdLocalObjectId = static_cast<std::int32_t>(index + 1U);
							op.createdStableObjectId = request.targetObjectId; op.displayName = request.displayName;
							creationLocals[index] = op.createdLocalObjectId;
							if (request.targetObjectId != TerraEngine::EMPTY_UUID) reservedCreates[request.targetObjectId] = op.createdLocalObjectId;
							break;
						case SceneMutationOperation::DeleteObject: op.kind = TransactionWriteKind::DeleteObject; break;
						case SceneMutationOperation::ReparentObject: op.kind = TransactionWriteKind::ReparentObject; break;
						case SceneMutationOperation::SetObjectName: op.kind = TransactionWriteKind::SetName; break;
						case SceneMutationOperation::SetObjectTag: op.kind = TransactionWriteKind::SetTag; break;
						case SceneMutationOperation::SetObjectLayer: op.kind = TransactionWriteKind::SetLayer; break;
						case SceneMutationOperation::SetObjectActive: op.kind = TransactionWriteKind::SetActive; break;
						case SceneMutationOperation::SetObjectStatic: op.kind = TransactionWriteKind::SetStatic; break;
						case SceneMutationOperation::AddComponent: op.kind = TransactionWriteKind::AddComponent; break;
						case SceneMutationOperation::RemoveComponent: op.kind = TransactionWriteKind::RemoveComponent; break;
						case SceneMutationOperation::UpdateComponent:
							if (request.componentName == Transform::kComponentName) reason = "Generic Transform property edits are not spatial authority.";
							else (void)PrepareMutationProperty(request, op, reason);
							if (request.typedComponentPayload.valid) ++batch.typedMutationCount;
							else ++batch.jsonMutationFallbackCount;
							break;
						case SceneMutationOperation::AddScriptBinding:
							op.kind = TransactionWriteKind::AddScriptBinding;
							if (op.stringValue.empty()) reason = "Script binding requires a nonempty managed class name.";
							break;
						default:
							// These existing envelopes have no authored side effect. Keep
							// the informational receipt without claiming an applied write.
							informational[index] = true;
							receipt.accepted = true;
							receipt.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Info, request,
								"Mutation operation has no direct ObjectManager side effect.", "Its owning prefab system handles the operation at its approved boundary."));
							break;
						}
						if (reason.empty() && !informational[index]) generic.writeSet.operations.push_back(std::move(op));
					}
				} catch (const std::exception& error) { reason = error.what(); }
				catch (...) { reason = "Generic authored preparation threw an unknown exception."; }
				receipt.nativeOperationCount = generic.writeSet.operations.size() - receipt.nativeOperationBegin;
				if (!reason.empty()) {
					lowered = false;
					receipt.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request, reason,
						"Rebuild the request from supported reflected authored data."));
				}
				batch.results.push_back(std::move(receipt));
			}
			// Every admitted identity receives one terminal outcome, including values
			// replaced by coalescing. Superseded work owns no applied write.
			for (const auto& request : superseded) {
				SceneMutationResult receipt; receipt.requestId = request.requestId;
				receipt.sourceObjectId = request.sourceObjectId; receipt.sourceObjectUuid = request.sourceObjectUuid;
				receipt.sourceBindingId = request.sourceBindingId; receipt.sourceFrameOrder = request.sourceFrameOrder;
				receipt.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Info, request,
					"Request was superseded by a later coalesced value.", "No authored write was applied for this identity."));
				batch.diagnostics.insert(batch.diagnostics.end(), receipt.diagnostics.begin(), receipt.diagnostics.end());
				batch.results.push_back(std::move(receipt));
				if (request.typedSpatialTransaction) {
					auto canceled = CanceledExecutionResult(*request.typedSpatialTransaction);
					std::scoped_lock lock(requestsMutex);
					AccumulateTransactionDiagnostics(transactionDiagnostics, *request.typedSpatialTransaction, canceled);
					const auto firstReceipt = completedNavigationSourceMutationReceipts.size();
					SettleProducerReceipts(canceled, capturedIntents, completedNavigationSourceMutationReceipts, nextNavigationSourceChangeSequence);
					batch.navigationSourceMutationReceipts.insert(batch.navigationSourceMutationReceipts.end(),
						completedNavigationSourceMutationReceipts.begin() + firstReceipt, completedNavigationSourceMutationReceipts.end());
					batch.transactionResults.push_back(std::move(canceled));
				}
			}
			TransactionResult genericResult;
			genericResult.transactionId = generic.id;
			if (!requestSnapshot.empty() && !generic.writeSet.operations.empty() && lowered) {
				try { genericResult = CommitTransaction(objects, generic); }
				catch (const TransactionPublicationFailure& failure) {
					std::scoped_lock lock(requestsMutex);
					AccumulateTransactionDiagnostics(transactionDiagnostics, generic, failure.committedResult);
					for (std::size_t index = 0; index < requestSnapshot.size(); ++index) {
						const auto& request = requestSnapshot[index]; if (!request.typedSpatialTransaction) continue;
						auto receipt = ProjectSpatialReceipt(failure.committedResult, generic, batch.results[index], *request.typedSpatialTransaction);
						AccumulateTransactionDiagnostics(transactionDiagnostics, *request.typedSpatialTransaction, receipt);
						SettleProducerReceipts(receipt, capturedIntents, completedNavigationSourceMutationReceipts, nextNavigationSourceChangeSequence);
					}
					throw;
				}
			} else {
				genericResult.status = TransactionStatus::Rejected;
				genericResult.rejectedWriteCount = LogicalWriteCount(generic);
				genericResult.message = "Generic request cohort was rejected during detached lowering.";
			}
			const bool committed = genericResult.status == TransactionStatus::Committed;
			for (std::size_t index = 0; index < requestSnapshot.size(); ++index) {
				const auto& request = requestSnapshot[index]; auto& receipt = batch.results[index];
				if (!informational[index]) {
					receipt.accepted = committed;
					if (committed) {
						receipt.appliedWriteCount = 0U;
						for (std::size_t opIndex = receipt.nativeOperationBegin; opIndex < receipt.nativeOperationBegin + receipt.nativeOperationCount; ++opIndex)
							receipt.appliedWriteCount += LogicalWriteCount(generic.writeSet.operations[opIndex]);
						++batch.acceptedMutationCount;
						if (creationLocals[index] != 0) receipt.createdObjectId = genericResult.createdObjects.at(creationLocals[index]);
					} else {
						++batch.rejectedMutationCount;
						for (const auto& conflict : genericResult.conflicts) {
							if (conflict.operationIndex >= receipt.nativeOperationBegin && conflict.operationIndex < receipt.nativeOperationBegin + receipt.nativeOperationCount)
								receipt.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request, conflict.humanMessage,
									"Refresh authored data before resubmitting the rejected cohort."));
						}
						if (receipt.diagnostics.empty()) receipt.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request,
							"The atomic generic cohort was rejected; this request was not applied.", "Resolve the failing request before resubmitting the cohort."));
					}
				}
				batch.diagnostics.insert(batch.diagnostics.end(), receipt.diagnostics.begin(), receipt.diagnostics.end());
				if (request.typedSpatialTransaction) {
					auto spatialReceipt = ProjectSpatialReceipt(genericResult, generic, receipt, *request.typedSpatialTransaction);
					TerraEngine::Animation::AnimationRootMotionResultPublisher::Instance().ObserveSceneTransactionResult(spatialReceipt);
					batch.transactionResults.push_back(std::move(spatialReceipt));
				}
			}
			batch.applied = committed;
			if (!requestSnapshot.empty() && !generic.writeSet.operations.empty()) {
				std::scoped_lock lock(requestsMutex);
				AccumulateTransactionDiagnostics(transactionDiagnostics, generic, genericResult);
				batch.transactionResults.push_back(genericResult);
			}
			batch.acceptedAnimationPublications.insert(batch.acceptedAnimationPublications.end(), genericResult.acceptedAnimationPublications.begin(), genericResult.acceptedAnimationPublications.end());
			const std::vector<TransactionResult> transactionResults = DrainPreparedTransactions(objects, capturedTransactions, capturedIntents);
			batch.transactionResults.insert(batch.transactionResults.end(), transactionResults.begin(), transactionResults.end());
			for (const TransactionResult& transactionResult : transactionResults)
			{
				// Unknown transaction IDs are ignored by the animation publisher. Known
				// root-motion IDs become observable only at this protected scene flush.
				TerraEngine::Animation::AnimationRootMotionResultPublisher::Instance()
					.ObserveSceneTransactionResult(transactionResult);
				if (transactionResult.status == TransactionStatus::Committed)
				{
					batch.applied = true;
					batch.acceptedMutationCount += transactionResult.appliedWriteCount;
					batch.acceptedAnimationPublications.insert(batch.acceptedAnimationPublications.end(), transactionResult.acceptedAnimationPublications.begin(), transactionResult.acceptedAnimationPublications.end());
				}
				else
				{
					batch.rejectedMutationCount += transactionResult.rejectedWriteCount;
				}
			}
			const std::vector<TransactionPublicationGroupResult> publicationGroupResults = DrainPreparedPublicationGroups(objects, capturedGroups, capturedIntents);
			batch.publicationGroupResults = publicationGroupResults;
			for (const TransactionPublicationGroupResult& groupResult : publicationGroupResults)
			{
				if (groupResult.status == TransactionStatus::Committed)
				{
					batch.applied = true;
					batch.acceptedMutationCount += groupResult.appliedWriteCount;
					batch.acceptedAnimationPublications.insert(batch.acceptedAnimationPublications.end(), groupResult.aggregateResult.acceptedAnimationPublications.begin(), groupResult.aggregateResult.acceptedAnimationPublications.end());
				}
				else
				{
					batch.rejectedMutationCount += groupResult.rejectedWriteCount;
				}
			}

			// Resolve copied navigation intents from the same immutable receipts used by
			// every other transactional producer. An intent with no receipt remains
			// queued; it cannot become visible ahead of its associated scene commit.
			std::unordered_map<std::string, TransactionStatus> navigationTransactionStatuses;
			for(const TransactionResult& result : batch.transactionResults)
			{
				navigationTransactionStatuses[result.transactionId] = result.status;
			}
			for(const TransactionPublicationGroupResult& groupResult : batch.publicationGroupResults)
			{
				for(const TransactionPublicationChunkReceipt& chunk : groupResult.chunkReceipts)
				{
					navigationTransactionStatuses[chunk.transactionId] = groupResult.status;
				}
			}
			{
				std::scoped_lock lock(requestsMutex);
				std::vector<NavigationSourceMutationIntent> unresolved;
				unresolved.reserve(capturedIntents.size());
				for(NavigationSourceMutationIntent& intent : capturedIntents)
				{
					const auto decision = navigationTransactionStatuses.find(intent.transactionId);
					if(decision == navigationTransactionStatuses.end())
					{
						unresolved.push_back(std::move(intent));
						continue;
					}
					NavigationSourceMutationReceipt receipt;
					receipt.intent = std::move(intent);
					receipt.transactionStatus = decision->second;
					receipt.accepted = decision->second == TransactionStatus::Committed;
					if(receipt.accepted) {
						// Content revisions identify a copied mesh/terrain value and may be
						// hashes, so they are not a safe ordering relation. Stamp one queue
						// sequence only after the scene transaction is irrevocably visible.
						// Refuse the astronomically unlikely wrap instead of reusing an old
						// sequence across a retained navigation lifetime.
						if(nextNavigationSourceChangeSequence == 0U) {
							receipt.accepted = false;
							receipt.transactionStatus = TransactionStatus::Rejected;
							receipt.message = "Navigation source change sequence is exhausted.";
						} else {
							receipt.intent.committedChangeSequence = nextNavigationSourceChangeSequence++;
						}
					}
					receipt.message = receipt.accepted
						? "Navigation source change committed with its scene transaction."
						: (receipt.message.empty() ? "Navigation source change rejected with its scene transaction."
							: receipt.message);
					batch.navigationSourceMutationReceipts.push_back(receipt);
					completedNavigationSourceMutationReceipts.push_back(std::move(receipt));
				}
				capturedIntents.swap(unresolved);
			}

			RestorePendingIntents(capturedIntents);
			batch.formattedCommitSummary = FormatCommitSummary(batch);
			return batch;
		} catch (...) {
			// Cleanup is best effort after a required execution failure. Receipt or
			// diagnostic allocation cannot replace the first production exception,
			// or prevent the remaining captured owners from being consumed.
			const auto failure = std::current_exception();
			try {
				std::scoped_lock lock(requestsMutex);
				for (const auto& result : batch.transactionResults) SettleProducerReceipts(result, capturedIntents,
					completedNavigationSourceMutationReceipts, nextNavigationSourceChangeSequence);
			} catch (...) {}
			try { CancelPreparedExecution(capturedTransactions, capturedGroups, capturedIntents); } catch (...) {}
			capturedTransactions.clear(); capturedGroups.clear(); capturedIntents.clear();
			std::rethrow_exception(failure);
		}
	}

	std::vector<TransactionResult> SceneMutationQueue::DrainTransactions(ObjectManager& objects)
	{
		std::vector<Transaction> pending;
		std::vector<NavigationSourceMutationIntent> intents;
		{ std::scoped_lock lock(requestsMutex); pending.swap(transactions); intents.swap(navigationSourceMutationIntents); }
		try { auto result = DrainPreparedTransactions(objects, pending, intents); RestorePendingIntents(intents); return result; }
		catch (...) {
			const auto failure = std::current_exception();
			std::vector<TransactionPublicationGroup> groups;
			try { CancelPreparedExecution(pending, groups, intents); } catch (...) {}
			pending.clear(); intents.clear();
			std::rethrow_exception(failure);
		}
	}

	std::vector<TransactionResult> SceneMutationQueue::DrainPreparedTransactions(ObjectManager& objects, std::vector<Transaction>& pending,
		std::vector<NavigationSourceMutationIntent>& navigationSourceMutationIntents)
	{
		{
			std::scoped_lock lock(requestsMutex);
			completedEditorTransactions.reserve(completedEditorTransactions.size() + pending.size());
			completedPublicationGroupResults.reserve(completedPublicationGroupResults.size() + pending.size());
			completedNavigationSourceMutationReceipts.reserve(completedNavigationSourceMutationReceipts.size() + navigationSourceMutationIntents.size());
		}

		std::vector<TransactionResult> results;
		results.reserve(pending.size());
		std::vector<TransactionResult> canceled;
		std::vector<EditorTransactionCompletion> completions;
		canceled.reserve(pending.size()); completions.reserve(pending.size());
		for (const auto& transaction : pending) {
			canceled.push_back(CanceledExecutionResult(transaction));
			if (transaction.origin == TransactionOrigin::Editor
				&& (!transaction.undoGroupName.empty() || transaction.originName == "UndoRedoReplay")) completions.push_back({transaction, {}});
			else completions.push_back({{}, {}});
		}
		auto retain = [&](std::size_t index, TransactionResult result, bool producerSettlement) {
			std::scoped_lock lock(requestsMutex);
			AccumulateTransactionDiagnostics(transactionDiagnostics, pending[index], result);
			RetainAutomationTransactionReceipt(pending[index], result);
			if (producerSettlement) SettleProducerReceipts(result, navigationSourceMutationIntents,
				completedNavigationSourceMutationReceipts, nextNavigationSourceChangeSequence);
			if (pending[index].origin == TransactionOrigin::Editor
				&& (!pending[index].undoGroupName.empty() || pending[index].originName == "UndoRedoReplay")) {
				completions[index].result = std::move(result);
				completedEditorTransactions.push_back(std::move(completions[index]));
			}
		};
		for (std::size_t index = 0; index < pending.size(); ++index) {
			TransactionResult result;
			try { result = CommitTransaction(objects, pending[index]); }
			catch (TransactionPublicationFailure& failure) {
				// The current batch committed. Consume its receipt exactly once and
				// terminally cancel the detached remainder before propagating failure.
				{
					std::scoped_lock lock(requestsMutex);
					for (const auto& earlier : results) SettleProducerReceipts(earlier, navigationSourceMutationIntents,
						completedNavigationSourceMutationReceipts, nextNavigationSourceChangeSequence);
				}
				retain(index, failure.committedResult, true);
				for (std::size_t later = index + 1; later < pending.size(); ++later) retain(later, std::move(canceled[later]), true);
				pending.clear();
				throw;
			}
			catch (const TransactionRestorationFailure&) {
				// The invariant breach has no ordinary rejection result. The current
				// transaction is consumed; unstarted work still receives cancellation.
				{
					std::scoped_lock lock(requestsMutex);
					for (const auto& earlier : results) SettleProducerReceipts(earlier, navigationSourceMutationIntents,
						completedNavigationSourceMutationReceipts, nextNavigationSourceChangeSequence);
				}
				for (std::size_t later = index + 1; later < pending.size(); ++later) retain(later, std::move(canceled[later]), true);
				pending.clear();
				throw;
			}
			retain(index, result, false);
			results.push_back(std::move(result));
		}
		pending.clear();
		return results;
	}

	std::vector<TransactionPublicationGroupResult> SceneMutationQueue::DrainPublicationGroups(ObjectManager& objects)
	{
		std::vector<TransactionPublicationGroup> pending;
		std::vector<NavigationSourceMutationIntent> intents;
		{ std::scoped_lock lock(requestsMutex); pending.swap(publicationGroups); intents.swap(navigationSourceMutationIntents); }
		try { auto result = DrainPreparedPublicationGroups(objects, pending, intents); RestorePendingIntents(intents); return result; }
		catch (...) {
			const auto failure = std::current_exception();
			std::vector<Transaction> transactions;
			try { CancelPreparedExecution(transactions, pending, intents); } catch (...) {}
			pending.clear(); intents.clear();
			std::rethrow_exception(failure);
		}
	}

	std::vector<TransactionPublicationGroupResult> SceneMutationQueue::DrainPreparedPublicationGroups(ObjectManager& objects, std::vector<TransactionPublicationGroup>& pending,
		std::vector<NavigationSourceMutationIntent>& navigationSourceMutationIntents)
	{
		{
			std::scoped_lock lock(requestsMutex);
			completedEditorTransactions.reserve(completedEditorTransactions.size() + pending.size());
			completedPublicationGroupResults.reserve(completedPublicationGroupResults.size() + pending.size());
			completedNavigationSourceMutationReceipts.reserve(completedNavigationSourceMutationReceipts.size() + navigationSourceMutationIntents.size());
		}

		std::vector<TransactionPublicationGroupResult> results;
		std::vector<TransactionPublicationGroupResult> canceled;
		results.reserve(pending.size()); canceled.reserve(pending.size());
		for (const auto& group : pending) {
			TransactionPublicationGroupResult result;
			result.groupId = group.groupId; result.status = TransactionStatus::Canceled;
			result.aggregateResult.transactionId = group.groupId;
			result.aggregateResult.status = TransactionStatus::Canceled;
			result.message = "Publication group canceled because required-frame execution failed.";
			result.chunkReceipts.reserve(group.chunks.size());
			for (std::size_t index = 0; index < group.chunks.size(); ++index) {
				const auto& chunk = group.chunks[index];
				result.chunkReceipts.push_back({chunk.id, index, TransactionStatus::Canceled, LogicalWriteCount(chunk)});
				result.rejectedWriteCount += LogicalWriteCount(chunk);
			}
			result.aggregateResult.rejectedWriteCount = result.rejectedWriteCount;
			canceled.push_back(std::move(result));
		}
		auto settle = [&](const TransactionPublicationGroupResult& result) {
			SettleProducerReceipts(result.aggregateResult, navigationSourceMutationIntents,
				completedNavigationSourceMutationReceipts, nextNavigationSourceChangeSequence);
			for (const auto& chunk : result.chunkReceipts) {
				TransactionResult receipt;
				receipt.transactionId = chunk.transactionId; receipt.status = result.status;
				if (result.status == TransactionStatus::Committed) receipt.appliedWriteCount = chunk.writeCount;
				else receipt.rejectedWriteCount = chunk.writeCount;
				SettleProducerReceipts(receipt, navigationSourceMutationIntents,
					completedNavigationSourceMutationReceipts, nextNavigationSourceChangeSequence);
			}
		};
		for (std::size_t index = 0; index < pending.size(); ++index) {
			try { results.push_back(CommitTransactionPublicationGroup(objects, pending[index])); }
			catch (TransactionGroupPublicationFailure& failure) {
				std::scoped_lock lock(requestsMutex);
				// Earlier results never reached the caller because this drain failed.
				// Transfer the cohort once; ordinary successful drains return directly.
				for (auto& earlier : results) { settle(earlier); completedPublicationGroupResults.push_back(std::move(earlier)); }
				settle(failure.committedGroupResult);
				completedPublicationGroupResults.push_back(std::move(failure.committedGroupResult));
				for (std::size_t later = index + 1; later < pending.size(); ++later) {
					settle(canceled[later]); completedPublicationGroupResults.push_back(std::move(canceled[later]));
				}
				pending.clear();
				throw;
			}
			catch (const TransactionRestorationFailure&) {
				std::scoped_lock lock(requestsMutex);
				for (auto& earlier : results) { settle(earlier); completedPublicationGroupResults.push_back(std::move(earlier)); }
				for (std::size_t later = index + 1; later < pending.size(); ++later) {
					settle(canceled[later]); completedPublicationGroupResults.push_back(std::move(canceled[later]));
				}
				pending.clear();
				throw;
			}
		}
		pending.clear();
		return results;
	}
}
