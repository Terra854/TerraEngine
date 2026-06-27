// WorkerSafety.cpp - thread-local worker context and deterministic violation log.

#include "WorkerSafety.h"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <tuple>
#include <utility>

namespace TerraEngine::Scheduling {
	namespace {
		thread_local bool workerScopeActive = false;
		thread_local std::string workerScopeName;

		std::mutex& ViolationMutex()
		{
			static std::mutex mutex;
			return mutex;
		}

		std::vector<WorkerSafetyViolation>& ViolationLog()
		{
			static std::vector<WorkerSafetyViolation> violations;
			return violations;
		}

		std::string ToString(WorkerSafetySeverity severity)
		{
			switch (severity)
			{
			case WorkerSafetySeverity::Error:
				return "error";
			case WorkerSafetySeverity::Warning:
				return "warning";
			case WorkerSafetySeverity::Info:
			default:
				return "info";
			}
		}

		int SeverityRank(WorkerSafetySeverity severity)
		{
			switch (severity)
			{
			case WorkerSafetySeverity::Error:
				return 0;
			case WorkerSafetySeverity::Warning:
				return 1;
			case WorkerSafetySeverity::Info:
			default:
				return 2;
			}
		}

		void SortViolations(std::vector<WorkerSafetyViolation>& violations)
		{
			std::stable_sort(violations.begin(), violations.end(), [](const WorkerSafetyViolation& left, const WorkerSafetyViolation& right) {
				return std::make_tuple(SeverityRank(left.severity), left.workloadName, left.operationName, left.message)
					< std::make_tuple(SeverityRank(right.severity), right.workloadName, right.operationName, right.message);
			});
		}
	}

	WorkerSafetyScope::WorkerSafetyScope(std::string workloadName)
		: previousWorkloadName(workerScopeName)
		, previousWorkerState(workerScopeActive)
	{
		workerScopeActive = true;
		workerScopeName = std::move(workloadName);
	}

	WorkerSafetyScope::~WorkerSafetyScope()
	{
		workerScopeName = std::move(previousWorkloadName);
		workerScopeActive = previousWorkerState;
	}

	bool IsWorkerSafetyScopeActive()
	{
		return workerScopeActive;
	}

	std::string CurrentWorkerSafetyWorkload()
	{
		return workerScopeName;
	}

	void ReportWorkerSafetyViolation(WorkerSafetySeverity severity, std::string operationName, std::string message, std::string recommendedAction)
	{
		std::scoped_lock lock(ViolationMutex());
		ViolationLog().push_back({
			severity,
			workerScopeName,
			std::move(operationName),
			std::move(message),
			std::move(recommendedAction)
		});
	}

	std::vector<WorkerSafetyViolation> SnapshotWorkerSafetyViolations()
	{
		std::scoped_lock lock(ViolationMutex());
		std::vector<WorkerSafetyViolation> snapshot = ViolationLog();
		SortViolations(snapshot);
		return snapshot;
	}

	std::vector<WorkerSafetyViolation> DrainWorkerSafetyViolations()
	{
		std::scoped_lock lock(ViolationMutex());
		std::vector<WorkerSafetyViolation> drained = std::move(ViolationLog());
		ViolationLog().clear();
		SortViolations(drained);
		return drained;
	}

	std::string FormatWorkerSafetyViolations(const std::vector<WorkerSafetyViolation>& violations)
	{
		if (violations.empty())
		{
			return "Worker safety violations: none";
		}
		std::ostringstream stream;
		stream << "Worker safety violations:";
		for (const WorkerSafetyViolation& violation : violations)
		{
			stream << "\n- " << ToString(violation.severity)
				<< " workload='" << violation.workloadName
				<< "' operation='" << violation.operationName
				<< "': " << violation.message
				<< " Action: " << violation.recommendedAction;
		}
		return stream.str();
	}

}
