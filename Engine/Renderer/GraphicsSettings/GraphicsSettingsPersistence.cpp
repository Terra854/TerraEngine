// GraphicsSettingsPersistence.cpp — copied IO jobs and protected restore handoff.
#include "GraphicsSettingsPersistence.h"
#include "GraphicsStartupStore.h"
#include "GraphicsSettingsCatalog.h"
#include "ThreadPool/ThreadPool.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <future>
#include <limits>
#include <mutex>
#include <set>
#include <unordered_map>
#include <type_traits>
#include <stdexcept>

namespace TerraEngine::Renderer::GraphicsSettings
{
	namespace
	{
		constexpr GraphicsRequestId durableBit = GraphicsRequestId{1} << 63;
		std::atomic<std::uint64_t> nextDurableId{1};
		bool Terminal(GraphicsRequestState state)
		{
			return state == GraphicsRequestState::Applied || state == GraphicsRequestState::Rejected
				|| state == GraphicsRequestState::Cancelled;
		}
		void ObserveId(GraphicsRequestId id)
		{
			if(!(id & durableBit)) return;
			const auto next = (id & ~durableBit) + 1;
			auto previous = nextDurableId.load(std::memory_order_relaxed);
			while(previous < next && !nextDurableId.compare_exchange_weak(previous, next, std::memory_order_relaxed)) {}
		}
		GraphicsRequestId AllocateId()
		{
			const auto sequence = nextDurableId.fetch_add(1, std::memory_order_relaxed);
			if(sequence >= durableBit) throw std::overflow_error("Graphics persistence request IDs exhausted.");
			return durableBit | sequence;
		}
		bool ValidField(const GraphicsSettingDescriptor& descriptor, const GraphicsValue& value)
		{
			if(value.index() != static_cast<std::size_t>(descriptor.valueKind)) return false;
			if(const auto scalar = std::get_if<float>(&value); scalar && !std::isfinite(*scalar)) return false;
			const auto bound = [&](const std::optional<GraphicsValue>& edge, bool minimum) {
				if(!edge) return true;
				if(edge->index() != value.index()) return false;
				return std::visit([&](const auto& typed) {
					using T = std::decay_t<decltype(typed)>;
					if constexpr(std::is_arithmetic_v<T>) return minimum ? typed >= std::get<T>(*edge) : typed <= std::get<T>(*edge);
					else return false;
				}, value);
			};
			return bound(descriptor.minimum, true) && bound(descriptor.maximum, false)
				&& (descriptor.options.empty() || std::find(descriptor.options.begin(), descriptor.options.end(), value) != descriptor.options.end());
		}
	}
	GraphicsRequestReason ValidateGraphicsStartupTransaction(const GraphicsStartupTransaction& transaction,
		std::span<const GraphicsSettingDescriptor> catalog)
	{
		const auto& request = transaction.request;
		if(request.target != GraphicsTarget{}) return GraphicsRequestReason::InvalidScope;
		if(request.operation != GraphicsRequestOperation::StageNextLaunch || request.patches.empty()
			|| !request.resetSettings.empty() || request.preset) return GraphicsRequestReason::InvalidValue;
		bool startupField = false;
		std::set<GraphicsSettingId> visited;
		for(const auto& field : request.patches)
		{
			if(field.id == GraphicsSettingId::Invalid || !visited.insert(field.id).second) return GraphicsRequestReason::InvalidSetting;
			const auto* descriptor = FindGraphicsSetting(catalog, field.id);
			if(!descriptor || std::count_if(catalog.begin(), catalog.end(), [&](const auto& row) { return row.id == field.id; }) != 1)
				return GraphicsRequestReason::InvalidSetting;
			if(!descriptor->gameScope || descriptor->authoringScope != GraphicsAuthoringScope::None) return GraphicsRequestReason::InvalidScope;
			if(!descriptor->available) return descriptor->unavailableReason == GraphicsRequestReason::None
				? GraphicsRequestReason::Unsupported : descriptor->unavailableReason;
			if(descriptor->activation == GraphicsActivation::ProfileOnly) return GraphicsRequestReason::InvalidScope;
			// Coverage, reconstruction and the real raster selector share strict
			// known-unavailable versus malformed classification during staging.
			if(IsGraphicsAASetting(field.id))
			{
				const auto reason = ValidateGraphicsAASettingValue(*descriptor, field.value);
				if(reason != GraphicsRequestReason::None) return reason;
			}
			if(!ValidField(*descriptor, field.value)) return GraphicsRequestReason::InvalidValue;
			startupField |= descriptor->activation == GraphicsActivation::NextLaunch;
		}
		visited.clear();
		for(const auto& precondition : transaction.fieldPreconditions)
		{
			if(!visited.insert(precondition.id).second) return GraphicsRequestReason::InvalidSetting;
			const auto* descriptor = FindGraphicsSetting(catalog, precondition.id);
			if(!descriptor || std::count_if(catalog.begin(), catalog.end(), [&](const auto& row) { return row.id == precondition.id; }) != 1)
				return GraphicsRequestReason::InvalidSetting;
			if(!descriptor->gameScope || descriptor->authoringScope != GraphicsAuthoringScope::None)
				return GraphicsRequestReason::InvalidScope;
			if(!ValidField(*descriptor, precondition.value)) return GraphicsRequestReason::InvalidValue;
		}
		return startupField ? GraphicsRequestReason::None : GraphicsRequestReason::InvalidValue;
	}

