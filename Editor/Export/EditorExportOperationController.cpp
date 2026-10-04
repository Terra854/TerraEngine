#include "EditorExportOperationController.h"
#include "ExportPipeline.h"
#include "Engine/ThreadPool/ThreadPool.h"
#include <limits>
#include <stdexcept>
#include <type_traits>

EditorExportOperationController::EditorExportOperationController(Executor executor, OwnerHooks hooks)
	: executor_(std::move(executor)), hooks_(std::move(hooks)), terminalIds_(64) {
	if (!executor_) executor_ = [](const ExportRequest& request, ExportProgressCallback progress, std::shared_ptr<ExportCancellationState> cancellation) {
		ExportPipeline pipeline;
		return pipeline.Execute(request, std::move(progress), std::move(cancellation));
	};
}
EditorExportOperationController::~EditorExportOperationController() {
	// Owners explicitly drain before releasing dependencies. This final safety
	// net still settles owned captures if a frontend exits by exception.
	try { StopAndDrain(); } catch (...) {}
}
void EditorExportOperationController::RequireOwner() const {
	if (std::this_thread::get_id() != owner_) throw std::logic_error("Export lifecycle changes require the protected editor owner.");
}
bool EditorExportOperationController::HasPendingOperation() {
	RequireOwner(); Poll(); return activeId_ != 0;
}
EditorExportAdmission EditorExportOperationController::Admit(EditorExportOperationRequest request) {
	RequireOwner(); Poll();
	if (!accepting_ || request.projectGeneration != generation_) return {false, 0, "Export project generation is no longer active."};
	if (activeId_) return {false, 0, "An export is still awaiting its final result."};
	if (nextId_ == std::numeric_limits<std::uint64_t>::max()) return {false, 0, "Export operation identity space exhausted."};
	const auto id = nextId_;
	try {
		auto operation = std::make_shared<Operation>();
		operation->observation.operationId = id;
		operation->observation.revision = 1;
		operation->observation.intent = std::move(request);
		const auto captured = operation->observation.intent.request;
		const auto execute = executor_;
		auto task = ThreadPool::instance().createTask([operation, captured, execute] {
			if (operation->cancellation->requested.load(std::memory_order_relaxed)) {
				ExportResult result; result.runId = captured.runId; result.canceled = true;
				result.failureCategory = ExportFailureCategory::Canceled; result.exitCode = 130;
				result.message = "Export canceled before execution."; return result;
			}
			return execute(captured, [operation](const ExportProgress& progress) {
				std::lock_guard lock(operation->mailbox);
				if (!operation->observation.result) {
					operation->observation.progress = progress;
					++operation->observation.revision;
				}
			}, operation->cancellation);
		});
		operation->future = task->getFuture();
		{ std::lock_guard lock(operationsMutex_); operations_[id] = operation; }
		activeId_ = id;
		if (hooks_.beforeSubmission) hooks_.beforeSubmission();
		ThreadPool::instance().enqueue(task);
		// The private reservation becomes public only after enqueue succeeds.
		// Readers cannot mistake a failed reservation for an issued/expired ID.
		{ std::lock_guard lock(operationsMutex_); ++nextId_; }
	} catch (const std::exception& error) {
		{ std::lock_guard lock(operationsMutex_); operations_.erase(id); }
		activeId_ = 0;
		return {false, 0, error.what()};
	} catch (...) {
		{ std::lock_guard lock(operationsMutex_); operations_.erase(id); }
		activeId_ = 0;
		return {false, 0, "Export submission raised an unknown exception."};
	}
	return {true, id, {}};
}
EditorExportLookupDisposition EditorExportOperationController::MissingDispositionLocked(std::uint64_t id) const {
	return id != 0 && id < nextId_ ? EditorExportLookupDisposition::Expired : EditorExportLookupDisposition::Unknown;
}
EditorExportLookup EditorExportOperationController::Lookup(std::uint64_t id) const {
	std::lock_guard mapLock(operationsMutex_);
	if (id == 0 || id >= nextId_) return {};
	const auto found = operations_.find(id);
	if (found == operations_.end()) return {MissingDispositionLocked(id), {}};
	std::lock_guard mailboxLock(found->second->mailbox);
	return {found->second->observation.result ? EditorExportLookupDisposition::Terminal : EditorExportLookupDisposition::Active,
		found->second->observation};
}
std::optional<EditorExportOperationObservation> EditorExportOperationController::Observe(std::uint64_t id) const {
	return Lookup(id).observation;
}
void EditorExportOperationController::Adopt(const std::shared_ptr<Operation>& operation) {
	{
		std::lock_guard lock(operation->mailbox);
		if (operation->observation.result) return;
	}
	if (!operation->futureObserved) {
		if (!operation->future.valid()) return;
		// ExportResult moves do not allocate. Retain either the exact worker value
		// or its exception before formatting/copying the owner's final receipt.
		static_assert(std::is_nothrow_move_constructible_v<ExportResult>);
		try { operation->workerResult.emplace(operation->future.get()); }
		catch (...) { operation->workerException = std::current_exception(); }
		operation->futureObserved = true;
	}
	if (hooks_.beforeAdoption) hooks_.beforeAdoption();
	ExportResult result;
	if (operation->workerResult) result = *operation->workerResult;
	else {
		try { std::rethrow_exception(operation->workerException); }
		catch (const std::exception& error) { result.failureCategory = ExportFailureCategory::UnexpectedFailure; result.exitCode = 10; result.message = error.what(); result.errorMessage = result.message; }
		catch (...) { result.failureCategory = ExportFailureCategory::UnexpectedFailure; result.exitCode = 10; result.message = "Export worker raised an unknown exception."; result.errorMessage = result.message; }
	}
	// Cancellation is checked under the same arbiter used by output commit. A
	// successful committed build cannot be relabeled by a late cancel callback.
	if (operation->cancellation->requested.load(std::memory_order_relaxed) && !operation->cancellation->WasCommitted()) {
		result.success = false; result.canceled = true; result.failureCategory = ExportFailureCategory::Canceled; result.exitCode = 130; result.message = "Export canceled.";
	}
	if (operation->cancellation->WasCommitted()) {
		// The committed output is authoritative even if returning/storing its
		// result raised an exception after the filesystem boundary completed.
		if (!result.success) result.message = "Export output committed successfully.";
		result.success = true; result.canceled = false; result.failureCategory = ExportFailureCategory::None; result.exitCode = 0; result.errorMessage.clear();
		if (result.outputDirectory.empty()) result.outputDirectory = operation->observation.intent.request.finalOutputDirectory;
	}
	if (result.runId.empty()) result.runId = operation->observation.intent.request.runId;
	operation->cancellation->MarkTerminal();
	{
		std::lock_guard mapLock(operationsMutex_);
		std::lock_guard mailboxLock(operation->mailbox);
		// All fallible preparation has completed. Publish the final value and
		// finished flag together; raw retained values are never public evidence.
		static_assert(std::is_nothrow_move_assignable_v<ExportResult>);
		if (terminalCount_ == terminalIds_.size()) RetireOldestLocked();
		operation->observation.result = std::move(result);
		operation->observation.progress.finished = true;
		operation->observation.progress.phase = operation->observation.result->success ? ExportPhase::Complete : ExportPhase::Failed;
		++operation->observation.revision;
		terminalIds_[(terminalHead_ + terminalCount_) % terminalIds_.size()] = operation->observation.operationId;
		++terminalCount_;
		if (activeId_ == operation->observation.operationId) activeId_ = 0;
	}
	operation->workerResult.reset();
	operation->workerException = {};
}
void EditorExportOperationController::Poll() {
	RequireOwner();
	std::shared_ptr<Operation> operation;
	{ std::lock_guard lock(operationsMutex_); if (!activeId_) return; operation = operations_.at(activeId_); ++pollVisits_; }
	if (operation->futureObserved || (operation->future.valid() && operation->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) Adopt(operation);
}
EditorExportConsumption EditorExportOperationController::Consume(std::uint64_t id, std::uint64_t observer) {
	RequireOwner(); Poll();
	std::lock_guard mapLock(operationsMutex_);
	const auto found = operations_.find(id); if (found == operations_.end()) return {MissingDispositionLocked(id), {}};
	std::lock_guard mailboxLock(found->second->mailbox);
	if (!found->second->observation.result) return {EditorExportLookupDisposition::Active, {}};
	if (found->second->consumedObservers.contains(observer)) return {EditorExportLookupDisposition::Terminal, {}};
	// A failed receipt copy leaves the observer eligible to retry consumption.
	auto copied = found->second->observation.result;
	found->second->consumedObservers.insert(observer);
	return {EditorExportLookupDisposition::Terminal, std::move(copied)};
}
std::optional<ExportResult> EditorExportOperationController::ConsumeFinal(std::uint64_t id, std::uint64_t observer) {
	return Consume(id, observer).result;
}
std::optional<std::filesystem::path> EditorExportOperationController::TakeLaunch(std::uint64_t id) {
	RequireOwner();
	std::lock_guard mapLock(operationsMutex_);
	const auto found = operations_.find(id); if (found == operations_.end()) return {};
	std::lock_guard mailboxLock(found->second->mailbox);
	auto& operation = *found->second;
	const auto& intent = operation.observation.intent;
	if (!accepting_ || intent.projectGeneration != generation_ || !intent.launchAfterSuccess || operation.launchConsumed || !operation.observation.result || !operation.observation.result->success) return {};
	auto copied = intent.launcherPath;
	operation.launchConsumed = true;
	return copied;
}
ExportCancellationDisposition EditorExportOperationController::RequestCancellation(std::uint64_t id) {
	RequireOwner();
	std::shared_ptr<Operation> operation;
	{ std::lock_guard lock(operationsMutex_); const auto found = operations_.find(id); if (found == operations_.end()) return ExportCancellationDisposition::AlreadyTerminal; operation = found->second; }
	return operation->cancellation->RequestCancellation();
}
void EditorExportOperationController::StopAndDrain() {
	RequireOwner(); accepting_ = false;
	std::shared_ptr<Operation> active;
	{ std::lock_guard lock(operationsMutex_); if (activeId_) active = operations_.at(activeId_); }
	if (active) {
		active->cancellation->RequestCancellation();
		// Physical settlement precedes any eviction. An adoption exception keeps
		// the exact worker outcome owned for retry, with admission still closed.
		// Neither map nor mailbox is locked while waiting for worker completion.
		if (active->future.valid()) active->future.wait();
		Adopt(active);
	}
	std::lock_guard lock(operationsMutex_);
	expiredCount_ += operations_.size();
	operations_.clear(); terminalHead_ = 0; terminalCount_ = 0; activeId_ = 0;
}
void EditorExportOperationController::BeginProject() {
	RequireOwner(); StopAndDrain();
	if (generation_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Export project generation exhausted.");
	++generation_; accepting_ = true;
}

void EditorExportOperationController::RetireOldestLocked() {
	// The owner already holds the map lock. FIFO entries contain only adopted
	// terminal IDs, never active work or an outcome awaiting adoption retry.
	operations_.erase(terminalIds_[terminalHead_]);
	terminalHead_ = (terminalHead_ + 1) % terminalIds_.size();
	--terminalCount_; ++expiredCount_;
}
void EditorExportOperationController::SetTerminalHistoryLimit(std::size_t limit) {
	RequireOwner();
	if (limit == 0) throw std::invalid_argument("Export terminal history limit must be positive.");
	// Prepare replacement storage first. Invalid capacity or allocation failure
	// leaves the existing limit and all retained observations unchanged.
	std::vector<std::uint64_t> replacement(limit);
	std::lock_guard lock(operationsMutex_);
	while (terminalCount_ > limit) RetireOldestLocked();
	for (std::size_t index = 0; index < terminalCount_; ++index) replacement[index] = terminalIds_[(terminalHead_ + index) % terminalIds_.size()];
	terminalIds_.swap(replacement); terminalHead_ = 0;
}
EditorExportRetentionDiagnostics EditorExportOperationController::RetentionDiagnostics() const {
	RequireOwner();
	std::lock_guard lock(operationsMutex_);
	const auto active = activeId_ ? operations_.at(activeId_) : nullptr;
	return {terminalIds_.size(), terminalCount_, active ? 1u : 0u,
		active && active->futureObserved ? 1u : 0u, nextId_ - 1, expiredCount_, pollVisits_};
}
