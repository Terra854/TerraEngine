// Protected editor ownership for both export frontends. Workers own only the
// captured request and shared mailbox, never a window/context/job-service.
#pragma once
#include "ExportTypes.h"
#include <future>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

// Missing issued IDs expired; zero and never-issued IDs are unknown. A terminal
// observation is a copied receipt, so evicting controller history cannot erase it.
enum class EditorExportLookupDisposition { Active, Terminal, Expired, Unknown };
struct EditorExportLookup {
	EditorExportLookupDisposition disposition = EditorExportLookupDisposition::Unknown;
	std::optional<EditorExportOperationObservation> observation;
};
struct EditorExportConsumption {
	EditorExportLookupDisposition disposition = EditorExportLookupDisposition::Unknown;
	std::optional<ExportResult> result;
};
struct EditorExportRetentionDiagnostics {
	std::size_t terminalLimit = 64;
	std::size_t retainedTerminal = 0;
	std::size_t active = 0;
	std::size_t adoptionPending = 0;
	std::uint64_t lastIssuedId = 0;
	std::uint64_t expiredCount = 0;
	std::uint64_t pollVisits = 0;
};

class EditorExportOperationController final {
public:
	// Dependency injection supplies controlled stage gates in native tests; the
	// ordinary constructor executes the real shared pipeline on ThreadPool.
	using Executor = std::function<ExportResult(const ExportRequest&, ExportProgressCallback, std::shared_ptr<ExportCancellationState>)>;
	// Native dependency seams stop at owner lifecycle boundaries. They never
	// expose worker mutation or fault controls through editor automation.
	struct OwnerHooks {
		std::function<void()> beforeSubmission;
		std::function<void()> beforeAdoption;
	};
	explicit EditorExportOperationController(Executor executor = {}, OwnerHooks hooks = {});
	~EditorExportOperationController();
	std::uint64_t Generation() const { return generation_; }
	// Protected owner query keeps control requests from waiting on the worker
	// occupied by the export they need to cancel or observe.
	bool HasPendingOperation();
	EditorExportAdmission Admit(EditorExportOperationRequest request);
	EditorExportLookup Lookup(std::uint64_t id) const;
	EditorExportConsumption Consume(std::uint64_t id, std::uint64_t observer);
	void SetTerminalHistoryLimit(std::size_t limit);
	EditorExportRetentionDiagnostics RetentionDiagnostics() const;
	// Compatibility adapters deliberately omit the typed absence reason. New
	// consumers use Lookup/Consume so expiry cannot look like unfinished work.
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
	void RetireOldestLocked();
	EditorExportLookupDisposition MissingDispositionLocked(std::uint64_t id) const;
	Executor executor_;
	OwnerHooks hooks_;
	std::thread::id owner_ = std::this_thread::get_id();
	// The map is owner-mutated; a separate lock permits concurrent copied
	// observations without calling Poll/get() from a reader thread.
	mutable std::mutex operationsMutex_;
	std::unordered_map<std::uint64_t, std::shared_ptr<Operation>> operations_;
	// Fixed storage is allocated before use. Appending an adopted receipt cannot
	// fail halfway between terminal publication and FIFO accounting.
	std::vector<std::uint64_t> terminalIds_;
	std::size_t terminalHead_ = 0;
	std::size_t terminalCount_ = 0;
	std::uint64_t expiredCount_ = 0;
	std::uint64_t pollVisits_ = 0;
	std::uint64_t generation_ = 1;
	std::uint64_t nextId_ = 1;
	std::uint64_t activeId_ = 0;
	bool accepting_ = true;
};