	struct GraphicsSettingsPersistence::State : std::enable_shared_from_this<State>
	{
		enum class Operation { Initialize, Stage, Cancel, Save, Read, Restore };
		struct Work
		{
			Operation operation = Operation::Initialize;
			GraphicsRequestId id = 0;
			std::string name;
			std::uint64_t expectedRevision = 0;
			std::vector<GraphicsSettingValue> values;
			GraphicsStartupTransaction transaction;
			GraphicsSettingsRequest restoreTemplate;
		};
		mutable std::mutex mutex;
		std::filesystem::path root;
		std::string identity;
		bool allowStartup = false, initialized = false, running = false, closed = false, startupFailure = false;
		GraphicsStartupStoreResult initialization;
		GraphicsStartupStoreSnapshot startup;
		std::deque<Work> queue;
		std::future<bool> worker;
		std::unordered_map<GraphicsRequestId, GraphicsRequestReceipt> receipts;
		std::unordered_map<std::string, Scripting::GraphicsPreferenceSnapshot> preferences;
		std::set<GraphicsRequestId> restores;
		std::vector<GraphicsPreferenceRestore> readyRestores;
		std::optional<GraphicsRequestId> admittedStage;
		std::vector<GraphicsSettingId> admittedFields;

		void ImportStartupLocked(GraphicsStartupStoreSnapshot snapshot)
		{
			// All potentially allocating receipt copies precede replacing the cache.
			// Existing in-memory restore/save receipts are separate request owners.
			auto proposed = receipts;
			for(const auto& receipt : snapshot.terminalReceipts) { proposed[receipt.requestId] = receipt; ObserveId(receipt.requestId); }
			if(snapshot.pending)
			{
				const auto& transaction = snapshot.pending->transaction;
				GraphicsRequestReceipt receipt;
				receipt.requestId = transaction.requestId; receipt.sequence = transaction.requestId & ~durableBit;
				receipt.state = GraphicsRequestState::ScheduledRestart; receipt.activation = GraphicsActivation::NextLaunch;
				receipt.requestedRevision = transaction.request.baseRevision;
				for(const auto& field : transaction.request.patches) receipt.affectedSettings.push_back(field.id);
				proposed[receipt.requestId] = std::move(receipt); ObserveId(transaction.requestId);
			}
			receipts.swap(proposed); startup = std::move(snapshot);
		}
		void FailLocked(GraphicsRequestId id, GraphicsRequestReason reason)
		{
			const auto found = receipts.find(id);
			if(found != receipts.end() && !Terminal(found->second.state))
			{
				found->second.state = GraphicsRequestState::Rejected;
				found->second.reason = reason; found->second.detail.clear();
			}
			if(admittedStage == id) { admittedStage.reset(); admittedFields.clear(); }
		}
		void FailWorkLocked(const Work& work, GraphicsRequestReason reason)
		{
			if(work.operation == Operation::Cancel)
			{
				if(auto found = receipts.find(work.id); found != receipts.end()) found->second.reason = reason;
			}
			else FailLocked(work.id, reason);
		}
		// Polling settles executor rejection without blocking or touching disk.
		// Failed unstarted work cannot leave Accepted receipts or reservations.
		void CollectLocked()
		{
			if(!worker.valid() || worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
			try { worker.get(); }
			catch(...)
			{
				for(const auto& work : queue) FailWorkLocked(work, GraphicsRequestReason::PreparationFailed);
				queue.clear();
				if(!initialized) { initialized = true; initialization.succeeded = false; initialization.reason = GraphicsRequestReason::PreparationFailed; }
				running = false;
			}
		}
		bool ConflictsLocked(std::span<const GraphicsSettingId> fields) const
		{
			if(!allowStartup) return false;
			if(!initialized || !initialization.succeeded) return !fields.empty();
			for(auto id : fields)
			{
				if(std::find(admittedFields.begin(), admittedFields.end(), id) != admittedFields.end()) return true;
				if(startup.pending)
					for(const auto& field : startup.pending->transaction.request.patches) if(field.id == id) return true;
			}
			return false;
		}
		void ScheduleLocked()
		{
			if(running || closed || queue.empty()) return;
			CollectLocked();
			try
			{
				auto owner = shared_from_this();
				auto task = ThreadPool::instance().createTask([owner]() { owner->Run(); return true; });
				auto future = task->getFuture();
				// Publish the future before dispatch, while the worker is excluded
				// by mutex. Executor rejection settles the same typed promise.
				running = true; worker = std::move(future);
				ThreadPool::instance().enqueue(task);
			}
			catch(...)
			{
				running = false;
				for(const auto& work : queue) FailWorkLocked(work, GraphicsRequestReason::PreparationFailed);
				queue.clear(); throw;
			}
		}

		GraphicsRequestReceipt Admit(Work work, GraphicsRequestReceipt receipt, std::vector<GraphicsSettingId> reserve = {})
		{
			std::lock_guard lock(mutex); CollectLocked();
			if(closed) { receipt.state = GraphicsRequestState::Rejected; receipt.reason = GraphicsRequestReason::StaleLifetime; return receipt; }
			// Until durable IDs have been loaded, assigning a public ID could reuse
			// an earlier launch's request. Return no admission instead of guessing.
			if(!initialized || !initialization.succeeded)
			{
				receipt.requestId = 0; receipt.sequence = 0; receipt.state = GraphicsRequestState::Rejected;
				receipt.reason = initialized ? initialization.reason : GraphicsRequestReason::TargetUnavailable;
				return receipt;
			}
			if(work.operation == Operation::Stage)
			{
				if(!allowStartup) { receipt.state = GraphicsRequestState::Rejected; receipt.reason = GraphicsRequestReason::InvalidScope; }
				else if(!initialized || !initialization.succeeded) { receipt.state = GraphicsRequestState::Rejected; receipt.reason = GraphicsRequestReason::TargetUnavailable; }
				else if(startup.pending || admittedStage) { receipt.state = GraphicsRequestState::Rejected; receipt.reason = GraphicsRequestReason::ConflictingRequest; }
			}
			receipts.emplace(receipt.requestId, receipt);
			if(Terminal(receipt.state)) return receipt;
			try { queue.push_back(std::move(work)); }
			catch(...) { receipts.erase(receipt.requestId); throw; }
			if(queue.back().operation == Operation::Stage) { admittedStage = receipt.requestId; admittedFields = std::move(reserve); }
			try { ScheduleLocked(); }
			catch(...)
			{
				FailLocked(receipt.requestId, GraphicsRequestReason::PreparationFailed);
				receipt.state = GraphicsRequestState::Rejected; receipt.reason = GraphicsRequestReason::PreparationFailed;
			}
			// The caller's owning receipt already exists before admission. Moving
			// it out cannot throw after the queue becomes observable to a worker.
			return receipt;
		}
		void Run()
		{
			for(;;)
			{
				Work work;
				{
					std::lock_guard lock(mutex);
					if(queue.empty() || closed) { running = false; return; }
					work = std::move(queue.front()); queue.pop_front();
				}
				try { Execute(work); }
				catch(...)
				{
					std::lock_guard lock(mutex); FailWorkLocked(work, GraphicsRequestReason::StorageFailed);
					if(work.operation == Operation::Initialize)
					{ initialized = true; initialization.succeeded = false; initialization.reason = GraphicsRequestReason::StorageFailed; }
				}
			}
		}
		void Execute(const Work& work)
		{
			// This method runs only on a task worker. The IO transaction serializes
			// checked updates; it never holds the renderer/coordinator cache mutex.
			if(work.operation == Operation::Initialize || work.operation == Operation::Stage || work.operation == Operation::Cancel)
			{
				auto store = OpenGraphicsStartupStore(root, identity);
				GraphicsStartupStoreSnapshot latest;
				auto result = store->Read(latest);
				GraphicsStartupStoreSnapshot proposed;
				GraphicsRequestReceipt terminalReceipt;
				// Prepare every owning copy before durability. After a successful
				// Commit, cache publication consists only of noexcept moves/scalars.
				if(result.succeeded && work.operation != Operation::Initialize)
				{
					proposed = latest;
					++proposed.storeRevision;
					if(work.operation == Operation::Stage)
					{
						proposed.pending = GraphicsStartupRecord{work.transaction};
						result = store->Stage(work.transaction, latest.storeRevision);
					}
					else
					{
						{ std::lock_guard lock(mutex); terminalReceipt = receipts.at(work.id); }
						terminalReceipt.state = GraphicsRequestState::Cancelled;
						terminalReceipt.reason = GraphicsRequestReason::ExplicitCancellation;
						proposed.pending.reset(); proposed.terminalReceipts.push_back(terminalReceipt);
						result = store->Cancel(work.id, latest.storeRevision, terminalReceipt);
					}
				}
				std::lock_guard lock(mutex);
				if(work.operation == Operation::Initialize)
				{
					if(!startupFailure)
					{
						if(result.succeeded) ImportStartupLocked(std::move(latest));
						initialization = std::move(result); initialized = true;
					}
				}
				else if(result.succeeded)
				{
					static_assert(std::is_nothrow_move_assignable_v<GraphicsStartupStoreSnapshot>);
					startup = std::move(proposed);
					auto& receipt = receipts.at(work.id);
					if(work.operation == Operation::Stage)
					{ receipt.state = GraphicsRequestState::ScheduledRestart; receipt.reason = GraphicsRequestReason::None; }
					else receipt = std::move(terminalReceipt);
				}
				else if(work.operation == Operation::Cancel && startup.pending && startup.pending->transaction.requestId == work.id)
				{
					// The staged transaction remains durable when cancellation fails.
					// Its receipt cannot become a contradictory terminal rejection.
					receipts.at(work.id).reason = result.reason;
				}
				else FailLocked(work.id, result.reason);
				if(admittedStage == work.id) { admittedStage.reset(); admittedFields.clear(); }
				return;
			}
			auto store = Scripting::OpenGraphicsPreferenceStore(root, identity);
			Scripting::GraphicsPreferenceSnapshot preference;
			GraphicsStartupStoreResult result;
			std::optional<std::unordered_map<std::string, Scripting::GraphicsPreferenceSnapshot>> proposedPreferences;
			if(work.operation == Operation::Save)
			{
				preference.values = work.values; preference.revision = work.expectedRevision + 1;
				{ std::lock_guard lock(mutex); proposedPreferences = preferences; }
				(*proposedPreferences)[work.name] = preference;
				result = store->Save(work.name, work.values, work.expectedRevision);
			}
			else result = store->Read(work.name, preference);
			std::lock_guard lock(mutex);
			if(!result.succeeded) { FailLocked(work.id, result.reason); return; }
			if(proposedPreferences) preferences.swap(*proposedPreferences);
			else preferences[work.name] = preference;
			if(work.operation == Operation::Restore)
			{
				if(closed) { FailLocked(work.id, GraphicsRequestReason::StaleLifetime); return; }
				if(preference.revision == 0 || preference.values.empty()) { FailLocked(work.id, GraphicsRequestReason::InvalidValue); return; }
				auto request = work.restoreTemplate;
				request.operation = GraphicsRequestOperation::Patch; request.patches = std::move(preference.values);
				request.resetSettings.clear(); request.preset.reset();
				std::vector<GraphicsSettingId> fields; for(const auto& field : request.patches) fields.push_back(field.id);
				if(ConflictsLocked(fields)) { FailLocked(work.id, GraphicsRequestReason::ConflictingRequest); return; }
				// Store only copied game requests. Native validation at drain time
				// checks the captured play/project epoch and startup-only settings.
				if(request.target != GraphicsTarget{}) { FailLocked(work.id, GraphicsRequestReason::InvalidScope); return; }
				restores.insert(work.id);
				try { readyRestores.push_back({work.id, std::move(request)}); }
				catch(...) { restores.erase(work.id); throw; }
				receipts.at(work.id).affectedSettings = std::move(fields);
			}
			else
			{
				auto& receipt = receipts.at(work.id); receipt.state = GraphicsRequestState::Applied;
				receipt.effectiveRevision = result.storeRevision;
			}
		}
	};

