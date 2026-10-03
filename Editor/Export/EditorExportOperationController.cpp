#include "EditorExportOperationController.h"
#include "ExportPipeline.h"
#include "Engine/ThreadPool/ThreadPool.h"
#include <limits>
#include <stdexcept>
#include <type_traits>

EditorExportOperationController::EditorExportOperationController(Executor executor) : executor_(std::move(executor)) {
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
	auto operation = std::make_shared<Operation>();
	const auto id = nextId_++;
	operation->observation.operationId = id;
	operation->observation.revision = 1;
	operation->observation.intent = std::move(request);
	const auto captured = operation->observation.intent.request;
	const auto execute = executor_;
	try {
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
		ThreadPool::instance().enqueue(task);
	} catch (const std::exception& error) {
		{ std::lock_guard lock(operationsMutex_); operations_.erase(id); }
		activeId_ = 0;
		return {false, 0, error.what()};
	}
	return {true, id, {}};
}
std::optional<EditorExportOperationObservation> EditorExportOperationController::Observe(std::uint64_t id) const {
	std::shared_ptr<Operation> operation;
	{ std::lock_guard lock(operationsMutex_); const auto found = operations_.find(id); if (found == operations_.end()) return {}; operation = found->second; }
	std::lock_guard lock(operation->mailbox);
	return operation->observation;
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
		std::lock_guard lock(operation->mailbox);
		// All fallible preparation has completed. Publish the final value and
		// finished flag together; raw retained values are never public evidence.
		static_assert(std::is_nothrow_move_assignable_v<ExportResult>);
		operation->observation.result = std::move(result);
		operation->observation.progress.finished = true;
		operation->observation.progress.phase = operation->observation.result->success ? ExportPhase::Complete : ExportPhase::Failed;
		++operation->observation.revision;
	}
	operation->workerResult.reset();
	operation->workerException = {};
	if (activeId_ == operation->observation.operationId) activeId_ = 0;
}
void EditorExportOperationController::Poll() {
	RequireOwner();
	std::vector<std::shared_ptr<Operation>> retained;
	{ std::lock_guard lock(operationsMutex_); for (const auto& [_, operation] : operations_) retained.push_back(operation); }
	for (const auto& operation : retained) {
		if (operation->futureObserved || (operation->future.valid() && operation->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) Adopt(operation);
	}
}
std::optional<ExportResult> EditorExportOperationController::ConsumeFinal(std::uint64_t id, std::uint64_t observer) {
	RequireOwner(); Poll();
	std::lock_guard mapLock(operationsMutex_);
	const auto found = operations_.find(id); if (found == operations_.end()) return {};
	std::lock_guard mailboxLock(found->second->mailbox);
	if (!found->second->observation.result || !found->second->consumedObservers.insert(observer).second) return {};
	return found->second->observation.result;
}
std::optional<std::filesystem::path> EditorExportOperationController::TakeLaunch(std::uint64_t id) {
	RequireOwner();
	std::lock_guard mapLock(operationsMutex_);
	const auto found = operations_.find(id); if (found == operations_.end()) return {};
	std::lock_guard mailboxLock(found->second->mailbox);
	auto& operation = *found->second;
	const auto& intent = operation.observation.intent;
	if (!accepting_ || intent.projectGeneration != generation_ || !intent.launchAfterSuccess || operation.launchConsumed || !operation.observation.result || !operation.observation.result->success) return {};
	operation.launchConsumed = true;
	return intent.launcherPath;
}
ExportCancellationDisposition EditorExportOperationController::RequestCancellation(std::uint64_t id) {
	RequireOwner();
	std::shared_ptr<Operation> operation;
	{ std::lock_guard lock(operationsMutex_); const auto found = operations_.find(id); if (found == operations_.end()) return ExportCancellationDisposition::AlreadyTerminal; operation = found->second; }
	return operation->cancellation->RequestCancellation();
}
void EditorExportOperationController::StopAndDrain() {
	RequireOwner(); accepting_ = false;
	// Only this protected owner mutates the map, and closed admission prevents
	// inserts during drain. Retain one shared owner at a time without allocating
	// a snapshot; concurrent readers still use the map lock for their lookups.
	auto visitOwned = [this](auto&& visit) {
		decltype(operations_)::const_iterator cursor;
		{ std::lock_guard lock(operationsMutex_); cursor = operations_.cbegin(); }
		for (;;) {
			std::shared_ptr<Operation> operation;
			{
				std::lock_guard lock(operationsMutex_);
				if (cursor == operations_.cend()) break;
				operation = cursor->second;
				++cursor;
			}
			visit(operation);
		}
	};
	std::exception_ptr firstFailure;
	auto attempt = [&firstFailure](auto&& action) {
		try { action(); }
		catch (...) { if (!firstFailure) firstFailure = std::current_exception(); }
	};
	visitOwned([&](const auto& operation) { attempt([&] { operation->cancellation->RequestCancellation(); }); });
	// Settle every physical worker before preparing any final receipt. No map,
	// mailbox or job-service lock is held during the potentially blocking wait.
	visitOwned([&](const auto& operation) { attempt([&] { if (operation->future.valid()) operation->future.wait(); }); });
	visitOwned([&](const auto& operation) { attempt([&] { Adopt(operation); }); });
	activeId_ = 0;
	if (firstFailure) std::rethrow_exception(firstFailure);
}
void EditorExportOperationController::BeginProject() {
	RequireOwner(); StopAndDrain();
	if (generation_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Export project generation exhausted.");
	++generation_; accepting_ = true;
}
