// Protected editor ownership for both export frontends. Workers own only the
// captured request and shared mailbox, never a window/context/job-service.
#pragma once
#include "ExportTypes.h"
#include <future>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

struct EditorExportOperationRequest {
	ExportRequest request;
	std::uint64_t projectGeneration = 0;
	bool launchAfterSuccess = false;
	std::filesystem::path launcherPath;
};
struct EditorExportOperationObservation {
	std::uint64_t operationId = 0;
	std::uint64_t revision = 0;
	EditorExportOperationRequest intent;
	ExportProgress progress;
	std::optional<ExportResult> result;
};
struct EditorExportAdmission {
	bool accepted = false;
	std::uint64_t operationId = 0;
	std::string error;
};

class EditorExportOperationController final {
public:
	// Dependency injection supplies controlled stage gates in native tests; the
	// ordinary constructor executes the real shared pipeline on ThreadPool.
	using Executor = std::function<ExportResult(const ExportRequest&, ExportProgressCallback, std::shared_ptr<ExportCancellationState>)>;
	explicit EditorExportOperationController(Executor executor = {});
	~EditorExportOperationController();
	std::uint64_t Generation() const { return generation_; }
	// Protected owner query keeps control requests from waiting on the worker
	// occupied by the export they need to cancel or observe.
	bool HasPendingOperation();
	EditorExportAdmission Admit(EditorExportOperationRequest request);
	std::optional<EditorExportOperationObservation> Observe(std::uint64_t id) const;
	std::optional<ExportResult> ConsumeFinal(std::uint64_t id, std::uint64_t observer);
	std::optional<std::filesystem::path> TakeLaunch(std::uint64_t id);
	ExportCancellationDisposition RequestCancellation(std::uint64_t id);
	void Poll();
	void BeginProject();
	void StopAndDrain();
private:
	struct Operation {
		mutable std::mutex mailbox;
		EditorExportOperationObservation observation;
		std::shared_ptr<ExportCancellationState> cancellation = std::make_shared<ExportCancellationState>();
		std::future<ExportResult> future;
		// Observe the worker once before any diagnostic formatting can allocate.
		// A failed owner adoption can retry from this retained truth even though
		// future.get() has already consumed the physical task's terminal value.
		std::optional<ExportResult> workerResult;
		std::exception_ptr workerException;
		bool futureObserved = false;
		std::unordered_set<std::uint64_t> consumedObservers;
		bool launchConsumed = false;
	};
	void RequireOwner() const;
	void Adopt(const std::shared_ptr<Operation>& operation);
	Executor executor_;
	std::thread::id owner_ = std::this_thread::get_id();
	// The map is owner-mutated; a separate lock permits concurrent copied
	// observations without calling Poll/get() from a reader thread.
	mutable std::mutex operationsMutex_;
	std::unordered_map<std::uint64_t, std::shared_ptr<Operation>> operations_;
	std::uint64_t generation_ = 1;
	std::uint64_t nextId_ = 1;
	std::uint64_t activeId_ = 0;
	bool accepting_ = true;
};