	GraphicsSettingsPersistence::GraphicsSettingsPersistence(const std::filesystem::path& root,
		std::string identity, bool allowStartup, std::optional<GraphicsStartupStoreSnapshot> initial)
		: state(std::make_shared<State>())
	{
		state->root = root; state->identity = std::move(identity); state->allowStartup = allowStartup;
		std::lock_guard lock(state->mutex);
		if(initial) { state->ImportStartupLocked(std::move(*initial)); state->initialized = true; state->initialization.succeeded = true; state->initialization.storeRevision = state->startup.storeRevision; }
		else if(!allowStartup) { state->initialized = true; state->initialization.succeeded = true; }
		else { state->queue.push_back({}); state->ScheduleLocked(); }
	}
	GraphicsSettingsPersistence::~GraphicsSettingsPersistence()
	{
		// Accepted durable writes may finish after this facade is retired. Their
		// task owns every dependency. Copied restores cannot outlive the facade.
		std::lock_guard lock(state->mutex); state->closed = true;
		for(const auto& work : state->queue) state->FailLocked(work.id, GraphicsRequestReason::StaleLifetime);
		state->queue.clear(); state->readyRestores.clear();
	}
	GraphicsRequestReceipt GraphicsSettingsPersistence::Stage(GraphicsStartupTransaction transaction,
		std::vector<GraphicsSettingDescriptor> catalog)
	{
		GraphicsRequestReceipt receipt; receipt.requestId = AllocateId(); receipt.sequence = receipt.requestId & ~durableBit;
		receipt.state = GraphicsRequestState::Accepted; receipt.activation = GraphicsActivation::NextLaunch;
		receipt.requestedRevision = transaction.request.baseRevision;
		transaction.requestId = receipt.requestId;
		receipt.reason = ValidateGraphicsStartupTransaction(transaction, catalog);
		if(receipt.reason != GraphicsRequestReason::None) receipt.state = GraphicsRequestState::Rejected;
		std::vector<GraphicsSettingId> fields;
		for(const auto& field : transaction.request.patches) fields.push_back(field.id);
		receipt.affectedSettings = fields;
		State::Work work; work.operation = State::Operation::Stage; work.id = receipt.requestId; work.transaction = std::move(transaction);
		return state->Admit(std::move(work), std::move(receipt), std::move(fields));
	}
	GraphicsRequestReceipt GraphicsSettingsPersistence::Cancel(GraphicsRequestId id)
	{
		std::lock_guard lock(state->mutex); state->CollectLocked();
		auto found = state->receipts.find(id);
		if(found == state->receipts.end()) return {id, 0, GraphicsRequestState::Rejected, GraphicsActivation::NextLaunch, GraphicsRequestReason::InvalidValue};
		auto result = found->second;
		if(!state->allowStartup || state->closed || Terminal(result.state)
			|| ((!state->startup.pending || state->startup.pending->transaction.requestId != id) && state->admittedStage != id))
		{ result.state = GraphicsRequestState::Rejected; result.reason = GraphicsRequestReason::ConflictingRequest; return result; }
		State::Work work; work.operation = State::Operation::Cancel; work.id = id;
		state->queue.push_back(std::move(work));
		try { state->ScheduleLocked(); }
		catch(...) { result.state = GraphicsRequestState::Rejected; result.reason = GraphicsRequestReason::PreparationFailed; return result; }
		result.state = GraphicsRequestState::Accepted; return result;
	}
	GraphicsRequestReceipt GraphicsSettingsPersistence::Save(std::string name, std::vector<GraphicsSettingValue> values, std::uint64_t revision)
	{
		GraphicsRequestReceipt receipt; receipt.requestId = AllocateId(); receipt.sequence = receipt.requestId & ~durableBit;
		receipt.state = GraphicsRequestState::Accepted; receipt.requestedRevision = revision;
		for(const auto& field : values) receipt.affectedSettings.push_back(field.id);
		State::Work work; work.operation = State::Operation::Save; work.id = receipt.requestId; work.name = std::move(name);
		work.values = std::move(values); work.expectedRevision = revision;
		return state->Admit(std::move(work), std::move(receipt));
	}
	GraphicsRequestReceipt GraphicsSettingsPersistence::Read(std::string name)
	{
		GraphicsRequestReceipt receipt; receipt.requestId = AllocateId(); receipt.sequence = receipt.requestId & ~durableBit;
		receipt.state = GraphicsRequestState::Accepted;
		State::Work work; work.operation = State::Operation::Read; work.id = receipt.requestId; work.name = std::move(name);
		return state->Admit(std::move(work), std::move(receipt));
	}
	GraphicsRequestReceipt GraphicsSettingsPersistence::Restore(std::string name, GraphicsSettingsRequest request)
	{
		GraphicsRequestReceipt receipt; receipt.requestId = AllocateId(); receipt.sequence = receipt.requestId & ~durableBit;
		receipt.state = GraphicsRequestState::Accepted; receipt.requestedRevision = request.baseRevision;
		State::Work work; work.operation = State::Operation::Restore; work.id = receipt.requestId;
		work.name = std::move(name); work.restoreTemplate = std::move(request);
		return state->Admit(std::move(work), std::move(receipt));
	}
	void GraphicsSettingsPersistence::RecordStartupFailure(GraphicsStartupStoreResult failure,
		std::optional<GraphicsRequestReceipt> receipt)
	{
		std::lock_guard lock(state->mutex);
		// Prepare the optional owning receipt before changing initialization. A
		// failed allocation cannot publish a partial launch-failure overlay.
		if(receipt) state->receipts.insert_or_assign(receipt->requestId, std::move(*receipt));
		failure.succeeded = false;
		if(failure.reason == GraphicsRequestReason::None) failure.reason = GraphicsRequestReason::StorageFailed;
		state->initialization = std::move(failure); state->initialized = true; state->startupFailure = true;
	}
	bool GraphicsSettingsPersistence::Ready() const
	{ std::lock_guard lock(state->mutex); state->CollectLocked(); return state->initialized && state->initialization.succeeded; }
	GraphicsStartupStoreResult GraphicsSettingsPersistence::InitializationResult() const
	{ std::lock_guard lock(state->mutex); state->CollectLocked(); return state->initialization; }
	std::optional<GraphicsStartupStoreSnapshot> GraphicsSettingsPersistence::StartupSnapshot() const
	{ std::lock_guard lock(state->mutex); state->CollectLocked(); if(!state->initialized || !state->initialization.succeeded) return {}; return state->startup; }
	std::optional<Scripting::GraphicsPreferenceSnapshot> GraphicsSettingsPersistence::PreferenceSnapshot(const std::string& name) const
	{ std::lock_guard lock(state->mutex); state->CollectLocked(); auto found = state->preferences.find(name); if(found == state->preferences.end()) return {}; return found->second; }
	std::optional<GraphicsRequestReceipt> GraphicsSettingsPersistence::GetReceipt(GraphicsRequestId id) const
	{ std::lock_guard lock(state->mutex); state->CollectLocked(); auto found = state->receipts.find(id); if(found == state->receipts.end()) return {}; return found->second; }
	bool GraphicsSettingsPersistence::Conflicts(std::span<const GraphicsSettingId> ids) const
	{ std::lock_guard lock(state->mutex); state->CollectLocked(); return state->ConflictsLocked(ids); }
	std::vector<GraphicsPreferenceRestore> GraphicsSettingsPersistence::DrainRestores()
	{
		std::lock_guard lock(state->mutex); state->CollectLocked();
		std::vector<GraphicsPreferenceRestore> result; result.swap(state->readyRestores); return result;
	}
	void GraphicsSettingsPersistence::UpdateRestoreReceipt(GraphicsRequestId id, const GraphicsRequestReceipt& receipt)
	{
		std::lock_guard lock(state->mutex);
		if(!state->restores.contains(id)) return;
		auto proposed = receipt; proposed.requestId = id;
		state->receipts.at(id) = std::move(proposed);
		if(Terminal(receipt.state) || receipt.state == GraphicsRequestState::RestartRequired) state->restores.erase(id);
	}
}
